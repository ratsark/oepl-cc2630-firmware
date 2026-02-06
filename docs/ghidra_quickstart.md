# Ghidra Quick Start Guide for CC2630 Analysis

## Installation

### macOS
```bash
# Install via Homebrew
brew install --cask ghidra

# Or download from https://ghidra-sre.org/
```

### Launch Ghidra
```bash
ghidraRun
# Or via Applications folder
```

## Project Setup

### 1. Create New Project
1. File → New Project
2. Select "Non-Shared Project"
3. Name: `OEPL_CC2630`
4. Location: Choose your workspace

### 2. Import CC2630 OEPL Binary

1. File → Import File
2. Select: `CC2630_5.8_OEPL_alpha.bin`
3. Format: **Raw Binary**
4. Click "Options..."
   - Language: **ARM → v7 → Cortex → LE (Little Endian)**
   - Base Address: **0x00000000**
5. Click OK

### 3. Import Stock Firmware (if you have it)

Repeat step 2 with your dumped TG-GR6000N firmware.

## Initial Analysis

### 1. Auto-Analysis

1. Double-click the imported binary to open in CodeBrowser
2. When prompted "Would you like to analyze?": **Yes**
3. In Analysis Options:
   - ✅ ASCII Strings
   - ✅ ARM Aggressive Instruction Finder
   - ✅ Data Reference
   - ✅ Function Start Search
   - ✅ Stack
   - Leave other defaults
4. Click "Analyze"
5. Wait for analysis to complete (progress bar in bottom-right)

### 2. Verify Vector Table

1. Go to address **0x00000000** (press 'G', type '0')
2. You should see:
   ```
   00000000: d0 40 00 20    Initial Stack Pointer = 0x200040d0
   00000004: 5d a9 00 00    Reset Handler = 0x0000a95d
   00000008: 6d a9 00 00    NMI Handler = 0x0000a96d
   0000000c: 71 a9 00 00    HardFault = 0x0000a971
   ```

### 3. Create Vector Table Structure

1. At address 0x00000000, press 'D' repeatedly to create DWORDs
2. Select addresses 0x00000004 through 0x000000ff
3. Right-click → Data → Create Pointer
4. Name them (right-click → Edit Label):
   - 0x00000000: `_initial_stack_pointer`
   - 0x00000004: `_reset_handler`
   - 0x00000008: `_nmi_handler`
   - 0x0000000c: `_hardfault_handler`
   - etc.

### 4. Navigate to Reset Handler

1. Double-click on the value at 0x00000004 (0x0000a95d)
2. OR: Press 'G' and go to **0xa95c** (note: ARM Thumb mode, actual address is 0xa95c)
3. You should see disassembled code
4. Right-click → Create Function
5. Name it `Reset_Handler`

## Key Search Strategies

### Search for Constants

#### Find PAN ID (0x4447)
1. Search → Memory (or press 'S')
2. Search for Hex: `47 44` (little-endian)
3. Look for references near radio initialization code

#### Find Packet Type Constants
Search for these hex values:
- `e5` - PKT_AVAIL_DATA_REQ
- `e6` - PKT_AVAIL_DATA_INFO
- `e4` - PKT_BLOCK_REQUEST
- `e8` - PKT_BLOCK_PART
- `ea` - PKT_XFER_COMPLETE

#### Find Display Resolution (for 6" display)
Common 6" resolutions:
- 800x600: Search for `20 03` (0x0320) and `58 02` (0x0258)
- 640x384: Search for `80 02` (0x0280) and `80 01` (0x0180)

### Search for Strings

1. Window → Defined Strings
2. Look for any debug strings, version info, etc.

### Identify SPI Functions

Look for patterns:
1. GPIO initialization (writing to peripheral registers)
2. Repeated byte writes (SPI TX)
3. Delay loops (waiting for BUSY signal)
4. Sequences of command/data writes

## Import Protocol Structures

### 1. Create Data Type Archive

File → Parse C Source...
1. Add files:
   - `/tmp/Shared_OEPL_Definitions/oepl-proto.h`
   - `/tmp/Shared_OEPL_Definitions/oepl-definitions.h`
2. Click Parse
3. Save as "OEPL_Structures.gdt"

### 2. Apply Structures

When you find packet buffers:
1. Right-click on address
2. Data → Choose Data Type
3. Select `AvailDataReq`, `AvailDataInfo`, etc.

## Useful Keyboard Shortcuts

- **G**: Go to address
- **L**: Rename/label
- **;**: Add comment
- **D**: Create data (BYTE, WORD, DWORD)
- **P**: Create pointer
- **F**: Create function
- **S**: Search memory
- **X**: Show cross-references (XREFs)
- **Ctrl+E**: Edit function signature
- **Ctrl+L**: Go to label
- **Ctrl+F**: Find text in listing

## Analysis Workflow

### Step 1: Understand Startup
1. Follow Reset_Handler
2. Identify SystemInit()
3. Find main()

### Step 2: Find Radio Init
1. Look for writes to radio registers
2. CC2630 radio is at peripheral base 0x40040000
3. Search for constants like 0x4004xxxx

### Step 3: Find Display Init
1. Look for GPIO configuration
2. Identify SPI peripheral setup
3. Find command sequence sent to display

### Step 4: Find Main Loop
1. Locate infinite loop in main()
2. Identify state machine
3. Find timer/sleep functions

### Step 5: Find Protocol Handlers
1. Search for packet type constants
2. Trace RX interrupt handlers
3. Map packet processing functions

## Compare Binaries

### Side-by-Side Comparison

1. Open both binaries in separate CodeBrowser windows
2. Window → Tile Horizontally
3. Synchronize navigation (Tool → Synchronize)
4. Compare corresponding functions

### Version Tracking

1. Tools → Version Tracking → Create Session
2. Add both programs
3. Auto-version matching will find similar functions
4. Manually match remaining functions

## Tips

- **Start with known constants**: PAN ID, packet types, resolutions
- **Follow cross-references**: Use 'X' key liberally
- **Name everything**: Good labels make analysis easier
- **Use comments**: Document your findings as you go
- **Save often**: Analysis takes time
- **Export findings**: Bookmarks, comments can be exported

## Expected Findings

### Functions You Should Find:
- `radio_init()`
- `radio_tx()`, `radio_rx()`
- `display_init()`
- `display_update()`
- `handle_avail_data_req()`
- `handle_block_request()`
- `sleep_until_timer()`
- `read_battery()`
- `read_temperature()`

### Data Structures:
- MAC address (8 bytes, likely in flash)
- Configuration struct
- Image buffer
- Packet RX/TX buffers

## Next Steps After Initial Analysis

1. **Document display init sequence** → Critical for 6" tag
2. **Extract radio configuration** → Verify it matches OEPL protocol
3. **Map memory layout** → Where does image data go?
4. **Identify GPIO pins** → Display control, buttons, LED
5. **Compare with stock firmware** → What's different?

## Exporting Data

### Export Function List
1. Window → Functions
2. Select all (Cmd+A)
3. Right-click → Export → CSV

### Export Memory Map
1. Window → Memory Map
2. File → Export Memory Map

### Export Disassembly
1. File → Export Program
2. Format: HTML, ASCII, or C/C++

## Resources

- Ghidra Documentation: https://ghidra-sre.org/
- CC2630 TRM: https://www.ti.com/lit/ug/swcu117i/swcu117i.pdf
- ARM Cortex-M3 Guide: ARM documentation
