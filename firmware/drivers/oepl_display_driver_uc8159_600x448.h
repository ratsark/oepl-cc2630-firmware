#ifndef OEPL_DISPLAY_DRIVER_UC8159_600X448_H
#define OEPL_DISPLAY_DRIVER_UC8159_600X448_H

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// Display parameters
#define DISPLAY_WIDTH_600X448   600
#define DISPLAY_HEIGHT_600X448  448
#define DISPLAY_BPP             4    // 4 bits per pixel (BWR: 0x0=black, 0x3=white, 0x4=red)

// UC8159 Commands (from stock firmware analysis)
#define CMD_WAKE                0xAB
#define CMD_NOP                 0x00
#define CMD_ENABLE              0x06
#define CMD_DATA_START          0x20
#define CMD_DATA_32K            0x52
#define CMD_DATA_64K            0xD8
#define CMD_ADDR                0x03
#define CMD_SLEEP               0xB9
#define CMD_POWER_ON            0x04  // PON - Power on for refresh
#define CMD_DISPLAY_REFRESH     0x12  // DRF - Display refresh

// Display driver interface
typedef struct {
    void (*init)(void);
    void (*draw)(const uint8_t* framebuffer, size_t len);
    void (*sleep)(void);
    void (*wake)(void);
} oepl_display_driver_t;

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------
extern const oepl_display_driver_t oepl_display_driver_uc8159_600x448;

// -----------------------------------------------------------------------------
//                          Public Function Declarations
// -----------------------------------------------------------------------------

/**
 * Initialize the UC8159 display controller
 * Uses initialization sequence extracted from TG-GR6000N stock firmware
 */
void uc8159_init(void);

/**
 * Draw framebuffer to display
 * @param framebuffer Pointer to 4bpp BWR image data (600×448 / 2 = 134,400 bytes)
 * @param len Length of framebuffer data
 *
 * Note: Due to RAM constraints, caller should pass data in chunks.
 * For streaming from radio, use uc8159_draw_begin/stream/end.
 */
void uc8159_draw(const uint8_t* framebuffer, size_t len);

/**
 * Put display into low-power sleep mode
 */
void uc8159_sleep(void);

/**
 * Wake display from sleep mode
 */
void uc8159_wake(void);

/**
 * Fill entire display with a single byte value
 * @param fill_byte Byte to fill (4bpp BWR: 0x00=black, 0x33=white, 0x44=red)
 * Useful for testing without needing a full framebuffer
 */
void uc8159_fill(uint8_t fill_byte);

#endif // OEPL_DISPLAY_DRIVER_UC8159_600X448_H
