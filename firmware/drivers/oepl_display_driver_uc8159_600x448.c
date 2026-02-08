// -----------------------------------------------------------------------------
//  UC8159 600x448 E-Paper Display Driver for TG-GR6000N
//  v31b: Working display driver with OTP waveform load
//
//  4bpp BWR color mapping (per nibble):
//    0x0 = Black, 0x3 = White, 0x4 = Red
//
//  Init sequence (from stock firmware binary analysis):
//  1. Hardware reset
//  2. OTP waveform read: CMD 0x65 DATA 0x01 → CMD 0xAB → CMD 0x65 DATA 0x00
//  3. PON (power on)
//  4. Register config (PSR, PWR, PFS, BTST, PLL, TSE, CDI, TCON, TRES, VDCS, E5)
//  5. Power cycle: POF → PON
//  6. Data transfer: DTM1 (0x10) + pixel data + DATA_STOP (0x11)
//  7. Display refresh: DRF (0x12) → wait BUSY ~26s
//
//  Pin assignments (from stock firmware binary analysis):
//    DIO9=MOSI, DIO8=MISO, DIO10=CLK, DIO20=CS
//    DIO15=DC, DIO13=BUSY, DIO14=RST
// -----------------------------------------------------------------------------

#include "oepl_display_driver_uc8159_600x448.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "rtt.h"
#include "gpio.h"
#include "ioc.h"
#include <string.h>

#ifndef HWREG
#define HWREG(addr) (*((volatile uint32_t *)(addr)))
#endif

// Pin assignments — from stock firmware binary analysis
#define PIN_DC          15  // DIO15 — Data/Command
#define PIN_BUSY        13  // DIO13 — BUSY (HIGH=ready, LOW=busy)
#define PIN_RST         14  // DIO14 — Reset (active LOW)
#define PIN_CS          20  // DIO20 — EPD display CS

static bool initialized = false;

// -----------------------------------------------------------------------------
//                          Low-level helpers
// -----------------------------------------------------------------------------

static inline uint8_t busy(void)
{
    return oepl_hw_gpio_get(PIN_BUSY) ? 1 : 0;
}

// Wait for BUSY HIGH (ready). UC8159: LOW=busy, HIGH=ready.
// Returns true if BUSY went HIGH before timeout. Reports time waited.
static bool wait_busy(uint32_t timeout_ms, const char *label)
{
    rtt_puts(label);
    rtt_puts(":B=");
    rtt_put_hex8(busy());

    bool ok = false;
    uint32_t i;
    for (i = 0; i < timeout_ms; i++) {
        if (oepl_hw_gpio_get(PIN_BUSY)) {
            ok = true;
            break;
        }
        oepl_hw_delay_ms(1);
    }

    if (ok) {
        rtt_puts(" OK@");
        rtt_put_hex32(i);
    } else {
        rtt_puts(" TO@");
        rtt_put_hex32(timeout_ms);
    }
    rtt_puts(" B=");
    rtt_put_hex8(busy());
    rtt_puts("\r\n");
    return ok;
}

// Poll BUSY for up to timeout_ms, counting how long it stays LOW.
// Reports BUSY transitions via RTT.
static void poll_busy_detailed(uint32_t timeout_ms, const char *label)
{
    rtt_puts(label);
    rtt_puts(": B=");
    uint8_t prev = busy();
    rtt_put_hex8(prev);

    uint32_t low_count = 0;
    uint32_t high_count = 0;

    for (uint32_t i = 0; i < timeout_ms; i++) {
        uint8_t cur = busy();
        if (cur != prev) {
            // State changed — report transition
            if (prev == 0) {
                rtt_puts(" LOW:");
                rtt_put_hex32(low_count);
                rtt_puts("ms");
            } else {
                rtt_puts(" HIGH:");
                rtt_put_hex32(high_count);
                rtt_puts("ms");
            }
            prev = cur;
            low_count = 0;
            high_count = 0;
        }
        if (cur == 0)
            low_count++;
        else
            high_count++;

        oepl_hw_delay_ms(1);
    }

    // Report final state
    if (prev == 0) {
        rtt_puts(" LOW:");
        rtt_put_hex32(low_count);
        rtt_puts("ms(end)");
    } else {
        rtt_puts(" HIGH:");
        rtt_put_hex32(high_count);
        rtt_puts("ms(end)");
    }
    rtt_puts("\r\n");
}

// Send a command byte: DC=LOW, one CS frame
static void epd_cmd(uint8_t c)
{
    oepl_hw_gpio_set(PIN_DC, false);
    oepl_hw_spi_cs_assert();
    oepl_hw_spi_send_raw(&c, 1);
    oepl_hw_spi_cs_deassert();
}

// Send command + data in a single CS frame
static void epd_write(uint8_t c, const uint8_t *data, uint16_t len)
{
    oepl_hw_gpio_set(PIN_DC, false);
    oepl_hw_spi_cs_assert();
    oepl_hw_spi_send_raw(&c, 1);
    if (len > 0) {
        oepl_hw_gpio_set(PIN_DC, true);
        oepl_hw_spi_send_raw(data, len);
    }
    oepl_hw_spi_cs_deassert();
}


// -----------------------------------------------------------------------------
//                          Public Functions
// -----------------------------------------------------------------------------

void uc8159_init(void)
{
    if (initialized) return;

    rtt_puts("\r\n=== EPD v31b ===\r\n");

    // 1. Init GPIOs and SPI
    oepl_hw_gpio_init();
    oepl_hw_spi_init();

    // 2. Hardware reset (stock firmware: double reset)
    oepl_hw_gpio_set(PIN_RST, false);
    oepl_hw_delay_ms(100);
    oepl_hw_gpio_set(PIN_RST, true);
    oepl_hw_delay_ms(200);
    rtt_puts("RST1 ");
    wait_busy(5000, "RST1_W");

    // 3. OTP waveform read (stock firmware sequence)
    //    CMD 0x65 DATA 0x01 = enter flash pass-through
    //    CMD 0xAB = wake up internal flash (reads waveform LUT from OTP)
    //    Wait BUSY = waveform load complete
    //    CMD 0x65 DATA 0x00 = exit flash pass-through
    { uint8_t d[] = {0x01}; epd_write(0x65, d, 1); }
    rtt_puts("OTP: EN ");
    epd_cmd(0xAB);
    rtt_puts("AB ");
    wait_busy(5000, "OTP_W");
    { uint8_t d[] = {0x00}; epd_write(0x65, d, 1); }
    rtt_puts("OTP done\r\n");

    // 4. Power On (before register config, per stock firmware)
    epd_cmd(0x04);
    rtt_puts("PON1 ");
    wait_busy(5000, "PON1_W");

    // 5. Register configuration (stock firmware values)
    // Panel Setting (stock=0xC7, fix HM: SHL=0 for correct left-to-right source shift)
    { uint8_t d[] = {0xC3, 0x08}; epd_write(0x00, d, 2); }
    // Power Setting
    { uint8_t d[] = {0x37, 0x00, 0x05, 0x05}; epd_write(0x01, d, 4); }
    // Power Off Sequence
    { uint8_t d[] = {0x00}; epd_write(0x03, d, 1); }
    // Booster Soft Start (phase C = 0x2D, stock; was 0x1D)
    { uint8_t d[] = {0xC7, 0xCC, 0x2D}; epd_write(0x06, d, 3); }
    // PLL
    { uint8_t d[] = {0x3C}; epd_write(0x30, d, 1); }
    // Temperature Sensor
    { uint8_t d[] = {0x00}; epd_write(0x41, d, 1); }
    // CDI
    { uint8_t d[] = {0x77}; epd_write(0x50, d, 1); }
    // TCON
    { uint8_t d[] = {0x22}; epd_write(0x60, d, 1); }
    // Resolution: 600x448
    { uint8_t d[] = {0x02, 0x58, 0x01, 0xC0}; epd_write(0x61, d, 4); }
    // VCOM DC (stock = 0x1F, was 0x1E)
    { uint8_t d[] = {0x1F}; epd_write(0x82, d, 1); }
    // Flash mode = 0x00 (ensure not in flash pass-through)
    { uint8_t d[] = {0x00}; epd_write(0x65, d, 1); }
    // Force temperature = 0x03 (stock firmware value)
    { uint8_t d[] = {0x03}; epd_write(0xE5, d, 1); }
    rtt_puts("CFG ");

    // 6. Power cycle: POF then PON (stock firmware does this after config)
    epd_cmd(0x02);  // Power Off
    rtt_puts("POF ");
    wait_busy(5000, "POF_W");

    epd_cmd(0x04);  // Power On
    rtt_puts("PON2 ");
    wait_busy(5000, "PON2_W");

    rtt_puts("B=");
    rtt_put_hex8(busy());
    rtt_puts("\r\n");

    initialized = true;
    rtt_puts("v31b init OK\r\n");
}

void uc8159_fill(uint8_t fill_byte)
{
    if (!initialized) uc8159_init();

    rtt_puts("FILL 0x");
    rtt_put_hex8(fill_byte);
    rtt_puts("\r\n");

    // 4bpp BWR: 600*448/2 = 134,400 bytes (0x0=black, 0x3=white, 0x4=red)
    uint32_t total = (DISPLAY_WIDTH_600X448 * DISPLAY_HEIGHT_600X448) / 2;

    // DTM1 (0x10) + pixel data in ONE CS frame
    oepl_hw_gpio_set(PIN_DC, false);
    oepl_hw_spi_cs_assert();
    {
        uint8_t c = 0x10;
        oepl_hw_spi_send_raw(&c, 1);
    }
    oepl_hw_gpio_set(PIN_DC, true);
    {
        uint8_t chunk[256];
        memset(chunk, fill_byte, 256);
        uint32_t rem = total;
        while (rem > 0) {
            uint32_t n = (rem > 256) ? 256 : rem;
            oepl_hw_spi_send_raw(chunk, n);
            rem -= n;
        }
    }
    oepl_hw_spi_cs_deassert();

    rtt_puts("DTM1 B=");
    rtt_put_hex8(busy());
    rtt_puts("\r\n");

    // DATA_STOP (0x11)
    epd_cmd(0x11);
    rtt_puts("STOP B=");
    rtt_put_hex8(busy());
    rtt_puts("\r\n");

    // DISPLAY_REFRESH (0x12)
    epd_cmd(0x12);
    rtt_puts("REF sent B=");
    rtt_put_hex8(busy());
    rtt_puts("\r\n");

    // Poll BUSY for 30 seconds — real refresh takes 15-25s
    poll_busy_detailed(30000, "REF_POLL");

    rtt_puts("FILL done\r\n");
}

void uc8159_draw(const uint8_t *framebuffer, size_t len)
{
    if (!initialized) uc8159_init();

    uint32_t total = (DISPLAY_WIDTH_600X448 * DISPLAY_HEIGHT_600X448) / 2;
    if (len > total) len = total;

    rtt_puts("DRAW ");
    rtt_put_hex32(len);
    rtt_puts("\r\n");

    // DTM1 (0x10) + pixel data in ONE CS frame
    oepl_hw_gpio_set(PIN_DC, false);
    oepl_hw_spi_cs_assert();
    {
        uint8_t c = 0x10;
        oepl_hw_spi_send_raw(&c, 1);
    }
    oepl_hw_gpio_set(PIN_DC, true);
    oepl_hw_spi_send_raw(framebuffer, len);

    // Pad remaining pixels with white (0x33) if framebuffer is shorter than display
    if (len < total) {
        uint8_t chunk[256];
        memset(chunk, 0x33, 256);
        uint32_t rem = total - len;
        while (rem > 0) {
            uint32_t n = (rem > 256) ? 256 : rem;
            oepl_hw_spi_send_raw(chunk, n);
            rem -= n;
        }
    }
    oepl_hw_spi_cs_deassert();

    // DATA_STOP (0x11)
    epd_cmd(0x11);

    // DISPLAY_REFRESH (0x12)
    epd_cmd(0x12);
    rtt_puts("REF ");

    // Wait for refresh to complete (up to 30s)
    wait_busy(30000, "DRAW_W");

    rtt_puts("DRAW done\r\n");
}

void uc8159_sleep(void)
{
    // VCOM interval for sleep
    { uint8_t d[] = {0x17}; epd_write(0x50, d, 1); }
    // VCOM DC = 0
    { uint8_t d[] = {0x00}; epd_write(0x82, d, 1); }
    // Power Off
    epd_cmd(0x02);
    wait_busy(5000, "POFF");
    // Deep Sleep
    { uint8_t d[] = {0xA5}; epd_write(0x07, d, 1); }
    oepl_hw_delay_ms(10);
}

void uc8159_wake(void)
{
    initialized = false;
    uc8159_init();
}

const oepl_display_driver_t oepl_display_driver_uc8159_600x448 = {
    .init = uc8159_init,
    .draw = uc8159_draw,
    .sleep = uc8159_sleep,
    .wake = uc8159_wake
};
