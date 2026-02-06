// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include "oepl_display_driver_uc8159_600x448.h"
#include "oepl_hw_abstraction_cc2630.h"
#include <string.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define CHUNK_SIZE  4096  // Transfer in 4KB chunks

// Pin assignments (must match HAL configuration)
#define PIN_DISPLAY_DC      12
#define PIN_DISPLAY_RST     13
#define PIN_DISPLAY_BUSY    14

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void send_command(uint8_t cmd);
static void send_data(uint8_t data);
static void wait_busy(void);
static void set_dc(bool is_data);
static void set_rst(bool level);
static bool read_busy(void);

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static bool initialized = false;

// 32-byte initialization table (from stock firmware @ 0x0000615C)
static const uint8_t init_table[32] = {
    0x00, 0x00, 0x00, 0x00,  // Initial flags
    0xFF, 0xFF, 0xFF, 0xFF,  // Bitmask
    0x00, 0x00, 0x00, 0x00,  // Reserved
    0x40, 0x42, 0x0F, 0x00,  // Config (0x40 = DC/RST pin bit)
    0x08, 0x00, 0x00, 0x00,  // Timing
    0x00, 0x00, 0x00, 0x00,  // Reserved
    0x00, 0x00, 0x00, 0x00,  // Reserved
    0x00, 0x00, 0x00, 0x00   // Reserved
};

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void uc8159_init(void)
{
    if (initialized) {
        return;
    }

    // Initialize SPI
    oepl_hw_spi_init();

    // Initialize GPIO pins
    oepl_hw_gpio_init();

    // Copy initialization table to display state (if needed by HAL)
    // memcpy(display_state, init_table, sizeof(init_table));

    // Hardware reset sequence
    set_rst(0);   // Assert reset (LOW)
    oepl_hw_delay_ms(10);
    set_rst(1);   // Release reset (HIGH)
    oepl_hw_delay_ms(10);

    // Wake sequence
    send_command(CMD_WAKE);  // 0xAB

    // Send 4× NOP delays (from stock firmware analysis)
    send_command(CMD_NOP);   // 0x00
    send_command(CMD_NOP);
    send_command(CMD_NOP);
    send_command(CMD_NOP);

    // Wait for controller ready
    wait_busy();

    // Enable display
    send_command(CMD_ENABLE); // 0x06

    initialized = true;
}

void uc8159_draw(const uint8_t* framebuffer, size_t len)
{
    if (!initialized) {
        uc8159_init();
    }

    uint32_t addr = 0;
    const uint8_t* data_ptr = framebuffer;
    size_t remaining = len;

    // Transfer data in chunks (based on stock firmware FUN_00004322)
    while (remaining > 0) {
        uint32_t chunk_size = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        uint8_t cmd;

        // Select command based on address range
        // (from stock firmware @ lines 9461, 9510, 9489)
        if (addr >= 0x10000) {
            cmd = CMD_DATA_64K;  // 0xD8 for >= 64KB
        } else if (addr >= 0x8000) {
            cmd = CMD_DATA_32K;  // 0x52 for >= 32KB
        } else {
            cmd = CMD_DATA_START;  // 0x20 for < 32KB
        }

        // Enable display before data transfer
        send_command(CMD_ENABLE);  // 0x06
        wait_busy();

        // Send addressing command
        send_command(cmd);
        send_data((addr >> 0) & 0xFF);   // Column low
        send_data((addr >> 8) & 0xFF);   // Column high
        send_data((addr >> 16) & 0xFF);  // Row

        wait_busy();

        // Transfer chunk data
        for (uint32_t i = 0; i < chunk_size; i++) {
            send_data(*data_ptr++);
        }

        // Update counters
        addr += chunk_size;
        remaining -= chunk_size;
    }

    // Trigger display refresh
    // Power on the EPD drivers
    send_command(CMD_POWER_ON);  // 0x04
    wait_busy();

    // Trigger display refresh
    send_command(CMD_DISPLAY_REFRESH);  // 0x12
    wait_busy();

    // Note: NOT putting display to sleep immediately after refresh
    // This allows the update to complete visibly
    // Sleep will be called separately when needed
}

void uc8159_sleep(void)
{
    send_command(CMD_SLEEP);  // 0xB9
    wait_busy();
}

void uc8159_wake(void)
{
    // Wake from sleep (same as init wake sequence)
    send_command(CMD_WAKE);  // 0xAB

    // Send 4× NOP delays
    send_command(CMD_NOP);
    send_command(CMD_NOP);
    send_command(CMD_NOP);
    send_command(CMD_NOP);

    wait_busy();

    // Enable display
    send_command(CMD_ENABLE);  // 0x06
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void send_command(uint8_t cmd)
{
    set_dc(false);  // Command mode (DC = LOW)
    oepl_hw_spi_transfer(&cmd, 1);
}

static void send_data(uint8_t data)
{
    set_dc(true);   // Data mode (DC = HIGH)
    oepl_hw_spi_transfer(&data, 1);
}

static void wait_busy(void)
{
    uint32_t timeout = 5000000;  // 5 second timeout

    while (read_busy() && timeout > 0) {
        oepl_hw_delay_us(10);
        timeout--;
    }

    if (timeout == 0) {
        // Timeout error - display not responding
        oepl_hw_crash("Display BUSY timeout");
    }
}

static void set_dc(bool is_data)
{
    // Use HAL GPIO function
    oepl_hw_gpio_set(PIN_DISPLAY_DC, is_data);
}

static void set_rst(bool level)
{
    // Use HAL GPIO function
    oepl_hw_gpio_set(PIN_DISPLAY_RST, level);
}

static bool read_busy(void)
{
    // Use HAL GPIO function
    // Read BUSY pin (LOW = ready, HIGH = busy)
    return oepl_hw_gpio_get(PIN_DISPLAY_BUSY);
}

// -----------------------------------------------------------------------------
//                          Driver Interface Structure
// -----------------------------------------------------------------------------

const oepl_display_driver_t oepl_display_driver_uc8159_600x448 = {
    .init = uc8159_init,
    .draw = uc8159_draw,
    .sleep = uc8159_sleep,
    .wake = uc8159_wake
};
