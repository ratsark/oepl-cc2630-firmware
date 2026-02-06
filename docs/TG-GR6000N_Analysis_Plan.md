# TG-GR6000N CC2630 Firmware Development Plan

## Hardware Details
- **Chip**: CC2630F128 (128KB Flash, 20KB SRAM, ARM Cortex-M3)
- **Display**: 6.0" E-Paper (likely 800x600 or similar resolution)
- **Board**: SLGA-6P0-WWD-02-D (2017.08.29)
- **Programming**: UART bootloader via cc2538-bsl (same as TG-GR42)

## Available Resources

### 1. Binaries to Analyze
- `CC2630_5.8_OEPL_alpha.bin` (53KB) - OpenEPaperLink firmware (probably for TG-GR42)
- Your dumped stock firmware from TG-GR6000N

### 2. Reference Source Code Repositories
- **Shared_OEPL_Definitions**: Protocol definitions (IEEE 802.15.4 MAC layer)
- **Tag_FW_EFR32xG22**: Reference implementation for similar ARM Cortex-M based tags
- **Tag_FW_nRF52811**: Another ARM reference (nRF52811 is also Cortex-M4)

### 3. Key Protocol Information

#### Radio Configuration
- **PAN ID**: 0x4447 (or 0x1337 for SubGHz)
- **Protocol**: IEEE 802.15.4
- **Max packet**: 125 bytes payload
- **Channel**: Configurable (likely channel 11-26 in 2.4GHz)

#### Packet Types (from oepl-proto.h)
```c
PKT_AVAIL_DATA_REQ     0xE5  // Tag announces itself to AP
PKT_AVAIL_DATA_INFO    0xE6  // AP tells tag what data is available
PKT_BLOCK_REQUEST      0xE4  // Tag requests image data blocks
PKT_BLOCK_PART         0xE8  // AP sends image data block
PKT_XFER_COMPLETE      0xEA  // Transfer complete
PKT_PING/PONG          0xED/0xEE
```

#### Tag Communication Flow
1. Tag wakes up on timer/button/NFC
2. Sends `AvailDataReq` with battery, temp, RSSI, capabilities
3. AP responds with `AvailDataInfo` (dataVer, dataSize, dataType, nextCheckIn)
4. If data available, tag sends `blockRequest` for image blocks
5. AP sends image in `blockPart` packets (99 bytes data per part, 42 parts max = 4KB blocks)
6. Tag sends `XFER_COMPLETE` when done
7. Tag goes to sleep until next check-in

## Reverse Engineering Strategy

### Phase 1: Binary Analysis Setup
1. Load both binaries in Ghidra
2. Set processor to ARM Cortex-M3, Little Endian
3. Load address: 0x00000000 (CC2630 flash base)
4. Identify ARM vector table (first 64 bytes)
5. Locate reset handler and main()

### Phase 2: Identify Key Functions
Using the OEPL source as reference, locate these in the binary:

#### From oepl_radio.c/h:
- Radio initialization (IEEE 802.15.4 setup)
- Packet TX/RX handlers
- MAC layer functions
- Channel configuration

#### From oepl_display.c/h:
- Display initialization sequence
- SPI communication to e-paper controller
- Frame buffer management
- Display update routines

#### From oepl_hw_abstraction.c/h:
- GPIO configuration
- Timer setup
- Power management (sleep modes)
- Battery voltage ADC

#### From oepl_app.c:
- Main state machine
- Check-in logic
- Block transfer handling
- Protocol implementation

### Phase 3: Extract Critical Information

#### Display Driver (HIGH PRIORITY for 6" tag)
The 6" display likely uses one of these controllers:
- UC8159 (common for 6" displays)
- IL0373/IL0371
- SSD1619
- Custom controller

**What to extract:**
1. SPI pin configuration (CLK, MOSI, CS, DC, RST, BUSY)
2. Initialization command sequence
3. LUT (Look-Up Table) data
4. Refresh commands
5. Resolution configuration

#### Radio Configuration
**What to extract:**
1. CC2630 radio register settings
2. Channel number
3. TX power settings
4. MAC address location in flash
5. Packet buffer handling

#### Memory Layout
**What to extract:**
1. Image storage location (flash sectors)
2. Configuration storage (NVS)
3. Bootloader location
4. Application start address

### Phase 4: Create New Firmware

#### Option A: Port from TI SDK
Use Texas Instruments CC2630 SDK as base:
- Start with TI-RTOS or bare-metal examples
- Port OEPL protocol layer from EFR32xG22 source
- Implement display driver based on reverse-engineered init sequence
- Test incrementally

#### Option B: Hybrid Approach
- Extract working display driver assembly from stock firmware
- Wrap it with C stubs
- Build OEPL protocol on top using reference source
- Gradually replace assembly with C code

## Ghidra Analysis Checklist

### Initial Setup
- [ ] Install Ghidra (if not installed)
- [ ] Create new project: "OEPL_CC2630"
- [ ] Import CC2630_5.8_OEPL_alpha.bin
- [ ] Import TG-GR6000N stock firmware

### Binary Configuration
- [ ] Set language: ARM v7 Little Endian
- [ ] Set base address: 0x00000000
- [ ] Auto-analyze with default options
- [ ] Import oepl-proto.h structures as Data Type Archive

### Analysis Tasks
- [ ] Identify vector table entries (SP, Reset, NMI, HardFault, etc.)
- [ ] Follow reset handler to find main()
- [ ] Search for IEEE 802.15.4 PAN ID (0x4447 or 0x1337)
- [ ] Search for packet type constants (0xE1-0xEE)
- [ ] Locate SPI initialization (look for GPIO and peripheral setup)
- [ ] Find string references (if any debugging strings exist)
- [ ] Identify function prologue/epilogue patterns
- [ ] Create struct overlays for protocol packets
- [ ] Annotate known functions with names from OEPL source

### Comparison Analysis
- [ ] Compare OEPL binary vs stock firmware side-by-side
- [ ] Identify differences in initialization
- [ ] Find OEPL-specific packet handlers
- [ ] Locate display update differences

## Display Controller Investigation

### Common 6" E-Paper Controllers

Check stock firmware for these initialization patterns:

#### UC8159 (Very likely)
```
Typical init: 0x01 (power setting)
             0x00 (panel setting)
             0x06 (boost soft start)
             0x04 (power on)
             0x50 (VCOM and data interval)
             0x30 (PLL control)
             0x61 (resolution: 800x600 = 0x0320 x 0x0258)
```

#### IL0373
```
Typical init: 0x01 (driver output control)
             0x0C (booster soft start)
             0x2C (write VCOM)
             0x3C (border waveform)
```

### How to Find Init Sequence in Binary
1. Look for consecutive byte writes to SPI
2. Search for resolution values: 800 (0x0320), 600 (0x0258)
3. Find GPIO toggle patterns (CS, DC, RST pulses)
4. Locate delay loops between commands

## Next Steps

1. **Get your stock firmware dump ready** - Where is it located?
2. **Install Ghidra** (or confirm you have it)
3. **Start with OEPL binary analysis** - Understand the protocol implementation
4. **Compare with stock firmware** - Find display init differences
5. **Create minimal test firmware** - Blink LED first, then display init
6. **Integrate OEPL protocol** - Port from EFR32xG22 reference

## Useful References

### TI CC2630 Documentation
- CC2630 Technical Reference Manual
- CC2630 Datasheet
- SimpleLink SDK examples

### Display Controllers
- UC8159 datasheet
- IL0373 datasheet
- SSD1619 datasheet

### OEPL Protocol
- `/tmp/Shared_OEPL_Definitions/oepl-proto.h`
- `/tmp/Tag_FW_EFR32xG22/firmware/oepl_radio.c`
- `/tmp/Tag_FW_EFR32xG22/firmware/oepl_app.c`
