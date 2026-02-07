// -----------------------------------------------------------------------------
//  CC2630 RF Core Driver for OEPL
//  Bare-metal IEEE 802.15.4 radio using TI driverlib
// -----------------------------------------------------------------------------

#include "oepl_rf_cc2630.h"
#include "rtt.h"

#include <string.h>

// TI driverlib
#include "prcm.h"
#include "rfc.h"
#include "rf_common_cmd.h"
#include "rf_ieee_cmd.h"
#include "rf_data_entry.h"
#include "rf_mailbox.h"

// FCFG1 IEEE MAC address registers
#define FCFG1_MAC_15_4_0    (*(volatile uint32_t *)0x50001294)
#define FCFG1_MAC_15_4_1    (*(volatile uint32_t *)0x50001298)

// OEPL channel map: 6 channels -> IEEE 802.15.4 channels
const uint8_t oepl_channel_map[OEPL_NUM_CHANNELS] = {11, 15, 20, 25, 26, 27};

// RF overrides for IEEE 802.15.4 2.4 GHz (CC2630)
// Minimal set - mostly use defaults
static uint32_t rf_overrides[] = {
    // DC/DC mode for CC2630 (internal bias, differential mode)
    // 0x00354038 = set DCDC config
    // 0x000F8883 = adjust synth
    END_OVERRIDE
};

// --- Static command structures (persist in RAM for RF core access) ---
static rfc_CMD_RADIO_SETUP_t rf_cmd_setup;
static rfc_CMD_FS_t rf_cmd_fs;
static rfc_CMD_IEEE_TX_t rf_cmd_tx;
static rfc_CMD_IEEE_RX_t rf_cmd_rx;
static rfc_ieeeRxOutput_t rf_rx_output;

// RX data queue: single entry, circular
#define RX_BUF_SIZE 256
static uint8_t rx_buf[RX_BUF_SIZE] __attribute__((aligned(4)));
static rfc_dataEntryGeneral_t *rx_entry = (rfc_dataEntryGeneral_t *)rx_buf;
static dataQueue_t rx_queue;

// TX buffer
static uint8_t tx_buf[128];

// -----------------------------------------------------------------------------
//  Internal helpers
// -----------------------------------------------------------------------------

static void rf_wait_boot(void)
{
    // Poll RFCPEIFG for BOOT_DONE (bit 30)
    // Bounded wait to avoid hang
    for (volatile uint32_t i = 0; i < 500000; i++) {
        uint32_t flags = HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG);
        if (flags & IRQ_BOOT_DONE) {
            RFCCpeIntClear(IRQ_BOOT_DONE);
            return;
        }
    }
}

static rf_status_t rf_send_cmd(uint32_t cmd_ptr)
{
    uint32_t cmdsta = RFCDoorbellSendTo(cmd_ptr);
    if ((cmdsta & 0xFF) != CMDSTA_Done) {
        rtt_puts("RF cmd rejected: CMDSTA=0x");
        rtt_put_hex8(cmdsta & 0xFF);
        rtt_puts("\r\n");
        return RF_ERR_SETUP;
    }
    return RF_OK;
}

static rf_status_t rf_wait_cmd_done(volatile uint16_t *status_ptr, uint32_t timeout_loops)
{
    for (volatile uint32_t i = 0; i < timeout_loops; i++) {
        uint16_t s = *status_ptr;
        if (s >= DONE_OK) {
            if (s == DONE_OK) return RF_OK;
            rtt_puts("RF cmd status=0x");
            rtt_put_hex8((s >> 8) & 0xFF);
            rtt_put_hex8(s & 0xFF);
            rtt_puts("\r\n");
            return RF_ERR_SETUP;
        }
    }
    rtt_puts("RF cmd timeout\r\n");
    return RF_ERR_TIMEOUT;
}

// -----------------------------------------------------------------------------
//  Public API
// -----------------------------------------------------------------------------

rf_status_t oepl_rf_init(void)
{
    rtt_puts("RF: Powering RF core...\r\n");

    // 1. Power on RF core domain
    PRCMPowerDomainOn(PRCM_DOMAIN_RFCORE);

    // Wait for power domain to come up
    for (volatile uint32_t i = 0; i < 500000; i++) {
        if (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) == PRCM_DOMAIN_POWER_ON)
            break;
    }
    if (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_ON) {
        rtt_puts("RF: Power domain FAILED\r\n");
        return RF_ERR_POWER;
    }
    rtt_puts("RF: Power domain ON\r\n");

    // 2. Enable RF core clock domain
    PRCMDomainEnable(PRCM_DOMAIN_RFCORE);
    PRCMLoadSet();
    while (!PRCMLoadGet()) {}

    // 3. Enable CPE/CPERAM/RFC clocks
    RFCClockEnable();
    rtt_puts("RF: Clocks enabled\r\n");

    // 4. Wait for RF core boot
    rf_wait_boot();
    rtt_puts("RF: Boot done\r\n");

    // 5. Send CMD_RADIO_SETUP for IEEE 802.15.4
    memset(&rf_cmd_setup, 0, sizeof(rf_cmd_setup));
    rf_cmd_setup.commandNo = CMD_RADIO_SETUP;
    rf_cmd_setup.status = IDLE;
    rf_cmd_setup.pNextOp = NULL;
    rf_cmd_setup.startTime = 0;
    rf_cmd_setup.startTrigger.triggerType = TRIG_NOW;
    rf_cmd_setup.condition.rule = COND_NEVER;
    rf_cmd_setup.mode = 0x01;  // IEEE 802.15.4
    rf_cmd_setup.config.frontEndMode = 0x00;  // Differential
    rf_cmd_setup.config.biasMode = 0;         // Internal bias
    rf_cmd_setup.config.analogCfgMode = 0x00; // Write analog config (first time)
    rf_cmd_setup.config.bNoFsPowerUp = 0;     // Power up FS
    rf_cmd_setup.txPower = 0x9330;            // 5 dBm (typical CC2630 setting)
    rf_cmd_setup.pRegOverride = rf_overrides;

    rf_status_t rc = rf_send_cmd((uint32_t)&rf_cmd_setup);
    if (rc != RF_OK) {
        rtt_puts("RF: RADIO_SETUP send FAILED\r\n");
        return RF_ERR_SETUP;
    }

    rc = rf_wait_cmd_done(&rf_cmd_setup.status, 500000);
    if (rc != RF_OK) {
        rtt_puts("RF: RADIO_SETUP FAILED\r\n");
        return RF_ERR_SETUP;
    }
    rtt_puts("RF: RADIO_SETUP OK\r\n");

    // 6. Set up RX data queue (single entry, circular)
    memset(rx_buf, 0, sizeof(rx_buf));
    rx_entry->pNextEntry = (uint8_t *)rx_entry;  // Circular: points to self
    rx_entry->status = DATA_ENTRY_PENDING;
    rx_entry->config.type = DATA_ENTRY_TYPE_GEN;
    rx_entry->config.lenSz = 1;  // 1-byte length prefix in each element
    rx_entry->length = RX_BUF_SIZE - sizeof(rfc_dataEntryGeneral_t) + 1;

    rx_queue.pCurrEntry = (uint8_t *)rx_entry;
    rx_queue.pLastEntry = NULL;  // Circular queue

    return RF_OK;
}

rf_status_t oepl_rf_set_channel(uint8_t oepl_channel_idx)
{
    if (oepl_channel_idx >= OEPL_NUM_CHANNELS) {
        rtt_puts("RF: Invalid channel index\r\n");
        return RF_ERR_FS;
    }

    uint8_t ieee_ch = oepl_channel_map[oepl_channel_idx];

    // IEEE 802.15.4: freq = 2405 + 5 * (channel - 11) MHz
    uint16_t freq_mhz = 2405 + 5 * (ieee_ch - 11);

    rtt_puts("RF: CMD_FS ch=");
    rtt_put_hex8(ieee_ch);
    rtt_puts(" freq=");
    rtt_put_hex8((freq_mhz >> 8) & 0xFF);
    rtt_put_hex8(freq_mhz & 0xFF);
    rtt_puts("\r\n");

    memset(&rf_cmd_fs, 0, sizeof(rf_cmd_fs));
    rf_cmd_fs.commandNo = CMD_FS;
    rf_cmd_fs.status = IDLE;
    rf_cmd_fs.pNextOp = NULL;
    rf_cmd_fs.startTime = 0;
    rf_cmd_fs.startTrigger.triggerType = TRIG_NOW;
    rf_cmd_fs.condition.rule = COND_NEVER;
    rf_cmd_fs.frequency = freq_mhz;
    rf_cmd_fs.fractFreq = 0;
    rf_cmd_fs.synthConf.bTxMode = 0;  // RX mode
    rf_cmd_fs.synthConf.refFreq = 0;

    rf_status_t rc = rf_send_cmd((uint32_t)&rf_cmd_fs);
    if (rc != RF_OK) return RF_ERR_FS;

    rc = rf_wait_cmd_done(&rf_cmd_fs.status, 500000);
    if (rc != RF_OK) {
        rtt_puts("RF: CMD_FS FAILED\r\n");
        return RF_ERR_FS;
    }

    rtt_puts("RF: CMD_FS OK\r\n");
    return RF_OK;
}

rf_status_t oepl_rf_tx(const uint8_t *payload, uint8_t len)
{
    if (len > sizeof(tx_buf)) return RF_ERR_TX;

    memcpy(tx_buf, payload, len);

    memset(&rf_cmd_tx, 0, sizeof(rf_cmd_tx));
    rf_cmd_tx.commandNo = CMD_IEEE_TX;
    rf_cmd_tx.status = IDLE;
    rf_cmd_tx.pNextOp = NULL;
    rf_cmd_tx.startTime = 0;
    rf_cmd_tx.startTrigger.triggerType = TRIG_NOW;
    rf_cmd_tx.condition.rule = COND_NEVER;
    rf_cmd_tx.txOpt.bIncludePhyHdr = 0;  // Auto PHY header
    rf_cmd_tx.txOpt.bIncludeCrc = 0;     // Auto CRC
    rf_cmd_tx.payloadLen = len;
    rf_cmd_tx.pPayload = tx_buf;

    rf_status_t rc = rf_send_cmd((uint32_t)&rf_cmd_tx);
    if (rc != RF_OK) return RF_ERR_TX;

    rc = rf_wait_cmd_done(&rf_cmd_tx.status, 500000);
    if (rc != RF_OK) {
        rtt_puts("RF: TX FAILED\r\n");
        return RF_ERR_TX;
    }

    return RF_OK;
}

rf_status_t oepl_rf_rx_start(uint8_t ieee_channel, uint32_t timeout_us)
{
    // Reset RX entry
    rx_entry->status = DATA_ENTRY_PENDING;
    rx_queue.pCurrEntry = (uint8_t *)rx_entry;

    memset(&rf_rx_output, 0, sizeof(rf_rx_output));

    memset(&rf_cmd_rx, 0, sizeof(rf_cmd_rx));
    rf_cmd_rx.commandNo = CMD_IEEE_RX;
    rf_cmd_rx.status = IDLE;
    rf_cmd_rx.pNextOp = NULL;
    rf_cmd_rx.startTime = 0;
    rf_cmd_rx.startTrigger.triggerType = TRIG_NOW;
    rf_cmd_rx.condition.rule = COND_NEVER;
    rf_cmd_rx.channel = ieee_channel;  // 0 = keep current, 11-26 = IEEE channel
    rf_cmd_rx.rxConfig.bAutoFlushCrc = 1;
    rf_cmd_rx.rxConfig.bAutoFlushIgn = 0;
    rf_cmd_rx.rxConfig.bIncludePhyHdr = 0;
    rf_cmd_rx.rxConfig.bIncludeCrc = 0;
    rf_cmd_rx.rxConfig.bAppendRssi = 1;
    rf_cmd_rx.rxConfig.bAppendCorrCrc = 0;
    rf_cmd_rx.rxConfig.bAppendSrcInd = 0;
    rf_cmd_rx.rxConfig.bAppendTimestamp = 0;
    rf_cmd_rx.pRxQ = &rx_queue;
    rf_cmd_rx.pOutput = &rf_rx_output;

    // Disable frame filtering (accept everything)
    rf_cmd_rx.frameFiltOpt.frameFiltEn = 0;
    rf_cmd_rx.frameFiltOpt.autoAckEn = 0;

    // Accept all frame types
    rf_cmd_rx.frameTypes.bAcceptFt0Beacon = 1;
    rf_cmd_rx.frameTypes.bAcceptFt1Data = 1;
    rf_cmd_rx.frameTypes.bAcceptFt2Ack = 1;
    rf_cmd_rx.frameTypes.bAcceptFt3MacCmd = 1;
    rf_cmd_rx.frameTypes.bAcceptFt4Reserved = 1;
    rf_cmd_rx.frameTypes.bAcceptFt5Reserved = 1;
    rf_cmd_rx.frameTypes.bAcceptFt6Reserved = 1;
    rf_cmd_rx.frameTypes.bAcceptFt7Reserved = 1;

    // Use end trigger with timeout
    if (timeout_us > 0) {
        rf_cmd_rx.endTrigger.triggerType = TRIG_REL_START;
        // RAT runs at 4 MHz, so timeout in RAT ticks = timeout_us * 4
        rf_cmd_rx.endTime = timeout_us * 4;
    } else {
        rf_cmd_rx.endTrigger.triggerType = TRIG_NEVER;
        rf_cmd_rx.endTime = 0;
    }

    rf_status_t rc = rf_send_cmd((uint32_t)&rf_cmd_rx);
    if (rc != RF_OK) return RF_ERR_RX;

    return RF_OK;
}

void oepl_rf_rx_stop(void)
{
    // Send CMD_ABORT to stop RX
    RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_ABORT));

    // Wait for RX command to finish
    rf_wait_cmd_done(&rf_cmd_rx.status, 100000);
}

uint8_t *oepl_rf_rx_get(uint8_t *out_len, int8_t *out_rssi)
{
    if (rx_entry->status != DATA_ENTRY_FINISHED) {
        return NULL;
    }

    // Data format in general entry with lenSz=1:
    // [1-byte length] [frame data...] [RSSI byte if appended]
    uint8_t *data = &rx_entry->data;
    uint8_t pkt_len = data[0];  // Length prefix

    if (pkt_len < 2 || pkt_len > (RX_BUF_SIZE - 20)) {
        *out_len = 0;
        return NULL;
    }

    // RSSI is appended after the frame data
    // Frame data starts at data[1], length is pkt_len
    // RSSI is the last byte of the received data (after frame, before we stripped CRC)
    *out_rssi = (int8_t)data[pkt_len];  // RSSI byte appended by radio
    *out_len = pkt_len - 1;  // Subtract the RSSI byte from length

    return &data[1];  // Skip length byte, return frame data
}

void oepl_rf_rx_flush(void)
{
    rx_entry->status = DATA_ENTRY_PENDING;
}

void oepl_rf_shutdown(void)
{
    // Abort any running command
    RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_ABORT));

    // Wait briefly
    for (volatile uint32_t i = 0; i < 10000; i++) __asm volatile("nop");

    // Power down synth
    RFCSynthPowerDown();

    // Disable RF core clocks
    RFCClockDisable();

    // Power off RF core domain
    PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);

    rtt_puts("RF: Shutdown\r\n");
}

void oepl_rf_get_mac(uint8_t mac[8])
{
    // Read 8-byte IEEE MAC from FCFG1
    // FCFG1_MAC_15_4_1 contains the upper 4 bytes (MSB first in memory)
    // FCFG1_MAC_15_4_0 contains the lower 4 bytes
    uint32_t hi = FCFG1_MAC_15_4_1;
    uint32_t lo = FCFG1_MAC_15_4_0;

    // Store in big-endian order (MSB first, as OEPL expects)
    mac[0] = (hi >> 24) & 0xFF;
    mac[1] = (hi >> 16) & 0xFF;
    mac[2] = (hi >> 8) & 0xFF;
    mac[3] = hi & 0xFF;
    mac[4] = (lo >> 24) & 0xFF;
    mac[5] = (lo >> 16) & 0xFF;
    mac[6] = (lo >> 8) & 0xFF;
    mac[7] = lo & 0xFF;
}
