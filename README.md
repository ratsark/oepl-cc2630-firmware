# OpenEPaperLink Firmware for CC2630 (TG-GR6000N)

Custom open-source OEPL firmware for the Solum TG-GR6000N 6.0" e-paper tag.

## Hardware

- **Chip**: Texas Instruments CC2630F128 (ARM Cortex-M3, 128KB Flash, 20KB RAM)
- **Radio**: 2.4 GHz IEEE 802.15.4 (OEPL protocol)
- **Display**: 6.0" UC8159 e-paper controller, 600x448 monochrome
- **Model**: Solum TG-GR6000N

## Project Structure

```
oepl-cc2630-firmware/
├── Makefile              Build system
├── openocd.cfg           JTAG debug configuration (J-Link + CC26x0)
├── firmware/             Source code
│   ├── main.c            Entry point and test patterns
│   ├── startup_cc2630.c  Reset handler and vector table
│   ├── ccfg.c            CC2630 customer configuration (bootloader settings)
│   ├── cc2630f128.lds    Linker script
│   ├── oepl_app.c/h      OEPL application state machine
│   ├── oepl_radio_cc2630.c/h    Radio interface (2.4 GHz 802.15.4)
│   ├── oepl_hw_abstraction_cc2630.c/h  Hardware abstraction layer
│   ├── oepl_nvm_cc2630.c/h      Non-volatile memory (flash storage)
│   ├── oepl_compression.c/h     Image decompression
│   └── drivers/
│       ├── oepl_display_driver_uc8159_600x448.c/h   UC8159 display driver
│       └── oepl_display_driver_common_cc2630.c/h     Display abstraction
├── docs/                 Analysis and development documentation
├── reference/            Stock firmware binaries and OEPL reference binary
└── tools/                Utility scripts (D/L pin control, etc.)
```

## Build Requirements

### TI SimpleLink SDK (driverlib)

```bash
# Sparse clone the SDK (only driverlib, ~few MB)
cd ~/Code
mkdir -p ti && cd ti
git clone --filter=blob:none --sparse --branch lpf2-8.32.00.07 --depth 1 \
    https://github.com/TexasInstruments/cc13xx_cc26xx_sdk.git \
    simplelink_cc13xx_cc26xx_sdk_8_32_00_07
cd simplelink_cc13xx_cc26xx_sdk_8_32_00_07
git sparse-checkout set source/ti/devices/cc13x1_cc26x1
```

Then build the driverlib (needs to be done once):
```bash
SDK_DIR=~/Code/ti/simplelink_cc13xx_cc26xx_sdk_8_32_00_07
DRIVERLIB_DIR=$SDK_DIR/source/ti/devices/cc13x1_cc26x1/driverlib
mkdir -p $DRIVERLIB_DIR/bin/gcc

for f in $DRIVERLIB_DIR/*.c; do
    arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -Os -ffunction-sections -fdata-sections \
        -DDeviceFamily_CC26X1 -I$SDK_DIR/source \
        -c "$f" -o "$DRIVERLIB_DIR/bin/gcc/$(basename "$f" .c).o"
done

arm-none-eabi-ar rcs $DRIVERLIB_DIR/bin/gcc/driverlib.lib $DRIVERLIB_DIR/bin/gcc/*.o
```

### ARM Toolchain

```bash
# Debian/Raspberry Pi
sudo apt install gcc-arm-none-eabi gdb-multiarch libnewlib-arm-none-eabi
```

### Programming Tools

```bash
# UART bootloader programmer
pip3 install cc2538-bsl

# OpenOCD for JTAG (optional, for J-Link debugging)
sudo apt install openocd
```

## Building

```bash
make                    # Build firmware
make size               # Show memory usage
make clean              # Clean build artifacts
```

## Programming

### Via UART Bootloader

The D/L pin must be pulled low to enter bootloader mode on power-up.
On the Raspberry Pi, D/L is connected to GPIO 17:

```bash
# Enter bootloader mode
./tools/dl_pin.sh low
# Power cycle the tag, then:
make program SERIAL_PORT=/dev/ttyUSB0

# Return to normal boot
./tools/dl_pin.sh high
# Power cycle the tag
```

### Via JTAG (J-Link)

```bash
make jtag-flash         # Flash via JTAG
```

## Debugging (JTAG)

```bash
# Terminal 1: Start OpenOCD debug server
make debug-server

# Terminal 2: Connect GDB
make debug
```

In GDB:
```
(gdb) monitor halt
(gdb) info registers
(gdb) break main
(gdb) continue
```

## Status

- [x] Stock firmware dumped and analyzed
- [x] Display init sequence extracted from stock firmware
- [x] GPIO mapping partially documented
- [x] Firmware compiles
- [ ] **JTAG debugging** - debugger arrived, need to verify basic execution
- [ ] **GPIO pin mapping** - need to confirm with JTAG/logic analyzer
- [ ] **Display test** - show test pattern on screen
- [ ] **Radio communication** - 2.4 GHz OEPL protocol
- [ ] **Full OEPL integration**

## Known Issues

- **Framebuffer**: 600x448 = 33.6 KB exceeds 20 KB RAM. Must use line-by-line streaming.
- **Pin mapping unconfirmed**: GPIO assignments are educated guesses from stock firmware analysis.
- **One device bricked**: CCFG byte 51 was changed to 0x00, disabling ROM bootloader. Recoverable via JTAG.

## Based On

- [OpenEPaperLink](https://github.com/OpenEPaperLink/OpenEPaperLink) project
- CC2630 OEPL alpha firmware (reference binary in `reference/`)
- TG-GR6000N stock firmware (display init sequence)

## Documentation

See `docs/` for detailed analysis:
- `LESSONS_LEARNED.md` - Critical pitfalls and fixes
- `GPIO_PINOUT.md` - Pin mapping analysis
- `CC2630_OEPL_ANALYSIS.md` - OEPL firmware reverse engineering
- `COMPLETE_INIT_SEQUENCE.md` - UC8159 display init
