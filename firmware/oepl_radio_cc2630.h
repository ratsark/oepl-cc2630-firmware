#ifndef OEPL_RADIO_CC2630_H
#define OEPL_RADIO_CC2630_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- OEPL Protocol Constants ---
#define PROTO_PAN_ID            0x4447

// Packet types (from oepl-proto.h)
#define PKT_AVAIL_DATA_REQ      0xE5
#define PKT_AVAIL_DATA_INFO     0xE6
#define PKT_BLOCK_REQUEST       0xE4
#define PKT_BLOCK_PART          0xE8
#define PKT_BLOCK_REQUEST_ACK   0xE9
#define PKT_XFER_COMPLETE       0xEA
#define PKT_XFER_COMPLETE_ACK   0xEB
#define PKT_PING                0xED
#define PKT_PONG                0xEE

// Hardware type for 6" BWR (from oepl-definitions.h)
#define HW_TYPE                 0x35  // SOLUM_M3_BWR_60

// Block transfer constants
#define BLOCK_PART_DATA_SIZE    99
#define BLOCK_MAX_PARTS         42
#define BLOCK_DATA_SIZE         4096UL
#define BLOCK_REQ_PARTS_BYTES   6

// Wakeup reasons
#define WAKEUP_REASON_TIMED         0
#define WAKEUP_REASON_FIRSTBOOT     0xFC
#define WAKEUP_REASON_NETWORK_SCAN  0xFD

// Capabilities
#define CAPABILITY_SUPPORTS_COMPRESSION  0x02

// Data types
#define DATATYPE_NOUPDATE       0x00

// --- Protocol Structs (packed, little-endian on wire) ---

struct __attribute__((packed)) MacFrameBcast {
    uint8_t fcs[2];       // {0x01, 0xC8} for broadcast
    uint8_t seq;
    uint16_t dstPan;      // PROTO_PAN_ID (LE)
    uint16_t dstAddr;     // 0xFFFF (broadcast)
    uint16_t srcPan;      // PROTO_PAN_ID (LE)
    uint8_t src[8];       // tag MAC (8 bytes)
};  // 17 bytes

struct __attribute__((packed)) MacFrameNormal {
    uint8_t fcs[2];       // {0x41, 0xCC} for unicast
    uint8_t seq;
    uint16_t pan;         // PROTO_PAN_ID (LE)
    uint8_t dst[8];       // destination MAC
    uint8_t src[8];       // source MAC
};  // 21 bytes

struct __attribute__((packed)) AvailDataReq {
    uint8_t checksum;
    uint8_t lastPacketLQI;
    int8_t  lastPacketRSSI;
    int8_t  temperature;
    uint16_t batteryMv;
    uint8_t hwType;
    uint8_t wakeupReason;
    uint8_t capabilities;
    uint16_t tagSoftwareVersion;
    uint8_t currentChannel;
    uint8_t customMode;
    uint8_t reserved[8];
};  // 21 bytes

struct __attribute__((packed)) AvailDataInfo {
    uint8_t checksum;
    uint64_t dataVer;
    uint32_t dataSize;
    uint8_t dataType;
    uint8_t dataTypeArgument;
    uint16_t nextCheckIn;
};  // 17 bytes

struct __attribute__((packed)) BlockRequest {
    uint8_t checksum;
    uint64_t ver;
    uint8_t blockId;
    uint8_t type;
    uint8_t requestedParts[BLOCK_REQ_PARTS_BYTES];
};  // 17 bytes

struct __attribute__((packed)) BlockRequestAck {
    uint8_t checksum;
    uint16_t pleaseWaitMs;
};  // 3 bytes

struct __attribute__((packed)) BlockPart {
    uint8_t checksum;
    uint8_t blockId;
    uint8_t blockPart;
    uint8_t data[BLOCK_PART_DATA_SIZE];
};  // 102 bytes

struct __attribute__((packed)) BlockData {
    uint16_t size;
    uint16_t checksum;
    uint8_t data[];
};  // 4 bytes header

// --- Radio State ---
typedef struct {
    uint8_t mac[8];
    uint8_t ap_mac[8];
    uint8_t current_channel;     // OEPL channel index (0-5)
    uint8_t current_ieee_ch;     // IEEE channel (11,15,20,25,26,27)
    int8_t  last_rssi;
    uint8_t last_lqi;
    uint8_t seq;
    bool    ap_found;
} radio_state_t;

// --- Public API ---

// Initialize protocol layer (call after oepl_rf_init)
void oepl_radio_init(void);

// Scan channels for AP by sending PING, listening for PONG
// Returns OEPL channel index (0-5) or -1 if no AP found
int8_t oepl_radio_scan_channels(void);

// Send AvailDataReq on current channel, wait for AvailDataInfo response
// Returns true if AP responded with data info
bool oepl_radio_checkin(struct AvailDataInfo *out_info);

// Send BlockRequest, receive block parts
// parts_rcvd is in/out bitmap — accumulates across retries
// Returns number of parts received so far (caller checks >= BLOCK_MAX_PARTS)
uint8_t oepl_radio_request_block(uint8_t block_id, uint64_t data_ver, uint8_t data_type,
                                  uint8_t *block_buf, uint8_t *parts_rcvd);

// Send XferComplete
bool oepl_radio_send_xfer_complete(void);

// Get radio state
radio_state_t *oepl_radio_get_state(void);

#endif // OEPL_RADIO_CC2630_H
