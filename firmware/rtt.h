// -----------------------------------------------------------------------------
//     Minimal SEGGER RTT implementation for CC2630
// -----------------------------------------------------------------------------
// J-Link scans RAM for the magic string "SEGGER RTT" to find the control
// block, then reads from the ring buffer asynchronously. No CPU halts needed.
//
// Use JLinkRTTClient or JLinkRTTViewerExe to see the output.
// -----------------------------------------------------------------------------

#ifndef RTT_H
#define RTT_H

#include <stdint.h>

void rtt_init(void);
void rtt_putc(char c);
void rtt_puts(const char *s);
void rtt_put_hex8(uint8_t val);
void rtt_put_hex32(uint32_t val);

#endif
