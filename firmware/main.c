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
#include "drivers/oepl_display_driver_uc8159_600x448.h"

#define HWREG(addr) (*((volatile uint32_t *)(addr)))

// --- PRCM ---
#define PRCM_BASE               0x40082000
#define PRCM_NONBUF_BASE        0x60082000
#define PRCM_O_PDCTL0PERIPH     0x138
#define PRCM_O_PDSTAT0PERIPH    0x14C
#define PRCM_O_GPIOCLKGR        0x48
#define PRCM_O_CLKLOADCTL       0x28

// Block buffer for image download (4KB per block)
static uint8_t block_buf[BLOCK_DATA_SIZE];

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
        rtt_puts("No PONG, trying direct checkin...\r\n");
        for (uint8_t c = 0; c < OEPL_NUM_CHANNELS; c++) {
            rf_status_t rc = oepl_rf_set_channel(c);
            if (rc != RF_OK) continue;

            uint8_t ieee_ch = oepl_channel_map[c];
            radio_state_t *rst = oepl_radio_get_state();
            rst->current_channel = c;
            rst->current_ieee_ch = ieee_ch;
            rst->ap_found = true;
            memset(rst->ap_mac, 0xFF, 8);

            if (oepl_radio_checkin(info)) return true;
        }
        return false;
    }

    // AP found, do checkin
    rtt_puts("AP found, checkin...\r\n");
    return oepl_radio_checkin(info);
}

static bool download_image(struct AvailDataInfo *info)
{
    uint32_t data_size = info->dataSize;
    uint8_t num_blocks = (data_size + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE;

    rtt_puts("Download: size=");
    rtt_put_hex32(data_size);
    rtt_puts(" blocks=");
    rtt_put_hex8(num_blocks);
    rtt_puts(" type=");
    rtt_put_hex8(info->dataType);
    rtt_puts("\r\n");

    uint32_t total_received = 0;

    for (uint8_t blk = 0; blk < num_blocks; blk++) {
        rtt_puts("Block ");
        rtt_put_hex8(blk);
        rtt_puts("/");
        rtt_put_hex8(num_blocks);

        uint16_t block_size = 0;
        bool ok = oepl_radio_request_block(blk, info->dataVer, info->dataType,
                                            block_buf, &block_size);
        if (!ok) {
            rtt_puts(" FAIL\r\n");
            // Retry once
            rtt_puts("Retry...\r\n");
            ok = oepl_radio_request_block(blk, info->dataVer, info->dataType,
                                           block_buf, &block_size);
            if (!ok) {
                rtt_puts(" FAIL again\r\n");
                return false;
            }
        }
        rtt_puts(" OK\r\n");

        total_received += block_size;

        // Print first 16 bytes for verification
        rtt_puts("  [");
        uint8_t dump = (block_size > 16) ? 16 : (uint8_t)block_size;
        for (uint8_t i = 0; i < dump; i++) {
            rtt_put_hex8(block_buf[i]);
            if (i < dump - 1) rtt_puts(" ");
        }
        rtt_puts("]\r\n");
    }

    rtt_puts("Download complete: ");
    rtt_put_hex32(total_received);
    rtt_puts(" bytes\r\n");

    // Send XferComplete to AP
    rtt_puts("Sending XferComplete...\r\n");
    oepl_radio_send_xfer_complete();

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

    // --- Display test pattern (proves display is working) ---
    display_test_pattern();

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
                // AP has data for us — download it
                if (download_image(&info)) {
                    rtt_puts("*** IMAGE DOWNLOADED ***\r\n");
                } else {
                    rtt_puts("Download failed\r\n");
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
