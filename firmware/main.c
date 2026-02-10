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

// TI driverlib
#include "sys_ctrl.h"

// PRCM registers (now from driverlib includes via sys_ctrl.h)
#include "prcm.h"
#include "hw_memmap.h"
#include "hw_prcm.h"

// RTC-timed sleep
#include "aon_rtc.h"

// Block buffers for image download (4100 bytes each: 4-byte header + 4096 data)
// bw_buf: cached B/W block, red_buf: cached Red block
static uint8_t bw_buf[BLOCK_XFER_BUFFER_SIZE];
static uint8_t red_buf[BLOCK_XFER_BUFFER_SIZE];
static int8_t bw_cache_id, red_cache_id;  // which block ID is cached (-1 = none)

static void delay_cycles(volatile uint32_t n)
{
    while (n--) __asm volatile ("nop");
}

// Enter sleep with timed wakeup after `seconds` seconds.
// Shuts down RF core (~8mA savings) and polls AON_RTC for accurate timing.
// WFI doesn't reliably wake on CC2630 (PRCM intercepts, NVIC pending IRQ
// doesn't wake CPU without active JLink debug polling). Using RTC-timed
// polling instead: accurate timing, RF off, CPU polls at ~100ms intervals.
static void enter_sleep(uint32_t seconds)
{
    rtt_puts("SLEEP ");
    rtt_put_hex8((seconds >> 8) & 0xFF);
    rtt_put_hex8(seconds & 0xFF);
    rtt_puts("s\r\n");

    // Shutdown RF core (biggest power consumer)
    oepl_rf_shutdown();

    // Use AON_RTC for accurate sleep timing (16.16 format)
    AONRTCEnable();
    uint32_t now = AONRTCCurrentCompareValueGet();
    uint32_t target = now + (seconds << 16);

    // Poll RTC until target time reached (~100ms between checks)
    while ((int32_t)(AONRTCCurrentCompareValueGet() - target) < 0) {
        delay_cycles(480000);  // ~10ms at 48MHz
    }

    rtt_puts("WAKE\r\n");
}

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

// Download a specific block into a buffer, with retries
// Accumulates parts across attempts — missing parts requested on retry
static bool download_block(uint8_t block_id, struct AvailDataInfo *info,
                            uint8_t *buf, uint16_t *out_size)
{
    rtt_puts("B");
    rtt_put_hex8(block_id);

    uint8_t parts_rcvd[BLOCK_REQ_PARTS_BYTES];
    memset(parts_rcvd, 0, sizeof(parts_rcvd));
    memset(buf, 0x00, BLOCK_XFER_BUFFER_SIZE);

    for (uint8_t attempt = 0; attempt < 15; attempt++) {
        if (attempt > 0) {
            rtt_puts("R");
            oepl_hw_delay_ms(500);
        }
        uint8_t got = oepl_radio_request_block(block_id, info->dataVer, info->dataType,
                                                buf, parts_rcvd);
        if (got >= BLOCK_MAX_PARTS) {
            rtt_puts("+");
            *out_size = BLOCK_XFER_BUFFER_SIZE;
            return true;
        }
        // Accept 41/42 only after 8 attempts
        if (got >= BLOCK_MAX_PARTS - 1 && attempt >= 7) {
            rtt_puts("~");
            *out_size = BLOCK_XFER_BUFFER_SIZE;
            return true;
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

// Download image and stream to display.
// On block download failure, fills with white and continues instead of aborting.
// Returns true if image was displayed (even partially).
static bool download_and_display(struct AvailDataInfo *info)
{
    uint32_t data_size = info->dataSize;
    uint8_t data_type = info->dataType;
    uint16_t width = DISPLAY_WIDTH_600X448;
    uint16_t height = DISPLAY_HEIGHT_600X448;
    uint16_t row_bytes = width / 8;           // 75 bytes per 1bpp row
    uint32_t plane_size = (uint32_t)row_bytes * height;  // 33,600 bytes
    bool has_red = (data_type == 0x21) && (data_size >= plane_size * 2);
    dl_failed_blocks = 0;

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

    // Open display for pixel data: DTM1 (cmd 0x10) in one CS frame
    oepl_hw_gpio_set(15, false);  // DC = command
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x10; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_gpio_set(15, true);   // DC = data

    // Stream rows to display
    uint8_t bw_line[75];
    uint8_t red_line[75];
    uint8_t row_4bpp[300];

    for (uint16_t y = 0; y < height; y++) {
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
        oepl_hw_spi_send_raw(row_4bpp, 300);

        // Progress every 64 rows
        if ((y & 0x3F) == 0) {
            rtt_puts(".");
        }
    }

    oepl_hw_spi_cs_deassert();

    if (dl_failed_blocks > 0) {
        rtt_puts("\r\nDATA PARTIAL (");
        rtt_put_hex8(dl_failed_blocks);
        rtt_puts(" failed)\r\n");
    } else {
        rtt_puts("\r\nDATA OK\r\n");
    }

    // DATA_STOP (0x11)
    oepl_hw_gpio_set(15, false);
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x11; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_spi_cs_deassert();

    // DISPLAY_REFRESH (0x12)
    oepl_hw_gpio_set(15, false);
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x12; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_spi_cs_deassert();

    rtt_puts("REF...");

    // Wait for refresh (~26 seconds)
    for (uint32_t i = 0; i < 30000; i++) {
        if (oepl_hw_gpio_get(13)) break;  // BUSY HIGH = ready
        oepl_hw_delay_ms(1);
    }
    rtt_puts("done\r\n");

    // Only send XferComplete if download was fully successful.
    // On partial failure, AP keeps data pending for retry next checkin.
    if (dl_failed_blocks == 0) {
        oepl_radio_send_xfer_complete();
        rtt_puts("XferComplete\r\n");
    } else {
        rtt_puts("Skipping XferComplete (retry next checkin)\r\n");
    }

    return true;
}

int main(void)
{
    // --- Init RTT first (so we can debug early) ---
    rtt_init();

    // --- Power up PERIPH domain (for GPIO) ---
    HWREG(PRCM_BASE + PRCM_O_PDCTL0PERIPH) = 1;
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (HWREG(PRCM_BASE + PRCM_O_PDSTAT0PERIPH) & 1) break;

    // --- Enable GPIO clock ---
    HWREG(PRCM_BASE + PRCM_O_GPIOCLKGR) = 0x01;
    HWREG(PRCM_NONBUF_BASE + PRCM_O_CLKLOADCTL) = 0x01;
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (HWREG(PRCM_BASE + PRCM_O_CLKLOADCTL) & 0x02) break;

    // Wait for RTT client to connect
    delay_cycles(24000000);  // ~3 seconds

    // --- Boot message ---
    rtt_puts("\r\n=== CC2630 OEPL Tag ===\r\n");
    rtt_puts("RST=");
    rtt_put_hex8((uint8_t)SysCtrlResetSourceGet());
    rtt_puts("\r\n");

    // Print MAC in human-readable form
    uint8_t mac[8];
    oepl_rf_get_mac(mac);
    rtt_puts("MAC: ");
    print_mac_msb(mac);
    rtt_puts("\r\n");

    // --- Initialize display ---
    uc8159_init();
    rtt_puts("Display init OK\r\n");

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

    // --- Splash screen: scan for AP and show boot info ---
    {
        int8_t splash_ch = oepl_radio_scan_channels();
        int8_t temp_c;
        uint16_t bat_mv;
        oepl_hw_get_temperature(&temp_c);
        oepl_hw_get_voltage(&bat_mv);
        radio_state_t *rst = oepl_radio_get_state();
        splash_display(mac, bat_mv, temp_c, splash_ch >= 0, rst->current_ieee_ch);
    }

    // --- Main loop: periodic checkin + download ---
    // First 2 checkins use busy-wait (keeps JLink/RTT alive for debugging).
    // After that, use WFI sleep (CPU halts, RF powered off, RTC wakeup).
    uint32_t checkin_count = 0;
    bool use_sleep = false;
    while (1) {
        rtt_puts("\r\n=== Checkin #");
        rtt_put_hex32(checkin_count);
        rtt_puts(" ===\r\n");

        struct AvailDataInfo info;
        memset(&info, 0, sizeof(info));

        bool checkin_ok = do_scan_and_checkin(&info);

        if (checkin_ok) {
            rtt_puts("Checkin OK: dataType=");
            rtt_put_hex8(info.dataType);
            rtt_puts(" nextCheckIn=");
            rtt_put_hex8((info.nextCheckIn >> 8) & 0xFF);
            rtt_put_hex8(info.nextCheckIn & 0xFF);
            rtt_puts("\r\n");

            if (info.dataType != DATATYPE_NOUPDATE) {
                if (download_and_display(&info)) {
                    rtt_puts("*** IMAGE DISPLAYED ***\r\n");
                    oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
                } else {
                    rtt_puts("Display failed\r\n");
                }
            } else {
                rtt_puts("No pending data\r\n");
                oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
            }

            // AP sends nextCheckIn in minutes; convert to seconds
            uint32_t wait_sec = (uint32_t)info.nextCheckIn * 60;
            if (wait_sec < 30) wait_sec = 30;
            if (wait_sec > 3600) wait_sec = 3600;

            if (use_sleep) {
                enter_sleep(wait_sec);
            } else {
                rtt_puts("Sleep ");
                rtt_put_hex8((wait_sec >> 8) & 0xFF);
                rtt_put_hex8(wait_sec & 0xFF);
                rtt_puts("s (busy)\r\n");
                for (uint32_t s = 0; s < wait_sec; s++)
                    delay_cycles(8000000);
            }
        } else {
            if (use_sleep) {
                rtt_puts("Checkin failed, retry in 30s\r\n");
                enter_sleep(30);
            } else {
                rtt_puts("Checkin failed, retry in 30s (busy)\r\n");
                for (uint32_t s = 0; s < 30; s++)
                    delay_cycles(8000000);
            }
        }

        // After WFI sleep, RF core was shut down — re-init before next checkin
        if (use_sleep) {
            rc = oepl_rf_init();
            if (rc != RF_OK) {
                rtt_puts("RF re-init FAILED\r\n");
                goto idle;
            }
            oepl_radio_init();
        }

        checkin_count++;
        if (checkin_count >= 2) {
            use_sleep = true;
        }
    }

idle:
    rtt_puts("Entering idle\r\n");
    while (1) delay_cycles(8000000);

    return 0;
}
