// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include "oepl_radio_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// OEPL packet types (from protocol analysis)
#define PKT_AVAIL_DATA_REQ      0x41
#define PKT_AVAIL_DATA_INFO     0x42
#define PKT_BLOCK_REQUEST       0x43
#define PKT_BLOCK_DATA          0x44
#define PKT_XFER_COMPLETE       0x45

// Radio state
typedef enum {
    RADIO_STATE_IDLE,
    RADIO_STATE_TX,
    RADIO_STATE_RX,
    RADIO_STATE_SCANNING
} radio_state_t;

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static radio_rx_callback_t rx_callback = NULL;
static radio_state_t radio_state = RADIO_STATE_IDLE;
static uint8_t current_channel = 0;
static uint8_t mac_address[8];

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void radio_configure_2_4ghz(void);
static void radio_send_packet(const uint8_t* data, size_t len);
static void radio_generate_mac_address(void);

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void oepl_radio_init(void)
{
    oepl_hw_debugprint(DBG_RADIO, "Initializing 2.4 GHz IEEE 802.15.4 radio...\n");

    // Generate MAC address from chip ID
    radio_generate_mac_address();

    oepl_hw_debugprint(DBG_RADIO, "MAC: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac_address[0], mac_address[1], mac_address[2], mac_address[3],
                      mac_address[4], mac_address[5], mac_address[6], mac_address[7]);

    // Configure radio for 900MHz proprietary mode
    radio_configure_2_4ghz();

    // Scan for best channel
    int8_t best_channel = oepl_radio_scan_channels();
    if (best_channel >= 0) {
        oepl_radio_set_channel(best_channel);
        oepl_hw_debugprint(DBG_RADIO, "Using channel: %d\n", best_channel);
    } else {
        // Default to channel 0
        oepl_radio_set_channel(0);
        oepl_hw_debugprint(DBG_RADIO, "No channel found, using default: 0\n");
    }

    radio_state = RADIO_STATE_IDLE;

    oepl_hw_debugprint(DBG_RADIO, "Radio initialized\n");
}

void oepl_radio_set_rx_callback(radio_rx_callback_t callback)
{
    rx_callback = callback;
}

void oepl_radio_send_avail_data_req(void)
{
    oepl_hw_debugprint(DBG_RADIO, "Sending AvailDataReq...\n");

    // Build AvailDataReq packet
    // Format (from CC2630 OEPL analysis):
    // [PKT_TYPE] [MAC_ADDR(8)] [HWID] [BATT_MV(2)] [TEMP] [FLAGS]

    uint8_t packet[16];
    size_t offset = 0;

    // Packet type
    packet[offset++] = PKT_AVAIL_DATA_REQ;

    // MAC address
    for (size_t i = 0; i < 8; i++) {
        packet[offset++] = mac_address[i];
    }

    // Hardware ID
    packet[offset++] = oepl_hw_get_hwid();

    // Battery voltage
    uint16_t voltage_mv = 0;
    oepl_hw_get_voltage(&voltage_mv);
    packet[offset++] = (voltage_mv >> 8) & 0xFF;
    packet[offset++] = voltage_mv & 0xFF;

    // Temperature
    int8_t temp_degc = 0;
    oepl_hw_get_temperature(&temp_degc);
    packet[offset++] = (uint8_t)temp_degc;

    // Flags (TODO: define properly)
    packet[offset++] = 0x00;

    // Send packet
    radio_send_packet(packet, offset);

    oepl_hw_debugprint(DBG_RADIO, "AvailDataReq sent (%d bytes)\n", offset);
}

void oepl_radio_send_block_request(uint32_t block_id)
{
    oepl_hw_debugprint(DBG_RADIO, "Sending BlockRequest for block %d...\n", block_id);

    // Build BlockRequest packet
    uint8_t packet[16];
    size_t offset = 0;

    packet[offset++] = PKT_BLOCK_REQUEST;

    // MAC address
    for (size_t i = 0; i < 8; i++) {
        packet[offset++] = mac_address[i];
    }

    // Block ID (32-bit)
    packet[offset++] = (block_id >> 24) & 0xFF;
    packet[offset++] = (block_id >> 16) & 0xFF;
    packet[offset++] = (block_id >> 8) & 0xFF;
    packet[offset++] = block_id & 0xFF;

    radio_send_packet(packet, offset);

    oepl_hw_debugprint(DBG_RADIO, "BlockRequest sent\n");
}

void oepl_radio_send_xfer_complete(void)
{
    oepl_hw_debugprint(DBG_RADIO, "Sending XferComplete...\n");

    // Build XferComplete packet
    uint8_t packet[16];
    size_t offset = 0;

    packet[offset++] = PKT_XFER_COMPLETE;

    // MAC address
    for (size_t i = 0; i < 8; i++) {
        packet[offset++] = mac_address[i];
    }

    radio_send_packet(packet, offset);

    oepl_hw_debugprint(DBG_RADIO, "XferComplete sent\n");
}

int8_t oepl_radio_scan_channels(void)
{
    oepl_hw_debugprint(DBG_RADIO, "Scanning channels...\n");

    radio_state = RADIO_STATE_SCANNING;

    int8_t best_channel = -1;
    int8_t best_rssi = -128;

    // Scan each OEPL channel (0-4)
    for (uint8_t ch = OEPL_CHANNEL_MIN; ch <= OEPL_CHANNEL_MAX; ch++) {
        oepl_radio_set_channel(ch);

        // Enable RX and listen for energy
        oepl_radio_rx_enable(true);
        oepl_hw_delay_ms(50);  // Sample period

        // TODO: Measure RSSI/LQI on this channel
        // For now, just use channel 0
        int8_t rssi = -80;  // Placeholder

        oepl_hw_debugprint(DBG_RADIO, "Channel %d: RSSI=%d dBm\n", ch, rssi);

        if (rssi > best_rssi) {
            best_rssi = rssi;
            best_channel = ch;
        }

        oepl_radio_rx_enable(false);
    }

    radio_state = RADIO_STATE_IDLE;

    oepl_hw_debugprint(DBG_RADIO, "Best channel: %d (RSSI=%d dBm)\n", best_channel, best_rssi);

    return best_channel;
}

void oepl_radio_set_channel(uint8_t channel)
{
    if (channel > OEPL_CHANNEL_MAX) {
        channel = OEPL_CHANNEL_MAX;
    }

    current_channel = channel;

    // TODO: Configure CC2630 RF Core to new channel
    // This requires TI RF driver configuration

    oepl_hw_debugprint(DBG_RADIO, "Set channel: %d\n", channel);
}

void oepl_radio_rx_enable(bool enable)
{
    if (enable) {
        radio_state = RADIO_STATE_RX;
        // TODO: Enable CC2630 RF RX
    } else {
        radio_state = RADIO_STATE_IDLE;
        // TODO: Disable CC2630 RF RX
    }
}

bool oepl_radio_is_idle(void)
{
    return radio_state == RADIO_STATE_IDLE;
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void radio_configure_2_4ghz(void)
{
    // Configure CC2630 RF Core for 2.4 GHz IEEE 802.15.4 proprietary mode
    // This is based on the CC2630 OEPL firmware analysis

    // TODO: Use TI RF driver to configure:
    // - Frequency: 2.4 GHz IEEE 802.15.4 (channels 11-26)
    // - OEPL uses channels 0-4 mapped to 802.15.4 channels
    // - Data rate: 250 kbps (802.15.4 standard)
    // - TX power: Maximum allowed
    // - PAN ID: 0x4447
    //
    // NOTE: CC2630 is a 2.4 GHz part (not Sub-GHz).
    // AP ping confirmed working with OEPL closed-source 4.2" firmware.

    oepl_hw_debugprint(DBG_RADIO, "Configuring 2.4 GHz IEEE 802.15.4 mode...\n");

    // Placeholder configuration
    // In production, this uses TI SmartRF Studio settings
}

static void radio_send_packet(const uint8_t* data, size_t len)
{
    if (radio_state != RADIO_STATE_IDLE) {
        oepl_hw_debugprint(DBG_RADIO, "Radio busy, cannot send\n");
        return;
    }

    radio_state = RADIO_STATE_TX;

    // TODO: Use CC2630 RF Core to transmit packet
    // This requires TI RF driver TX API

    oepl_hw_debugprint(DBG_RADIO, "TX: %d bytes on channel %d\n", len, current_channel);

    // Simulate transmission delay
    oepl_hw_delay_ms(10);

    radio_state = RADIO_STATE_IDLE;
}

static void radio_generate_mac_address(void)
{
    // Generate MAC address from CC2630 chip unique ID
    // CC2630 has factory-programmed unique ID in FCFG

    // TODO: Read from FCFG1 registers
    // For now, use placeholder

    mac_address[0] = 0x02;  // Locally administered
    mac_address[1] = 0x00;
    mac_address[2] = 0x00;
    mac_address[3] = 0x00;
    mac_address[4] = 0x00;
    mac_address[5] = 0x00;
    mac_address[6] = 0x00;
    mac_address[7] = 0x01;
}
