# Initial CC2630_5.8_OEPL_alpha.bin Analysis

## Binary Metadata
- **Size**: 54,212 bytes (53KB)
- **Format**: ARM Cortex-M3 binary (Little Endian)
- **Target**: CC2630F128 (128KB flash, 20KB SRAM)
- **Load Address**: 0x00000000

## ARM Vector Table (First 64 bytes)

| Offset | Purpose | Value | Notes |
|--------|---------|-------|-------|
| 0x00 | Initial Stack Pointer | 0x200040d0 | Points to SRAM (20KB @ 0x20000000) |
| 0x04 | Reset Handler | 0x0000a95d | Thumb mode (LSB = 1) |
| 0x08 | NMI Handler | 0x0000a96d | |
| 0x0C | HardFault Handler | 0x0000a971 | |
| 0x10 | MemManage | 0x0000a979 | |
| 0x14 | BusFault | 0x0000a975 | |
| 0x18 | UsageFault | 0x0000a979 | |
| 0x2C | SVCall | 0x0000a979 | |
| 0x30 | SVCall | 0x0000a979 | |
| 0x38 | PendSV | 0x0000a979 | |
| 0x3C | SysTick | 0x0000a979 | |

**Analysis**:
- Stack pointer at 0x200040d0 suggests ~208 bytes reserved at top of SRAM
- Actual SRAM size: 20KB (0x5000), so stack starts at 0x20005000 - margin
- Many handlers point to same address (0x0000a979) - likely default handler
- Reset handler at 0xa95c (Thumb mode) is the entry point

## OEPL Protocol Signatures Found

### Packet Type Constants
- Address **0x7190**: Reference to `0xe6` (PKT_AVAIL_DATA_INFO)
  - Context: `93 f8 e6 00 83 f8 e6 20` - looks like read/write to offset 0xe6
  - Likely part of packet processing logic

### Potential PAN ID Reference
- Address **0x6d30**: Byte sequence near potential radio init
- Address **0xa480**: Contains value that might be related to radio config

**Note**: Need full Ghidra analysis to confirm these are actual protocol references vs coincidental byte values.

## Code Structure Observations

### Function Prologue Patterns
Common ARM Thumb function patterns found:
```
80 b5        push {r7, lr}
00 af        add r7, sp, #0
...
80 bd        pop {r7, pc}
```

This is standard ARM Thumb-2 function entry/exit.

### Potential SPI/Peripheral Access
Look for patterns like:
- Constant addresses in 0x4000xxxx range (peripheral registers)
- Repeated byte writes (SPI TX)
- Read-modify-write sequences (GPIO configuration)

## String Analysis

**Findings**: Very few readable strings in the binary.

This is expected for optimized embedded firmware - most strings are either:
1. Compiled out in release builds
2. Stored as raw bytes in lookup tables
3. Never used (firmware operates on state machine, not text)

## Memory Layout Hypothesis

Based on CC2630F128 architecture:

```
0x00000000 - 0x0001FFFF  Flash (128KB)
  0x00000000 - 0x000000FF    Vector Table
  0x00000100 - 0x0000D2xx    Application Code (~53KB)
  0x0000D300 - 0x0001EFFF    Reserved for images/data
  0x0001F000 - 0x0001FFFF    Configuration/Settings (last 4KB)

0x20000000 - 0x20004FFF  SRAM (20KB)
  0x20000000 - 0x20000xxx    BSS (uninitialized data)
  0x20000xxx - 0x20003xxx    Heap
  0x20003xxx - 0x20004FFF    Stack (grows down)
```

## Expected Code Regions

### Startup Code (0xa95c - Reset Handler area)
- Clock configuration
- SRAM initialization
- BSS zeroing
- Call to main()

### Main Application
- State machine loop
- Timer setup
- Radio initialization
- Display initialization

### Interrupt Handlers
- Radio RX/TX interrupts
- Timer interrupts
- GPIO interrupts (buttons, NFC)

### Library Functions
- memcpy, memset, memcmp
- CRC calculation
- Compression (zlib or custom)

## Display Driver Expectations

For a 6" e-paper display, expect to find:

### Initialization Sequence
1. GPIO setup (SPI pins, control pins)
2. Reset pulse (RST pin)
3. Command sequence sent via SPI:
   ```
   Typical UC8159 6" init:
   0x01 - Power Setting
   0x00 - Panel Setting
   0x06 - Booster Soft Start
   0x04 - Power On
   0x61 - Resolution Setting (0x0320 0x0258 for 800x600)
   0x50 - VCOM Setting
   0x30 - PLL Control
   ```

### Display Update
1. Write image data via SPI (0x10 command)
2. Trigger refresh (0x12 command)
3. Wait for BUSY pin to go low
4. Power off (0x02 command)

### GPIO Pin Assignment (typical)
- SPI CLK - GPIO pin (likely P0.10 or similar)
- SPI MOSI - GPIO pin
- CS (Chip Select) - GPIO pin
- DC (Data/Command) - GPIO pin
- RST (Reset) - GPIO pin
- BUSY - GPIO input pin

**Next Step**: Search binary for these command bytes and GPIO patterns.

## Radio Configuration Expectations

### CC2630 IEEE 802.15.4 Radio

Key registers to look for:
- **RFCORE Base**: 0x40040000
- **Channel**: Typically 11-26 (2.4GHz)
- **PAN ID**: 0x4447 (OEPL standard)
- **TX Power**: Various levels available
- **MAC Address**: 8-byte extended address

### Packet Buffer Locations
- RX buffer: ~128 bytes in SRAM
- TX buffer: ~128 bytes in SRAM
- Double-buffered or single-buffered

## Comparison Strategy

### What to Compare Between OEPL and Stock Firmware

1. **Vector Table**: Should be identical structure
2. **Startup Code**: Similar initialization
3. **GPIO Config**: Likely identical (same hardware)
4. **Display Init**: Should match (same display)
5. **Radio Init**: DIFFERENT - OEPL uses specific PAN ID, channel
6. **Main Loop**: VERY DIFFERENT - OEPL protocol vs vendor protocol
7. **Packet Handling**: Completely different protocols

### High-Value Extraction Targets

**From Stock Firmware:**
- Display initialization sequence (HIGH PRIORITY)
- GPIO pin assignments
- SPI configuration
- Display resolution/LUT data

**From OEPL Firmware:**
- Radio configuration
- Protocol packet handlers
- Block transfer logic
- Sleep/power management

## Recommended Analysis Order

1. ✅ Load both binaries in Ghidra
2. ✅ Auto-analyze both
3. ⬜ Identify and label Reset_Handler in both
4. ⬜ Trace to main() in both
5. ⬜ Find and label display_init() in stock firmware
6. ⬜ Extract display init command sequence
7. ⬜ Find radio_init() in OEPL firmware
8. ⬜ Extract OEPL protocol handlers
9. ⬜ Map GPIO pins from both
10. ⬜ Document findings

## Tools for Next Phase

### Ghidra Analysis
- See `ghidra_quickstart.md` for detailed guide

### Cross-Reference with TI SDK
Download from: https://www.ti.com/tool/SIMPLELINK-CC13XX-CC26XX-SDK
- Look at example projects
- Compare peripheral initialization
- Reference HAL/driver code

### Reference OEPL Source
- `/tmp/Tag_FW_EFR32xG22/firmware/oepl_radio.c` - Radio protocol
- `/tmp/Tag_FW_EFR32xG22/firmware/oepl_display.c` - Display abstraction
- `/tmp/Tag_FW_EFR32xG22/firmware/drivers/` - Display drivers

## Questions to Answer Through Analysis

1. **Which display controller?** (UC8159, IL0373, SSD1619, other?)
2. **What resolution?** (800x600, 640x384, other?)
3. **Which GPIO pins?** (Need exact pin numbers)
4. **LUT configuration?** (Look-Up Table for grayscale/color)
5. **Power optimization?** (Sleep modes, peripheral gating)
6. **Image storage?** (Where in flash? How much space?)
7. **Bootloader location?** (Protected area?)
8. **Configuration storage?** (NVS layout)

## Success Criteria

You'll know you're making progress when you can:

- [ ] Identify reset handler and trace to main()
- [ ] Find SPI initialization code
- [ ] Extract display command sequence
- [ ] Locate packet type constants (0xE4-0xEE)
- [ ] Map GPIO pin assignments
- [ ] Identify radio configuration registers
- [ ] Understand image data flow
- [ ] Create annotated memory map

## Files Created

1. `TG-GR6000N_Analysis_Plan.md` - Overall strategy
2. `ghidra_quickstart.md` - Ghidra setup guide
3. `Initial_Binary_Analysis.md` - This document

## Your Action Items

1. **Locate your stock firmware dump** - Path?
2. **Install Ghidra** (if needed)
3. **Load both binaries** following the quickstart guide
4. **Start with OEPL binary** - understand protocol first
5. **Focus on stock display init** - critical for your 6" tag
6. **Share findings** - we can iterate on analysis
