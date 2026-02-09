// -----------------------------------------------------------------------------
//                 CC2630 Customer Configuration (CCFG)
// -----------------------------------------------------------------------------
// Extracted from TG-GR6000N stock firmware - this is the EXACT CCFG
// that the bootloader expects to boot properly
//
// ⚠️  CRITICAL WARNING - DO NOT MODIFY WITHOUT UNDERSTANDING! ⚠️
//
// This CCFG structure contains critical boot configuration:
//
// Byte 48 (BL_ENABLE): 0xFF (modified from stock 0xC5)
//   - 0xC5 = backdoor pin check enabled (DIO11 LOW enters bootloader)
//   - 0xFF = backdoor pin check DISABLED (DIO11 is ignored on boot)
//   - Changed to 0xFF to allow standby wakeup — without this, DIO11
//     floats LOW during standby and ROM enters bootloader on every wakeup
//   - UART flashing still works if DIO11 is pulled LOW by cc2538-bsl
//     (the bootloader itself is still enabled, just the pin check is off)
//   - JTAG can always reflash the CCFG to re-enable if needed
//
// Byte 51 (BOOTLOADER_ENABLE): 0xC5
//   - This MUST be 0xC5 to keep the ROM bootloader functional
//   - Setting to 0x00 will PERMANENTLY DISABLE the bootloader
//
// Bytes 68-71 (IMAGE_VALID): 0x00000000
//   - Points to flash vector table start address
//   - 0x00000000 is correct - tells bootloader where our application starts
//   - Invalid address here forces permanent bootloader mode
//
// The stock CCFG works perfectly:
//   - Boots application immediately on power-up (IMAGE_VALID is set)
//   - Enters bootloader ONLY when D/L pin is pulled low (BL_ENABLE is set)
//
// LESSON LEARNED: We bricked a device by changing byte 51 from 0xC5 to 0x00.
//                 DO NOT make this mistake again!
//
// -----------------------------------------------------------------------------

#include <stdint.h>

// CCFG structure - exactly 88 bytes from stock firmware
__attribute__((section(".ccfg"), used))
const uint8_t ccfg_data[88] = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x82, 0xff, 0xfd, 0xff, 0x54, 0x00,
    0x3a, 0xff, 0xbf, 0xf3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x0b, 0xfe, 0xc5, 0xff, 0xff, 0xff, 0xff, 0xc5, 0xff, 0xff, 0xff,  // Byte 48: 0xFF (backdoor disabled)
    0xc5, 0xc5, 0xc5, 0xff, 0xc5, 0xc5, 0xc5, 0xff, 0x00, 0x00, 0x00, 0x00,  // Bytes 68-71: IMAGE_VALID=0x00000000
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
};
