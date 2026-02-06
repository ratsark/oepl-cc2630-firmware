#ifndef OEPL_APP_H
#define OEPL_APP_H

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// Application states
typedef enum {
    APP_STATE_INIT,
    APP_STATE_CHECK_STORAGE,
    APP_STATE_RADIO_INIT,
    APP_STATE_SEND_AVAIL_REQ,
    APP_STATE_WAIT_FOR_DATA,
    APP_STATE_DOWNLOAD_IMAGE,
    APP_STATE_UPDATE_DISPLAY,
    APP_STATE_SLEEP
} app_state_t;

// Image slot information
typedef struct {
    bool valid;
    uint32_t image_size;
    uint8_t md5[16];
    uint32_t timestamp;
} image_slot_t;

// -----------------------------------------------------------------------------
//                          Public Function Declarations
// -----------------------------------------------------------------------------

/**
 * Initialize OEPL application
 */
void oepl_app_init(void);

/**
 * Run main application state machine
 * Called periodically from main loop
 */
void oepl_app_run(void);

/**
 * Get current application state
 */
app_state_t oepl_app_get_state(void);

/**
 * Handle received radio packet
 * Called by radio layer when packet is received
 */
void oepl_app_radio_rx_callback(const uint8_t* data, size_t len, int8_t rssi);

#endif // OEPL_APP_H
