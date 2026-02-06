#ifndef OEPL_COMPRESSION_H
#define OEPL_COMPRESSION_H

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// Compression types
typedef enum {
    COMPRESSION_NONE = 0,
    COMPRESSION_ZLIB = 1,
    COMPRESSION_G5 = 2  // OEPL custom compression
} compression_type_t;

// Decompression callback for line-by-line output
typedef void (*decompress_line_callback_t)(const uint8_t* line_data, size_t len);

// -----------------------------------------------------------------------------
//                          Public Function Declarations
// -----------------------------------------------------------------------------

/**
 * Initialize compression library
 */
void oepl_compression_init(void);

/**
 * Decompress data
 * @param compressed Pointer to compressed data
 * @param compressed_size Size of compressed data
 * @param decompressed Output buffer for decompressed data
 * @param decompressed_size Size of output buffer
 * @return Actual decompressed size, or 0 on error
 */
size_t oepl_decompress(const uint8_t* compressed,
                       size_t compressed_size,
                       uint8_t* decompressed,
                       size_t decompressed_size);

/**
 * Decompress data line-by-line (for large images)
 * Calls callback for each decompressed line
 * @param compressed Pointer to compressed data
 * @param compressed_size Size of compressed data
 * @param line_width Width of each line in bytes
 * @param callback Function to call for each line
 * @return true on success, false on error
 */
bool oepl_decompress_streaming(const uint8_t* compressed,
                               size_t compressed_size,
                               size_t line_width,
                               decompress_line_callback_t callback);

#endif // OEPL_COMPRESSION_H
