#ifndef OEPL_HW_ABSTRACTION_CC2630_H
#define OEPL_HW_ABSTRACTION_CC2630_H

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// Debug levels
typedef enum {
    DBG_SYSTEM,
    DBG_RADIO,
    DBG_DISPLAY,
    DBG_NVM,
    DBG_APP
} debug_level_t;

// Hardware ID for TG-GR6000N
#define HWID_TG_GR6000N  0x80

// -----------------------------------------------------------------------------
//                          Public Function Declarations
// -----------------------------------------------------------------------------

/**
 * Initialize all hardware peripherals
 */
void oepl_hw_init(void);

/**
 * SPI Functions
 */
void oepl_hw_spi_init(void);
void oepl_hw_spi_transfer(const uint8_t* data, size_t len);
void oepl_hw_spi_transfer_read(uint8_t* data, size_t len);
void oepl_hw_spi_cs_assert(void);
void oepl_hw_spi_cs_deassert(void);
void oepl_hw_spi_send_raw(const uint8_t* data, size_t len);
void oepl_hw_spi_read_raw(uint8_t* data, size_t len);

/**
 * GPIO Functions
 */
void oepl_hw_gpio_init(void);
void oepl_hw_gpio_set(uint8_t pin, bool level);
bool oepl_hw_gpio_get(uint8_t pin);

/**
 * Timing Functions
 */
void oepl_hw_delay_ms(uint32_t ms);
void oepl_hw_delay_us(uint32_t us);
uint32_t oepl_hw_get_time_ms(void);

/**
 * Temperature Sensor
 */
bool oepl_hw_get_temperature(int8_t* temp_degc);

/**
 * Battery Voltage
 */
bool oepl_hw_get_voltage(uint16_t* voltage_mv);

/**
 * LED Control
 */
void oepl_hw_set_led(uint8_t color, bool on);

/**
 * Power Management
 */
void oepl_hw_enter_deepsleep(void);

/**
 * Hardware ID
 */
uint8_t oepl_hw_get_hwid(void);

/**
 * Screen Properties
 */
bool oepl_hw_get_screen_properties(size_t* x, size_t* y, size_t* bpp);

/**
 * Debug Print
 */
void oepl_hw_debugprint(debug_level_t level, const char* fmt, ...);

/**
 * Crash/Error Handler
 */
void oepl_hw_crash(const char* message) __attribute__((noreturn));

/**
 * Watchdog
 */
void oepl_hw_watchdog_init(void);
void oepl_hw_watchdog_feed(void);

#endif // OEPL_HW_ABSTRACTION_CC2630_H
