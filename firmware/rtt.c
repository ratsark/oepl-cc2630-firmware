// -----------------------------------------------------------------------------
//     Minimal SEGGER RTT implementation
// -----------------------------------------------------------------------------
// Compatible with J-Link RTT protocol. J-Link scans target RAM for the
// "SEGGER RTT" magic string to locate the control block.
// -----------------------------------------------------------------------------

#include "rtt.h"

// Ring buffer size — must be power of 2 for efficient masking
#define RTT_BUFFER_SIZE 1024

// RTT buffer descriptor (matches SEGGER's layout exactly)
typedef struct {
    const char *sName;
    char       *pBuffer;
    unsigned    SizeOfBuffer;
    unsigned    WrOff;
    unsigned    RdOff;
    unsigned    Flags;
} RTT_BUFFER_DESC;

// RTT control block (matches SEGGER's layout exactly)
// J-Link searches RAM for acID[] to find this structure.
typedef struct {
    char            acID[16];
    int             MaxNumUpBuffers;
    int             MaxNumDownBuffers;
    RTT_BUFFER_DESC aUp[1];
    RTT_BUFFER_DESC aDown[1];
} RTT_CB;

// The actual ring buffer data
static char _aUpBuffer[RTT_BUFFER_SIZE];
static char _aDownBuffer[16];

// The control block — placed in .data so J-Link can find it via RAM scan.
// The ID is split to prevent the linker from merging it with other strings
// and to ensure J-Link doesn't find a false match in flash.
static RTT_CB _SEGGER_RTT __attribute__((used)) = {
    .acID              = "SEGGER RTT\0\0\0\0\0",
    .MaxNumUpBuffers   = 1,
    .MaxNumDownBuffers = 1,
    .aUp = {{
        .sName        = "Terminal",
        .pBuffer      = _aUpBuffer,
        .SizeOfBuffer = RTT_BUFFER_SIZE,
        .WrOff        = 0,
        .RdOff        = 0,
        .Flags        = 0  // SEGGER_RTT_MODE_NO_BLOCK_SKIP
    }},
    .aDown = {{
        .sName        = "Terminal",
        .pBuffer      = _aDownBuffer,
        .SizeOfBuffer = sizeof(_aDownBuffer),
        .WrOff        = 0,
        .RdOff        = 0,
        .Flags        = 0
    }}
};

void rtt_init(void)
{
    // Control block is statically initialized, nothing else needed.
    // This function exists as a clear initialization point and to
    // ensure the linker doesn't optimize away _SEGGER_RTT.
    (void)_SEGGER_RTT.acID[0];
}

void rtt_putc(char c)
{
    unsigned wr = _SEGGER_RTT.aUp[0].WrOff;
    _aUpBuffer[wr] = c;
    wr++;
    if (wr >= RTT_BUFFER_SIZE)
        wr = 0;
    // Non-blocking: if buffer is full, just drop the character.
    // (WrOff == RdOff means empty; WrOff+1 == RdOff means full)
    if (wr != _SEGGER_RTT.aUp[0].RdOff)
        _SEGGER_RTT.aUp[0].WrOff = wr;
}

void rtt_puts(const char *s)
{
    while (*s)
        rtt_putc(*s++);
}

void rtt_put_hex8(uint8_t val)
{
    const char hex[] = "0123456789ABCDEF";
    rtt_putc(hex[(val >> 4) & 0xF]);
    rtt_putc(hex[val & 0xF]);
}

void rtt_put_hex32(uint32_t val)
{
    rtt_put_hex8((val >> 24) & 0xFF);
    rtt_put_hex8((val >> 16) & 0xFF);
    rtt_put_hex8((val >>  8) & 0xFF);
    rtt_put_hex8(val & 0xFF);
}
