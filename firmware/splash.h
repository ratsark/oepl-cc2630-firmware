#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>
#include <stdbool.h>

// Display branded splash screen with tag info.
// Streams 4bpp rows directly to UC8159 — no framebuffer needed.
//   mac:        8-byte MAC address (LSB-first, wire order)
//   battery_mv: battery voltage in millivolts
//   temp_c:     temperature in degrees C
//   ap_found:   true if AP was found during scan
//   channel:    IEEE 802.15.4 channel number (11-27)
void splash_display(const uint8_t *mac, uint16_t battery_mv, int8_t temp_c,
                    bool ap_found, uint8_t channel);

#endif // SPLASH_H
