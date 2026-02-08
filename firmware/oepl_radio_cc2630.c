// -----------------------------------------------------------------------------
//  OEPL Protocol Layer for CC2630
//  Builds IEEE 802.15.4 frames with OEPL payload, uses oepl_rf_cc2630 for TX/RX
//
//  IMPORTANT: CMD_IEEE_TX is a foreground command that requires CMD_IEEE_RX
//  to be active as the background command. So the pattern is always:
//    1. Start CMD_IEEE_RX (background)
//    2. Send CMD_IEEE_TX (foreground, returns to RX after TX)
//    3. Wait for response in RX queue
//    4. Stop RX when done
// -----------------------------------------------------------------------------

#include "oepl_radio_cc2630.h"
#include "oepl_rf_cc2630.h"
#include "rf_mailbox.h"
#include "rtt.h"
#include <string.h>

// --- Static State ---
static radio_state_t radio_st;
static uint8_t tx_frame[64];

// --- Helpers ---

static void add_crc(void *p, uint8_t len)
{
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++)
        total += ((uint8_t *)p)[c];
    ((uint8_t *)p)[0] = total;
}

static bool check_crc(const void *p, uint8_t len)
{
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++)
        total += ((const uint8_t *)p)[c];
    return ((const uint8_t *)p)[0] == total;
}

static void build_bcast_header(struct MacFrameBcast *f)
{
    f->fcs[0] = 0x01;  // Data frame, no ACK request (broadcast)
    f->fcs[1] = 0xC8;  // Short dst addr, long src addr, no PAN compress
    f->seq = radio_st.seq++;
    f->dstPan = PROTO_PAN_ID;
    f->dstAddr = 0xFFFF;
    f->srcPan = PROTO_PAN_ID;
    memcpy(f->src, radio_st.mac, 8);
}

static void build_unicast_header(struct MacFrameNormal *f, const uint8_t *dst_mac)
{
    f->fcs[0] = 0x41;
    f->fcs[1] = 0xCC;
    f->seq = radio_st.seq++;
    f->pan = PROTO_PAN_ID;
    memcpy(f->dst, dst_mac, 8);
    memcpy(f->src, radio_st.mac, 8);
}

// Poll RX queue for a packet, with bounded wait
// Returns pointer to received frame data, or NULL on timeout
static uint8_t *wait_for_rx(uint32_t wait_loops, uint8_t *out_len, int8_t *out_rssi)
{
    for (volatile uint32_t w = 0; w < wait_loops; w++) {
        uint8_t *pkt = oepl_rf_rx_get(out_len, out_rssi);
        if (pkt) return pkt;
    }
    return NULL;
}

// --- Public API ---

void oepl_radio_init(void)
{
    memset(&radio_st, 0, sizeof(radio_st));
    oepl_rf_get_mac(radio_st.mac);
    radio_st.ap_found = false;
}

int8_t oepl_radio_scan_channels(void)
{
    rtt_puts("Scan:");

    for (uint8_t ch = 0; ch < OEPL_NUM_CHANNELS; ch++) {
        rf_status_t rc = oepl_rf_set_channel(ch);
        if (rc != RF_OK) continue;

        uint8_t ieee_ch = oepl_channel_map[ch];
        rtt_puts(" ");
        rtt_put_hex8(ieee_ch);

        // Build PING frame
        struct MacFrameBcast *hdr = (struct MacFrameBcast *)tx_frame;
        build_bcast_header(hdr);
        tx_frame[sizeof(struct MacFrameBcast)] = PKT_PING;
        uint8_t tx_len = sizeof(struct MacFrameBcast) + 1;

        // Try up to 5 times per channel
        for (uint8_t attempt = 0; attempt < 5; attempt++) {
            // 1. Start RX first (background, 300ms timeout)
            rc = oepl_rf_rx_start(ieee_ch, 300000);
            if (rc != RF_OK) continue;

            // 2. TX PING (foreground within RX context)
            rc = oepl_rf_tx(tx_frame, tx_len);
            if (rc != RF_OK) {
                oepl_rf_rx_stop();
                continue;
            }

            // 3. Wait for PONG response — keep polling while RX is active
            for (uint8_t w = 0; w < 10; w++) {
                uint8_t pkt_len;
                int8_t rssi;
                uint8_t *pkt = wait_for_rx(500000, &pkt_len, &rssi);
                if (pkt) {
                    // PONG: MacFrameNormal(21) + PKT_PONG(1) + channel(1)
                    if (pkt_len >= sizeof(struct MacFrameNormal) + 2) {
                        uint8_t pkt_type = pkt[sizeof(struct MacFrameNormal)];
                        if (pkt_type == PKT_PONG) {
                            struct MacFrameNormal *resp = (struct MacFrameNormal *)pkt;
                            memcpy(radio_st.ap_mac, resp->src, 8);
                            radio_st.current_channel = ch;
                            radio_st.current_ieee_ch = ieee_ch;
                            radio_st.last_rssi = rssi;
                            radio_st.ap_found = true;

                            oepl_rf_rx_stop();
                            oepl_rf_rx_flush();

                            rtt_puts(" PONG! RSSI=");
                            rtt_put_hex8((uint8_t)rssi);
                            rtt_puts("\r\n");
                            return (int8_t)ch;
                        }
                    }
                    oepl_rf_rx_flush();
                } else {
                    // No packet — check if RX is still active
                    if (oepl_rf_rx_status() != ACTIVE) break;
                }
            }

            // 4. Stop RX
            oepl_rf_rx_stop();
        }
    }

    rtt_puts(" none\r\n");
    return -1;
}

bool oepl_radio_checkin(struct AvailDataInfo *out_info)
{
    if (!radio_st.ap_found) return false;

    // Build AvailDataReq frame:
    // MacFrameBcast(17) + PKT_TYPE(1) + AvailDataReq(21) + pad(1) = 40 bytes
    // AP checks ret==40 exactly — the padding byte is required!
    struct MacFrameBcast *hdr = (struct MacFrameBcast *)tx_frame;
    build_bcast_header(hdr);
    tx_frame[sizeof(struct MacFrameBcast)] = PKT_AVAIL_DATA_REQ;

    struct AvailDataReq *req = (struct AvailDataReq *)&tx_frame[sizeof(struct MacFrameBcast) + 1];
    memset(req, 0, sizeof(struct AvailDataReq));
    req->lastPacketLQI = radio_st.last_lqi;
    req->lastPacketRSSI = radio_st.last_rssi;
    req->temperature = 25;
    req->batteryMv = 3000;
    req->hwType = HW_TYPE;
    req->wakeupReason = WAKEUP_REASON_FIRSTBOOT;
    req->capabilities = 0;
    req->tagSoftwareVersion = 0x0001;
    req->currentChannel = radio_st.current_channel;
    req->customMode = 0;
    add_crc(req, sizeof(struct AvailDataReq));

    // Padding byte after struct (AP expects exactly 40 bytes MPDU)
    tx_frame[sizeof(struct MacFrameBcast) + 1 + sizeof(struct AvailDataReq)] = 0x00;
    uint8_t tx_len = sizeof(struct MacFrameBcast) + 1 + sizeof(struct AvailDataReq) + 1;

    rtt_puts("TX ADR len=");
    rtt_put_hex8(tx_len);
    rtt_puts("\r\n");

    // 1. Start RX (background, 5s timeout) with explicit channel
    rf_status_t rc = oepl_rf_rx_start(radio_st.current_ieee_ch, 5000000);
    if (rc != RF_OK) return false;

    // 2. TX AvailDataReq (foreground within RX)
    rc = oepl_rf_tx(tx_frame, tx_len);
    if (rc != RF_OK) {
        oepl_rf_rx_stop();
        rtt_puts("TX fail\r\n");
        return false;
    }
    rtt_puts("TX OK\r\n");

    // 3. Wait for AvailDataInfo response — keep polling while RX is active
    for (uint8_t attempts = 0; attempts < 50; attempts++) {
        uint8_t pkt_len;
        int8_t rssi;
        uint8_t *pkt = wait_for_rx(500000, &pkt_len, &rssi);
        if (!pkt) {
            // No packet yet — check if RX is still active
            if (oepl_rf_rx_status() != 0x0002) {
                rtt_puts("RX: ended\r\n");
                break;
            }
            continue;  // RX still active, keep polling
        }

        // Dump first bytes of received frame
        rtt_puts("RX: len=");
        rtt_put_hex8(pkt_len);
        rtt_puts(" [");
        uint8_t dump_len = (pkt_len > 16) ? 16 : pkt_len;
        for (uint8_t i = 0; i < dump_len; i++) {
            rtt_put_hex8(pkt[i]);
            if (i < dump_len - 1) rtt_puts(" ");
        }
        rtt_puts("]\r\n");

        if (pkt_len >= sizeof(struct MacFrameNormal) + 1 + sizeof(struct AvailDataInfo)) {
            uint8_t pkt_type = pkt[sizeof(struct MacFrameNormal)];
            if (pkt_type == PKT_AVAIL_DATA_INFO) {
                struct AvailDataInfo *info = (struct AvailDataInfo *)&pkt[sizeof(struct MacFrameNormal) + 1];
                if (check_crc(info, sizeof(struct AvailDataInfo))) {
                    memcpy(out_info, info, sizeof(struct AvailDataInfo));
                    radio_st.last_rssi = rssi;
                    oepl_rf_rx_stop();
                    oepl_rf_rx_flush();
                    rtt_puts("Got AvailDataInfo type=");
                    rtt_put_hex8(info->dataType);
                    rtt_puts("\r\n");
                    return true;
                }
                rtt_puts("CRC fail\r\n");
            }
        }
        oepl_rf_rx_flush();
    }

    oepl_rf_rx_stop();
    rtt_puts("No AvailDataInfo\r\n");
    return false;
}

bool oepl_radio_send_xfer_complete(void)
{
    if (!radio_st.ap_found) return false;

    struct MacFrameNormal *hdr = (struct MacFrameNormal *)tx_frame;
    build_unicast_header(hdr, radio_st.ap_mac);
    tx_frame[sizeof(struct MacFrameNormal)] = PKT_XFER_COMPLETE;
    uint8_t tx_len = sizeof(struct MacFrameNormal) + 1;

    // Start RX, then TX (explicit channel)
    rf_status_t rc = oepl_rf_rx_start(radio_st.current_ieee_ch, 500000);
    if (rc != RF_OK) return false;

    rtt_puts("TX XferComplete\r\n");
    rc = oepl_rf_tx(tx_frame, tx_len);
    oepl_rf_rx_stop();
    return rc == RF_OK;
}

// Determine MAC header size from Frame Control field
static uint8_t mac_hdr_size(const uint8_t *pkt, uint8_t pkt_len)
{
    if (pkt_len < 3) return 0;
    uint8_t dst_mode = (pkt[1] >> 2) & 0x03;
    uint8_t src_mode = (pkt[1] >> 6) & 0x03;
    bool pan_compress = (pkt[0] >> 6) & 0x01;

    uint8_t sz = 3;  // FCS(2) + seq(1)
    if (dst_mode == 2) sz += 2 + 2;       // dst PAN + short addr
    else if (dst_mode == 3) sz += 2 + 8;  // dst PAN + extended addr
    if (src_mode == 2) sz += (pan_compress ? 0 : 2) + 2;
    else if (src_mode == 3) sz += (pan_compress ? 0 : 2) + 8;
    return sz;
}

bool oepl_radio_request_block(uint8_t block_id, uint64_t data_ver, uint8_t data_type,
                               uint8_t *block_buf, uint16_t *out_size)
{
    if (!radio_st.ap_found) return false;

    struct MacFrameNormal *hdr = (struct MacFrameNormal *)tx_frame;
    build_unicast_header(hdr, radio_st.ap_mac);
    tx_frame[sizeof(struct MacFrameNormal)] = PKT_BLOCK_REQUEST;

    struct BlockRequest *breq = (struct BlockRequest *)&tx_frame[sizeof(struct MacFrameNormal) + 1];
    memset(breq, 0, sizeof(struct BlockRequest));
    breq->ver = data_ver;
    breq->blockId = block_id;
    breq->type = data_type;
    // Request all parts (set bits 0-41)
    memset(breq->requestedParts, 0xFF, BLOCK_REQ_PARTS_BYTES);
    breq->requestedParts[5] &= 0x03;
    add_crc(breq, sizeof(struct BlockRequest));

    uint8_t tx_len = sizeof(struct MacFrameNormal) + 1 + sizeof(struct BlockRequest);

    rtt_puts("BRQ b=");
    rtt_put_hex8(block_id);

    // Single long RX session: covers ack + wait + all parts (15s)
    rf_status_t rc = oepl_rf_rx_start(radio_st.current_ieee_ch, 15000000);
    if (rc != RF_OK) { rtt_puts(" RXfail\r\n"); return false; }

    // TX block request
    rc = oepl_rf_tx(tx_frame, tx_len);
    if (rc != RF_OK) {
        oepl_rf_rx_stop();
        rtt_puts(" TXfail\r\n");
        return false;
    }
    rtt_puts(" TX+\r\n");

    // Receive ack + parts in one continuous RX session
    uint8_t parts_received[BLOCK_REQ_PARTS_BYTES];
    memset(parts_received, 0, sizeof(parts_received));
    uint8_t total_parts = 0;
    memset(block_buf, 0x00, BLOCK_DATA_SIZE);
    bool got_ack = false;
    uint8_t other_pkts = 0;

    for (volatile uint32_t w = 0; w < 30000000; w++) {
        uint8_t pkt_len;
        int8_t rssi;
        uint8_t *pkt = oepl_rf_rx_get(&pkt_len, &rssi);
        if (!pkt) {
            // Periodically check if RX is still active
            if ((w & 0xFFFFF) == 0 && w > 0) {
                if (oepl_rf_rx_status() != ACTIVE) break;
            }
            continue;
        }

        // Parse header size from frame control
        uint8_t hsz = mac_hdr_size(pkt, pkt_len);
        if (hsz > 0 && pkt_len > hsz) {
            uint8_t pkt_type = pkt[hsz];

            if (pkt_type == PKT_BLOCK_REQUEST_ACK && pkt_len >= hsz + 1 + 3) {
                struct BlockRequestAck *ack = (struct BlockRequestAck *)&pkt[hsz + 1];
                got_ack = true;
                rtt_puts("ACK w=");
                rtt_put_hex8((ack->pleaseWaitMs >> 8) & 0xFF);
                rtt_put_hex8(ack->pleaseWaitMs & 0xFF);
                rtt_puts("\r\n");
            } else if (pkt_type == PKT_BLOCK_PART && pkt_len >= hsz + 1 + 3) {
                struct BlockPart *bp = (struct BlockPart *)&pkt[hsz + 1];
                if (bp->blockId == block_id && bp->blockPart < BLOCK_MAX_PARTS) {
                    uint16_t offset = (uint16_t)bp->blockPart * BLOCK_PART_DATA_SIZE;
                    uint16_t copy_len = BLOCK_PART_DATA_SIZE;
                    if (offset + copy_len > BLOCK_DATA_SIZE)
                        copy_len = BLOCK_DATA_SIZE - offset;
                    memcpy(&block_buf[offset], bp->data, copy_len);

                    uint8_t byte_idx = bp->blockPart / 8;
                    uint8_t bit_idx = bp->blockPart % 8;
                    if (!(parts_received[byte_idx] & (1 << bit_idx))) {
                        parts_received[byte_idx] |= (1 << bit_idx);
                        total_parts++;
                    }
                }
            } else {
                other_pkts++;
            }
        } else {
            other_pkts++;
        }

        oepl_rf_rx_flush();
        if (total_parts >= BLOCK_MAX_PARTS) break;
    }
    oepl_rf_rx_stop();

    rtt_puts("BP:");
    rtt_put_hex8(total_parts);
    rtt_puts("/");
    rtt_put_hex8(BLOCK_MAX_PARTS);
    if (!got_ack) rtt_puts(" noACK");
    if (other_pkts) { rtt_puts(" oth="); rtt_put_hex8(other_pkts); }
    rtt_puts("\r\n");

    *out_size = BLOCK_DATA_SIZE;
    return total_parts > 0;
}

radio_state_t *oepl_radio_get_state(void)
{
    return &radio_st;
}
