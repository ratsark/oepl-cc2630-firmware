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

#define HWREG(addr) (*((volatile uint32_t *)(addr)))

// --- PRCM ---
#define PRCM_BASE               0x40082000
#define PRCM_NONBUF_BASE        0x60082000
#define PRCM_O_PDCTL0PERIPH     0x138
#define PRCM_O_PDSTAT0PERIPH    0x14C
#define PRCM_O_GPIOCLKGR        0x48
#define PRCM_O_CLKLOADCTL       0x28

// Block buffers for image download (4KB each)
// bw_buf: cached B/W block, red_buf: cached Red block
static uint8_t bw_buf[BLOCK_DATA_SIZE];
static uint8_t red_buf[BLOCK_DATA_SIZE];
static int8_t bw_cache_id, red_cache_id;  // which block ID is cached (-1 = none)

static void delay_cycles(volatile uint32_t n)
{
    while (n--) __asm volatile ("nop");
}

static void delay_seconds(uint32_t sec)
{
    // ~8M cycles per second at 48MHz (loop + nop ~= 6 cycles)
    for (uint32_t i = 0; i < sec; i++)
        delay_cycles(8000000);
}

static void print_mac_msb(const uint8_t *mac_lsb)
{
    // Print MAC in human-readable MSB-first order (reverse of wire order)
    for (int i = 7; i >= 0; i--) {
        rtt_put_hex8(mac_lsb[i]);
        if (i > 0) rtt_puts(":");
    }
}

// Fill display with a test pattern
static void display_test_pattern(void)
{
    rtt_puts("\r\n--- DISPLAY TEST ---\r\n");
    uc8159_fill(0x33);  // All white (4bpp BWR: 0x00=black, 0x33=white, 0x44=red)
    rtt_puts("Display test complete\r\n");
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
static bool download_block(uint8_t block_id, struct AvailDataInfo *info,
                            uint8_t *buf, uint16_t *out_size)
{
    rtt_puts("B");
    rtt_put_hex8(block_id);

    for (uint8_t attempt = 0; attempt < 15; attempt++) {
        if (attempt > 0) {
            rtt_puts("R");
            // Increasing backoff: 500ms first 4, then 1s, then 2s
            uint16_t delay = (attempt < 5) ? 500 : (attempt < 10) ? 1000 : 2000;
            oepl_hw_delay_ms(delay);
        }
        if (oepl_radio_request_block(block_id, info->dataVer, info->dataType,
                                      buf, out_size)) {
            rtt_puts("+");
            return true;
        }
    }
    rtt_puts("!");
    return false;
}

// Ensure a block is in the B/W cache
static bool ensure_bw_block(uint8_t block_id, struct AvailDataInfo *info)
{
    if (bw_cache_id == block_id) return true;
    uint16_t sz;
    if (!download_block(block_id, info, bw_buf, &sz)) return false;
    bw_cache_id = block_id;
    return true;
}

// Ensure a block is in the Red cache
static bool ensure_red_block(uint8_t block_id, struct AvailDataInfo *info)
{
    if (red_cache_id == block_id) return true;
    uint16_t sz;
    if (!download_block(block_id, info, red_buf, &sz)) return false;
    red_cache_id = block_id;
    return true;
}

// Get bytes from the image at a given offset, using cached blocks
static bool get_image_bytes(uint32_t offset, uint8_t *out, uint16_t len,
                             struct AvailDataInfo *info, bool is_red_plane)
{
    while (len > 0) {
        uint8_t block_id = (uint8_t)(offset / BLOCK_DATA_SIZE);
        uint16_t block_off = (uint16_t)(offset % BLOCK_DATA_SIZE);
        uint16_t avail = (uint16_t)BLOCK_DATA_SIZE - block_off;
        if (avail > len) avail = len;

        uint8_t *cache;
        if (is_red_plane) {
            if (!ensure_red_block(block_id, info)) return false;
            cache = red_buf;
        } else {
            if (!ensure_bw_block(block_id, info)) return false;
            cache = bw_buf;
        }

        memcpy(out, &cache[block_off], avail);
        out += avail;
        offset += avail;
        len -= avail;
    }
    return true;
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

// Download image and stream to display
static bool download_and_display(struct AvailDataInfo *info)
{
    uint32_t data_size = info->dataSize;
    uint8_t data_type = info->dataType;
    uint16_t width = DISPLAY_WIDTH_600X448;
    uint16_t height = DISPLAY_HEIGHT_600X448;
    uint16_t row_bytes = width / 8;           // 75 bytes per 1bpp row
    uint32_t plane_size = (uint32_t)row_bytes * height;  // 33,600 bytes
    bool has_red = (data_type == 0x21) && (data_size >= plane_size * 2);

    rtt_puts("DL+DISP: sz=");
    rtt_put_hex32(data_size);
    rtt_puts(" t=");
    rtt_put_hex8(data_type);
    rtt_puts(has_red ? " BWR" : " BW");
    rtt_puts("\r\n");

    bw_cache_id = -1;
    red_cache_id = -1;

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

        // Get B/W line
        if (!get_image_bytes(bw_offset, bw_line, row_bytes, info, false)) {
            rtt_puts("BW fail\r\n");
            oepl_hw_spi_cs_deassert();
            return false;
        }

        // Get Red line (if 2bpp)
        if (has_red) {
            uint32_t red_offset = plane_size + (uint32_t)y * row_bytes;
            if (!get_image_bytes(red_offset, red_line, row_bytes, info, true)) {
                rtt_puts("RD fail\r\n");
                oepl_hw_spi_cs_deassert();
                return false;
            }
        } else {
            memset(red_line, 0, row_bytes);
        }

        // Convert to 4bpp
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
    rtt_puts("\r\nDATA OK\r\n");

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

    // Send XferComplete
    oepl_radio_send_xfer_complete();
    rtt_puts("XferComplete\r\n");

    return true;
}

int main(void)
{
    // --- Init RTT ---
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

    // --- Startup delay (allow RTT connection) ---
    delay_cycles(24000000);  // ~3 seconds

    // --- Boot message ---
    rtt_puts("\r\n=== CC2630 OEPL Tag ===\r\n");

    // Print MAC in human-readable form
    uint8_t mac[8];
    oepl_rf_get_mac(mac);
    rtt_puts("MAC: ");
    print_mac_msb(mac);
    rtt_puts("\r\n");

    // --- Initialize display (no fill, saves 26s) ---
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

    // --- Main loop: periodic checkin + download ---
    uint32_t checkin_count = 0;
    while (1) {
        rtt_puts("\r\n=== Checkin #");
        rtt_put_hex32(checkin_count);
        rtt_puts(" ===\r\n");

        struct AvailDataInfo info;
        memset(&info, 0, sizeof(info));

        if (do_scan_and_checkin(&info)) {
            rtt_puts("Checkin OK: dataType=");
            rtt_put_hex8(info.dataType);
            rtt_puts(" nextCheckIn=");
            rtt_put_hex8((info.nextCheckIn >> 8) & 0xFF);
            rtt_put_hex8(info.nextCheckIn & 0xFF);
            rtt_puts("\r\n");

            if (info.dataType != DATATYPE_NOUPDATE) {
                // AP has data for us — download and display it
                if (download_and_display(&info)) {
                    rtt_puts("*** IMAGE DISPLAYED ***\r\n");
                } else {
                    rtt_puts("Display failed\r\n");
                }
            } else {
                rtt_puts("No pending data\r\n");
            }

            // Wait nextCheckIn seconds (minimum 30s for testing)
            uint16_t wait_sec = info.nextCheckIn;
            if (wait_sec < 30) wait_sec = 30;
            rtt_puts("Sleep ");
            rtt_put_hex8((wait_sec >> 8) & 0xFF);
            rtt_put_hex8(wait_sec & 0xFF);
            rtt_puts("s\r\n");
            delay_seconds(wait_sec);
        } else {
            rtt_puts("Checkin failed, retry in 30s\r\n");
            delay_seconds(30);
        }

        checkin_count++;
    }

idle:
    rtt_puts("Entering idle\r\n");
    while (1) delay_cycles(8000000);

    return 0;
}
