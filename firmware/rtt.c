// -----------------------------------------------------------------------------
//     Minimal SEGGER RTT implementation + UART TX debug output
// -----------------------------------------------------------------------------
// Compatible with J-Link RTT protocol. J-Link scans target RAM for the
// "SEGGER RTT" magic string to locate the control block.
//
// Also outputs all debug text to UART0 TX (DIO3) at 115200 baud for
// debugging without J-Link. The FTDI adapter connected for cc2538-bsl
// flashing receives this output.
// -----------------------------------------------------------------------------

#include "rtt.h"
#include "uart.h"
#include "ioc.h"
#include "prcm.h"
#include "hw_memmap.h"

// UART TX pin — DIO3 is CC2630 ROM bootloader TX (connects to FTDI RX)
#define UART_TX_PIN     3
#define UART_RX_PIN     2
#define UART_BAUD       115200
#define SYSTEM_CLK_HZ   48000000

// Ring buffer size — must be power of 2 for efficient masking
#define RTT_BUFFER_SIZE 512

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

static void uart_init(void)
{
    // SERIAL power domain should already be up (for SPI), but ensure it
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL);
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON);

    // Enable UART0 peripheral clock
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    while (!PRCMLoadGet());

    // Configure UART pins (TX only needed, but set RX too for completeness)
    IOCPinTypeUart(UART0_BASE, UART_RX_PIN, UART_TX_PIN, IOID_UNUSED, IOID_UNUSED);

    // Configure UART0: 115200, 8N1
    UARTConfigSetExpClk(UART0_BASE, SYSTEM_CLK_HZ, UART_BAUD,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);
}

static void uart_putc(char c)
{
    UARTCharPut(UART0_BASE, (uint8_t)c);
}

void rtt_init(void)
{
    // Control block is statically initialized, nothing else needed.
    // This function exists as a clear initialization point and to
    // ensure the linker doesn't optimize away _SEGGER_RTT.
    (void)_SEGGER_RTT.acID[0];

    // Initialize UART TX for debug output without J-Link
    uart_init();
}

void rtt_putc(char c)
{
    // RTT output (for J-Link)
    unsigned wr = _SEGGER_RTT.aUp[0].WrOff;
    _aUpBuffer[wr] = c;
    wr++;
    if (wr >= RTT_BUFFER_SIZE)
        wr = 0;
    // Non-blocking: if buffer is full, just drop the character.
    // (WrOff == RdOff means empty; WrOff+1 == RdOff means full)
    if (wr != _SEGGER_RTT.aUp[0].RdOff)
        _SEGGER_RTT.aUp[0].WrOff = wr;

    // UART output (for FTDI serial)
    uart_putc(c);
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
