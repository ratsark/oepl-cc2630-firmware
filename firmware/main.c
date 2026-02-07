// -----------------------------------------------------------------------------
//     CC2630 OEPL Tag Firmware
// -----------------------------------------------------------------------------
// Phase 2: RF init + OEPL protocol checkin with AP
// Uses SEGGER RTT for debug output via J-Link.
// -----------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>
#include "rtt.h"
#include "oepl_rf_cc2630.h"
#include "oepl_radio_cc2630.h"

#define HWREG(addr) (*((volatile uint32_t *)(addr)))

// --- PRCM ---
#define PRCM_BASE               0x40082000
#define PRCM_NONBUF_BASE        0x60082000
#define PRCM_O_PDCTL0PERIPH     0x138
#define PRCM_O_PDSTAT0PERIPH    0x14C
#define PRCM_O_GPIOCLKGR        0x48
#define PRCM_O_CLKLOADCTL       0x28

static void delay_cycles(volatile uint32_t n)
{
    while (n--) __asm volatile ("nop");
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

    // Print MAC
    uint8_t mac[8];
    oepl_rf_get_mac(mac);
    rtt_puts("MAC: ");
    for (int i = 0; i < 8; i++) {
        rtt_put_hex8(mac[i]);
        if (i < 7) rtt_puts(":");
    }
    rtt_puts("\r\n");

    // --- Initialize RF core ---
    rf_status_t rc = oepl_rf_init();
    if (rc != RF_OK) {
        rtt_puts("RF init FAILED: ");
        rtt_put_hex8(rc);
        rtt_puts("\r\n");
        goto heartbeat;
    }
    rtt_puts("RF init OK\r\n");

    // --- Initialize OEPL radio protocol layer ---
    oepl_radio_init();

    // --- Scan for AP ---
    rtt_puts("\r\n--- AP SCAN ---\r\n");
    int8_t ch = oepl_radio_scan_channels();
    if (ch < 0) {
        rtt_puts("No AP - trying direct checkin on all channels\r\n");
        // Even without PONG, try sending AvailDataReq on each channel
        // The AP might still process it
        for (uint8_t c = 0; c < OEPL_NUM_CHANNELS; c++) {
            rc = oepl_rf_set_channel(c);
            if (rc != RF_OK) continue;

            uint8_t ieee_ch = oepl_channel_map[c];
            rtt_puts("Try ch=");
            rtt_put_hex8(ieee_ch);
            rtt_puts("\r\n");

            // Manually set radio state for this channel
            radio_state_t *rst = oepl_radio_get_state();
            rst->current_channel = c;
            rst->current_ieee_ch = ieee_ch;
            rst->ap_found = true;  // Force so checkin proceeds
            // Use broadcast (all 0xFF) as AP MAC
            memset(rst->ap_mac, 0xFF, 8);

            struct AvailDataInfo info;
            if (oepl_radio_checkin(&info)) {
                rtt_puts("*** AP RESPONDED! ***\r\n");
                goto heartbeat;
            }
        }
        rtt_puts("No response on any channel\r\n");
    } else {
        rtt_puts("AP found, sending checkin...\r\n");
        struct AvailDataInfo info;
        if (oepl_radio_checkin(&info)) {
            rtt_puts("*** CHECKIN SUCCESS ***\r\n");
        } else {
            rtt_puts("Checkin: no data response\r\n");
        }
    }

    rtt_puts("--- END ---\r\n");

heartbeat:
    // --- Main loop: heartbeat + periodic checkin ---
    rtt_puts("Entering main loop\r\n");
    uint32_t count = 0;
    while (1) {
        rtt_puts("tick ");
        rtt_put_hex32(count);
        rtt_puts("\r\n");
        count++;
        delay_cycles(8000000);
    }

    return 0;
}
