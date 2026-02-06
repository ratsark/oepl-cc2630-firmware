# TG-GR6000N Display Initialization Analysis

## Analysis Date: 2026-01-28

This document contains the reverse-engineered display initialization sequence from the stock TG-GR6000N firmware.

---

## Key Findings Summary

### Display Configuration
- **Resolution**: 600×448 pixels (confirmed at 0xf0fa in firmware)
- **Display Controller**: Likely **UC8159** or similar e-paper controller
- **Communication**: SPI interface with command/data protocol

### Critical Memory Addresses
- **0x200005AC**: SPI control structure base
- **0x20003358**: Display configuration structure
- **0x200034D4**: Peripheral interface register
- **0x003D0900**: Magic constant loaded into display register at offset +0x10

---

## Display Initialization Call Chain

```
FUN_00004322 (Main display init - called 11 times)
    │
    ├─> FUN_000042b2 (Hardware setup - called first)
    │       │
    │       ├─> FUN_00006a02 (SPI/queue initialization)
    │       ├─> FUN_0000614c (Display struct init at 0x20003358)
    │       ├─> FUN_00006134 (Controller setup)
    │       └─> FUN_00004266 (Send init command sequence)
    │               │
    │               └─> Sends: 0xAB 0x00 0x00 0x00 0x00
    │
    └─> Regional display init loop
            │
            ├─> Command 0x20 (Set X-address) - 0x1000 increments
            ├─> Command 0xD8 (Display command) - 0xF000 increments
            └─> Command 0x52 (Display command) - 0x7000 increments
```

---

## Function Breakdown

### FUN_00004322 - Primary Display Initialization

**Address**: 0x00004322
**Called**: 11 times throughout firmware
**Purpose**: High-level display configuration with region-by-region init

**Key Operations**:
```assembly
1. Call FUN_000042b2 (hardware setup)
2. Setup address range iteration:
   - r5 = start address
   - r4 = end address
   - r6 = current address (0x1000 byte increments)
3. Three different command sequences based on alignment:

   a) If address aligned to 0x10000 boundary:
      - Send 0xD8 command
      - Send address bytes (high, low)
      - Increment by 0xF000

   b) If address aligned to 0x8000 boundary:
      - Send 0x52 command
      - Send address bytes (high, low)
      - Increment by 0x7000

   c) Otherwise:
      - Send 0x20 command
      - Send address bytes (high, low)
      - Increment by 0x1000

4. Call FUN_000042f6 (polling/wait for completion)
```

**Display Commands Identified**:
- **0x20**: Set X-address (likely column address)
- **0xD8**: Unknown display command (possibly row address or mode setting)
- **0x52**: Unknown display command (possibly update trigger)

---

### FUN_000042b2 - Hardware Initialization

**Address**: 0x000042b2
**Called**: 3 times (always before display operations)
**Purpose**: Initialize SPI interface and display controller

**Key Operations**:
```assembly
1. Load SPI control structure at 0x200005AC into r4
2. Setup SPI queue/buffer descriptors:
   - r1 = r4 + 0x4
   - r0 = r4 + 0xc
   - Call FUN_00006a02 (SPI init)

3. Load display config structure at 0x20003358 into r5
4. Initialize display structure:
   - Call FUN_0000614c (display init)

5. Load magic constant 0x003D0900 into display[+0x10]
   - This is a critical configuration value

6. Setup peripheral interface:
   - Load value from 0x200034D8
   - Call FUN_00006134 (controller setup)
   - Store result to 0x200034D4

7. Send initial command sequence:
   - Call FUN_00004266
```

**Critical Constants**:
- **0x200005AC**: SPI hardware control block
- **0x20003358**: Display configuration structure
- **0x003D0900**: Magic display register value (needs decoding)

---

### FUN_00004266 - Initial Command Sequence

**Address**: 0x00004266
**Called**: 4 times
**Purpose**: Send display controller initialization command

**Command Sequence**:
```
0xAB 0x00 0x00 0x00 0x00
```

**Analysis**:
This is likely a **Panel Power On** or **Initial Boot** command for the display controller.

In UC8159 datasheet:
- **0xAB** is not a standard UC8159 command
- Could be vendor-specific or undocumented

Possible interpretations:
- Custom initialization for this specific panel
- Soft reset with parameters
- Power sequencing command

---

### FUN_000042f6 - Display Busy/Status Polling

**Address**: 0x000042f6
**Called**: 3 times (after each display command sequence)
**Purpose**: Wait for display controller to complete operation

**Key Operations**:
```assembly
1. Loop calling FUN_0000424a (status check)
2. Wait until bit 31 of return value is clear
3. Check byte at 0x20000761
4. If set to 0x01, call FUN_0000345a
```

This is a **BUSY** polling loop - standard for e-paper displays.

---

## Display Controller Identification

### Command Analysis

Based on the command bytes observed:
- **0xAB**: Unknown (not standard UC8159)
- **0x20**: Possibly column address set
- **0x52**: Possibly update/refresh trigger
- **0xD8**: Unknown mode/configuration command

### Likely Candidates

**1. UC8159 (Most Likely)**
- Supports 600×448 resolution
- Common in 6.0" e-paper displays
- Uses similar SPI command protocol
- May have vendor-specific extensions (0xAB command)

**2. IL0373/IL0371**
- Also supports similar resolutions
- Less common for 6.0" displays

**3. Custom/OEM Controller**
- Could be a modified/rebranded UC8159
- Vendor may have added custom commands

---

## Next Steps for Full Extraction

### 1. Extract Complete Init Sequence

Search for more functions that send command sequences:
- Look for patterns like FUN_00004266
- Find functions that call FUN_0000422c repeatedly with small values

### 2. Find LUT (Look-Up Table) Data

E-paper displays require waveform LUTs. Search for:
- Large data arrays (50-200 bytes)
- Functions that transfer bulk data to display
- Patterns of repeating byte sequences

### 3. Identify GPIO Pin Mappings

Find SPI pin configuration:
- Search for GPIO register writes (0x40000000 range)
- Look for patterns setting up MOSI, CLK, CS, DC, RST pins
- CC2630 GPIO registers are in IOC (IO Control) module

### 4. Decompile Helper Functions

Focus on:
- **FUN_0000614c**: Display structure initialization
- **FUN_00006134**: Controller setup
- **FUN_00006a02**: SPI queue initialization
- **FUN_0000422c**: Byte sender (reveals SPI protocol)

### 5. Search for More Commands

Commands to look for (UC8159 standard):
- **0x01**: Panel Setting Register (PSR)
- **0x04**: Power ON
- **0x10**: Deep Sleep
- **0x12**: Display Refresh
- **0x13**: Display Update Control
- **0x50**: VCOM and Data Interval Setting
- **0x61**: Resolution Setting (likely has 600×448 here!)

---

## Recommended Tools

### For Further Analysis

1. **Ghidra Functions**
   - Rename FUN_00004322 → `display_init_regions`
   - Rename FUN_000042b2 → `display_hardware_setup`
   - Rename FUN_00004266 → `send_init_cmd_0xAB`
   - Rename FUN_000042f6 → `display_wait_busy`

2. **Cross-Reference Search**
   - Find all calls to FUN_0000422c with constant values
   - Build complete command list
   - Look for data structures passed to display functions

3. **Data Type Definition**
   - Define struct at 0x200005AC as SPI_Control
   - Define struct at 0x20003358 as Display_Config
   - This will make the decompiled code much more readable

---

## Memory Map Summary

```
ROM/Flash:
  0x00000000 - 0x0001FFFF : Program code
  0x0000F0FA               : Display resolution (600×448)

RAM:
  0x20000000 - 0x20004FFF : SRAM (20KB)
  0x20000761              : Display busy flag
  0x200005AC              : SPI control structure
  0x200034D4              : Peripheral interface register
  0x200034D8              : Peripheral interface pointer
  0x20003358              : Display configuration structure

Peripherals:
  0x40000000 - 0x4FFFFFFF : CC2630 peripherals
  0x10000000 - 0x1FFFFFFF : ROM (CC2630 bootloader/drivers)
```

---

## Display Configuration Structure (Hypothesis)

Based on usage at **0x20003358**:

```c
struct DisplayConfig {
    uint32_t unknown_00;
    uint32_t unknown_04;
    uint32_t unknown_08;
    uint32_t unknown_0c;
    uint32_t magic_10;      // Set to 0x003D0900 in init
    uint32_t unknown_14;
    // ... more fields
};
```

The value **0x003D0900** breaks down as:
- Could be bit flags for display modes
- Could be timing parameters
- Needs more analysis to decode

---

## Command Bytes Reference

All commands sent via **FUN_0000422c**:

| Command | Context | Likely Purpose |
|---------|---------|----------------|
| 0xAB | Init sequence | Unknown - possibly vendor init |
| 0x00 | After 0xAB (4×) | Parameters for 0xAB command |
| 0x05 | Status check | Get display status |
| 0x20 | Regional init | Set column/X address |
| 0x52 | Regional init | Unknown - possibly refresh |
| 0xB9 | After regions | Unknown - possibly sleep/standby |
| 0xD8 | Regional init | Unknown - possibly row/Y address |

---

## Questions for Further Investigation

1. **What does 0x003D0900 control?**
   - Display mode flags?
   - Timing parameters?
   - Resolution encoding?

2. **What are commands 0x52 and 0xD8?**
   - Not standard UC8159 commands
   - Could be vendor extensions
   - Need to test with actual hardware

3. **Where is the resolution (600×448) actually sent to the controller?**
   - Not visible in init sequence yet
   - Might be in FUN_00006134 or FUN_0000614c
   - Could be embedded in 0x003D0900 value

4. **What are the SPI pin assignments?**
   - Need to find GPIO configuration code
   - Look for IOC (IO Control) register writes

5. **Where are the LUT tables?**
   - Essential for grayscale rendering
   - Typically 100-200 bytes
   - Should be transferred during init

---

## Files Generated

- **stock_listing.txt**: Full Ghidra disassembly (91,311 lines)
- **This file**: Analysis of display initialization

---

## Status

- ✅ Display resolution confirmed (600×448)
- ✅ Main init functions identified
- ✅ Basic command sequence extracted (0xAB 0x00...)
- ✅ SPI control structures located
- ⏳ Complete command list (partial)
- ⏳ LUT data (not found yet)
- ⏳ GPIO pin mapping (not found yet)
- ⏳ Display controller confirmed (likely UC8159, needs verification)

---

**Next Recommended Action**: Decompile **FUN_0000422c** to understand the SPI protocol and byte transmission mechanism. This will reveal how command vs data bytes are distinguished (DC pin control).

