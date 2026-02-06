# TG-GR6000N Firmware Development - Lessons Learned

This document captures critical lessons learned during the development of custom OpenEPaperLink firmware for the Solum TG-GR6000N e-paper tag (CC2630F128).

---

## ⚠️ CRITICAL: CCFG Bootloader Configuration (Device Bricking Risk!)

### What Happened
We accidentally **bricked a device** by modifying byte 51 of the CCFG from `0xC5` to `0x00`, thinking this would make the device boot our application automatically. Instead, it **permanently disabled the ROM bootloader**, making the device unrecoverable via UART.

### The Problem
The CC2630 CCFG has two separate bootloader-related fields:

1. **BOOTLOADER_ENABLE** (byte 51, bits 24-31 of BL_CONFIG register)
   - `0xC5` = ROM bootloader is **enabled** (CORRECT VALUE)
   - `0x00` = ROM bootloader is **permanently disabled** (BRICKS DEVICE!)
   - Controls whether the bootloader exists at all

2. **BL_ENABLE** (byte 48, bits 0-7 of BL_CONFIG register)
   - `0xC5` = D/L pin backdoor **enabled**
   - `0x00` = D/L pin backdoor **disabled**
   - Controls whether pulling D/L low enters bootloader

3. **IMAGE_VALID** (bytes 68-71, separate register at CCFG offset 0x44)
   - `0x00000000` = Vector table at flash address 0x00000000 (our app)
   - Invalid value = Forces permanent bootloader mode
   - Controls what runs on normal boot

### The Correct Understanding

The stock CCFG is **already perfect**:
- `BOOTLOADER_ENABLE = 0xC5` → Bootloader works
- `BL_ENABLE = 0xC5` → D/L pin can enter bootloader
- `IMAGE_VALID = 0x00000000` → Boots our application normally

**Boot behavior with stock CCFG:**
- Power on with D/L **not connected** → Boots application immediately ✅
- Power on with D/L **pulled low** → Enters bootloader for flashing ✅

This is exactly what we want! No modification needed!

### What We Did Wrong
We misunderstood the documentation and changed byte 51 to `0x00`, thinking it would:
- Prevent automatic bootloader entry ❌ (Wrong!)

What it actually did:
- **Completely disabled the ROM bootloader** ❌
- Made the device **unrecoverable via UART** ❌
- Required JTAG/SWD to recover (which we don't have) ❌

### The Fix
**Never touch byte 51!** Keep it at `0xC5` (stock value).

The stock CCFG already does what we need:
```c
// Bytes 48-51: BL_CONFIG register
0xc5,  // byte 48: BL_ENABLE = 0xC5 (D/L pin works)
0x0b,  // byte 49: BL_PIN_NUMBER = 0x0B (pin 11)
0xfe,  // byte 50: (reserved)
0xc5,  // byte 51: BOOTLOADER_ENABLE = 0xC5 (KEEP THIS!)

// Bytes 68-71: IMAGE_VALID register
0x00, 0x00, 0x00, 0x00  // IMAGE_VALID = 0x00000000 (boot our app)
```

### Recovery
If you brick a device this way:
- UART bootloader will **not respond** even with D/L low
- Device will appear completely dead
- **JTAG/SWD programmer required** (TI CC Debugger, J-Link, etc.)
- OR consider the device a loss and use a fresh one

---

## CC2630 Initialization Requirements

### Critical: SetupTrimDevice() Must Be Called First

The CC2630 **cannot run** without calling `SetupTrimDevice()` from `driverlib/setup.h` at the very start of `Reset_Handler()`.

**What it does:**
- Loads trim values from CCFG and factory calibration area (FCFG)
- Configures voltage regulators
- Sets up clock sources (XOSC, RCOSC)
- Initializes power domains
- Configures ADC reference voltages

**Where to call it:**
```c
void Reset_Handler(void)
{
    uint32_t *src, *dest;

    // CRITICAL: Load trim values and configure device
    // This MUST be called before any other code
    SetupTrimDevice();

    // Now safe to initialize .data and .bss
    // ...
}
```

**What happens without it:**
- Device may not boot at all
- Peripherals don't work correctly
- Clock frequencies are wrong
- Voltage regulators misconfigured
- No output, appears dead

---

## Avoid Infinite Loops in Early Initialization

### The Problem
We had this code in `configure_clocks()`:

```c
PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

// WRONG - Infinite loop if condition never becomes true!
while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON);
```

This caused a hang **before UART was initialized**, so we had:
- No serial output
- No way to debug
- Device appeared bricked

### The Fix
Replace infinite loops with bounded delays or timeouts:

```c
PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

// Brief delay to let power domain stabilize
for (volatile uint32_t i = 0; i < 10000; i++) {
    __asm volatile ("nop");
}
```

Or better yet, trust that `SetupTrimDevice()` already configured power domains correctly.

---

## Hardware Debugging Strategy

When you have **no output and no LED**, debugging is hard. Here's what we learned:

### 1. Verify UART Connection First
- Test with known-good bootloader (pull D/L low)
- If bootloader responds → UART hardware works
- If bootloader doesn't respond → check wiring

### 2. Read Back Flash
Even without running firmware, you can verify what's in flash:
```bash
python cc2538_bsl.py -r -a 0 -l 256 -p /dev/tty.usbserial-XXX /tmp/readback.bin
```

Look for:
- Correct stack pointer (first 4 bytes)
- Correct reset vector (bytes 4-7)
- Actual code present (not all 0xFF or 0x00)

### 3. Check CCFG
Read back CCFG area:
```bash
python cc2538_bsl.py -r -a 130984 -l 88 -p /dev/tty.usbserial-XXX /tmp/ccfg.bin
```

Verify critical bytes:
- Byte 51 should be `0xC5`
- Bytes 68-71 should be `0x00 0x00 0x00 0x00`

### 4. Bootloader Won't Respond?
If bootloader doesn't respond even with D/L low:
- **Either** our firmware is running (good!)
- **Or** bootloader was disabled in CCFG (bricked!)

To tell the difference:
- Try serial at 115200 baud (our firmware)
- Try reading back flash (will fail if bootloader disabled)

---

## Memory Constraints

### CC2630F128 Limits
- **Flash**: 128KB total
  - 131,000 bytes code/data
  - 88 bytes CCFG (at 0x1FFA8-0x1FFFF)
- **RAM**: 20KB total
  - Stack: 2KB (0x20004800-0x20004FFF)
  - Data/BSS: 18KB (0x20000000-0x200047FF)

### Display Frame Buffer Challenge
Full 600×448 monochrome framebuffer requires:
- (600 × 448) ÷ 8 = 33,600 bytes
- **This is 164% of total RAM!**

**Solution**: Don't allocate full framebuffer. Instead:
1. Generate image data line-by-line
2. Stream to display via SPI
3. Or decompress from flash in chunks

**For testing**: Use 1KB test buffer (enough for ~13 rows)

---

## Build System Notes

### TI SimpleLink SDK Version
- We use: **simplelink_cc13xx_cc26xx_sdk_8_32_00_07**
- Device family: **cc13x1_cc26x1** (NOT cc26x0!)
- Path: `/Applications/ti/simplelink_cc13xx_cc26xx_sdk_8_32_00_07`

### Important Compiler Flags
```makefile
CFLAGS = -mcpu=cortex-m3 -mthumb -march=armv7-m
DEFINES = -DDeviceFamily_CC26X1 -DSIMPLELINK_SDK_AVAILABLE
```

### Linker Script Requirements
```lds
MEMORY {
    FLASH (RX) : ORIGIN = 0x00000000, LENGTH = 0x0001FFA8  /* 131,000 bytes */
    FLASH_CCFG (RX) : ORIGIN = 0x0001FFA8, LENGTH = 88     /* CCFG area */
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x00004800  /* 18KB data */
    STACK (RW) : ORIGIN = 0x20004800, LENGTH = 0x00000800  /* 2KB stack */
}
```

### Critical Linker Symbols
Must provide for newlib:
```lds
.heap : {
    _heap_start = .;
    end = .;      /* Required by newlib */
    _end = .;
    __end = .;
    _heap_end = .;
} > SRAM
```

---

## Pin Assignments (Educated Guesses)

**Note**: These are **NOT confirmed**. They're based on common CC2630 conventions and may be wrong.

```c
#define PIN_UART_TX             3   // DIO3 - UART TX for debug
#define PIN_SPI_CLK             10  // DIO10 - SPI CLK
#define PIN_SPI_MOSI            9   // DIO9 - SPI MOSI
#define PIN_SPI_CS              11  // DIO11 - Chip Select
#define PIN_DISPLAY_DC          12  // DIO12 - Data/Command
#define PIN_DISPLAY_RST         13  // DIO13 - Reset
#define PIN_DISPLAY_BUSY        14  // DIO14 - Busy (input)
```

**TODO**: Verify these with logic analyzer or trial and error

---

## Development Workflow

### Successful Flash Procedure
1. Pull D/L pin low (force bootloader entry)
2. Power cycle device
3. Flash firmware:
   ```bash
   python cc2538_bsl.py -e -w -v firmware.bin -p /dev/tty.usbserial-XXX
   ```
4. Disconnect D/L pin (or pull high)
5. Power cycle device
6. Check serial output at 115200 baud:
   ```bash
   screen /dev/tty.usbserial-XXX 115200
   ```

### What We Expect to See
```
*** TG-GR6000N Firmware Starting ***
========================================
OpenEPaperLink CC2630 Firmware
TG-GR6000N (600x448 UC8159)
Version: 1.0.0
========================================
Hardware ID: 0xXX
Display: 600x448 @ 1 bpp
Battery: XXXX mV
Temperature: XX C
Initializing UC8159 display controller...
Display initialized
Showing test pattern...
Transferring 1024 bytes (solid black) to display...
```

---

## Debugging Checklist

When firmware doesn't work:

- [ ] `SetupTrimDevice()` called at start of Reset_Handler?
- [ ] CCFG byte 51 is `0xC5` (bootloader enabled)?
- [ ] CCFG bytes 68-71 are `0x00 0x00 0x00 0x00` (IMAGE_VALID)?
- [ ] Vector table at address 0x00000000 in binary?
- [ ] No infinite loops in early init (before UART)?
- [ ] UART TX pin configured via IOC?
- [ ] UART peripheral clock enabled (PRCM)?
- [ ] Power domains enabled and stable?
- [ ] D/L pin disconnected for normal boot?
- [ ] Correct baud rate (115200 for app, 500000 for bootloader)?

---

## Known Issues / TODO

### Not Yet Working
- [x] ~~Device won't boot~~ (Fixed: added SetupTrimDevice())
- [x] ~~Device stuck in bootloader~~ (Fixed: understood CCFG correctly)
- [x] ~~No UART output~~ (Fixed: removed infinite loop)
- [ ] **Haven't verified firmware boots on fresh device yet**
- [ ] **Pin assignments not confirmed**
- [ ] **Display not showing test pattern**

### Next Steps
1. Flash firmware on fresh device (Device #2)
2. Verify UART output appears
3. Use logic analyzer to confirm SPI signals on display
4. Determine correct GPIO pins if current guesses are wrong
5. Get test pattern visible on display
6. Implement full display driver
7. Add OEPL radio protocol

---

## Useful Commands

### Read Flash
```bash
# Read vector table (first 256 bytes)
python cc2538_bsl.py -r -a 0 -l 256 -p PORT readback.bin

# Read CCFG (88 bytes at 0x1FFA8)
python cc2538_bsl.py -r -a 130984 -l 88 -p PORT ccfg.bin
```

### Check ELF Symbols
```bash
arm-none-eabi-nm build/firmware.elf | grep -E "Reset_Handler|_estack|main"
arm-none-eabi-objdump -h build/firmware.elf
```

### Verify Binary
```bash
hexdump -C firmware.bin | head -10  # Check vector table
ls -lh firmware.bin                 # Should be exactly 128KB (131,072 bytes)
```

---

## References

- [CC2630 Technical Reference Manual](https://www.ti.com/lit/pdf/swcu117)
- [TI SimpleLink SDK Documentation](https://dev.ti.com/tirex/explore/node?node=A__AD4.b0ZXQVUfJTRUU-YaOQ__com.ti.SIMPLELINK_LOWPOWER_F3_SDK__BSEc4rl__LATEST)
- [CC2630 Driverlib API Guide](https://software-dl.ti.com/simplelink/esd/simplelink_cc13xx_cc26xx_sdk/latest/exports/docs/driverlib/index.html)
- [CCFG Configuration Guide](https://dev.ti.com/tirex/explore/node?node=A__AOlCuU3JGD0TJFk7JaAvpA__com.ti.SIMPLELINK_LOWPOWER_F3_SDK__BSEc4rl__LATEST)

---

## Summary of Critical Fixes

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| Device bricked | Changed CCFG byte 51 to 0x00 | **NEVER change byte 51!** Keep at 0xC5 |
| Device won't boot | Missing `SetupTrimDevice()` call | Call at start of Reset_Handler() |
| No UART output | Infinite loop before uart_init() | Replace infinite loops with bounded delays |
| RAM overflow | 33.6KB framebuffer > 20KB RAM | Use 1KB test buffer, stream to display |

---

**Last Updated**: 2026-01-29
**Devices Sacrificed**: 1 (RIP Device #1, you taught us well)
**Devices Remaining**: 12
