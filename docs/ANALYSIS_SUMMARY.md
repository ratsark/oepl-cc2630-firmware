# TG-GR6000N Stock Firmware Analysis Summary

## Quick Facts

### Hardware
- **Model**: Solum TG-GR6000N
- **MCU**: CC2630F128 (128KB Flash, 20KB SRAM, ARM Cortex-M3)
- **Display**: 6.0" E-Paper, **600x448 pixels**
- **Programming**: UART bootloader (RXD, TXD, RST, T/M, GND, BAT, D/L)

### Firmware Files
- **Stock firmware**: `stock.bin` (131,072 bytes = full 128KB flash)
- **OEPL firmware**: `CC2630_5.8_OEPL_alpha.bin` (54,212 bytes = 53KB)

## Stock Firmware Analysis

### Vector Table (ARM Cortex-M3)
```
0x00000000: a8 43 00 20  -> Initial SP = 0x200043a8
0x00000004: e9 ee 00 00  -> Reset Handler = 0x0000eee8 (Thumb mode)
0x00000008: 01 ca 01 10  -> NMI = 0x1001ca00 (ROM routine)
0x0000000c: 01 ca 01 10  -> HardFault = 0x1001ca00 (ROM routine)
```

**Notable**: Many interrupt handlers point to `0x1001ca00`, which is in CC2630 ROM. This is normal - the ROM contains bootloader and utility functions.

### Display Configuration Found!

At offset **0xf0fa**, there's a display configuration structure:

```c
// Hypothetical structure based on found data
struct DisplayConfig {
    uint16_t width;   // 0xf0fa: 600 (0x0258)
    uint16_t height;  // 0xf0fc: 448 (0x01c0)
    uint8_t  data[...];  // Additional configuration bytes
};
```

**Raw bytes around resolution:**
```
0x0f0fa: 58 02 c0 01 78 00 03 c0 8c 03 01 d0 02 55 24 07
```

Interpretation:
- **0x0258** = 600 (width)
- **0x01C0** = 448 (height)
- Followed by what appear to be timing or LUT parameters

### Display Resolution: 600x448

This is confirmed! The TG-GR6000N uses a **600x448 pixel display**.

**Frame buffer size calculations:**
- 1BPP (Black/White): 600 × 448 ÷ 8 = 33,600 bytes
- 2BPP (4 grayscale): 600 × 448 ÷ 4 = 67,200 bytes

## Next Steps for Development

### 1. Identify Display Controller

The display is likely driven by one of these controllers:
- **UC8159** (very common for 6" displays)
- **IL0373/IL0371**
- **SSD1620/SSD1619**
- **HINK-E060A04** (specific 600x448 panel)

**Action**: Search stock firmware for initialization commands.

### 2. Extract Display Init Sequence

Using Ghidra, look for:
1. SPI initialization (GPIO configuration)
2. Command sequences sent to display
3. LUT (Look-Up Table) data
4. Power-on/off sequences

### 3. GPIO Pin Mapping

Need to identify from stock firmware:
- SPI CLK, MOSI pins
- CS (Chip Select)
- DC (Data/Command)
- RST (Reset)
- BUSY (input from display)
- LED GPIO (if present)
- Button GPIOs (if any)

### 4. Build New Firmware

**Option A: Port OEPL**
1. Start with TI CC2630 SDK
2. Port OEPL protocol from EFR32xG22 reference
3. Implement 600x448 display driver
4. Use extracted init sequence from stock firmware

**Option B: Modify Stock**
1. Reverse engineer stock protocol
2. Patch in OEPL compatibility
3. (More difficult, not recommended)

## File Organization

Your project directory now contains:

```
TG-GR6000N-firmware/
├── README.md                       # Master guide
├── ANALYSIS_SUMMARY.md            # This file
├── TG-GR6000N_Analysis_Plan.md    # Development strategy
├── Initial_Binary_Analysis.md      # OEPL binary findings
├── ghidra_quickstart.md           # Ghidra setup guide
├── stock.bin                       # Your dumped firmware (128KB)
└── CC2630_5.8_OEPL_alpha.bin      # OpenEPaperLink firmware (53KB)
```

## Reference Repositories (in /tmp)

- `/tmp/Shared_OEPL_Definitions/` - Protocol headers
- `/tmp/Tag_FW_EFR32xG22/` - Reference firmware
- `/tmp/Tag_FW_nRF52811/` - Reference firmware
- `/tmp/OpenEPaperLink/` - Main repo

## Commands Used

### Dump firmware from tag
```bash
python cc2538-bsl.py -p /dev/tty.usbserial-* \\
  --bootloader-invert-lines -r stock.bin
```

### Analyze firmware
```bash
# View vector table
xxd -l 256 stock.bin

# Search for resolution
python3 << 'EOF'
import struct
data = open('stock.bin', 'rb').read()
# Search for 600 (0x0258) and 448 (0x01C0)
w = struct.pack('<H', 600)
h = struct.pack('<H', 448)
for i in range(len(data)):
    if data[i:i+2] == w:
        if data[i+2:i+4] == h:
            print(f"Found 600x448 at 0x{i:04x}")
EOF
```

## Key Findings Summary

✅ **Display Resolution Confirmed**: 600x448 pixels
✅ **Firmware Size**: Full 128KB flash dump obtained
✅ **Vector Table**: Validated, points to 0x0000eee8 reset handler
✅ **ROM Handlers**: Uses CC2630 ROM routines for some interrupts
✅ **Display Config Structure**: Located at 0xf0fa

⏳ **Still To Determine**:
- Display controller IC (UC8159, IL0373, SSD1619, other?)
- GPIO pin assignments
- SPI configuration
- Display initialization command sequence
- LUT data

## Recommended Tools

1. **Ghidra** - For deep analysis and decompilation
2. **IDA Pro / Binary Ninja** - Alternative disassemblers
3. **TI Code Composer Studio** - For CC2630 development
4. **SimpleLink SDK** - CC2630 examples and drivers

## Contact OEPL Developers

Before investing too much time in reverse engineering, consider:

1. **Open a GitHub issue** asking about CC2630 source code:
   https://github.com/OpenEPaperLink/OpenEPaperLink/issues

2. **Tag atc1441** (the contributor who added CC2630 binary):
   @atc1441 Do you have the source code for CC2630_5.8_OEPL_alpha.bin?

3. **Join OEPL Discord** (link in wiki) and ask about:
   - CC2630 source code availability
   - TG-GR6000N support
   - Contributing a 600x448 display driver

They may already have the source code or be willing to share development tips!

## Success Criteria

You'll have a working firmware when you can:

- [x] Dump stock firmware
- [x] Identify display resolution (600x448)
- [ ] Extract display initialization sequence
- [ ] Create minimal CC2630 blink LED test
- [ ] Initialize display with static test pattern
- [ ] Implement OEPL radio protocol
- [ ] Connect to access point
- [ ] Receive and display image from AP
- [ ] Full integration test

## Resources

- **CC2630 Datasheet**: https://www.ti.com/lit/ds/symlink/cc2630.pdf
- **CC2630 TRM**: https://www.ti.com/lit/ug/swcu117i/swcu117i.pdf
- **OEPL Wiki**: https://github.com/OpenEPaperLink/OpenEPaperLink/wiki
- **OEPL Discord**: (check wiki for invite)

---

**Current Status**: ✅ Stock firmware analyzed, display resolution confirmed
**Next Step**: Load binaries in Ghidra and extract display init sequence
