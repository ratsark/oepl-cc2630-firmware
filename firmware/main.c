// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
// -----------------------------------------------------------------------------
//     CC2630 OEPL Tag Firmware
// -----------------------------------------------------------------------------
// Full OEPL tag: scan → checkin → block download → display
// Uses SEGGER RTT for debug output via J-Link.
// -----------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>
#include "rtt.h"
#include "oepl_rf_cc2630.h"
#include "oepl_radio_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "drivers/oepl_display_driver_uc8159_600x448.h"
#include "splash.h"
#include "oepl_ota_cc2630.h"
#include "inflate.h"
#include "oepl_nfc_cc2630.h"
#include "fault.h"

// TI driverlib
#include "sys_ctrl.h"

// PRCM registers (now from driverlib includes via sys_ctrl.h)
#include "prcm.h"
#include "hw_memmap.h"
#include "hw_prcm.h"

// RTC-timed sleep
#include "aon_rtc.h"

// Deep sleep: AON wakeup controller, event routing, interrupts, IO latch
#include "aon_wuc.h"
#include "aon_batmon.h"
#include "hw_aon_batmon.h"
#include "aon_event.h"
#include "interrupt.h"
#include "hw_ints.h"
#include "aon_ioc.h"
#include "hw_rfc_pwr.h"
#include "ioc.h"
#include "gpio.h"
#include "aux_wuc.h"
#include "osc.h"
#include "vims.h"
#include "ddi.h"
#include "hw_ddi_0_osc.h"
#include "hw_aon_wuc.h"
#include "watchdog.h"

// Block buffers for image download (4100 bytes each: 4-byte header + 4096 data)
// bw_buf: cached B/W block (also used by OTA), red_buf: cached Red block
uint8_t bw_buf[BLOCK_XFER_BUFFER_SIZE];
static uint8_t red_buf[BLOCK_XFER_BUFFER_SIZE];
static int8_t bw_cache_id, red_cache_id;  // which block ID is cached (-1 = none)

// Warm boot detection: .noinit survives WFI return path (debug tag with JLink).
// Hardware reset source covers full standby wakeup (battery tag, no debugger).
#define WARMBOOT_MAGIC_A 0xDEE5BEEF
#define WARMBOOT_MAGIC_B 0x2116CAFE
__attribute__((section(".noinit"))) static volatile uint32_t warmboot_magic_a;
__attribute__((section(".noinit"))) static volatile uint32_t warmboot_magic_b;

static void delay_cycles(volatile uint32_t n)
{
    while (n--) {
        if ((n & 0xFFFF) == 0) oepl_hw_wdt_kick();
        __asm volatile ("nop");
    }
}

// AON RTC interrupt handler — wakes CPU from deep sleep (WFI).
// Overrides the weak alias to Default_Handler in startup_cc2630.c.
// Without this ISR and IntEnable(INT_AON_RTC_COMB), WFI hangs forever
// because the Cortex-M3 requires an NVIC-enabled interrupt to wake.
void AON_RTC_Handler(void)
{
    AONRTCEventClear(AON_RTC_CH0 | AON_RTC_CH2);   // CH2: idle tick (oepl_hw_idle)
}

// Enter standby with RTC-timed wakeup.
//
// Sequence follows TI's PowerCC26XX.c Power_sleep() step for step: IOs
// frozen, crystal off, AUX allowed to sleep, RF/serial/peripheral/CPU domains
// off, uLDO requested, VIMS cache off, recharge configured, SLEEPDEEP + WFI.
// Execution resumes after PRCMDeepSleep() on wakeup; the wake path undoes it
// in TI's order (AUX must be back on before anything touches OSC/DDI).
//
// Measured on the bench (PPK2, debugger detached): the previous version,
// which powered off PERIPH only, sat at ~1.8 mA while "asleep".
//
// With a debugger attached the JTAG domain stays on, which prevents standby:
// WFI then simply returns and the wake path runs anyway.
static bool debugger_attached(void)
{
    return (HWREG(0xE000EDF0) & 0x1) != 0;   // CoreDebug DHCSR.C_DEBUGEN
}

// Once SCLK_LF runs from the 32 kHz crystal, bypass the LF clock qualifiers
// (as TI's PowerCC26XX driver does). The qualifier measures the LF clock
// against the HF oscillator, which keeps HF running and blocks standby:
// measured ~1.2 mA "asleep" with them active.
static bool lf_qualifiers_bypassed;
#ifdef DIAG_SLEEP_ONLY
__attribute__((section(".noinit"), used)) static uint32_t sleep_snap[16];
#endif
static void lf_clock_finalize(void)
{
    if (lf_qualifiers_bypassed) return;
    if (OSCClockSourceGet(OSC_SRC_CLK_LF) != OSC_XOSC_LF) return;   // not switched yet
    DDI16BitfieldWrite(AUX_DDI0_OSC_BASE, DDI_0_OSC_O_CTL0,
                       DDI_0_OSC_CTL0_BYPASS_XOSC_LF_CLK_QUAL_M |
                       DDI_0_OSC_CTL0_BYPASS_RCOSC_LF_CLK_QUAL_M,
                       DDI_0_OSC_CTL0_BYPASS_RCOSC_LF_CLK_QUAL_S, 0x3);
    OSCClockLossEventEnable();
    lf_qualifiers_bypassed = true;
    rtt_puts("LF clock on XOSC_LF, qualifiers bypassed\r\n");
}

static void enter_sleep(uint32_t seconds)
{
    lf_clock_finalize();

    rtt_puts("SLEEP ");
    rtt_put_hex8((seconds >> 8) & 0xFF);
    rtt_put_hex8(seconds & 0xFF);
    rtt_puts("s\r\n");

    // Shut down RF core (main loop will re-init after wakeup)
    oepl_rf_shutdown();

    // Idle tick off, or it would wake the tag out of standby every 2 ms
    oepl_hw_idle_tick(false);

    // Configure RTC wakeup (16.16 fixed-point format: seconds in upper 16 bits)
    AONRTCEnable();
    uint32_t now = AONRTCCurrentCompareValueGet();
    uint32_t target = now + (seconds << 16);
    AONRTCCompareValueSet(AON_RTC_CH0, target);
    AONRTCChannelEnable(AON_RTC_CH0);
    AONRTCEventClear(AON_RTC_CH0);

    // Combined event drives INT_AON_RTC_COMB; without it WFI never wakes.
    AONRTCCombinedEventConfig(AON_RTC_CH0);
    IntPendClear(INT_AON_RTC_COMB);
    IntEnable(INT_AON_RTC_COMB);

    // Save warm boot magic (covers a fault/reset during or after sleep)
    warmboot_magic_a = WARMBOOT_MAGIC_A;
    warmboot_magic_b = WARMBOOT_MAGIC_B;

    // Route RTC CH0 to MCU wakeup event
    AONEventMcuWakeUpSet(AON_EVENT_MCU_WU0, AON_EVENT_RTC_CH0);

    bool dbg = debugger_attached();
    rtt_puts(dbg ? "SLEEP WFI (debugger: no standby)\r\n" : "SLEEP standby\r\n");
    // --- no RTT/peripheral/AUX access from here until the wake path restores it ---

    // Step numbers follow TI's PowerCC26XX.c Power_sleep() (cc26x0).
#ifdef DIAG_SLEEP_ONLY
    sleep_snap[1] = HWREG(AUX_DDI0_OSC_BASE + DDI_0_OSC_O_CTL0);   // AUX still on here
#endif
    // 1. Freeze the IOs on the boundary between MCU and AON
    AONIOCFreezeEnable();
    // 2. If XOSC_HF is active, force it off (RF init switches it back on)
    if (OSCClockSourceGet(OSC_SRC_CLK_HF) == OSC_XOSC_HF) {
        OSCHF_SwitchToRcOscTurnOffXosc();
    }
    // 3. Allow AUX to power down (it is forced on at boot and after each
    //    wake; a forced-on AUX blocks standby -- read back as AUXCTL=1)
    AONWUCAuxWakeupEvent(AONWUC_AUX_ALLOW_SLEEP);
    // 4. Make sure writes take effect
    SysCtrlAonSync();
    // 7. Clock settings take effect
    PRCMLoadSet();
    // 8. Request power off of the MCU-voltage-domain domains
    PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE | PRCM_DOMAIN_SERIAL |
                       PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_CPU);
    // 9. Request uLDO during standby (VDCTL.ULDO; without it the chip only
    //    reaches deep sleep on the main regulator, ~1 mA measured)
    PRCMMcuUldoConfigure(true);
    // 10. VIMS cache off and not retained
    uint32_t modeVIMS;
    do { modeVIMS = VIMSModeGet(VIMS_BASE); } while (modeVIMS == VIMS_MODE_CHANGING);
    if (modeVIMS == VIMS_MODE_ENABLED) VIMSModeSet(VIMS_BASE, VIMS_MODE_OFF);
    PRCMCacheRetentionDisable();
    // 11. Recharge parameters
    SysCtrlSetRechargeBeforePowerDown(XOSC_IN_HIGH_POWER_MODE);
    // 12. Make sure all writes have taken effect
    SysCtrlAonSync();

#ifdef DIAG_SLEEP_ONLY
    // Snapshot of the power configuration at the moment of sleep (noinit)
    sleep_snap[0]++;
    sleep_snap[3] = HWREG(AON_WUC_BASE + AON_WUC_O_PWRSTAT);
    sleep_snap[4] = HWREG(AON_WUC_BASE + AON_WUC_O_CTL0);
    sleep_snap[5] = HWREG(AON_WUC_BASE + AON_WUC_O_JTAGCFG);
    sleep_snap[7] = HWREG(PRCM_BASE + PRCM_O_PDSTAT0);
    sleep_snap[8] = HWREG(PRCM_BASE + PRCM_O_PDSTAT1);
    sleep_snap[10] = HWREG(PRCM_BASE + PRCM_O_PDCTL1);
    sleep_snap[11] = HWREG(AON_WUC_BASE + AON_WUC_O_AUXCTL);
    sleep_snap[14] = HWREG(PRCM_BASE + PRCM_O_VDCTL);
    sleep_snap[15] = 0x5AFE5AFE;
#endif
    (void)dbg;

    // 13. Invoke deep sleep to go to STANDBY
    PRCMDeepSleep();

    // --- Wake ---
    // 14. Restore VIMS and cache retention
    if (modeVIMS == VIMS_MODE_ENABLED) VIMSModeSet(VIMS_BASE, modeVIMS);
    PRCMCacheRetentionEnable();
    // 15. Start forcing power to AUX
    AONWUCAuxWakeupEvent(AONWUC_AUX_WAKEUP);
    // 16. Re-power the peripheral domains we use (RF core stays off until RF init)
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL | PRCM_DOMAIN_PERIPH);
    // 18. Clock settings take effect
    PRCMLoadSet();
    // 19. Release request for uLDO
    PRCMMcuUldoConfigure(false);
    // 21. Wait until the domains are back on
    for (volatile uint32_t i = 0; i < 2000000; i++)
        if (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL | PRCM_DOMAIN_PERIPH) == PRCM_DOMAIN_POWER_ON)
            break;
    // 22. RTC shadow values up to date
    SysCtrlAonSync();
    // 24. Disable IO freeze
    AONIOCFreezeDisable();
    SysCtrlAonSync();
    // 25. Wait for AUX to power up (everything OSC/DDI-related needs it)
    for (volatile uint32_t i = 0; i < 5000000; i++)
        if (AONWUCPowerStatusGet() & AONWUC_AUX_POWER_ON) break;
    SysCtrlAdjustRechargeAfterPowerDown(0);

    warmboot_magic_a = 0;
    warmboot_magic_b = 0;
    IntDisable(INT_AON_RTC_COMB);
    IntPendClear(INT_AON_RTC_COMB);
    AONRTCEventClear(AON_RTC_CH0);

    // GPIO clock back on
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMLoadSet();
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (PRCMLoadGet()) break;

    rtt_puts("WAKE\r\n");
}

// Retry interval after `streak` consecutive failures: RETRY_MIN_S doubling,
// capped at RETRY_MAX_S (see the main loop)
#ifndef RETRY_MIN_S
#define RETRY_MIN_S 30
#endif
#ifndef RETRY_MAX_S
#define RETRY_MAX_S 900
#endif
static uint32_t retry_backoff_s(uint8_t streak)
{
    uint32_t s = RETRY_MIN_S;
    while (streak-- > 0 && s < RETRY_MAX_S) s <<= 1;
    return s > RETRY_MAX_S ? RETRY_MAX_S : s;
}

#ifdef BENCH_PIN_WATCH
// -----------------------------------------------------------------------------
//  Bench: what is the 8-pad IC in the top-left corner for?
//
//  It has its own ~17.3 mm antenna, its own supply, and exactly one line to the
//  CC2630 — package pin 32, which the datasheet says is DIO22. The stock
//  firmware's PIN table configures DIO22 and its ISR samples DIO21. Nothing
//  else is known, so watch the line and see what moves it.
//
//  DIO26/27 are the stock firmware's two buttons. They are watched here purely
//  as a positive control: if pressing a button logs an edge, the watcher works,
//  and "DIO22 never fired" is a real result rather than a broken harness.
//
//  Every DIO needs IOC_INPUT_ENABLE to read anything — without it every read
//  returns 0 and the whole scan looks like "all pins low", which cost an
//  afternoon the first time.
//
//    make EXTRA_DEFINES=-DBENCH_PIN_WATCH                 # pull-up (default)
//    make EXTRA_DEFINES="-DBENCH_PIN_WATCH -DBENCH_PIN_WATCH_PULLDOWN"
//    ... -DBENCH_PIN_WATCH_SECS=600                       # default 300
// -----------------------------------------------------------------------------
#ifndef BENCH_PIN_WATCH_SECS
#define BENCH_PIN_WATCH_SECS  300
#endif

static void pw_dec(uint32_t v)
{
    char b[11];
    uint8_t n = 0;
    if (v == 0) { rtt_putc('0'); return; }
    while (v && n < sizeof(b)) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) rtt_putc(b[--n]);
}

static void bench_pin_watch(void)
{
    static const uint8_t pins[] = { 21, 22, 26, 27 };
    const uint8_t n = sizeof(pins) / sizeof(pins[0]);
#ifdef BENCH_PIN_WATCH_PULLDOWN
    const uint32_t pull = IOC_IOPULL_DOWN;
#else
    const uint32_t pull = IOC_IOPULL_UP;
#endif

    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON) { }
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }

    uint8_t prev[sizeof(pins)];
    uint32_t edges[sizeof(pins)];
    for (uint8_t i = 0; i < n; i++) {
        IOCPortConfigureSet(pins[i], IOC_PORT_GPIO, pull | IOC_INPUT_ENABLE);
        GPIO_setOutputEnableDio(pins[i], GPIO_OUTPUT_DISABLE);
        edges[i] = 0;
    }
    oepl_hw_delay_ms(5);                  // let the pulls settle before sampling
    for (uint8_t i = 0; i < n; i++)
        prev[i] = oepl_hw_gpio_get(pins[i]) ? 1 : 0;

    rtt_puts("PINWATCH: ");
    rtt_puts(pull == IOC_IOPULL_UP ? "pull-up" : "pull-down");
    rtt_puts(", start ");
    for (uint8_t i = 0; i < n; i++) {
        rtt_puts(" DIO");
        pw_dec(pins[i]);
        rtt_puts("=");
        pw_dec(prev[i]);
    }
    rtt_puts("\r\n  (DIO26/27 are the stock buttons - press one to prove this works)\r\n");

    // oepl_hw_get_time_ms() is a stub that returns 0, so time comes from the
    // AON RTC instead: AONRTCCurrentCompareValueGet() is seconds in 16.16
    // fixed point, the same source enter_sleep() uses.
    uint32_t t0 = AONRTCCurrentCompareValueGet();
    uint32_t last_report = t0;
    for (;;) {
        uint32_t now = AONRTCCurrentCompareValueGet();
        uint32_t elapsed = (now - t0) >> 16;              // whole seconds
        if (elapsed >= (uint32_t)BENCH_PIN_WATCH_SECS) break;
        oepl_hw_wdt_kick();     // the watchdog bites at ~90 s; this loop is longer

        for (uint8_t i = 0; i < n; i++) {
            uint8_t v = oepl_hw_gpio_get(pins[i]) ? 1 : 0;
            if (v != prev[i]) {
                prev[i] = v;
                edges[i]++;
                rtt_puts("PIN t+");
                pw_dec(elapsed);
                rtt_puts(".");
                pw_dec((((now - t0) & 0xFFFF) * 10u) >> 16);
                rtt_puts("s DIO");
                pw_dec(pins[i]);
                rtt_puts(v ? " ->1\r\n" : " ->0\r\n");
            }
        }

        if (((now - last_report) >> 16) >= 10u) {  // heartbeat: silence stays visibly alive
            last_report = now;
            rtt_puts("PINWATCH alive t+");
            pw_dec(elapsed);
            rtt_puts("s edges");
            for (uint8_t i = 0; i < n; i++) {
                rtt_puts(" D");
                pw_dec(pins[i]);
                rtt_puts("=");
                pw_dec(edges[i]);
            }
            rtt_puts("\r\n");
        }
    }

    rtt_puts("PINWATCH: done, totals");
    for (uint8_t i = 0; i < n; i++) {
        rtt_puts(" DIO");
        pw_dec(pins[i]);
        rtt_puts("=");
        pw_dec(edges[i]);
    }
    rtt_puts("\r\n");
}
#endif // BENCH_PIN_WATCH

static void print_mac_msb(const uint8_t *mac_lsb)
{
    // Print MAC in human-readable MSB-first order (reverse of wire order)
    for (int i = 7; i >= 0; i--) {
        rtt_put_hex8(mac_lsb[i]);
        if (i > 0) rtt_puts(":");
    }
}

static bool do_scan_and_checkin(struct AvailDataInfo *info)
{
    // Scan for AP
    rtt_puts("\r\n--- SCAN ---\r\n");
    int8_t ch = oepl_radio_scan_channels();
#ifdef BENCH_FORCE_SCAN_FAIL
    ch = -1;   // bench: exercise the direct check-in fallback
#endif
    if (ch < 0) {
        // Try direct checkin on all channels as fallback
        rtt_puts("Direct checkin...\r\n");
        for (uint8_t c = 0; c < OEPL_NUM_CHANNELS; c++) {
            rf_status_t rc = oepl_rf_set_channel(c);
            if (rc != RF_OK) continue;

            uint8_t ieee_ch = oepl_channel_map[c];
            radio_state_t *rst = oepl_radio_get_state();
            rst->current_channel = c;
            rst->current_ieee_ch = ieee_ch;
            rst->ap_found = true;
            memset(rst->ap_mac, 0xFF, 8);

            rtt_puts("Ch ");
            rtt_put_hex8(ieee_ch);
            rtt_puts(": ");
            if (oepl_radio_checkin(info)) return true;
        }
        return false;
    }

    // AP found, do checkin
    rtt_puts("AP found, checkin...\r\n");
    return oepl_radio_checkin(info);
}

// Check the BlockData header the AP prepends to each block: data length and
// a 16-bit sum of the data bytes. This catches a known AP failure mode where
// the C6 radio's request to the ESP32 is dropped and it serves the previous
// block's contents relabelled with the new block id.
static bool block_checksum_ok(const uint8_t *buf, uint32_t expected_len)
{
    const struct BlockData *bd = (const struct BlockData *)buf;
    if (bd->size != expected_len) return false;
    uint16_t sum = 0;
    for (uint32_t i = 0; i < expected_len; i++)
        sum += buf[BLOCK_HEADER_SIZE + i];
    return sum == bd->checksum;
}

#define BLOCK_EMPTY_LIMIT 6

// Download a specific block into a buffer, with retries
// Accumulates parts across attempts — missing parts requested on retry
bool download_block(uint8_t block_id, struct AvailDataInfo *info,
                    uint8_t *buf, uint16_t *out_size)
{
    rtt_puts("B");
    rtt_put_hex8(block_id);

    uint8_t parts_rcvd[BLOCK_REQ_PARTS_BYTES];
    memset(parts_rcvd, 0, sizeof(parts_rcvd));
    memset(buf, 0x00, BLOCK_XFER_BUFFER_SIZE);

    uint32_t remaining = info->dataSize - (uint32_t)block_id * BLOCK_DATA_SIZE;
    uint32_t data_len = (remaining > BLOCK_DATA_SIZE) ? BLOCK_DATA_SIZE : remaining;

    uint8_t zero_count = 0;  // consecutive attempts with 0 parts received
    for (uint8_t attempt = 0; attempt < 15; attempt++) {
        if (attempt > 0) {
            rtt_puts("R");
            // After an empty response (no ACK, no parts) the AP is usually
            // busy -- e.g. its ESP32 rendering another tag's content -- for
            // seconds, not milliseconds: give it growing pauses.
            oepl_hw_delay_ms(zero_count ? 500UL * zero_count : 100);
        }
        uint8_t got = oepl_radio_request_block(block_id, info->dataVer, info->dataType,
                                                buf, parts_rcvd);
        // Accept 41/42 only after 8 attempts
        bool complete = (got >= BLOCK_MAX_PARTS) ||
                        (got >= BLOCK_MAX_PARTS - 1 && attempt >= 7);
        if (complete) {
            if (block_checksum_ok(buf, data_len)) {
                rtt_puts(got >= BLOCK_MAX_PARTS ? "+" : "~");
                *out_size = BLOCK_XFER_BUFFER_SIZE;
                return true;
            }
            // Wrong contents: start over with a full (forced) request so the
            // AP re-fetches the block rather than resending its buffer.
            rtt_puts("C!");
            memset(parts_rcvd, 0, sizeof(parts_rcvd));
            continue;
        }
        // If the AP isn't responding at all, give up after BLOCK_EMPTY_LIMIT
        // empty responses in a row (~20 s of patience with the pauses above;
        // 3 wasn't enough to ride out routine AP stalls on the bench)
        if (got == 0) {
            zero_count++;
            if (zero_count >= BLOCK_EMPTY_LIMIT) {
                rtt_puts("X");
                break;
            }
        } else {
            zero_count = 0;
        }
    }
    rtt_puts("!");
    return false;
}

// Ensure a block is in the B/W cache.
// On download failure, fills buffer with white (0x00) and caches the block ID
// to avoid re-attempting the same failed block on every row.
static bool ensure_bw_block(uint8_t block_id, struct AvailDataInfo *info)
{
    if (bw_cache_id == block_id) return true;
    uint16_t sz;
    if (!download_block(block_id, info, bw_buf, &sz)) {
        memset(bw_buf, 0x00, BLOCK_XFER_BUFFER_SIZE);
        bw_cache_id = block_id;
        return false;
    }
    bw_cache_id = block_id;
    return true;
}

// Ensure a block is in the Red cache.
// On download failure, fills buffer with 0x00 (no red) and caches.
static bool ensure_red_block(uint8_t block_id, struct AvailDataInfo *info)
{
    if (red_cache_id == block_id) return true;
    uint16_t sz;
    if (!download_block(block_id, info, red_buf, &sz)) {
        memset(red_buf, 0x00, BLOCK_XFER_BUFFER_SIZE);
        red_cache_id = block_id;
        return false;
    }
    red_cache_id = block_id;
    return true;
}

// Count of blocks that failed download (reset before each image)
static uint8_t dl_failed_blocks;

// Get bytes from the image at a given offset, using cached blocks.
// Each block has a 4-byte BlockData header (size + checksum) followed by
// BLOCK_DATA_SIZE bytes of actual image data. We skip the header.
// On block download failure, uses white data (ensure_*_block fills buffer).
static void get_image_bytes(uint32_t offset, uint8_t *out, uint16_t len,
                             struct AvailDataInfo *info, bool is_red_plane)
{
    while (len > 0) {
        // A failed block means the image won't be shown (see
        // download_and_display): stop fetching the rest
        if (dl_failed_blocks >= 1) {
            memset(out, 0x00, len);
            return;
        }

        uint8_t block_id = (uint8_t)(offset / BLOCK_DATA_SIZE);
        uint16_t block_off = (uint16_t)(offset % BLOCK_DATA_SIZE);
        uint16_t avail = (uint16_t)BLOCK_DATA_SIZE - block_off;
        if (avail > len) avail = len;

        uint8_t *cache;
        if (is_red_plane) {
            if (!ensure_red_block(block_id, info)) dl_failed_blocks++;
            cache = red_buf;
        } else {
            if (!ensure_bw_block(block_id, info)) dl_failed_blocks++;
            cache = bw_buf;
        }

        // Skip BLOCK_HEADER_SIZE (4 bytes) at start of each block
        memcpy(out, &cache[BLOCK_HEADER_SIZE + block_off], avail);
        out += avail;
        offset += avail;
        len -= avail;
    }
}

// Convert 1 byte B/W + 1 byte Red (8 pixels) to 4 bytes of 4bpp UC8159
// B/W: bit=1 → black, bit=0 → white. Red: bit=1 → red (overrides B/W)
// UC8159 4bpp: 0x0=black, 0x3=white, 0x4=red
static void bwr_to_4bpp(uint8_t bw, uint8_t red, uint8_t out[4])
{
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t hi_bw  = (bw >> 7) & 1;
        uint8_t hi_red = (red >> 7) & 1;
        uint8_t lo_bw  = (bw >> 6) & 1;
        uint8_t lo_red = (red >> 6) & 1;

        uint8_t hi_nib = hi_red ? 0x4 : (hi_bw ? 0x0 : 0x3);
        uint8_t lo_nib = lo_red ? 0x4 : (lo_bw ? 0x0 : 0x3);

        out[i] = (hi_nib << 4) | lo_nib;
        bw <<= 2;
        red <<= 2;
    }
}

// --- Stack high-water mark ---
// The stack is whatever sits between .noinit and the top of SRAM, and the
// zlib decoder made that margin worth watching. Paint it at cold boot, scan
// it at each check-in: the figure only ever falls, and it survives standby
// (SRAM is retained and the wake path does not reset SP).
extern uint32_t _enoinit;
#define STACK_PAINT 0x5A5A5A5AUL

static void stack_paint(void)
{
    uint32_t *sp;
    __asm volatile ("mov %0, sp" : "=r" (sp));
    for (uint32_t *p = &_enoinit; p < sp - 16; p++) *p = STACK_PAINT;
}

// Bytes of stack that have never been touched.
static uint32_t stack_free(void)
{
    uint32_t *p = &_enoinit;
    while (*p == STACK_PAINT) p++;
    return (uint32_t)((uint8_t *)p - (uint8_t *)&_enoinit);
}


// Open the panel for pixel data: DTM1 (cmd 0x10), then hold CS for the rows.
static void epd_begin_pixels(void)
{
    oepl_hw_gpio_set(15, false);  // DC = command
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x10; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_gpio_set(15, true);   // DC = data
}

// --- Compressed images (DATATYPE_IMG_ZLIB) ---
//
// The AP compresses pictures for any tag reporting a firmware version at or
// above the threshold in its tagtypes/35.json ("zlib_compression": "27", read
// as hex, so 39). The payload is a 4-byte uncompressed length followed by a
// zlib stream (4 KB window — the AP's miniz is built with a 4 KB dictionary
// and stamps CINFO=4) whose content is a 6-byte header, then plane 1, then
// plane 2 for a red image.
//
// Nothing here fits in RAM, so the compressed image is staged in flash the
// same way firmware is, then decoded straight out of it. Plane 1 goes back to
// flash because the panel wants the two planes interleaved as 4bpp pixels and
// the stream delivers them one after the other; plane 2 is interleaved
// against it row by row as it decodes, so the panel is fed in one pass.
#define IMG_ZLIB_IN_ADDR    OTA_STAGING_ADDR              // sectors 16-20
#define IMG_ZLIB_IN_MAX     (5UL * OTA_SECTOR_SIZE)       // 20 KB of input
#define IMG_PLANE1_ADDR     (OTA_STAGING_ADDR + IMG_ZLIB_IN_MAX)   // sectors 21-29
#define IMG_PLANE1_MAX      (9UL * OTA_SECTOR_SIZE)       // 36 KB >= 33600

struct zlib_sink {
    uint32_t off;              // absolute offset in the decompressed stream
    uint32_t plane_size;       // bytes per plane
    uint8_t  hdr[6];
    uint8_t  planes;
    bool     header_done;
    bool     panel_open;
    bool     failed;
    // plane 1 -> flash, programmed in chunks as it arrives
    uint32_t p1_addr;          // next flash address to program
    uint8_t  chunk[256];
    uint16_t chunk_len;
    // plane 2 -> panel, a row at a time against plane 1 read back from flash
    uint8_t  row[DISPLAY_WIDTH_600X448 / 8];
    uint16_t row_len;
    uint16_t row_y;
};

static struct zlib_sink zsink;

static bool zsink_flush_chunk(void)
{
    if (zsink.chunk_len == 0) return true;
    if ((zsink.p1_addr & (OTA_SECTOR_SIZE - 1)) == 0) {
        if (oepl_flash_erase_sector(zsink.p1_addr) != 0) return false;
    }
    if (oepl_flash_program(zsink.chunk, zsink.p1_addr, zsink.chunk_len) != 0) return false;
    zsink.p1_addr += zsink.chunk_len;
    zsink.chunk_len = 0;
    return true;
}

// One row of plane 2 has arrived: pair it with the same row of plane 1 (from
// flash) and push 4bpp pixels at the panel.
static void zsink_emit_row(void)
{
    static uint8_t row_4bpp[DISPLAY_WIDTH_600X448 / 2];
    const uint8_t *bw = (const uint8_t *)(IMG_PLANE1_ADDR +
                                          (uint32_t)zsink.row_y * (DISPLAY_WIDTH_600X448 / 8));
    for (uint16_t x = 0; x < DISPLAY_WIDTH_600X448 / 8; x++) {
        uint8_t b = (zsink.planes == 2) ? bw[x] : zsink.row[x];
        uint8_t r = (zsink.planes == 2) ? zsink.row[x] : 0;
        bwr_to_4bpp(b, r, &row_4bpp[x * 4]);
    }
    oepl_hw_spi_send_raw(row_4bpp, sizeof(row_4bpp));
    zsink.row_y++;
    zsink.row_len = 0;
    if ((zsink.row_y & 0x3F) == 0) rtt_puts(".");
}

// Called by the decoder with each run of decompressed bytes, in order.
static bool zlib_sink(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    const uint16_t row_bytes = DISPLAY_WIDTH_600X448 / 8;

    while (len && !zsink.failed) {
        // 1. the 6-byte image header
        if (!zsink.header_done) {
            zsink.hdr[zsink.off] = *data++;
            len--;
            if (++zsink.off < sizeof(zsink.hdr)) continue;

            uint16_t w = (uint16_t)(zsink.hdr[1] | (zsink.hdr[2] << 8));
            uint16_t h = (uint16_t)(zsink.hdr[3] | (zsink.hdr[4] << 8));
            zsink.planes = zsink.hdr[5];
            rtt_puts("ZIMG: ");
            rtt_put_hex32(((uint32_t)w << 16) | h);
            rtt_puts(" planes=");
            rtt_put_hex8(zsink.planes);
            rtt_puts("\r\n");
            if (zsink.hdr[0] != sizeof(zsink.hdr) || w != DISPLAY_WIDTH_600X448 ||
                h != DISPLAY_HEIGHT_600X448 || (zsink.planes != 1 && zsink.planes != 2)) {
                rtt_puts("ZIMG: header mismatch\r\n");
                zsink.failed = true;
                return false;
            }
            zsink.plane_size = (uint32_t)row_bytes * DISPLAY_HEIGHT_600X448;
            if (zsink.planes == 2 && zsink.plane_size > IMG_PLANE1_MAX) {
                zsink.failed = true;
                return false;
            }
            zsink.header_done = true;
            zsink.p1_addr = IMG_PLANE1_ADDR;
            // A one-plane image goes straight at the panel, so open it now;
            // a two-plane image waits until plane 1 has been stored.
            if (zsink.planes == 1) {
                epd_begin_pixels();
                zsink.panel_open = true;
            }
            continue;
        }

        uint32_t img_off = zsink.off - sizeof(zsink.hdr);

        // 2. plane 1: to flash for a red image, to the panel for a plain one
        if (img_off < zsink.plane_size && zsink.planes == 2) {
            uint32_t n = zsink.plane_size - img_off;
            if (n > len) n = len;
            for (uint32_t i = 0; i < n; i++) {
                zsink.chunk[zsink.chunk_len++] = data[i];
                if (zsink.chunk_len == sizeof(zsink.chunk) && !zsink_flush_chunk()) {
                    rtt_puts("ZIMG: plane1 flash write failed\r\n");
                    zsink.failed = true;
                    return false;
                }
            }
            data += n; len -= n; zsink.off += n;
            if (zsink.off - sizeof(zsink.hdr) == zsink.plane_size) {
                if (!zsink_flush_chunk()) { zsink.failed = true; return false; }
                oepl_flash_cache_invalidate();   // about to read plane 1 back
                epd_begin_pixels();
                zsink.panel_open = true;
            }
            continue;
        }

        // 3. the plane the panel is fed from, a row at a time
        uint32_t n = len;
        for (uint32_t i = 0; i < n; i++) {
            zsink.row[zsink.row_len++] = data[i];
            if (zsink.row_len == row_bytes) {
                if (zsink.row_y >= DISPLAY_HEIGHT_600X448) {
                    zsink.failed = true;
                    return false;
                }
                zsink_emit_row();
            }
        }
        data += n; len -= n; zsink.off += n;
    }
    return !zsink.failed;
}

// Stage a compressed image in flash, decode it into the panel, refresh.
// Returns true only if the whole stream decoded and checksummed.
static bool download_and_display_zlib(struct AvailDataInfo *info)
{
    rtt_puts("ZIMG: staging ");
    rtt_put_hex32(info->dataSize);
    rtt_puts(" bytes\r\n");

    if (!oepl_ota_stage_download(info, IMG_ZLIB_IN_ADDR, IMG_ZLIB_IN_MAX))
        return false;

    oepl_flash_cache_invalidate();
    const uint8_t *in = (const uint8_t *)IMG_ZLIB_IN_ADDR;
    uint32_t total = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                     ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);

    memset(&zsink, 0, sizeof(zsink));

    rtt_puts("EPD wake...");
    uc8159_wake();
    rtt_puts("OK\r\n");

    uint32_t produced = 0;
    // red_buf is the decoder's 4 KB window: nothing else needs it here, and a
    // second 4 KB buffer would not fit in RAM.
    int rc = inflate_zlib(in + 4, info->dataSize - 4, red_buf, 4096,
                          zlib_sink, NULL, total, &produced);

    if (zsink.panel_open) oepl_hw_spi_cs_deassert();

    if (rc != INFLATE_OK || zsink.failed || zsink.row_y != DISPLAY_HEIGHT_600X448) {
        rtt_puts("\r\nZIMG: decode failed rc=");
        rtt_put_hex8((uint8_t)(-rc));
        rtt_puts(" rows=");
        rtt_put_hex32(zsink.row_y);
        rtt_puts(" out=");
        rtt_put_hex32(produced);
        rtt_puts(" -- not refreshing\r\n");
        uc8159_sleep();
        return false;
    }

    rtt_puts("\r\nZIMG: decoded ");
    rtt_put_hex32(produced);
    rtt_puts(" bytes, checksum OK\r\n");

    // Refresh first, then tell the AP: transmitting during the refresh's peak
    // current loses the frame (see download_and_display).
    uc8159_refresh_and_sleep();
    rtt_puts(oepl_radio_send_xfer_complete()
                 ? "XferComplete ACKed\r\n"
                 : "XferComplete NOT acked (AP will re-offer)\r\n");
    return true;
}

// --- NFC content from the AP ---
//
// OEPL's web UI offers "Set NFC URL" (content mode 14) to any tag that
// reports CAPABILITY_HAS_NFC, and sends the result as DATATYPE_NFC_RAW_CONTENT:
// the bytes are already a finished NDEF TLV (03 <len> D1 01 <len> 55 <prefix>
// <url> FE), so the tag just copies them into the chip's user memory.
// DATATYPE_NFC_URL_DIRECT hands over a bare URL instead and leaves the record
// to us.
static bool download_and_write_nfc(struct AvailDataInfo *info)
{
    if (!oepl_nfc_present()) {
        rtt_puts("NFC: content offered but no chip here\r\n");
        return false;
    }
    uint32_t len = info->dataSize;
    if (len == 0 || len > BLOCK_DATA_SIZE) {
        rtt_puts("NFC: content size out of range\r\n");
        return false;
    }

    bw_cache_id = -1;
    if (!ensure_bw_block(0, info)) {
        rtt_puts("NFC: download failed\r\n");
        return false;
    }
    const uint8_t *payload = &bw_buf[BLOCK_HEADER_SIZE];

    bool ok;
    if (info->dataType == DATATYPE_NFC_URL_DIRECT) {
        // bare URL: wrap it in a URI record ourselves (prefix byte 0 = none)
        uint8_t rec[BLOCK_DATA_SIZE > 256 ? 256 : BLOCK_DATA_SIZE];
        if (len + 9 > sizeof(rec)) return false;
        uint16_t n = 0;
        rec[n++] = 0x03;
        rec[n++] = (uint8_t)(len + 5);
        rec[n++] = 0xD1;
        rec[n++] = 0x01;
        rec[n++] = (uint8_t)(len + 1);
        rec[n++] = 0x55;              // URI record
        rec[n++] = 0x00;              // no prefix substitution
        for (uint32_t i = 0; i < len; i++) rec[n++] = payload[i];
        rec[n++] = 0xFE;
        ok = oepl_nfc_write_user(rec, n);
    } else {
        ok = oepl_nfc_write_user(payload, (uint16_t)len);
    }

    if (ok) {
        rtt_puts("NFC: content written\r\n");
        rtt_puts(oepl_radio_send_xfer_complete()
                     ? "XferComplete ACKed\r\n"
                     : "XferComplete NOT acked (AP will re-offer)\r\n");
    } else {
        rtt_puts("NFC: write failed\r\n");
    }
    return ok;
}

// Put the tag's own identity in the chip, so tapping an unresponsive or flat
// tag still says what it is. The chip answers from the reader's field alone.
static void nfc_write_identity(void)
{
    if (!oepl_nfc_present()) return;

    // Don't overwrite content the AP pushed. "Set NFC URL" writes a URI
    // record (type 'U', 0x55) that is meant to outlive a reboot -- the chip
    // is non-volatile, so a tag can be given a URL once and put back into
    // display mode. Only an empty chip or our own text record gets replaced.
    uint8_t blk[NFC_BLOCK_SIZE];
    if (oepl_nfc_read_block(NFC_FIRST_USER_BLK, blk) &&
        blk[0] == 0x03 && blk[5] == 0x55) {
        rtt_puts("NFC: leaving the URL the AP pushed\r\n");
        return;
    }

    uint8_t mac[8];
    oepl_rf_get_mac(mac);
    int8_t tc = 0;
    uint16_t mv = 0;
    oepl_hw_get_temperature(&tc);
    oepl_hw_get_voltage(&mv);

    static const char hexd[] = "0123456789ABCDEF";
    char text[64];
    char *q = text;
    const char *p = "OEPL ";
    while (*p) *q++ = *p++;
    for (int i = 7; i >= 0; i--) { *q++ = hexd[mac[i] >> 4]; *q++ = hexd[mac[i] & 15]; }
    p = "  v";
    while (*p) *q++ = *p++;
    *q++ = (char)('0' + ((TAG_FW_VERSION & 0xFF) / 10));
    *q++ = (char)('0' + ((TAG_FW_VERSION & 0xFF) % 10));
    *q++ = ' '; *q++ = ' ';
    *q++ = (char)('0' + (mv / 1000)); *q++ = '.';
    *q++ = (char)('0' + ((mv / 100) % 10));
    *q++ = (char)('0' + ((mv / 10) % 10));
    *q++ = 'V';

    uint8_t rec[96];
    uint16_t n = oepl_nfc_make_text(rec, sizeof(rec), text, (uint8_t)(q - text));
    if (n) oepl_nfc_write_user(rec, n);
}

// Download image and stream to display.
// On block download failure, fills with white and continues instead of aborting.
// Returns true if image was displayed (even partially).
static bool download_and_display(struct AvailDataInfo *info)
{
    uint32_t data_size = info->dataSize;
    uint8_t data_type = info->dataType;
    uint16_t width = DISPLAY_WIDTH_600X448;
    uint16_t height = DISPLAY_HEIGHT_600X448;
    uint16_t row_bytes = width / 8;           // 75 (6.0") / 80 (7.5") bytes per 1bpp row
    uint32_t plane_size = (uint32_t)row_bytes * height;  // 33,600 (6.0") / 30,720 (7.5") bytes
    bool has_red = (data_type == 0x21) && (data_size >= plane_size * 2);
    dl_failed_blocks = 0;
#ifdef DIAG_TELEMETRY
    g_diag_requests = g_diag_rf_nok = g_diag_rf_full = 0;
    uint8_t diag_xfer = 0;
#endif

    rtt_puts("DL+DISP: sz=");
    rtt_put_hex32(data_size);
    rtt_puts(" t=");
    rtt_put_hex8(data_type);
    rtt_puts(has_red ? " BWR" : " BW");
    rtt_puts("\r\n");

    bw_cache_id = -1;
    red_cache_id = -1;

    // Full re-init display before each update (UC8159 requires fresh init before each DRF)
    rtt_puts("EPD wake...");
    uc8159_wake();
    rtt_puts("OK\r\n");

    epd_begin_pixels();

    // Stream rows to display
    uint8_t bw_line[DISPLAY_WIDTH / 8];
    uint8_t red_line[DISPLAY_WIDTH / 8];
    uint8_t row_4bpp[DISPLAY_WIDTH / 2];

    for (uint16_t y = 0; y < height; y++) {
        // Abort at the first failed block: a partial image is never refreshed,
        // so downloading the rest would only burn radio time
        if (dl_failed_blocks >= 1) {
            rtt_puts("\r\nABORT: AP not serving data\r\n");
            break;   // no refresh will follow (see below), so stop streaming
        }

        uint32_t bw_offset = (uint32_t)y * row_bytes;

        // Get B/W line (failed blocks auto-fill white via ensure_bw_block)
        get_image_bytes(bw_offset, bw_line, row_bytes, info, false);

        // Get Red line (if 2bpp)
        if (has_red) {
            uint32_t red_offset = plane_size + (uint32_t)y * row_bytes;
            get_image_bytes(red_offset, red_line, row_bytes, info, true);
        } else {
            memset(red_line, 0, row_bytes);
        }

        // Convert to 4bpp (GD bit handles orientation)
        for (uint8_t x = 0; x < row_bytes; x++) {
            bwr_to_4bpp(bw_line[x], red_line[x], &row_4bpp[x * 4]);
        }

        // Send row to display
        oepl_hw_spi_send_raw(row_4bpp, sizeof(row_4bpp));

        // Progress every 64 rows
        if ((y & 0x3F) == 0) {
            rtt_puts(".");
        }
    }

    oepl_hw_spi_cs_deassert();

    if (dl_failed_blocks > 0) {
        // Incomplete image: don't refresh. The panel keeps showing the last
        // good image (its RAM now holds the partial one, but nothing is
        // displayed until DRF), no XferComplete is sent, and the AP re-offers
        // the image at the next check-in. Refreshing here used to paint the
        // missing blocks white -- a whole white panel when the radio died
        // on the first block.
        rtt_puts("\r\nDATA PARTIAL (");
        rtt_put_hex8(dl_failed_blocks);
        rtt_puts(" failed) -- not refreshing\r\n");
        uc8159_sleep();
#ifdef DIAG_TELEMETRY
        oepl_radio_set_diag_report(dl_failed_blocks, 0,
                                   (uint8_t)(g_diag_requests > 255 ? 255 : g_diag_requests),
                                   (uint8_t)(g_diag_rf_nok > 255 ? 255 : g_diag_rf_nok),
                                   (uint8_t)(g_diag_rf_full > 255 ? 255 : g_diag_rf_full));
#endif
        return false;
    }
    rtt_puts("\r\nDATA OK\r\n");

    // Refresh the panel, wait it out, power it off. XferComplete must not be
    // sent until this returns: transmitting during the refresh's peak current
    // draw lost the (then unacknowledged) frame on most cycles, and the AP
    // kept re-offering the image.
    uc8159_refresh_and_sleep();

    // Only send XferComplete if download was fully successful.
    // On partial failure, AP keeps data pending for retry next checkin.
    if (dl_failed_blocks == 0) {
        if (oepl_radio_send_xfer_complete()) {
            rtt_puts("XferComplete ACKed\r\n");
#ifdef DIAG_TELEMETRY
            diag_xfer = g_xfer_last_try;
#endif
        } else {
            rtt_puts("XferComplete NOT acked (AP will re-offer)\r\n");
#ifdef DIAG_TELEMETRY
            diag_xfer = g_xfer_tx_ok ? 0xF : 0xE;
#endif
        }
    } else {
        rtt_puts("Skipping XferComplete (retry next checkin)\r\n");
    }
#ifdef DIAG_TELEMETRY
    oepl_radio_set_diag_report(dl_failed_blocks, diag_xfer,
                               (uint8_t)(g_diag_requests > 255 ? 255 : g_diag_requests),
                               (uint8_t)(g_diag_rf_nok > 255 ? 255 : g_diag_rf_nok),
                               (uint8_t)(g_diag_rf_full > 255 ? 255 : g_diag_rf_full));
#endif

    return true;
}

int main(void)
{
    // --- Init RTT first (so we can debug early) ---
    rtt_init();

    // --- Check for warm boot (wakeup from standby) ---
    // Primary: IOC latch frozen (IOCLATCH_EN==0) — set by enter_sleep() before
    //   standby, synced via SysCtrlAonSync. TI-recommended detection method.
    // Fallback: SRAM magic in .noinit (survives WFI return + fault resets).
    bool ioc_frozen = (HWREG(AON_IOC_BASE + AON_IOC_O_IOCLATCH) & AON_IOC_IOCLATCH_EN) == 0;
    bool warm_boot = ioc_frozen ||
                     (warmboot_magic_a == WARMBOOT_MAGIC_A &&
                      warmboot_magic_b == WARMBOOT_MAGIC_B);
    warmboot_magic_a = 0;
    warmboot_magic_b = 0;

    // Post-standby adjustments
    if (ioc_frozen) {
        AONIOCFreezeDisable();
        SysCtrlAonUpdate();
        SysCtrlAdjustRechargeAfterPowerDown(0);
    }

    // --- Power up PERIPH domain (for GPIO) ---
    HWREG(PRCM_BASE + PRCM_O_PDCTL0PERIPH) = 1;
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (HWREG(PRCM_BASE + PRCM_O_PDSTAT0PERIPH) & 1) break;

    // --- Enable GPIO clock ---
    HWREG(PRCM_BASE + PRCM_O_GPIOCLKGR) = 0x01;
    HWREG(PRCM_NONBUF_BASE + PRCM_O_CLKLOADCTL) = 0x01;
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (HWREG(PRCM_BASE + PRCM_O_CLKLOADCTL) & 0x02) break;

    if (warm_boot) {
        delay_cycles(4800000);  // ~0.1s (shorter — no RTT client to wait for)
        rtt_puts("\r\n=== CC2630 OEPL Tag (WARM) ===\r\n");
    } else {
        delay_cycles(24000000);  // ~3 seconds (wait for RTT client)
        rtt_puts("\r\n=== CC2630 OEPL Tag ===\r\n");
    }

    // --- Boot message ---
    rtt_puts("RST=");
    rtt_put_hex8((uint8_t)SysCtrlResetSourceGet());
    rtt_puts(" IOC=");
    rtt_put_hex8(ioc_frozen ? 1 : 0);
    rtt_puts(" WARM=");
    rtt_put_hex8(warm_boot ? 1 : 0);
    rtt_puts("\r\n");


    if (!warm_boot) stack_paint();


    // The JTAG power domain is left on by the ROM boot. Nothing needs it once
    // the tag is running on its own, and TI's Power driver turns it off for
    // the same reason. Keep it on while a debugger is actually attached, or
    // the session dies on the next sleep.
#ifndef BENCH_JTAG_STAYS_ON
    if (!debugger_attached()) {
        AONWUCJtagPowerOff();
    }
#endif

    // Watchdog: from here on anything that hangs for ~90s gets reset.
    oepl_hw_wdt_init();

    // A watchdog reset is a warm reset with no fault record: synthesise one
    // so it is reported like a crash (PC 0xDEADD006 = watchdog). Standby
    // wakeups also come through reset, so only a cold boot counts.
    if (!warm_boot && SysCtrlResetSourceGet() == RSTSRC_WARMRESET &&
        !fault_record_valid(&g_fault)) {
        g_fault.pc = FAULT_PC_WATCHDOG;
        g_fault.lr = g_fault.sp = g_fault.cfsr = g_fault.bfar = 0;
        fault_record_seal(&g_fault);
    }

#ifdef DIAG_SLEEP_ONLY
    // make DIAG_SLEEP_ONLY=1: every DIO high-impedance except DIO5 (panel
    // supply) held low and DIO11 (SPI flash CS) held high, no radio, no
    // display -- just standby in a loop. The
    // PPK2 reading is then the MCU + board floor with nothing driven.
    // Pin sweep for hunting the board's sleep floor: DIAG_PIN_MODE picks what
    // the pins in DIAG_PIN_MASK get instead of hi-Z (0 = none/hi-Z, 1 = pull
    // down, 2 = pull up, 3 = driven low, 4 = driven high). A load fed through
    // a pin shows up as a step in the standby current for the group that
    // feeds it. Defaults reproduce the plain hi-Z build.
#ifndef DIAG_PIN_MODE
#define DIAG_PIN_MODE 0
#endif
#ifndef DIAG_PIN_MASK
#define DIAG_PIN_MASK 0xFFFFFFFFu
#endif
    for (uint8_t dio = 0; dio <= 30; dio++) {
        if (dio == 5 || dio == 11) continue;
        bool swept = (DIAG_PIN_MODE != 0) && ((DIAG_PIN_MASK >> dio) & 1);
        if (swept && DIAG_PIN_MODE >= 3) {
            IOCPinTypeGpioOutput(dio);
            GPIO_setOutputEnableDio(dio, GPIO_OUTPUT_ENABLE);
            if (DIAG_PIN_MODE == 3) GPIO_clearDio(dio); else GPIO_setDio(dio);
            continue;
        }
        uint32_t pull = IOC_NO_IOPULL;
        if (swept) pull = (DIAG_PIN_MODE == 1) ? IOC_IOPULL_DOWN : IOC_IOPULL_UP;
        GPIO_setOutputEnableDio(dio, GPIO_OUTPUT_DISABLE);
        IOCPortConfigureSet(dio, IOC_PORT_GPIO, pull | IOC_INPUT_DISABLE);
    }
    rtt_puts("PIN SWEEP mode=");
    rtt_put_hex8(DIAG_PIN_MODE);
    rtt_puts(" mask=");
    rtt_put_hex32(DIAG_PIN_MASK);
    rtt_puts("\r\n");
    IOCPinTypeGpioOutput(5);
    GPIO_setOutputEnableDio(5, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(5);   // panel supply off (high = on: +160 uA measured)
    IOCPinTypeGpioOutput(11);          // SPI flash CS: deselected
    GPIO_setOutputEnableDio(11, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(11);
    oepl_hw_flash_deep_sleep();
    GPIO_setOutputEnableDio(9, GPIO_OUTPUT_DISABLE);
    GPIO_setOutputEnableDio(10, GPIO_OUTPUT_DISABLE);
    IOCPortConfigureSet(9, IOC_PORT_GPIO, IOC_NO_IOPULL | IOC_INPUT_DISABLE);
    IOCPortConfigureSet(10, IOC_PORT_GPIO, IOC_NO_IOPULL | IOC_INPUT_DISABLE);

    rtt_puts("DIAG_SLEEP_ONLY\r\n");
    while (1) {
        enter_sleep(20);
        // What the power configuration actually looked like at the moment of
        // the sleep call. With a debugger attached the part never reaches
        // standby, but these are the registers that decide whether it would.
        static const char *names[] = { "wakes", "OSC_CTL0", "", "AONPWRSTAT",
                                       "AONCTL0", "JTAGCFG", "", "PDSTAT0",
                                       "PDSTAT1", "", "PDCTL1", "AUXCTL",
                                       "", "", "VDCTL", "magic" };
        for (uint8_t i = 0; i < 16; i++) {
            if (!names[i][0]) continue;
            rtt_puts(names[i]);
            rtt_puts("=");
            rtt_put_hex32(sleep_snap[i]);
            rtt_puts(" ");
        }
        rtt_puts("\r\n");
    }
#endif

    // Report the last OTA apply (written from RAM by apply_ota, read here on
    // the first boot of the image it installed)
    struct ota_apply_stat ota_stat = {0};
    bool ota_applied = oepl_ota_take_apply_report(&ota_stat);
    if (ota_applied) {
        rtt_puts("OTA apply: sectors=");
        rtt_put_hex8(ota_stat.sectors);
        rtt_puts(" retries=");
        rtt_put_hex8(ota_stat.retries);
        rtt_puts(" bad=");
        rtt_put_hex8(ota_stat.bad);
        rtt_puts("\r\n");
    }

    // Report a HardFault from the previous run (record written by the handler)
    bool had_fault = fault_record_valid(&g_fault);
    // A cold boot means any crash capture in RAM predates the power cycle
    // (SRAM keeps its contents for a moment) -- discard it, so tooling can't
    // mistake a stale capture for a fresh one.
    if (!had_fault) g_crash.magic = 0;
    g_fault.magic = 0;                 // consumed (or rejected): don't re-report
    struct fault_record last_fault = {0};
    char fault_str[40] = "";
    if (had_fault) {
        last_fault = g_fault;
        rtt_puts("LAST FAULT: PC="); rtt_put_hex32(last_fault.pc);
        rtt_puts(" LR="); rtt_put_hex32(last_fault.lr);
        rtt_puts(" SP="); rtt_put_hex32(last_fault.sp);
        rtt_puts(" CFSR="); rtt_put_hex32(last_fault.cfsr);
        rtt_puts(" BFAR="); rtt_put_hex32(last_fault.bfar);
        rtt_puts("\r\n");
        // Shown in red on the splash so a sealed tag's crash can be read off
        // the panel: "FAULT PC=xxxxxxxx CFSR=xxxxxxxx"
        static const char hexd[] = "0123456789ABCDEF";
        char *q = fault_str;
        memcpy(q, "FAULT PC=", 9); q += 9;
        for (int i = 28; i >= 0; i -= 4) *q++ = hexd[(last_fault.pc >> i) & 0xF];
        memcpy(q, " CFSR=", 6); q += 6;
        for (int i = 28; i >= 0; i -= 4) *q++ = hexd[(last_fault.cfsr >> i) & 0xF];
        *q = 0;
    }

    // Print MAC in human-readable form
    uint8_t mac[8];
    oepl_rf_get_mac(mac);
    rtt_puts("MAC: ");
    print_mac_msb(mac);
    rtt_puts("\r\n");

    // --- Board peripherals to their low-power state ---
    // Panel: unpowered, lines released. Don't run uc8159_init() here: it
    // powers the panel, and nothing would power it off again before standby;
    // both draw paths call uc8159_wake(). External flash: deep power-down.
    oepl_hw_gpio_init();
    oepl_hw_epd_pins_off();
#ifndef BENCH_NO_FLASH_DPD
    // Deep power-down for the external SPI flash. Done on every boot, not
    // just a cold one: a warm boot (our own reset) leaves it in standby,
    // which costs far more than deep power-down on a typical SPI NOR.
    oepl_hw_flash_deep_sleep();
#endif

    // --- NFC chip (a per-variant option; absent boards just skip it) ---
    if (!warm_boot && oepl_nfc_init()) nfc_write_identity();

    // --- Initialize RF core ---
    rf_status_t rc = oepl_rf_init();
    if (rc != RF_OK) {
        rtt_puts("RF init FAILED: ");
        rtt_put_hex8(rc);
        rtt_puts("\r\n");
        goto idle;
    }
    rtt_puts("RF init OK\r\n");

    // --- Initialize OEPL radio protocol layer ---
    oepl_radio_init();

    // --- Splash screen only on cold boot ---
    if (!warm_boot) {
        rtt_puts("SPLASH: drawing\r\n");
        oepl_radio_set_wakeup_reason(WAKEUP_REASON_SPLASH);
        int8_t splash_ch = oepl_radio_scan_channels();
        int8_t temp_c;
        uint16_t bat_mv;
        oepl_hw_get_temperature(&temp_c);
        oepl_hw_get_voltage(&bat_mv);
        radio_state_t *rst = oepl_radio_get_state();
        splash_display(mac, bat_mv, temp_c, splash_ch >= 0, rst->current_ieee_ch,
                       had_fault ? fault_str : NULL);
        rtt_puts("SPLASH: done\r\n");
    } else {
        rtt_puts("WARM BOOT: skipping splash\r\n");
    }

#ifdef BENCH_PIN_WATCH
    bench_pin_watch();
#endif

#ifdef BENCH_SPLASH_LOOP
    // Bench: refresh the panel, sleep, refresh again. Checks that the state
    // the pins are left in during standby still lets the panel come back.
    for (int bi = 0; bi < 3; bi++) {
        enter_sleep(20);
        int8_t bt; uint16_t bv;
        oepl_hw_get_temperature(&bt);
        oepl_hw_get_voltage(&bv);
        rtt_puts("BENCH: refresh after sleep\r\n");
        splash_display(mac, bv, bt, true, 11, NULL);
        rtt_puts("BENCH: refresh done\r\n");
    }
#endif

    // Tell the AP the first checkin after a crash is a fault reset (0xFE);
    // it shows up as wakeupReason in the AP tag DB, with the fault PC/status
    // encoded in that one checkin's battery/temperature/LQI fields.
    if (had_fault) {
        oepl_radio_set_wakeup_reason(WAKEUP_REASON_WDT_RESET);
        if (last_fault.pc == FAULT_PC_RF_DOORBELL) {
            uint32_t op = last_fault.lr;
            uint16_t cmd = (op & 1) ? (uint16_t)(0x8000 | ((op >> 16) & 0x7FFF))
                                    : (uint16_t)(op & 0x7FFF);
            oepl_radio_set_fault_report(FAULT_CLASS_DOORBELL, cmd,
                                        (uint8_t)((last_fault.sp & 3) << 6 | (last_fault.cfsr & 0x3F)));
        } else if (last_fault.pc == FAULT_PC_WATCHDOG) {
            oepl_radio_set_fault_report(FAULT_CLASS_WATCHDOG, 0, 0);
        } else {
            uint8_t cls = FAULT_CLASS_HARDFAULT | ((last_fault.pc >> 16) & 3) |
                          (((last_fault.pc >> 28) == 2) ? 4 : 0);
            oepl_radio_set_fault_report(cls, (uint16_t)(last_fault.pc & 0xFFFF),
                                        (uint8_t)((((last_fault.cfsr >> 16) & 0xF) << 4) |
                                                  ((last_fault.cfsr >> 8) & 0xF)));
        }
    }

    // A tag in standby can't be probed over JTAG, so how the apply went rides
    // to the AP in the first check-in after it, on the same telemetry channel
    // as a fault report (a real fault takes precedence).
    if (ota_applied && !had_fault) {
        oepl_radio_set_fault_report(FAULT_CLASS_OTA_APPLY,
                                    (uint16_t)(ota_stat.retries << 8 | ota_stat.bad),
                                    ota_stat.sectors);
    }

    // --- Main loop: periodic checkin + sleep ---
    // Standby (RTC wakeup) between every check-in.
    //
    // Back-off: when the AP can't be reached (AP down, out of range) or an
    // update can't be completed, retry after 30 s, 60 s, 120 s ... up to
    // 15 min instead of every 30 s. A failed check-in can cost ~30 s of
    // receive (scan plus direct check-in on every channel) and a failed
    // download up to a couple of minutes at ~8 mA; at a fixed 30 s retry a
    // tag in a bad spot could spend tens of mAh a day on nothing. 15 min
    // stays inside the AP's 20-minute window for pending data.
    uint32_t checkin_count = 0;
    uint8_t checkin_fail_streak = 0;
    uint32_t xfer_hold_until = 0;      // AON RTC second to allow transfers again
    uint64_t xfer_hold_ver = 0;        // the dataVer that earned the hold
    uint8_t xfer_fail_streak = 0;
    while (1) {
        rtt_puts("\r\nstack free=");
        rtt_put_hex32(stack_free());
        rtt_puts("\r\n=== Checkin #");
        rtt_put_hex32(checkin_count);
        rtt_puts(" ===\r\n");

        struct AvailDataInfo info;
        memset(&info, 0, sizeof(info));

#ifdef BENCH_FORCE_CHECKIN_FAIL
        bool checkin_ok = false;
#else
        bool checkin_ok = do_scan_and_checkin(&info);
#endif
        if (oepl_rf_hung()) {
            // Radio core wedged during this check-in: report it once and let
            // the shutdown/re-init either side of the next sleep clear it.
            uint32_t op = oepl_rf_hung_op();
            uint16_t cmd = (op & 1) ? (uint16_t)(0x8000 | ((op >> 16) & 0x7FFF))
                                    : (uint16_t)(op & 0x7FFF);
            oepl_radio_set_fault_report(FAULT_CLASS_DOORBELL, cmd,
                                        (uint8_t)((oepl_rf_hung_phase() & 3) << 6 |
                                                  oepl_rf_hung_cmdsta()));
            rtt_puts("RF hung: recovering at next wake\r\n");
            oepl_rf_hung_clear();
            checkin_ok = false;
        }
        uint32_t wait_sec;

        if (checkin_ok) {
            checkin_fail_streak = 0;
            rtt_puts("Checkin OK: dataType=");
            rtt_put_hex8(info.dataType);
            rtt_puts(" nextCheckIn=");
            rtt_put_hex8((info.nextCheckIn >> 8) & 0xFF);
            rtt_put_hex8(info.nextCheckIn & 0xFF);
            rtt_puts("\r\n");

            bool xfer_failed = false;
            bool xfer_done = false;   // only a completed update clears the failure streak
            // After a failed transfer the *transfer* backs off, not the
            // check-in: a tag that stops checking in looks dead to the AP for
            // up to RETRY_MAX_S, and an image the tag keeps rejecting would
            // otherwise cost a 21 KB download every minute. So keep the AP's
            // cadence and just refuse to start a new transfer until the hold
            // expires. AON RTC seconds survive standby.
            // A different dataVer means the AP is offering something new —
            // most likely the fix for whatever was failing — so it gets a
            // fresh attempt rather than inheriting the old hold. Not a free
            // one, though: an AP with several transfers queued offers a
            // different dataVer every check-in, and clearing the hold outright
            // turns that into a continuous download loop (seen on the bench).
            // Shrink the hold to one check-in instead of dropping it.
            if (xfer_hold_until && info.dataVer != xfer_hold_ver) {
                xfer_fail_streak = 0;
                uint32_t floor = AONRTCSecGet() + RETRY_MIN_S;
                if (xfer_hold_until > floor) xfer_hold_until = floor;
            }
            bool xfer_held = xfer_hold_until && (AONRTCSecGet() < xfer_hold_until);
            if (xfer_held && info.dataType != DATATYPE_NOUPDATE) {
                rtt_puts("Transfer held ");
                rtt_put_hex32(xfer_hold_until - AONRTCSecGet());
                rtt_puts("s more after x");
                rtt_put_hex8(xfer_fail_streak);
                rtt_puts(" failures\r\n");
                oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
            } else if (info.dataType == DATATYPE_FW_UPDATE) {
                if (oepl_ota_already_applied(info.dataVer)) {
                    rtt_puts("OTA: already applied, sending XferComplete\r\n");
                    oepl_radio_send_xfer_complete();
                    xfer_done = true;
                } else {
                    rtt_puts("*** FW UPDATE ***\r\n");
                    oepl_ota_download_and_apply(&info);
                    // Returns only on failure — will retry next checkin
                    rtt_puts("FW update failed, retry later\r\n");
                    xfer_failed = true;
                }
            } else if (info.dataType != DATATYPE_NOUPDATE) {
#ifdef BENCH_FORCE_XFER_FAIL
                bool shown = false;
#else
                bool shown;
                if (info.dataType == DATATYPE_NFC_RAW_CONTENT ||
                    info.dataType == DATATYPE_NFC_URL_DIRECT) {
                    shown = download_and_write_nfc(&info);
                } else if (info.dataType == DATATYPE_IMG_ZLIB) {
                    shown = download_and_display_zlib(&info);
                } else {
                    shown = download_and_display(&info);
                }
#endif
                if (shown) {
                    rtt_puts("*** IMAGE DISPLAYED ***\r\n");
                    xfer_done = true;
                } else {
                    rtt_puts("Display failed, will retry\r\n");
                    xfer_failed = true;
                }
                oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
            } else {
                rtt_puts("No pending data\r\n");
                oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
            }

            // AP sends nextCheckIn in minutes; convert to seconds
            wait_sec = (uint32_t)info.nextCheckIn * 60;
            if (wait_sec < RETRY_MIN_S) wait_sec = RETRY_MIN_S;
            if (wait_sec > 3600) wait_sec = 3600;

            if (xfer_failed) {
                if (xfer_fail_streak < 8) xfer_fail_streak++;
                xfer_hold_until = AONRTCSecGet() + retry_backoff_s(xfer_fail_streak);
                xfer_hold_ver = info.dataVer;
                rtt_puts("Update failed x");
                rtt_put_hex8(xfer_fail_streak);
                rtt_puts(", holding transfers ");
                rtt_put_hex32(retry_backoff_s(xfer_fail_streak));
                rtt_puts("s\r\n");
            } else if (xfer_done) {
                // (A "no pending data" check-in doesn't reset it: the AP can
                // flap between offering and not offering the same image.)
                xfer_fail_streak = 0;
                xfer_hold_until = 0;
            }
        } else {
            wait_sec = retry_backoff_s(checkin_fail_streak);
            if (checkin_fail_streak < 8) checkin_fail_streak++;
            rtt_puts("Checkin failed x");
            rtt_put_hex8(checkin_fail_streak);
            rtt_puts(", back off\r\n");
        }

#ifdef BENCH_TEST_HARDFAULT
        // Bench: a real bus fault (RF core registers with the domain off) on the
        // 3rd check-in, to verify the HardFault record against a known location
        if (checkin_count == 2) {
            rtt_puts("BENCH: forcing HardFault\r\n");
            PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);
            oepl_rf_rx_stop();
        }
#endif
#ifdef BENCH_TEST_DOORBELL_HANG
        // Bench: provoke a doorbell timeout (RF core powered off, then abort)
        // on the 3rd check-in to exercise crash capture and the fault report.
        if (checkin_count == 2) {
            rtt_puts("BENCH: forcing doorbell hang\r\n");
            HWREG(RFC_PWR_NONBUF_BASE + RFC_PWR_O_PWMCLKEN) = 0;   // RFCClockDisable(): CPE stops answering
            oepl_rf_rx_stop();
        }
#endif
        enter_sleep(wait_sec);

        // After sleep, RF core was shut down — re-init before next checkin
        {
            bool rf_ok = false;
            for (uint8_t retry = 0; retry < 5; retry++) {
                if (retry > 0) {
                    rtt_puts("RF re-init retry ");
                    rtt_put_hex8(retry);
                    rtt_puts("\r\n");
                    delay_cycles(48000000);  // ~1s delay between retries
                }
                rc = oepl_rf_init();
                if (rc == RF_OK) {
                    rf_ok = true;
                    break;
                }
            }
            if (!rf_ok) {
                // All retries failed — system reset instead of dying
                rtt_puts("RF re-init FAILED x5, RESET\r\n");
                // Brief delay so RTT/UART output flushes
                delay_cycles(4800000);
                // Reset via ROM Hard-API
                typedef void (*reset_fn_t)(void);
                uint32_t *hapi_table = (uint32_t *)0x10000048;
                reset_fn_t rom_reset = (reset_fn_t)hapi_table[6];
                rom_reset();
                // Should not reach here
                while (1) __asm volatile("nop");
            }
            oepl_radio_init();
        }

        checkin_count++;
    }

idle:
    // Fatal error — wait 10 seconds then reset to try again
    rtt_puts("Fatal error, reset in 10s\r\n");
    for (uint8_t i = 0; i < 10; i++)
        delay_cycles(48000000);  // ~1s each
    {
        typedef void (*reset_fn_t)(void);
        uint32_t *hapi_table = (uint32_t *)0x10000048;
        reset_fn_t rom_reset = (reset_fn_t)hapi_table[6];
        rom_reset();
    }
    while (1) __asm volatile("nop");

    return 0;
}
