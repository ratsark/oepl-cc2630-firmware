// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
// -----------------------------------------------------------------------------
//  CC2630 Hardware Abstraction Layer for OEPL display
//  Bare-metal using TI cc26x0 driverlib
// -----------------------------------------------------------------------------

#include "oepl_hw_abstraction_cc2630.h"
#include "rtt.h"
#include "drivers/oepl_display_driver_uc8159_600x448.h"

// TI driverlib
#include "ssi.h"
#include "gpio.h"
#include "ioc.h"
#include "prcm.h"
#include "hw_memmap.h"
#include "hw_types.h"  // for HWREG
#include "aon_batmon.h"
#include "watchdog.h"
#include "cpu.h"
#include "sys_ctrl.h"
#include "hw_ints.h"
#include "interrupt.h"
#include "aon_rtc.h"

// Pin assignments — from STOCK FIRMWARE binary analysis (v29)
// SPI pins: MOSI/MISO swapped vs OEPL HAL! Stock has mosiPin=9, misoPin=8
#define PIN_SPI_MOSI            9   // DIO9  — SSI0_TX (MOSI) — data TO display
#define PIN_SPI_MISO            8   // DIO8  — SSI0_RX (MISO) — data FROM display
#define PIN_SPI_CLK             10  // DIO10 — SSI0_CLK
// Display control pins — from stock firmware binary analysis
#define PIN_DISPLAY_BUSY        13  // DIO13 — BUSY input (HIGH=ready, LOW=busy)
#define PIN_DISPLAY_RST         14  // DIO14 — Reset (active LOW)
#define PIN_DISPLAY_DC          15  // DIO15 — Data/Command
#define PIN_SPI_CS              20  // DIO20 — EPD display CS
#define PIN_EPD_BS              18  // DIO18 — Bus select (LOW=4-wire SPI)
#define PIN_EPD_DIR             12  // DIO12 — SDA direction (LOW=write, HIGH=read)
#define PIN_EPD_POWER           5   // DIO5  — EPD power enable (high = on, confirmed by current draw)
#define PIN_FLASH_CS            11  // DIO11 — SPI flash CS

#define SPI_BITRATE             4000000  // 4 MHz
#define SYSTEM_CLOCK_HZ         48000000

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void oepl_hw_init(void)
{
    // PERIPH power domain and GPIO clock are already enabled in main.c
}

void oepl_hw_spi_init(void)
{
    // Power up SERIAL domain (required for SSI0)
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL);
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON);

    // Enable SSI0 peripheral clock
    PRCMPeripheralRunEnable(PRCM_PERIPH_SSI0);
    PRCMLoadSet();
    while (!PRCMLoadGet());

    // Configure SPI pins via IOC
    // IOCPinTypeSsiMaster(base, rxPin, txPin, fssPin, clkPin)
    // Stock firmware: DIO9=TX(MOSI), DIO8=RX(MISO), DIO10=CLK
    IOCPinTypeSsiMaster(SSI0_BASE, PIN_SPI_MISO, PIN_SPI_MOSI,
                        IOID_UNUSED, PIN_SPI_CLK);

    // Configure SSI0: SPI Mode 0, Master, 4MHz, 8-bit
    SSIConfigSetExpClk(SSI0_BASE, SYSTEM_CLOCK_HZ, SSI_FRF_MOTO_MODE_0,
                       SSI_MODE_MASTER, SPI_BITRATE, 8);
    SSIEnable(SSI0_BASE);

    // Drain any stale data from RX FIFO
    uint32_t dummy;
    while (SSIDataGetNonBlocking(SSI0_BASE, &dummy));

    rtt_puts("SPI init OK\r\n");
}

void oepl_hw_spi_cs_assert(void)
{
    GPIO_clearDio(PIN_SPI_CS);
}

void oepl_hw_spi_cs_deassert(void)
{
    GPIO_setDio(PIN_SPI_CS);
}

void oepl_hw_spi_send_raw(const uint8_t* data, size_t len)
{
    // Send bytes without toggling CS
    for (size_t i = 0; i < len; i++) {
        SSIDataPut(SSI0_BASE, data[i]);
        while (SSIBusy(SSI0_BASE));
        uint32_t dummy;
        SSIDataGet(SSI0_BASE, &dummy);
    }
}

void oepl_hw_spi_read_raw(uint8_t* data, size_t len)
{
    // Send 0xFF dummy bytes and capture received data (no CS toggle)
    for (size_t i = 0; i < len; i++) {
        SSIDataPut(SSI0_BASE, 0xFF);
        while (SSIBusy(SSI0_BASE));
        uint32_t rx;
        SSIDataGet(SSI0_BASE, &rx);
        data[i] = (uint8_t)rx;
    }
}

void oepl_hw_spi_transfer(const uint8_t* data, size_t len)
{
    GPIO_clearDio(PIN_SPI_CS);
    oepl_hw_spi_send_raw(data, len);
    GPIO_setDio(PIN_SPI_CS);
}

void oepl_hw_spi_transfer_read(uint8_t* data, size_t len)
{
    GPIO_clearDio(PIN_SPI_CS);

    for (size_t i = 0; i < len; i++) {
        SSIDataPut(SSI0_BASE, 0xFF);
        while (SSIBusy(SSI0_BASE));
        uint32_t rx;
        SSIDataGet(SSI0_BASE, &rx);
        data[i] = (uint8_t)rx;
    }

    GPIO_setDio(PIN_SPI_CS);
}

void oepl_hw_gpio_init(void)
{
    // EPD_BS1 (Bus Select 1) - output, LOW for 4-wire SPI mode
    IOCPinTypeGpioOutput(PIN_EPD_BS);
    GPIO_setOutputEnableDio(PIN_EPD_BS, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_EPD_BS);

    // EPD_DIR (SDA direction) - output, LOW for write mode
    IOCPinTypeGpioOutput(PIN_EPD_DIR);
    GPIO_setOutputEnableDio(PIN_EPD_DIR, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_EPD_DIR);

    // EPD_POWER - output, LOW (panel power off). uc8159_init() raises it
    // before talking to the panel; uc8159_sleep() drops it again. Left high,
    // it kept the panel's supply running through every sleep.
    IOCPinTypeGpioOutput(PIN_EPD_POWER);
    GPIO_setOutputEnableDio(PIN_EPD_POWER, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_EPD_POWER);

    // DC (Data/Command) - output, start LOW (command mode)
    IOCPinTypeGpioOutput(PIN_DISPLAY_DC);
    GPIO_setOutputEnableDio(PIN_DISPLAY_DC, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_DISPLAY_DC);

    // RST (Reset) - output, start HIGH (not in reset)
    IOCPinTypeGpioOutput(PIN_DISPLAY_RST);
    GPIO_setOutputEnableDio(PIN_DISPLAY_RST, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_DISPLAY_RST);

    // BUSY - input (HIGH=ready, LOW=busy — UC8159 standard)
    IOCPinTypeGpioInput(PIN_DISPLAY_BUSY);
    GPIO_setOutputEnableDio(PIN_DISPLAY_BUSY, GPIO_OUTPUT_DISABLE);

    // EPD CS - output, HIGH (deselected)
    IOCPinTypeGpioOutput(PIN_SPI_CS);
    GPIO_setOutputEnableDio(PIN_SPI_CS, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_SPI_CS);

    // Flash CS - output, HIGH (deselected, prevent interference)
    IOCPinTypeGpioOutput(PIN_FLASH_CS);
    GPIO_setOutputEnableDio(PIN_FLASH_CS, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_FLASH_CS);

    rtt_puts("GPIO OK\r\n");
}

// Release every panel line (high impedance, no pull, input buffer off) while
// its supply is off. Driving them high back-powers the unpowered controller;
// driving them low fights pull-ups on the board (+570 uA measured). Floating
// is what the bench measured lowest. uc8159_init() -> oepl_hw_gpio_init() /
// oepl_hw_spi_init() reconfigure them before power is applied.
// Note on the ~20 DIOs nothing here configures: the stock Solum firmware
// programs every pad through its TI-RTOS PIN table (drive low at 8 mA on
// DIO6/7/16/17, drive high on DIO3, pulls on 2/4/19/21/22/26/27, input buffer
// off on the rest). Copying that here was measured on 2026-09-18 and is NOT
// worth doing on this board: standby was unchanged (9.1 vs 9.2 uA) and the
// tag's own supply reading fell from 2960 mV to 2203 mV, i.e. those pins draw
// real current while awake. The panel's DC and DIR lines were the whole of
// the sleep-current problem; see oepl_hw_epd_pins_off().

void oepl_hw_epd_pins_off(void)
{
    static const uint8_t pins[] = {
        PIN_DISPLAY_RST, PIN_DISPLAY_DC, PIN_SPI_CS, PIN_EPD_BS, PIN_EPD_DIR,
        PIN_SPI_MOSI, PIN_SPI_CLK, PIN_SPI_MISO, PIN_DISPLAY_BUSY,
    };
    // The panel's control lines are pulled up rather than left floating. A
    // floating input on the panel side sits at an undefined level and its
    // input stage draws crowbar current, and that — not anything in the MCU —
    // was most of this tag's sleep current. Measured at 3.0 V with the
    // debugger detached, production firmware, median of the quiet seconds
    // between check-ins:
    //
    //   all nine lines floating                          59.8 uA
    //   + pull-up on DIO15 (DC) alone                    36.5 uA
    //   + pull-up on DIO12 (DIR) alone                   25.1 uA
    //   + pull-ups on DIO12 and DIO15                     8.6 uA
    //   + pull-ups on DIO12,13,14,15,18,20 (this set)     8.1 uA
    //   + pull-ups on the SPI bus as well              >2000 uA, never settles
    //
    // DC and DIR are what matter; BUSY/RST/BS/CS are worth a further ~0.5 uA
    // between them. The SPI bus (MOSI/MISO/CLK) is shared with the external
    // flash and must stay high-impedance — pulling it up keeps something on
    // that bus alive and the tag never reaches standby at all.
    //
    // uc8159_init() re-drives BS and DIR through oepl_hw_gpio_init() before it
    // powers the panel, so a pulled-up line during sleep cannot leave the
    // panel in the wrong bus mode.
#ifndef EPD_OFF_PULLUP_MASK
#define EPD_OFF_PULLUP_MASK 0x0014F000u   /* DIO12,13,14,15,18,20 */
#endif
    for (unsigned i = 0; i < sizeof(pins); i++) {
        bool pull_up = ((EPD_OFF_PULLUP_MASK >> pins[i]) & 1u) != 0;
        GPIO_setOutputEnableDio(pins[i], GPIO_OUTPUT_DISABLE);
        IOCPortConfigureSet(pins[i], IOC_PORT_GPIO,
                            (pull_up ? IOC_IOPULL_UP : IOC_NO_IOPULL) | IOC_INPUT_DISABLE);
    }
}

// Put the external SPI flash (unused by this firmware) into deep power-down:
// command 0xB9, bit-banged on CS=DIO11 / MOSI=DIO9 / CLK=DIO10 so the SERIAL
// domain isn't needed. CS then stays high (a floating CS cost ~280 uA).
void oepl_hw_flash_deep_sleep(void)
{
    IOCPinTypeGpioOutput(PIN_SPI_MOSI); GPIO_setOutputEnableDio(PIN_SPI_MOSI, GPIO_OUTPUT_ENABLE);
    IOCPinTypeGpioOutput(PIN_SPI_CLK);  GPIO_setOutputEnableDio(PIN_SPI_CLK, GPIO_OUTPUT_ENABLE);
    IOCPinTypeGpioOutput(PIN_FLASH_CS); GPIO_setOutputEnableDio(PIN_FLASH_CS, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_SPI_CLK);
    GPIO_clearDio(PIN_FLASH_CS);
    oepl_hw_delay_us(100);
    for (int b = 7; b >= 0; b--) {
        if (0xB9 & (1 << b)) GPIO_setDio(PIN_SPI_MOSI); else GPIO_clearDio(PIN_SPI_MOSI);
        oepl_hw_delay_us(50);
        GPIO_setDio(PIN_SPI_CLK);
        oepl_hw_delay_us(50);
        GPIO_clearDio(PIN_SPI_CLK);
    }
    GPIO_setDio(PIN_FLASH_CS);
    oepl_hw_delay_us(100);
}

void oepl_hw_epd_power(bool on)
{
    if (on) {
        GPIO_setDio(PIN_EPD_POWER);
        oepl_hw_delay_ms(100);   // let the supply stabilise
    } else {
        GPIO_clearDio(PIN_EPD_POWER);
    }
}

void oepl_hw_gpio_set(uint8_t pin, bool level)
{
    if (level) {
        GPIO_setDio(pin);
    } else {
        GPIO_clearDio(pin);
    }
}

bool oepl_hw_gpio_get(uint8_t pin)
{
    return (GPIO_readDio(pin) != 0);
}

// --- Idle: CPU sleep between RTC ticks ---
// While the tag waits (radio receive windows, the ~30 s panel refresh, any
// delay), the CPU sleeps (WFI, clocks and the RF core keep running) and wakes
// on a 2 ms tick from AON RTC channel 2. Spinning instead cost ~3 mA on top of
// the radio/panel for the whole wait. The tick is switched off before
// standby (enter_sleep) so it can't wake the tag out of it.
#define IDLE_TICK_INC   131     // 2 ms in RTC 16.16 compare units (65536 * 0.002)
static bool idle_tick_on;

void oepl_hw_idle_tick(bool on)
{
    if (on == idle_tick_on) return;
    if (on) {
        AONRTCEnable();
        AONRTCModeCh2Set(AON_RTC_MODE_CH2_CONTINUOUS);
        AONRTCIncValueCh2Set(IDLE_TICK_INC);
        AONRTCCompareValueSet(AON_RTC_CH2, AONRTCCurrentCompareValueGet() + IDLE_TICK_INC);
        AONRTCEventClear(AON_RTC_CH2);
        AONRTCChannelEnable(AON_RTC_CH2);
        AONRTCCombinedEventConfig(AON_RTC_CH0 | AON_RTC_CH2);
        SysCtrlAonSync();
        IntPendClear(INT_AON_RTC_COMB);
        IntEnable(INT_AON_RTC_COMB);
    } else {
        AONRTCChannelDisable(AON_RTC_CH2);
        AONRTCEventClear(AON_RTC_CH2);
        AONRTCCombinedEventConfig(AON_RTC_CH0);
        SysCtrlAonSync();
    }
    idle_tick_on = on;
}

void oepl_hw_idle(void)
{
    oepl_hw_idle_tick(true);
    oepl_hw_wdt_kick();
    CPUwfi();
}

// Milliseconds on the AON RTC (wraps every ~18 h; use unsigned differences)
uint32_t oepl_hw_rtc_ms(void)
{
    return (uint32_t)(((uint64_t)AONRTCCurrentCompareValueGet() * 1000) >> 16);
}

void oepl_hw_delay_ms(uint32_t ms)
{
    if (ms < 3) {                         // shorter than a tick: busy-wait
        for (uint32_t i = 0; i < ms; i++) oepl_hw_delay_us(1000);
        return;
    }
    uint32_t start = AONRTCCurrentCompareValueGet();
    uint32_t span = (uint32_t)(((uint64_t)ms << 16) / 1000);
    while ((uint32_t)(AONRTCCurrentCompareValueGet() - start) < span)
        oepl_hw_idle();
}

// --- Watchdog ---
// The WDT is clocked at 1.5 MHz (SCLK_HF/32) and stops in standby, so deep
// sleep needs no kicks. First expiry raises an (unhandled) interrupt, the
// second resets the MCU: a hang is cut off after 2 x WDT_RELOAD_S. Kicks are
// placed in the primitives every long operation already spins on
// (delay_ms/us, RX polling, the main loop), so nothing else has to think
// about it. A WDT reset shows up as RSTSRC_WARMRESET; main() reports it.
#define WDT_RELOAD_S  45
void oepl_hw_wdt_init(void)
{
    WatchdogReloadSet(WDT_RELOAD_S * 1500000UL);
    WatchdogStallEnable();      // pause while halted by a debugger
    WatchdogResetEnable();
    WatchdogEnable();
}

void oepl_hw_wdt_kick(void)
{
    WatchdogIntClear();         // clearing the interrupt reloads the counter
}

void oepl_hw_delay_us(uint32_t us)
{
    oepl_hw_wdt_kick();
    // CC2630 at 48 MHz. Volatile loop body is ~8 cycles (LDR+SUB+STR+NOP+CMP+BNE).
    // 48 cycles/us / 8 cycles/iter = 6 iterations/us
    volatile uint32_t delay = us * 6;
    while (delay--) {
        __asm volatile ("nop");
    }
}

uint32_t oepl_hw_get_time_ms(void)
{
    return 0;  // Not implemented — not needed for display driver
}

bool oepl_hw_get_temperature(int8_t* temp_degc)
{
    AONBatMonEnable();
    int32_t temp = AONBatMonTemperatureGetDegC();
    *temp_degc = (int8_t)temp;
    return true;
}

// The battery monitor is left enabled once something has asked for a reading.
// Switching it off after each read was measured against leaving it on at the
// 9 uA sleep floor and made no difference (9.1 vs 9.2 uA), while it did break
// the telemetry: BAT and TEMP read 0 for the first ~5 ms after every
// AONBatMonEnable(), and driverlib's "measurement ready" flags clear as a
// side effect of being polled, so a wait loop eats the flag it waits for.
// The AP logged bat=0mV until this went back to simply leaving it on.
bool oepl_hw_get_voltage(uint16_t* voltage_mv)
{
    AONBatMonEnable();
    uint32_t raw = AONBatMonBatteryVoltageGet();
    // Raw format: bits [10:8] = integer volts, bits [7:0] = fraction (0-255)
    uint32_t int_v = (raw >> 8) & 0x7;
    uint32_t frac = raw & 0xFF;
    *voltage_mv = (uint16_t)(int_v * 1000 + (frac * 1000) / 256);
    return true;
}

void oepl_hw_set_led(uint8_t color, bool on)
{
    (void)color;
    (void)on;
}

void oepl_hw_enter_deepsleep(void)
{
    rtt_puts("Deep sleep (stub)\r\n");
}

uint8_t oepl_hw_get_hwid(void)
{
    return OEPL_HW_TYPE;
}

bool oepl_hw_get_screen_properties(size_t* x, size_t* y, size_t* bpp)
{
    *x = DISPLAY_WIDTH;
    *y = DISPLAY_HEIGHT;
    *bpp = 1;
    return true;
}

void oepl_hw_debugprint(debug_level_t level, const char* fmt, ...)
{
    (void)level;
    (void)fmt;
    // We use RTT directly, no UART debug print needed
}

void oepl_hw_crash(const char* message)
{
    rtt_puts("CRASH: ");
    rtt_puts(message);
    rtt_puts("\r\n");
    __asm volatile ("cpsid i");
    while (1) {
        __asm volatile ("nop");
    }
}

void oepl_hw_watchdog_init(void)
{
}

void oepl_hw_watchdog_feed(void)
{
}
