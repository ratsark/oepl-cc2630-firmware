// -----------------------------------------------------------------------------
//  OTA Firmware Update for CC2630 OEPL Tag
//
//  Downloads firmware blocks to staging flash (sectors 16-30), then copies
//  staging to active area (sectors 0-15) using a RAM-resident function.
//
//  Flash layout (CC2630F128 = 128KB):
//    0x00000 - 0x0FFFF:  Active firmware (64KB max, sectors 0-15)
//    0x10000 - 0x1EFFF:  OTA staging area (60KB, sectors 16-30)
//    0x1F000 - 0x1FFFF:  CCFG sector (never touched)
// -----------------------------------------------------------------------------

#include "oepl_ota_cc2630.h"
#include "oepl_radio_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "rtt.h"
#include <string.h>

// ROM API for flash operations (from rom.h)
// ROM_API_TABLE is at 0x10000180
// ROM_API_FLASH_TABLE = ROM_API_TABLE[10]
// FlashSectorErase = ROM_API_FLASH_TABLE[5]
// FlashProgram     = ROM_API_FLASH_TABLE[6]
#define ROM_API_TABLE       ((uint32_t *)0x10000180)
#define ROM_API_FLASH_TABLE ((uint32_t *)(ROM_API_TABLE[10]))

// Flash status codes
#define FAPI_STATUS_SUCCESS 0x00000000

// ROM Hard-API table (HAPI) for ResetDevice
// HAPI table at 0x10000048, ResetDevice is at offset 6 (7th entry)
#define ROM_HAPI_TABLE_ADDR 0x10000048

// Flash function pointer types
typedef uint32_t (*flash_erase_fn_t)(uint32_t addr);
typedef uint32_t (*flash_program_fn_t)(uint8_t *buf, uint32_t addr, uint32_t len);

// --- Flash helpers for staging area writes ---
// These run from flash, which is safe because they only touch staging sectors
// (0x10000+) which don't overlap with the executing code sectors.

static uint32_t staging_erase_sector(uint32_t addr)
{
    flash_erase_fn_t erase = (flash_erase_fn_t)ROM_API_FLASH_TABLE[5];
    return erase(addr);
}

static uint32_t staging_program(uint8_t *data, uint32_t addr, uint32_t len)
{
    flash_program_fn_t program = (flash_program_fn_t)ROM_API_FLASH_TABLE[6];
    return program(data, addr, len);
}

// Verify flash contents match RAM buffer
static bool staging_verify(uint32_t addr, const uint8_t *data, uint32_t len)
{
    const uint8_t *flash = (const uint8_t *)addr;
    for (uint32_t i = 0; i < len; i++) {
        if (flash[i] != data[i]) return false;
    }
    return true;
}

// --- RAM-resident apply function ---
// This function runs entirely from SRAM. It erases active firmware sectors
// and programs them from staging, then triggers a system reset.
// It must NOT call any flash-resident functions, use string literals,
// or access any flash-based data.
//
// It reuses bw_buf (4100 bytes in .bss / SRAM) as a copy buffer.

__attribute__((section(".ramfunc"), noinline, long_call))
static void apply_ota(uint32_t staging_addr, uint32_t fw_size, uint8_t *ram_buf)
{
    // Disable all interrupts — we're about to erase our code
    __asm volatile("cpsid i");

    // Resolve ROM flash API function pointers
    // ROM_API_TABLE is in ROM (0x10000180), not flash — safe to read
    uint32_t *rom_table = (uint32_t *)0x10000180;
    uint32_t *flash_table = (uint32_t *)(rom_table[10]);
    flash_erase_fn_t rom_erase = (flash_erase_fn_t)flash_table[5];
    flash_program_fn_t rom_program = (flash_program_fn_t)flash_table[6];

    // Calculate number of active sectors to overwrite
    uint32_t num_sectors = (fw_size + OTA_SECTOR_SIZE - 1) / OTA_SECTOR_SIZE;

    // Copy each sector: staging → ram_buf → active
    for (uint32_t s = 0; s < num_sectors; s++) {
        uint32_t src_addr = staging_addr + s * OTA_SECTOR_SIZE;
        uint32_t dst_addr = s * OTA_SECTOR_SIZE;
        uint32_t chunk = fw_size - s * OTA_SECTOR_SIZE;
        if (chunk > OTA_SECTOR_SIZE) chunk = OTA_SECTOR_SIZE;

        // 1. Copy staging sector to RAM buffer
        //    (staging flash is still readable because we only erase active sectors)
        const uint8_t *src = (const uint8_t *)src_addr;
        for (uint32_t i = 0; i < chunk; i++) {
            ram_buf[i] = src[i];
        }
        // Pad remainder with 0xFF (erased state)
        for (uint32_t i = chunk; i < OTA_SECTOR_SIZE; i++) {
            ram_buf[i] = 0xFF;
        }

        // 2. Erase active sector
        rom_erase(dst_addr);

        // 3. Program active sector from RAM buffer
        rom_program(ram_buf, dst_addr, OTA_SECTOR_SIZE);
    }

    // Trigger system reset via ROM Hard-API ResetDevice function
    // HAPI table at 0x10000048, ResetDevice is entry 6 (offset 24 bytes)
    typedef void (*reset_fn_t)(void);
    uint32_t *hapi_table = (uint32_t *)0x10000048;
    reset_fn_t rom_reset = (reset_fn_t)hapi_table[6];
    rom_reset();

    // Should not reach here
    while (1) { __asm volatile("nop"); }
}

// --- Strict block download for OTA ---
// Requires ALL parts (no partial acceptance like image downloads).
// Retries up to 20 times. Returns true only if all parts received.
static bool ota_download_block(uint8_t block_id, struct AvailDataInfo *info,
                               uint8_t *buf, uint16_t *out_size)
{
    rtt_puts("B");
    rtt_put_hex8(block_id);

    uint8_t parts_rcvd[BLOCK_REQ_PARTS_BYTES];
    memset(parts_rcvd, 0, sizeof(parts_rcvd));
    memset(buf, 0xFF, BLOCK_XFER_BUFFER_SIZE);  // 0xFF = erased state (not 0x00)

    for (uint8_t attempt = 0; attempt < 20; attempt++) {
        if (attempt > 0) {
            rtt_puts("R");
            oepl_hw_delay_ms(500);
        }
        uint8_t got = oepl_radio_request_block(block_id, info->dataVer, info->dataType,
                                                buf, parts_rcvd);
        if (got >= BLOCK_MAX_PARTS) {
            rtt_puts("+");
            *out_size = BLOCK_XFER_BUFFER_SIZE;
            return true;
        }
        // OTA: NO partial acceptance — require ALL parts
    }
    rtt_puts("!");
    return false;
}

// Verify BlockData checksum: simple sum of all data bytes
static bool verify_block_checksum(const uint8_t *buf, uint32_t data_len)
{
    const struct BlockData *bd = (const struct BlockData *)buf;
    uint16_t expected = bd->checksum;
    uint16_t actual = 0;
    for (uint32_t i = 0; i < data_len; i++) {
        actual += buf[BLOCK_HEADER_SIZE + i];
    }
    return (actual == expected);
}

// --- Main OTA orchestrator ---

void oepl_ota_download_and_apply(struct AvailDataInfo *info)
{
    uint32_t fw_size = info->dataSize;

    rtt_puts("OTA: size=");
    rtt_put_hex32(fw_size);
    rtt_puts("\r\n");

    // Sanity checks
    if (fw_size == 0) {
        rtt_puts("OTA: empty\r\n");
        return;
    }
    if (fw_size > OTA_STAGING_SIZE) {
        rtt_puts("OTA: too large\r\n");
        return;
    }

    // Calculate number of blocks needed
    uint32_t num_blocks = (fw_size + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE;
    rtt_puts("OTA: blocks=");
    rtt_put_hex8((uint8_t)num_blocks);
    rtt_puts("\r\n");

    // Download each block to staging flash
    for (uint32_t block_id = 0; block_id < num_blocks; block_id++) {
        uint16_t block_size;

        // Strict download: requires ALL 42 parts
        if (!ota_download_block((uint8_t)block_id, info, bw_buf, &block_size)) {
            rtt_puts("\r\nOTA: DL fail b=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return;  // Abort — AP will retry next checkin
        }

        // Verify block checksum from BlockData header
        uint32_t remaining = fw_size - block_id * BLOCK_DATA_SIZE;
        uint32_t data_len = (remaining > BLOCK_DATA_SIZE) ? BLOCK_DATA_SIZE : remaining;

        if (!verify_block_checksum(bw_buf, data_len)) {
            rtt_puts("\r\nOTA: checksum fail b=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return;
        }
        rtt_puts("C");

        // Calculate staging flash address
        uint32_t staging_addr = OTA_STAGING_ADDR + block_id * OTA_SECTOR_SIZE;
        uint32_t data_offset = BLOCK_HEADER_SIZE;  // Skip BlockData header

        // Erase staging sector
        rtt_puts("E");
        uint32_t rc = staging_erase_sector(staging_addr);
        if (rc != FAPI_STATUS_SUCCESS) {
            rtt_puts("!\r\nOTA: erase fail s=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return;
        }

        // Program staging sector
        rtt_puts("P");
        rc = staging_program(&bw_buf[data_offset], staging_addr, data_len);
        if (rc != FAPI_STATUS_SUCCESS) {
            rtt_puts("!\r\nOTA: prog fail s=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return;
        }

        // Verify flash write
        rtt_puts("V");
        if (!staging_verify(staging_addr, &bw_buf[data_offset], data_len)) {
            rtt_puts("!\r\nOTA: verify fail s=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return;
        }

        rtt_puts("+ ");
    }

    // Final integrity check: verify vector table in staging
    // Valid ARM Cortex-M firmware must have SP in SRAM range and Reset vector in flash
    const uint32_t *staging_vectors = (const uint32_t *)OTA_STAGING_ADDR;
    uint32_t sp_val = staging_vectors[0];
    uint32_t reset_val = staging_vectors[1];
    if (sp_val < 0x20000000 || sp_val > 0x20005000 ||
        reset_val < 0x00000001 || reset_val > 0x00020000) {
        rtt_puts("\r\nOTA: bad vector table! SP=");
        rtt_put_hex32(sp_val);
        rtt_puts(" RST=");
        rtt_put_hex32(reset_val);
        rtt_puts("\r\n");
        return;
    }

    rtt_puts("\r\nOTA: all blocks OK, vectors valid\r\n");

    // Send XferComplete to AP before applying
    oepl_radio_send_xfer_complete();
    rtt_puts("OTA: XferComplete\r\n");

    // Apply OTA: copy staging to active area and reboot
    // This function does NOT return on success
    rtt_puts("OTA: APPLYING...\r\n");
    apply_ota(OTA_STAGING_ADDR, fw_size, bw_buf);

    // Should not reach here — apply_ota resets the CPU
    rtt_puts("OTA: apply returned?!\r\n");
}
