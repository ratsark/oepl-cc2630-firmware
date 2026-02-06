#ifndef OEPL_RADIO_CC2630_H
#define OEPL_RADIO_CC2630_H

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// OEPL radio configuration
#define OEPL_PAN_ID             0x4447
#define OEPL_CHANNEL_MIN        0
#define OEPL_CHANNEL_MAX        4
#define OEPL_NUM_CHANNELS       5

// Radio callback for received packets
typedef void (*radio_rx_callback_t)(const uint8_t* data, size_t len, int8_t rssi);

// -----------------------------------------------------------------------------
//                          Public Function Declarations
// -----------------------------------------------------------------------------

/**
 * Initialize radio (2.4 GHz IEEE 802.15.4)
 */
void oepl_radio_init(void);

/**
 * Set radio RX callback
 */
void oepl_radio_set_rx_callback(radio_rx_callback_t callback);

/**
 * Send AvailDataReq packet to AP
 */
void oepl_radio_send_avail_data_req(void);

/**
 * Send BlockRequest packet to AP
 */
void oepl_radio_send_block_request(uint32_t block_id);

/**
 * Send XferComplete packet to AP
 */
void oepl_radio_send_xfer_complete(void);

/**
 * Scan for best channel
 * @return Best channel (0-4) or -1 if none found
 */
int8_t oepl_radio_scan_channels(void);

/**
 * Set radio channel
 */
void oepl_radio_set_channel(uint8_t channel);

/**
 * Enable/disable radio RX
 */
void oepl_radio_rx_enable(bool enable);

/**
 * Check if radio is idle
 */
bool oepl_radio_is_idle(void);

#endif // OEPL_RADIO_CC2630_H
