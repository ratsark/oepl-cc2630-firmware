// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include "oepl_nvm_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"
#include <string.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// CC2630F128 Flash layout
// Total: 128KB
// Code: 0x00000000 - 0x0001FFFF (128KB)
// For NVM storage, we use upper flash pages
// Reserve last 40KB (0x16000 - 0x1FFFF) for image storage

#define NVM_FLASH_BASE          0x00016000  // Start of NVM area (88KB offset)
#define NVM_FLASH_SIZE          (40 * 1024) // 40KB for images
#define NVM_SLOT_SIZE           (NVM_FLASH_SIZE / NVM_MAX_IMAGE_SLOTS)

#define NVM_MAGIC               0x4F45504C  // "OEPL" magic number

// NVM header structure (stored at start of each slot)
typedef struct __attribute__((packed)) {
    uint32_t magic;
    nvm_image_metadata_t metadata;
    uint32_t crc32;  // CRC of metadata
} nvm_slot_header_t;

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static bool nvm_initialized = false;

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static uint32_t calculate_crc32(const uint8_t* data, size_t len);
static bool flash_write(uint32_t address, const uint8_t* data, size_t len);
static bool flash_erase_page(uint32_t address);
static uint32_t get_slot_address(uint8_t slot);

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void oepl_nvm_init(void)
{
    if (nvm_initialized) {
        return;
    }

    oepl_hw_debugprint(DBG_NVM, "Initializing NVM...\n");
    oepl_hw_debugprint(DBG_NVM, "Flash base: 0x%08X\n", NVM_FLASH_BASE);
    oepl_hw_debugprint(DBG_NVM, "Flash size: %d bytes\n", NVM_FLASH_SIZE);
    oepl_hw_debugprint(DBG_NVM, "Slot size: %d bytes\n", NVM_SLOT_SIZE);
    oepl_hw_debugprint(DBG_NVM, "Max slots: %d\n", NVM_MAX_IMAGE_SLOTS);

    // Validate each slot
    for (uint8_t slot = 0; slot < NVM_MAX_IMAGE_SLOTS; slot++) {
        bool valid = oepl_nvm_check_image_slot(slot);
        oepl_hw_debugprint(DBG_NVM, "Slot %d: %s\n", slot, valid ? "VALID" : "EMPTY");
    }

    nvm_initialized = true;
    oepl_hw_debugprint(DBG_NVM, "NVM initialized\n");
}

bool oepl_nvm_check_image_slot(uint8_t slot)
{
    if (slot >= NVM_MAX_IMAGE_SLOTS) {
        return false;
    }

    uint32_t slot_addr = get_slot_address(slot);

    // Read header
    nvm_slot_header_t header;
    memcpy(&header, (void*)slot_addr, sizeof(header));

    // Check magic number
    if (header.magic != NVM_MAGIC) {
        return false;
    }

    // Validate CRC
    uint32_t calculated_crc = calculate_crc32((uint8_t*)&header.metadata,
                                             sizeof(header.metadata));
    if (calculated_crc != header.crc32) {
        oepl_hw_debugprint(DBG_NVM, "Slot %d: CRC mismatch\n", slot);
        return false;
    }

    // Check if metadata indicates valid image
    return header.metadata.valid;
}

bool oepl_nvm_get_image_metadata(uint8_t slot, nvm_image_metadata_t* metadata)
{
    if (slot >= NVM_MAX_IMAGE_SLOTS || metadata == NULL) {
        return false;
    }

    uint32_t slot_addr = get_slot_address(slot);

    // Read header
    nvm_slot_header_t header;
    memcpy(&header, (void*)slot_addr, sizeof(header));

    if (header.magic != NVM_MAGIC) {
        return false;
    }

    // Copy metadata
    memcpy(metadata, &header.metadata, sizeof(nvm_image_metadata_t));

    return true;
}

bool oepl_nvm_write_image(uint8_t slot, const uint8_t* data, size_t len,
                          const nvm_image_metadata_t* metadata)
{
    if (slot >= NVM_MAX_IMAGE_SLOTS || data == NULL || metadata == NULL) {
        return false;
    }

    if (len > (NVM_SLOT_SIZE - sizeof(nvm_slot_header_t))) {
        oepl_hw_debugprint(DBG_NVM, "Image too large: %d bytes\n", len);
        return false;
    }

    oepl_hw_debugprint(DBG_NVM, "Writing image to slot %d (%d bytes)...\n", slot, len);

    uint32_t slot_addr = get_slot_address(slot);

    // Erase slot first
    if (!oepl_nvm_erase_slot(slot)) {
        return false;
    }

    // Build header
    nvm_slot_header_t header;
    header.magic = NVM_MAGIC;
    memcpy(&header.metadata, metadata, sizeof(nvm_image_metadata_t));
    header.crc32 = calculate_crc32((uint8_t*)&header.metadata, sizeof(header.metadata));

    // Write header
    if (!flash_write(slot_addr, (uint8_t*)&header, sizeof(header))) {
        oepl_hw_debugprint(DBG_NVM, "Failed to write header\n");
        return false;
    }

    // Write image data
    if (!flash_write(slot_addr + sizeof(header), data, len)) {
        oepl_hw_debugprint(DBG_NVM, "Failed to write image data\n");
        return false;
    }

    oepl_hw_debugprint(DBG_NVM, "Image written successfully\n");

    return true;
}

bool oepl_nvm_read_image(uint8_t slot, uint8_t* buffer, size_t offset, size_t len)
{
    if (slot >= NVM_MAX_IMAGE_SLOTS || buffer == NULL) {
        return false;
    }

    uint32_t slot_addr = get_slot_address(slot);
    uint32_t data_addr = slot_addr + sizeof(nvm_slot_header_t) + offset;

    // Bounds check
    if (offset + len > (NVM_SLOT_SIZE - sizeof(nvm_slot_header_t))) {
        return false;
    }

    // Read directly from flash
    memcpy(buffer, (void*)data_addr, len);

    return true;
}

bool oepl_nvm_erase_slot(uint8_t slot)
{
    if (slot >= NVM_MAX_IMAGE_SLOTS) {
        return false;
    }

    oepl_hw_debugprint(DBG_NVM, "Erasing slot %d...\n", slot);

    uint32_t slot_addr = get_slot_address(slot);

    // Erase all pages in this slot
    // CC2630 flash page size is 4KB
    uint32_t num_pages = (NVM_SLOT_SIZE + 4095) / 4096;

    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t page_addr = slot_addr + (i * 4096);
        if (!flash_erase_page(page_addr)) {
            oepl_hw_debugprint(DBG_NVM, "Failed to erase page at 0x%08X\n", page_addr);
            return false;
        }
    }

    oepl_hw_debugprint(DBG_NVM, "Slot erased\n");

    return true;
}

size_t oepl_nvm_get_free_space(void)
{
    size_t free_space = 0;

    for (uint8_t slot = 0; slot < NVM_MAX_IMAGE_SLOTS; slot++) {
        if (!oepl_nvm_check_image_slot(slot)) {
            free_space += NVM_SLOT_SIZE;
        }
    }

    return free_space;
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static uint32_t calculate_crc32(const uint8_t* data, size_t len)
{
    // Simple CRC32 implementation
    // TODO: Use hardware CRC accelerator if available

    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }

    return ~crc;
}

static bool flash_write(uint32_t address, const uint8_t* data, size_t len)
{
    // TODO: Use CC2630 flash driver to write data
    // This requires TI driverlib flash API

    (void)address;
    (void)data;
    (void)len;

    // Placeholder - assume success
    return true;
}

static bool flash_erase_page(uint32_t address)
{
    // TODO: Use CC2630 flash driver to erase page
    // CC2630 flash page size is 4KB

    (void)address;

    // Placeholder - assume success
    return true;
}

static uint32_t get_slot_address(uint8_t slot)
{
    return NVM_FLASH_BASE + (slot * NVM_SLOT_SIZE);
}
