#ifndef OEPL_DISPLAY_DRIVER_COMMON_CC2630_H
#define OEPL_DISPLAY_DRIVER_COMMON_CC2630_H

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
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t bpp;  // Bits per pixel
    const void* lut_data;  // LUT (Look-Up Table) for waveforms
    size_t lut_size;
} oepl_display_parameters_t;

// Display driver descriptor
typedef struct {
    const char* name;
    void (*init)(void);
    void (*draw)(const uint8_t* framebuffer, size_t len);
    void (*sleep)(void);
    void (*wake)(void);
    const oepl_display_parameters_t* parameters;
} oepl_display_driver_desc_t;

// -----------------------------------------------------------------------------
//                          Public Function Declarations
// -----------------------------------------------------------------------------

/**
 * Register display driver
 */
void oepl_display_driver_register(const oepl_display_driver_desc_t* driver);

/**
 * Get current display driver
 */
const oepl_display_driver_desc_t* oepl_display_driver_get_current(void);

/**
 * Initialize display
 */
void oepl_display_init(void);

/**
 * Update display with image data
 */
void oepl_display_update(const uint8_t* framebuffer, size_t len);

/**
 * Put display into low-power sleep
 */
void oepl_display_sleep(void);

/**
 * Wake display from sleep
 */
void oepl_display_wake(void);

#endif // OEPL_DISPLAY_DRIVER_COMMON_CC2630_H
