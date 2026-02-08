// -----------------------------------------------------------------------------
//  CC2630 RF Core Driver for OEPL
//  Bare-metal IEEE 802.15.4 radio using TI driverlib
//  Init sequence follows Contiki-NG ieee-mode.c (proven working)
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
#include "hw_rfc_pwr.h"
#include "hw_rfc_dbell.h"
#include "hw_memmap.h"
#include "osc.h"

// FCFG1 IEEE MAC address registers
#define FCFG1_MAC_15_4_0    (*(volatile uint32_t *)0x500012F0)
#define FCFG1_MAC_15_4_1    (*(volatile uint32_t *)0x500012F4)

// OEPL channel map: 6 channels -> IEEE 802.15.4 channels
const uint8_t oepl_channel_map[OEPL_NUM_CHANNELS] = {11, 15, 20, 25, 26, 27};

// IEEE 802.15.4 overrides from Contiki-NG smartrf-settings.c
static uint32_t rf_overrides[] = {
    0x00354038,   // Synth: Set RTRIM (POTAILRESTRIM) to 5
    0x4001402D,   // Synth: Correct CKVD latency setting (address)
    0x00608402,   // Synth: Correct CKVD latency setting (value)
    0x000784A3,   // Synth: Set FREF = 3.43 MHz (24 MHz / 7)
    0xA47E0583,   // Synth: Set loop bandwidth after lock to 80 kHz (K2)
    0xEAE00603,   // Synth: Set loop bandwidth after lock to 80 kHz (K3, LSB)
    0x00010623,   // Synth: Set loop bandwidth after lock to 80 kHz (K3, MSB)
    0x002B50DC,   // Adjust AGC DC filter
    0x05000243,   // Increase synth programming timeout
    0x002082C3,   // Increase synth programming timeout
    END_OVERRIDE
};

// CPE interrupt mask (matches Contiki-NG)
#define RF_CPE_IRQ_BASE  (IRQ_RX_ENTRY_DONE | IRQ_INTERNAL_ERROR | IRQ_RX_BUF_FULL)

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
    // Status field bits [11:10]:
    //   00 = running (IDLE/PENDING/ACTIVE)
    //   01 = done normally (DONE_OK=0x0400, IEEE_DONE_OK=0x2400, etc.)
    //   10 = error
    // Use masked comparison so both generic and IEEE-specific statuses work.
    for (volatile uint32_t i = 0; i < timeout_loops; i++) {
        uint16_t s = *status_ptr;
        if ((s & 0x0C00) == 0x0400) {
            // Done normally (includes DONE_OK, IEEE_DONE_OK, IEEE_DONE_ACK, etc.)
            return RF_OK;
        }
        if ((s & 0x0C00) == 0x0800) {
            // Error
            rtt_puts("RF cmd err=0x");
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
    // === Contiki-NG proven init sequence for CC26x0 IEEE 802.15.4 ===

    // 1. Switch to XOSC_HF - RF synth needs crystal oscillator as reference
    OSCHF_TurnOnXosc();
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        if (OSCHF_AttemptToSwitchToXosc()) break;
    }
    if (OSCClockSourceGet(OSC_SRC_CLK_HF) != OSC_XOSC_HF) {
        rtt_puts("RF: XOSC_HF FAIL\r\n");
        return RF_ERR_POWER;
    }

    // 2. Power off RF core first (RFCMODESEL must be set while powered off)
    PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);
    for (volatile uint32_t i = 0; i < 100000; i++) {
        if (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) == PRCM_DOMAIN_POWER_OFF)
            break;
    }

    // 3. Set RFCMODESEL = MODE2 for IEEE 802.15.4
    HWREG(PRCM_BASE + 0x1D0) = 0x02;

    // 4. Power on RF core
    PRCMPowerDomainOn(PRCM_DOMAIN_RFCORE);
    for (volatile uint32_t i = 0; i < 500000; i++) {
        if (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) == PRCM_DOMAIN_POWER_ON)
            break;
    }
    if (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_ON) {
        rtt_puts("RF: Power FAIL\r\n");
        return RF_ERR_POWER;
    }

    // 5. Enable RF core clocks (Contiki-NG: RF_CORE_CLOCKS_MASK)
    PRCMDomainEnable(PRCM_DOMAIN_RFCORE);
    PRCMLoadSet();
    while (!PRCMLoadGet()) {}
    // Enable all RF core submodule clocks
    HWREG(RFC_PWR_NONBUF_BASE + RFC_PWR_O_PWMCLKEN) = 0x7FF;

    // 6. Clear interrupts, wait for CPE boot
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIFG) = 0x0;
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIEN) = 0x0;
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFACKIFG) = 0;
    rf_wait_boot();
    rtt_puts("RF: boot OK\r\n");

    // 7. Enable additional clocks via RF_CMD0 (Contiki-NG rf_core_power_up)
    //    0x0607 with MDMRAM|RFERAM enables modem RAM + RFE RAM clocks
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFACKIFG) = 0;
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDR) =
        CMDR_DIR_CMD_2BYTE(0x0607,
            RFC_PWR_PWMCLKEN_MDMRAM_M | RFC_PWR_PWMCLKEN_RFERAM_M);
    // Wait for ACK
    for (volatile uint32_t i = 0; i < 100000; i++) {
        if (HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFACKIFG)) break;
    }

    // 8. Verify RF core is alive
    uint32_t cmdsta = RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_PING));
    if ((cmdsta & 0xFF) != CMDSTA_Done) {
        rtt_puts("RF: PING FAIL\r\n");
        return RF_ERR_BOOT;
    }

    // 9. Start Radio Timer (RAT) — direct command, needed for FG scheduling
    rtt_puts("RF: RAT...");
    cmdsta = RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_START_RAT));
    rtt_puts("sta=0x");
    rtt_put_hex8(cmdsta & 0xFF);
    rtt_puts("\r\n");

    // 10. Configure CPE interrupt enables (Contiki-NG rf_core_setup_interrupts)
    //     Route ERROR_IRQ to CPE1
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEISL) =
        IRQ_INTERNAL_ERROR | IRQ_RX_BUF_FULL;
    //     Enable base interrupts
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIEN) = RF_CPE_IRQ_BASE;
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIFG) = 0x0;

    // 11. CMD_RADIO_SETUP (IEEE 802.15.4 mode, no patches needed — ROM has IEEE)
    //     NOTE: No RFCAdi3VcoLdoVoltageMode — that's for prop-mode only
    memset(&rf_cmd_setup, 0, sizeof(rf_cmd_setup));
    rf_cmd_setup.commandNo = CMD_RADIO_SETUP;
    rf_cmd_setup.status = IDLE;
    rf_cmd_setup.pNextOp = NULL;
    rf_cmd_setup.startTime = 0;
    rf_cmd_setup.startTrigger.triggerType = TRIG_NOW;
    rf_cmd_setup.condition.rule = COND_NEVER;
    rf_cmd_setup.mode = 0x01;                    // IEEE 802.15.4
    rf_cmd_setup.config.frontEndMode = 0x00;     // Differential
    rf_cmd_setup.config.biasMode = 0x00;         // Internal bias
    rf_cmd_setup.config.analogCfgMode = 0x00;    // Write analog config
    rf_cmd_setup.config.bNoFsPowerUp = 0;        // Power up FS
    rf_cmd_setup.txPower = 0x9330;               // 5 dBm
    rf_cmd_setup.pRegOverride = rf_overrides;

    rtt_puts("RF: SETUP...");
    if (rf_send_cmd((uint32_t)&rf_cmd_setup) != RF_OK) return RF_ERR_SETUP;
    if (rf_wait_cmd_done(&rf_cmd_setup.status, 1000000) != RF_OK) {
        rtt_puts("FAIL\r\n");
        return RF_ERR_SETUP;
    }
    rtt_puts("OK\r\n");

    // 12. Set up RX data queue (single entry, circular)
    memset(rx_buf, 0, sizeof(rx_buf));
    rx_entry->pNextEntry = (uint8_t *)rx_entry;
    rx_entry->status = DATA_ENTRY_PENDING;
    rx_entry->config.type = DATA_ENTRY_TYPE_GEN;
    rx_entry->config.lenSz = 1;
    rx_entry->length = RX_BUF_SIZE - sizeof(rfc_dataEntryGeneral_t) + 1;

    rx_queue.pCurrEntry = (uint8_t *)rx_entry;
    rx_queue.pLastEntry = NULL;

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
        rtt_puts("RF: FS ch=");
        rtt_put_hex8(ieee_ch);
        rtt_puts(" FAIL\r\n");
        return RF_ERR_FS;
    }
    return RF_OK;
}

rf_status_t oepl_rf_tx(const uint8_t *payload, uint8_t len)
{
    if (len > sizeof(tx_buf)) return RF_ERR_TX;

    // Wait for CMD_IEEE_RX to be ACTIVE (required background for FG TX)
    for (volatile uint32_t i = 0; i < 200000; i++) {
        if (*(volatile uint16_t *)&rf_cmd_rx.status == ACTIVE) break;
    }
    if (*(volatile uint16_t *)&rf_cmd_rx.status != ACTIVE) {
        rtt_puts("RF: RX not active for TX\r\n");
        return RF_ERR_TX;
    }

    memcpy(tx_buf, payload, len);

    // Enable IRQ_LAST_FG_COMMAND_DONE for TX (Contiki-NG rf_core_cmd_done_en)
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIFG) = 0;
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIEN) =
        RF_CPE_IRQ_BASE | IRQ_LAST_FG_COMMAND_DONE;

    memset(&rf_cmd_tx, 0, sizeof(rf_cmd_tx));
    rf_cmd_tx.commandNo = CMD_IEEE_TX;
    rf_cmd_tx.status = IDLE;
    rf_cmd_tx.pNextOp = NULL;
    rf_cmd_tx.startTime = 0;
    rf_cmd_tx.startTrigger.triggerType = TRIG_NOW;
    rf_cmd_tx.condition.rule = COND_NEVER;
    rf_cmd_tx.txOpt.bIncludePhyHdr = 0;
    rf_cmd_tx.txOpt.bIncludeCrc = 0;
    rf_cmd_tx.payloadLen = len;
    rf_cmd_tx.pPayload = tx_buf;

    uint32_t cmdsta = RFCDoorbellSendTo((uint32_t)&rf_cmd_tx);

    if ((cmdsta & 0xFF) != CMDSTA_Done) {
        rtt_puts("RF: TX rejected sta=0x");
        rtt_put_hex8(cmdsta & 0xFF);
        rtt_puts("\r\n");
        // Restore base IRQ mask
        HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIEN) = RF_CPE_IRQ_BASE;
        return RF_ERR_TX;
    }

    // Wait for TX completion — poll both status field (volatile!) and RFCPEIFG
    rf_status_t result = RF_ERR_TIMEOUT;
    for (volatile uint32_t i = 0; i < 500000; i++) {
        uint16_t s = *(volatile uint16_t *)&rf_cmd_tx.status;
        if ((s & 0x0C00) == 0x0400) {
            result = RF_OK;
            break;
        }
        if ((s & 0x0C00) == 0x0800) {
            result = RF_ERR_TX;
            break;
        }
        // Also check interrupt flags as backup
        uint32_t ifg = HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG);
        if (ifg & IRQ_LAST_FG_COMMAND_DONE) {
            result = RF_OK;
            break;
        }
    }

    // Restore base IRQ mask (Contiki-NG rf_core_cmd_done_dis)
    HWREG(RFC_DBELL_NONBUF_BASE + RFC_DBELL_O_RFCPEIEN) = RF_CPE_IRQ_BASE;

    return result;
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

    // CCA configuration omitted for now — keep defaults (all disabled)

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
    if (rc != RF_OK) {
        rtt_puts("RF: RX start FAIL\r\n");
        return RF_ERR_RX;
    }

    // Wait for RX to become ACTIVE (may take many iterations after RAT start)
    for (volatile uint32_t i = 0; i < 500000; i++) {
        if (*(volatile uint16_t *)&rf_cmd_rx.status == ACTIVE) break;
    }
    volatile uint16_t s = *(volatile uint16_t *)&rf_cmd_rx.status;
    if (s != ACTIVE) {
        rtt_puts("RF: RX s=0x");
        rtt_put_hex8((s >> 8) & 0xFF);
        rtt_put_hex8(s & 0xFF);
        rtt_puts(" (not ACTIVE)\r\n");
    }

    return RF_OK;
}

uint16_t oepl_rf_rx_status(void)
{
    return *(volatile uint16_t *)&rf_cmd_rx.status;
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
    if (*(volatile uint8_t *)&rx_entry->status != DATA_ENTRY_FINISHED) {
        return NULL;
    }

    uint8_t *data = &rx_entry->data;
    uint8_t pkt_len = data[0];

    if (pkt_len < 2 || pkt_len > (RX_BUF_SIZE - 20)) {
        *out_len = 0;
        return NULL;
    }

    *out_rssi = (int8_t)data[pkt_len];
    *out_len = pkt_len - 1;

    return &data[1];
}

void oepl_rf_rx_flush(void)
{
    *(volatile uint8_t *)&rx_entry->status = DATA_ENTRY_PENDING;
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
    // FCFG1_MAC_15_4_1 contains the upper 4 bytes, MAC_15_4_0 the lower
    uint32_t hi = FCFG1_MAC_15_4_1;
    uint32_t lo = FCFG1_MAC_15_4_0;

    // IEEE 802.15.4 extended addresses are transmitted/stored LSB first.
    // FCFG1 stores: hi = MSB..., lo = ...LSB
    // So MAC value (MSB first) = hi:lo = 00:12:4B:00:18:18:80:B0
    // In IEEE 802.15.4 frame format (LSB first): B0:80:18:18:00:4B:12:00
    mac[0] = lo & 0xFF;         // LSB
    mac[1] = (lo >> 8) & 0xFF;
    mac[2] = (lo >> 16) & 0xFF;
    mac[3] = (lo >> 24) & 0xFF;
    mac[4] = hi & 0xFF;
    mac[5] = (hi >> 8) & 0xFF;
    mac[6] = (hi >> 16) & 0xFF;
    mac[7] = (hi >> 24) & 0xFF;  // MSB
}
