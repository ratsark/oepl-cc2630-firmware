// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include "oepl_app.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "oepl_radio_cc2630.h"
#include "oepl_nvm_cc2630.h"
#include "drivers/oepl_display_driver_uc8159_600x448.h"

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

#define MAX_IMAGE_SLOTS     3
#define CHECK_IN_INTERVAL_MS  (60 * 1000)  // 1 minute

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static app_state_t current_state = APP_STATE_INIT;
static image_slot_t image_slots[MAX_IMAGE_SLOTS];
static uint32_t last_checkin_time = 0;

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void handle_init_state(void);
static void handle_check_storage_state(void);
static void handle_radio_init_state(void);
static void handle_send_avail_req_state(void);
static void handle_wait_for_data_state(void);
static void handle_download_image_state(void);
static void handle_update_display_state(void);
static void handle_sleep_state(void);

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void oepl_app_init(void)
{
    oepl_hw_debugprint(DBG_APP, "OEPL application initializing...\n");

    // Initialize image slots
    for (size_t i = 0; i < MAX_IMAGE_SLOTS; i++) {
        image_slots[i].valid = false;
        image_slots[i].image_size = 0;
        image_slots[i].timestamp = 0;
    }

    current_state = APP_STATE_CHECK_STORAGE;
    last_checkin_time = oepl_hw_get_time_ms();

    oepl_hw_debugprint(DBG_APP, "OEPL application initialized\n");
}

void oepl_app_run(void)
{
    // State machine
    switch (current_state) {
        case APP_STATE_INIT:
            handle_init_state();
            break;

        case APP_STATE_CHECK_STORAGE:
            handle_check_storage_state();
            break;

        case APP_STATE_RADIO_INIT:
            handle_radio_init_state();
            break;

        case APP_STATE_SEND_AVAIL_REQ:
            handle_send_avail_req_state();
            break;

        case APP_STATE_WAIT_FOR_DATA:
            handle_wait_for_data_state();
            break;

        case APP_STATE_DOWNLOAD_IMAGE:
            handle_download_image_state();
            break;

        case APP_STATE_UPDATE_DISPLAY:
            handle_update_display_state();
            break;

        case APP_STATE_SLEEP:
            handle_sleep_state();
            break;

        default:
            oepl_hw_debugprint(DBG_APP, "Unknown state: %d\n", current_state);
            current_state = APP_STATE_INIT;
            break;
    }
}

app_state_t oepl_app_get_state(void)
{
    return current_state;
}

void oepl_app_radio_rx_callback(const uint8_t* data, size_t len, int8_t rssi)
{
    oepl_hw_debugprint(DBG_APP, "Radio RX: %d bytes, RSSI: %d dBm\n", len, rssi);

    // TODO: Parse OEPL packet and handle accordingly
    // - AvailDataInfo: Image available, start download
    // - BlockData: Image data block received
    // - etc.

    (void)data;
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void handle_init_state(void)
{
    oepl_hw_debugprint(DBG_APP, "State: INIT\n");
    current_state = APP_STATE_CHECK_STORAGE;
}

static void handle_check_storage_state(void)
{
    oepl_hw_debugprint(DBG_APP, "State: CHECK_STORAGE\n");

    // Check NVM for stored images
    bool has_valid_image = false;
    for (size_t i = 0; i < MAX_IMAGE_SLOTS; i++) {
        if (oepl_nvm_check_image_slot(i)) {
            image_slots[i].valid = true;
            has_valid_image = true;
            oepl_hw_debugprint(DBG_APP, "Found valid image in slot %d\n", i);
        }
    }

    if (has_valid_image) {
        // Display most recent image
        current_state = APP_STATE_UPDATE_DISPLAY;
    } else {
        // No stored images, try to get from AP
        current_state = APP_STATE_RADIO_INIT;
    }
}

static void handle_radio_init_state(void)
{
    oepl_hw_debugprint(DBG_APP, "State: RADIO_INIT\n");

    // Initialize radio layer
    oepl_radio_init();

    current_state = APP_STATE_SEND_AVAIL_REQ;
}

static void handle_send_avail_req_state(void)
{
    oepl_hw_debugprint(DBG_APP, "State: SEND_AVAIL_REQ\n");

    // Send AvailDataReq packet to AP
    oepl_radio_send_avail_data_req();

    current_state = APP_STATE_WAIT_FOR_DATA;
}

static void handle_wait_for_data_state(void)
{
    // Wait for response from AP
    // This state is exited via radio RX callback
    // Timeout after 5 seconds

    static uint32_t wait_start_time = 0;
    if (wait_start_time == 0) {
        wait_start_time = oepl_hw_get_time_ms();
    }

    uint32_t current_time = oepl_hw_get_time_ms();
    if (current_time - wait_start_time > 5000) {
        oepl_hw_debugprint(DBG_APP, "Timeout waiting for data from AP\n");
        wait_start_time = 0;
        current_state = APP_STATE_SLEEP;
    }
}

static void handle_download_image_state(void)
{
    oepl_hw_debugprint(DBG_APP, "State: DOWNLOAD_IMAGE\n");

    // TODO: Download image blocks from AP
    // - Send BlockRequest packets
    // - Receive BlockData responses
    // - Validate MD5
    // - Store in NVM

    current_state = APP_STATE_UPDATE_DISPLAY;
}

static void handle_update_display_state(void)
{
    oepl_hw_debugprint(DBG_APP, "State: UPDATE_DISPLAY\n");

    // TODO: Load image from NVM and display
    // - Decompress image data
    // - Transfer to display line-by-line
    // - Refresh display

    current_state = APP_STATE_SLEEP;
}

static void handle_sleep_state(void)
{
    uint32_t current_time = oepl_hw_get_time_ms();

    // Check if it's time for next check-in
    if (current_time - last_checkin_time >= CHECK_IN_INTERVAL_MS) {
        oepl_hw_debugprint(DBG_APP, "Wake up for check-in\n");
        last_checkin_time = current_time;
        current_state = APP_STATE_RADIO_INIT;
    } else {
        // Sleep until next check-in
        uint32_t sleep_time = CHECK_IN_INTERVAL_MS - (current_time - last_checkin_time);
        oepl_hw_debugprint(DBG_APP, "Sleeping for %d ms\n", sleep_time);

        // Enter deep sleep
        // In reality, we'd use RTC wakeup timer
        oepl_hw_delay_ms(sleep_time);
    }
}
