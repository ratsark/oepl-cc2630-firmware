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
#define DISPLAY_BPP             1    // 1 bit per pixel (monochrome)

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
 * @param framebuffer Pointer to image data (600×448 / 8 = 16,800 bytes)
 * @param len Length of framebuffer data
 *
 * Note: Due to RAM constraints (20KB), this function transfers data
 * in chunks from compressed storage rather than holding full framebuffer
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

#endif // OEPL_DISPLAY_DRIVER_UC8159_600X448_H
