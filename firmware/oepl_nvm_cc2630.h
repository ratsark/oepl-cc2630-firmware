#ifndef OEPL_NVM_CC2630_H
#define OEPL_NVM_CC2630_H

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

#define NVM_MAX_IMAGE_SLOTS     3
#define NVM_IMAGE_SLOT_SIZE     (40 * 1024)  // 40KB per slot

// Image metadata
typedef struct {
    bool valid;
    uint32_t image_size;
    uint32_t compressed_size;
    uint8_t md5[16];
    uint32_t timestamp;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
    uint8_t compression_type;  // 0=none, 1=zlib
} nvm_image_metadata_t;

// -----------------------------------------------------------------------------
//                          Public Function Declarations
// -----------------------------------------------------------------------------

/**
 * Initialize NVM
 */
void oepl_nvm_init(void);

/**
 * Check if image slot has valid image
 */
bool oepl_nvm_check_image_slot(uint8_t slot);

/**
 * Get image metadata from slot
 */
bool oepl_nvm_get_image_metadata(uint8_t slot, nvm_image_metadata_t* metadata);

/**
 * Write image data to slot
 */
bool oepl_nvm_write_image(uint8_t slot, const uint8_t* data, size_t len,
                          const nvm_image_metadata_t* metadata);

/**
 * Read image data from slot
 */
bool oepl_nvm_read_image(uint8_t slot, uint8_t* buffer, size_t offset, size_t len);

/**
 * Erase image slot
 */
bool oepl_nvm_erase_slot(uint8_t slot);

/**
 * Get total free space in NVM
 */
size_t oepl_nvm_get_free_space(void);

#endif // OEPL_NVM_CC2630_H
