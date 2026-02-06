// -----------------------------------------------------------------------------
//                   OpenEPaperLink CC2630 Firmware - Main Entry
//                          TG-GR6000N (600×448 Display)
// -----------------------------------------------------------------------------

#include "oepl_hw_abstraction_cc2630.h"
#include "drivers/oepl_display_driver_uc8159_600x448.h"
#include <stdint.h>
#include <stdbool.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

#define FIRMWARE_VERSION_MAJOR  1
#define FIRMWARE_VERSION_MINOR  0
#define FIRMWARE_VERSION_PATCH  0

// Test pattern for display validation
#define TEST_PATTERN_ENABLED    1

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
// CRITICAL: Full framebuffer (33,600 bytes) DOES NOT FIT in 20KB RAM!
// For testing, use small buffer. Production must use line-by-line transfer.
#define TEST_BUFFER_SIZE  1024  // 1KB test buffer
static uint8_t test_framebuffer[TEST_BUFFER_SIZE];

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void display_test_pattern(void);
static void display_checkerboard(void);
static void display_splash_screen(void);

// -----------------------------------------------------------------------------
//                                  Main Function
// -----------------------------------------------------------------------------

int main(void)
{
    // DIAGNOSTIC: Blink DIO13 (DISPLAY_RST pin) to prove we're alive
    // You can measure this with a multimeter on the display RST line
    // Comment out after confirming firmware runs
    #define DIAGNOSTIC_BLINK 1

    #if DIAGNOSTIC_BLINK
    // Toggle MANY pins so you can find ANY activity with multimeter
    volatile uint32_t *ioc_cfg = (volatile uint32_t *)0x40081000;  // IOC base
    volatile uint32_t *gpio_doe = (volatile uint32_t *)0x40022000; // GPIO DOE
    volatile uint32_t *gpio_dout = (volatile uint32_t *)0x40022004; // GPIO DOUT

    // Enable GPIO and IOC clocks
    volatile uint32_t *prcm_gpioclk = (volatile uint32_t *)0x40082048;
    *prcm_gpioclk = 0x01;

    // Configure DIO0-14 as GPIO outputs (try all possible pins!)
    for (int i = 0; i <= 14; i++) {
        ioc_cfg[i] = 0x00000000;  // GPIO mode
        *gpio_doe |= (1 << i);     // Enable output
    }

    // Blink ALL pins together - should see activity on SOME pin
    // Pattern: 2 seconds HIGH, 2 seconds LOW (slow for multimeter)
    while (1) {
        // All pins HIGH (~3V)
        for (int i = 0; i <= 14; i++) {
            *gpio_dout |= (1 << i);
        }
        for (volatile uint32_t i = 0; i < 4000000; i++);  // ~2 seconds delay

        // All pins LOW (~0V)
        for (int i = 0; i <= 14; i++) {
            *gpio_dout &= ~(1 << i);
        }
        for (volatile uint32_t i = 0; i < 4000000; i++);  // ~2 seconds delay
    }
    #endif

    // Initialize hardware abstraction layer
    oepl_hw_init();

    oepl_hw_debugprint(DBG_SYSTEM, "========================================\n");
    oepl_hw_debugprint(DBG_SYSTEM, "OpenEPaperLink CC2630 Firmware\n");
    oepl_hw_debugprint(DBG_SYSTEM, "TG-GR6000N (600x448 UC8159)\n");
    oepl_hw_debugprint(DBG_SYSTEM, "Version: %d.%d.%d\n",
                       FIRMWARE_VERSION_MAJOR,
                       FIRMWARE_VERSION_MINOR,
                       FIRMWARE_VERSION_PATCH);
    oepl_hw_debugprint(DBG_SYSTEM, "========================================\n");

    // Get and display hardware information
    uint8_t hwid = oepl_hw_get_hwid();
    oepl_hw_debugprint(DBG_SYSTEM, "Hardware ID: 0x%02X\n", hwid);

    size_t screen_x, screen_y, screen_bpp;
    if (oepl_hw_get_screen_properties(&screen_x, &screen_y, &screen_bpp)) {
        oepl_hw_debugprint(DBG_SYSTEM, "Display: %dx%d @ %d bpp\n",
                          screen_x, screen_y, screen_bpp);
    }

    // Read battery voltage
    uint16_t voltage_mv;
    if (oepl_hw_get_voltage(&voltage_mv)) {
        oepl_hw_debugprint(DBG_SYSTEM, "Battery: %d mV\n", voltage_mv);
    }

    // Read temperature
    int8_t temp_degc;
    if (oepl_hw_get_temperature(&temp_degc)) {
        oepl_hw_debugprint(DBG_SYSTEM, "Temperature: %d C\n", temp_degc);
    }

    // Initialize display driver
    oepl_hw_debugprint(DBG_DISPLAY, "Initializing UC8159 display controller...\n");
    uc8159_init();
    oepl_hw_debugprint(DBG_DISPLAY, "Display initialized\n");

#if TEST_PATTERN_ENABLED
    // Display test pattern for hardware validation
    oepl_hw_debugprint(DBG_DISPLAY, "Showing test pattern...\n");
    display_splash_screen();
    oepl_hw_delay_ms(3000);
#endif

    // Main loop
    oepl_hw_debugprint(DBG_APP, "Entering main loop\n");

    while (1) {
        // Feed watchdog
        oepl_hw_watchdog_feed();

        // TODO: Implement OEPL application logic
        // - Check for stored images in NVM
        // - If no image, send AvailDataReq to AP
        // - Process received data
        // - Update display
        // - Enter deep sleep until next check-in

        // For now, just blink LED and sleep
        oepl_hw_set_led(0, true);
        oepl_hw_delay_ms(100);
        oepl_hw_set_led(0, false);
        oepl_hw_delay_ms(900);

        // Log status every 10 seconds
        static uint32_t last_log_time = 0;
        uint32_t current_time = oepl_hw_get_time_ms();
        if (current_time - last_log_time >= 10000) {
            oepl_hw_debugprint(DBG_APP, "System running... uptime: %d ms\n", current_time);
            last_log_time = current_time;
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void display_splash_screen(void)
{
    // IMPORTANT: This is a minimal test to verify display communication
    // The test buffer is only 1KB - just enough to test SPI and command sequence
    // Production firmware MUST use line-by-line transfer from compressed storage

    oepl_hw_debugprint(DBG_DISPLAY, "Creating VISIBLE test pattern...\n");

    // Create a highly visible pattern: solid black bars
    // Display is 600 pixels wide = 75 bytes per row
    // 1024 bytes = approximately 13 rows of solid black

    // Fill with solid black pixels (0x00 = all pixels black on e-paper)
    for (size_t i = 0; i < TEST_BUFFER_SIZE; i++) {
        test_framebuffer[i] = 0x00;  // Solid black
    }

    oepl_hw_debugprint(DBG_DISPLAY, "Transferring %d bytes (solid black) to display...\n", TEST_BUFFER_SIZE);

    // Transfer test data - should create visible black bar at top of display
    uc8159_draw(test_framebuffer, TEST_BUFFER_SIZE);

    oepl_hw_debugprint(DBG_DISPLAY, "Black test pattern sent\n");
}

static void display_checkerboard(void)
{
    oepl_hw_debugprint(DBG_DISPLAY, "Creating checkerboard pattern...\n");

    // Create simple checkerboard with test buffer
    for (size_t i = 0; i < TEST_BUFFER_SIZE; i++) {
        test_framebuffer[i] = 0xAA;  // 10101010 pattern
    }

    uc8159_draw(test_framebuffer, TEST_BUFFER_SIZE);
    oepl_hw_debugprint(DBG_DISPLAY, "Checkerboard test sent\n");
}

static void display_test_pattern(void)
{
    oepl_hw_debugprint(DBG_DISPLAY, "Creating test pattern...\n");

    // Fill with alternating pattern
    for (size_t i = 0; i < TEST_BUFFER_SIZE; i++) {
        test_framebuffer[i] = (i % 4) * 0x55;
    }

    uc8159_draw(test_framebuffer, TEST_BUFFER_SIZE);
    oepl_hw_debugprint(DBG_DISPLAY, "Test pattern sent\n");
}
