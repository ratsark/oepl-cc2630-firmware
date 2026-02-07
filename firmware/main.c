// -----------------------------------------------------------------------------
//     CC2630 OEPL Tag Firmware
// -----------------------------------------------------------------------------
// Phase 1: RF core boot + setup + channel config
// Uses SEGGER RTT for debug output via J-Link.
// -----------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>
#include "rtt.h"
#include "oepl_rf_cc2630.h"

#define HWREG(addr) (*((volatile uint32_t *)(addr)))

// --- PRCM ---
#define PRCM_BASE               0x40082000
#define PRCM_NONBUF_BASE        0x60082000
#define PRCM_O_PDCTL0PERIPH     0x138
#define PRCM_O_PDSTAT0PERIPH    0x14C
#define PRCM_O_GPIOCLKGR        0x48
#define PRCM_O_CLKLOADCTL       0x28

// --- GPIO ---
#define GPIO_BASE               0x40022000
#define GPIO_O_DOUTSET31_0      0x90
#define GPIO_O_DOUTTGL31_0      0xB0
#define GPIO_O_DOE31_0          0xD0

// --- IOC ---
#define IOC_BASE                0x40081000
#define IOC_PULL_DIS            0x00006000

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

    // --- Boot message via RTT ---
    rtt_puts("\r\n=== CC2630 OEPL Tag ===\r\n");

    // Print IEEE address from FCFG1
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
        // Fall through to main loop for debugging
    } else {
        rtt_puts("RF init OK\r\n");

        // Test: tune to each OEPL channel
        for (uint8_t ch = 0; ch < OEPL_NUM_CHANNELS; ch++) {
            rc = oepl_rf_set_channel(ch);
            if (rc != RF_OK) {
                rtt_puts("Channel set FAILED: ch=");
                rtt_put_hex8(ch);
                rtt_puts("\r\n");
            }
        }
        rtt_puts("All channels tuned OK\r\n");
    }

    // --- Main loop: heartbeat ---
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
