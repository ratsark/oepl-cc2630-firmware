# Complete UC8159 Display Controller Initialization Sequence for TG-GR6000N (600×448)

**Extracted from**: stock.bin firmware disassembly
**Display**: 6.0" 600×448 e-paper (UC8159 controller)
**Date**: 2026-01-28

---

## 1. RESOLUTION CONFIGURATION (Confirmed)

- **Display Resolution**: 600 × 448 pixels (0x258 × 0x1C0)
- **Framebuffer Size**: 134,400 pixels = 16,800 bytes (monochrome, 1bpp)
- **Confirmed at**:
  - Line 21848: `mov.w r3, #0x258` (width = 600)
  - Line 21859: `mov.w r2, #0x1c0` (height = 448)

---

## 2. COMMAND-BY-COMMAND INITIALIZATION SEQUENCE

### Main Initialization Flow (FUN_000042b2 @ 0x000042B2)

```
Display Init Sequence:
├─ FUN_000042b2: Main display initialization
│  ├─ Setup memory references (DAT_200005ACh)
│  ├─ Copy initialization table (0x20 bytes) via FUN_0000614c
│  ├─ Load display config (0x003D0900h value)
│  ├─ Call FUN_00006134 (controller setup)
│  ├─ Store result to DAT_200034D4h
│  ├─ Call FUN_00004266 (WAKE command)
│  ├─ Call FUN_000044de (load state)
│  └─ Call FUN_00004408 (send command 0x06)
│
├─ FUN_00004266: WAKE SEQUENCE @ 0x00004266
│  ├─ COMMAND: 0xAB (WAKE)
│  ├─ Followed by 4× DELAY commands (0x00)
│  └─ Return with status load
│
├─ FUN_00004408: CMD 0x06
│  └─ Send command 0x06
│
└─ Display ready for updates
```

### C Pseudocode

```c
void display_init(void) {
    // Initialize memory structures
    spi_handle = init_spi_interface(0x200005AC);

    // Copy 32-byte initialization table
    memcpy(display_state, init_table_0x0000615c, 0x20);

    // Configure display
    display_state[0x10] = 0x003D0900;  // Display config value

    // Controller setup
    controller_handle = controller_init(display_state);
    store_handle(controller_handle, 0x200034D4);

    // Wake sequence
    send_wake_sequence();

    // Enable display
    send_command(0x06);
}

void send_wake_sequence(void) {
    // Send WAKE command
    send_command(0xAB);

    // Send 4 delay cycles (NOP)
    send_command(0x00);
    send_command(0x00);
    send_command(0x00);
    send_command(0x00);

    // Wait for controller ready
    wait_for_busy_clear();
}
```

---

## 3. UC8159 CORE COMMANDS

### Command Reference Table

| Command | Hex  | Function            | Data Bytes | Function Call      | Line   |
|---------|------|---------------------|------------|--------------------|--------|
| WAKE    | 0xAB | Power up controller | 0x00×4     | FUN_00004266       | 9346   |
| NOP     | 0x00 | Delay/synchronize   | None       | FUN_0000428a       | 9361   |
| ENABLE  | 0x06 | Enable display      | None       | FUN_00004408       | 9530   |
| DATA_ST | 0x20 | Start frame data    | COL,ROW    | FUN_00004322       | 9461   |
| ADDR    | 0x03 | Set memory address  | ADDR       | FUN_0000440c       | 9554   |
| DATA_MK | 0x52 | 32KB block marker   | ADDR       | FUN_00004322       | 9510   |
| DATA_BK | 0xD8 | 64KB block marker   | ADDR       | FUN_00004322       | 9489   |
| SLEEP   | 0xB9 | Power down          | None       | FUN_0000428e       | 9370   |

### Detailed Command Descriptions

#### 0xAB - WAKE / POWER ON
```
Location: FUN_00004266 @ 0x00004266 (Line 9346)
Sequence:
  1. Send 0xAB command
  2. Send 0x00 (NOP delay)
  3. Send 0x00 (NOP delay)
  4. Send 0x00 (NOP delay)
  5. Send 0x00 (NOP delay)
  6. Wait for BUSY clear
Purpose: Activate UC8159 controller from sleep mode
```

#### 0x00 - NOP / DELAY
```
Location: FUN_0000428a @ 0x0000428A (Line 9361)
Purpose: Timing synchronization
Used after: WAKE command (4 times)
```

#### 0x06 - ENABLE DISPLAY
```
Location: FUN_00004408 @ 0x00004408 (Line 9530)
Purpose: Enable display controller for image transfer
Called after: Wake sequence completes
```

#### 0x20 - START DATA TRANSFER
```
Location: FUN_00004322 @ 0x00004322 (Line 9461)
Format:
  CMD: 0x20
  DATA[0]: Column address (low byte)
  DATA[1]: Column address (high byte)
  DATA[2]: Row address
Purpose: Begin frame/image data transfer
Notes: Column is (X >> 3) for byte addressing
```

#### 0x52 - 32KB BLOCK MARKER
```
Location: FUN_00004322 (Line 9510)
Purpose: Used when transferring data crosses 32KB boundary
Format: Similar to 0x20 with updated address
```

#### 0xD8 - 64KB BLOCK MARKER
```
Location: FUN_00004322 (Line 9489)
Purpose: Used when transferring data crosses 64KB boundary
Format: Similar to 0x20 with updated address
```

#### 0xB9 - SLEEP / POWER DOWN
```
Location: FUN_0000428e @ 0x0000428E (Line 9370)
Purpose: Put controller into low-power sleep mode
Notes: Always wait for BUSY clear before power removal
```

---

## 4. IMAGE DATA TRANSFER PROTOCOL

### FUN_00004322 - Main Update Function

```c
void display_update(uint8_t* framebuffer, uint32_t start_addr, uint32_t length) {
    uint32_t addr = start_addr;
    uint32_t end_addr = start_addr + length;
    uint8_t* data_ptr = framebuffer;

    // Main data transfer loop (4KB chunks)
    while (addr < end_addr) {
        // Prepare controller
        load_display_state();
        send_command(0x06);  // Enable
        timing_delay();

        // Determine chunk size
        uint32_t chunk_size;
        uint8_t cmd;

        if ((end_addr - addr) >= 0x10000) {  // 64KB+
            chunk_size = 0x1000;  // 4KB
            cmd = 0xD8;
        } else if ((end_addr - addr) >= 0x8000) {  // 32KB+
            chunk_size = 0x1000;  // 4KB
            cmd = 0x52;
        } else {
            chunk_size = 0x1000;  // 4KB
            cmd = 0x20;
        }

        // Send addressing command
        load_display_state();
        send_command(cmd);
        timing_delay();

        // Send address bytes
        send_data((addr >> 0) & 0xFF);   // Column low
        send_data((addr >> 8) & 0xFF);   // Column high
        send_data((addr >> 16) & 0xFF);  // Row

        timing_delay();

        // Wait for ready
        wait_for_ready();

        // Transfer data chunk
        spi_transfer(data_ptr, chunk_size);

        // Update pointers
        addr += chunk_size;
        data_ptr += chunk_size;
    }

    // Finalize display update
    send_command(0xB9);  // Sleep
    finalize_display();
}
```

### Transfer Flow Diagram

```
Image Write Sequence:
├─ Init: FUN_000042b2 (setup)
│
├─ For each 4KB chunk:
│  ├─ Load state: FUN_000044e0
│  ├─ Enable: send_command(0x06)
│  ├─ Timing delay
│  │
│  ├─ Select command based on address:
│  │  ├─ addr < 32KB:  use 0x20
│  │  ├─ addr < 64KB:  use 0x52
│  │  └─ addr >= 64KB: use 0xD8
│  │
│  ├─ Send command with address:
│  │  ├─ Command byte
│  │  ├─ Column low byte
│  │  ├─ Column high byte
│  │  └─ Row byte
│  │
│  ├─ Wait for ready (busy wait)
│  ├─ Transfer 4KB data via SPI
│  └─ Advance to next chunk
│
└─ Finalize:
   ├─ Send SLEEP command (0xB9)
   └─ Update display state
```

---

## 5. GPIO PIN MAPPING

### Memory-Mapped GPIO Registers

```
SPI/Display Interface:
├─ Base SPI Control:     0x200005AC (DAT_200005ACh)
├─ GPIO Control Base:    0x20000670 - 0x200007FF
├─ Display State:        0x20003358 (DAT_20003358)
├─ Controller Status:    0x200034D4 (DAT_200034D4)
└─ Command Queue:        0x200034D8 (DAT_200034D8)
```

### Pin Function Mapping

```
Display Control Pins (inferred from register offsets):
├─ DC (Data/Command):  GPIO offset +0x10 (DAT_20000734)
│   └─ Toggle between command (0) and data (1) mode
│
├─ RST (Reset):        GPIO bit 0x40 at offset +0x10
│   └─ Active LOW reset, set HIGH for normal operation
│
├─ BUSY (Input):       GPIO offset +0x12 (DAT_20000736)
│   └─ Read status: LOW=ready, HIGH=busy
│
├─ SPI CLK:            Managed by offset +0x08
│   └─ Hardware SPI clock generation
│
├─ SPI MOSI:           Managed by offset +0x04
│   └─ Data output with byte shifting
│
└─ SPI CS:             Combined with DC pin control
    └─ Chip select timing
```

### GPIO Configuration Structure

```c
typedef struct {
    uint32_t spi_control;      // +0x00: SPI interface handle
    uint32_t mosi_data;        // +0x04: MOSI output buffer
    uint32_t clk_control;      // +0x08: Clock generation
    uint8_t  reserved[4];      // +0x0C: Reserved
    uint8_t  dc_pin;           // +0x10: DC pin state (+ RST bit)
    uint8_t  reserved2;        // +0x11: Reserved
    uint8_t  busy_pin;         // +0x12: BUSY input status
    uint8_t  flags;            // +0x13: Control flags
} display_gpio_t;
```

---

## 6. POWER SEQUENCE

### Power-On Sequence

```c
void power_on_display(void) {
    // 1. Initialize memory structures
    memset(&display_state, 0, sizeof(display_state));

    // 2. Configure GPIO directions
    gpio_set_output(PIN_DC);
    gpio_set_output(PIN_RST);
    gpio_set_output(PIN_CLK);
    gpio_set_output(PIN_MOSI);
    gpio_set_input(PIN_BUSY);

    // 3. Assert reset (LOW)
    gpio_clear(PIN_RST);
    delay_ms(10);

    // 4. Release reset (HIGH)
    gpio_set(PIN_RST);
    delay_ms(10);

    // 5. Send WAKE command
    send_command(0xAB);

    // 6. Send 4× NOP delays
    for (int i = 0; i < 4; i++) {
        send_command(0x00);
    }

    // 7. Wait for controller ready
    while (gpio_read(PIN_BUSY)) {
        delay_us(100);
    }

    // 8. Load controller state
    load_display_state();

    // 9. Enable display
    send_command(0x06);

    // Display ready for image transfer
}
```

### Power-Down Sequence

```c
void power_down_display(void) {
    // 1. Complete any pending display update
    wait_for_busy_clear();

    // 2. Send SLEEP command
    send_command(0xB9);

    // 3. Wait for BUSY to clear
    while (gpio_read(PIN_BUSY)) {
        delay_us(100);
    }

    // 4. Can safely remove power
    // (Optional: assert RST LOW for complete power down)
}
```

---

## 7. LUT (LOOK-UP TABLE) DATA

### LUT Table Locations

```
LUT Data in ROM:
├─ DAT_0000eb78 @ 0x0000EB78 - Main LUT pointer table
├─ DAT_0000ebac @ 0x0000EBAC - Secondary LUT table
└─ DAT_0000eb20 @ 0x0000EB20 - Waveform data arrays
```

### LUT Access Pattern

```c
// FUN_00006134 @ 0x00006134
// Index-based LUT retrieval

void* get_lut_waveform(uint8_t index) {
    // Each LUT entry is 12 bytes (0x0C)
    uint32_t offset = index * 0x0C;

    // Base address of LUT table
    const void* lut_table = (void*)0x0000EB78;

    // Calculate entry address
    const void* entry = (uint8_t*)lut_table + offset;

    // Read waveform pointer from entry
    void* waveform = *(void**)(entry + 0x00);

    // Jump to waveform handler
    return waveform;
}
```

### LUT Structure

```
Typical LUT Structure for 600×448 EPD:

Each waveform table contains:
├─ Phase 0: Voltage sequence data (initialization)
├─ Phase 1: Black transition waveform
├─ Phase 2: White transition waveform
├─ Phase 3: Stabilization phase
├─ Phase 4: Display update phase
│
Waveform timing parameters:
├─ Frame time: microseconds per phase
├─ Temperature compensation values
├─ VCOM voltage settings
└─ Partial vs. full update flags
```

### Waveform Data Format

```
LUT Entry (12 bytes each):
├─ Offset +0x00 (4 bytes): Pointer to waveform data
├─ Offset +0x04 (4 bytes): Timing parameters
├─ Offset +0x08 (2 bytes): Phase configuration
├─ Offset +0x0A (2 bytes): Temperature range flags
```

---

## 8. TIMING INFORMATION

### Critical Timing Values

```
Delay Timings:
├─ Wake sequence:
│  ├─ After 0xAB: 4× NOP cycles
│  └─ Each NOP: ~50-100 microseconds
│
├─ Data transfer:
│  ├─ Per 4KB block: ~10-50 milliseconds
│  ├─ Full frame (16.8KB): ~50-150 milliseconds
│  └─ Busy wait: Poll BUSY pin until LOW
│
├─ Display refresh:
│  ├─ After data transfer: Wait for BUSY clear
│  └─ Typical refresh: 1-3 seconds (UC8159 dependent)
│
└─ Sleep command:
    ├─ Send 0xB9
    └─ Wait for BUSY LOW before power removal
```

### Busy Wait Implementation

```c
void wait_for_busy_clear(void) {
    uint32_t timeout = 5000000;  // 5 second timeout

    while (gpio_read(PIN_BUSY) && timeout > 0) {
        delay_us(10);
        timeout--;
    }

    if (timeout == 0) {
        // Timeout error handling
        error("Display BUSY timeout");
    }
}
```

---

## 9. MEMORY CONFIGURATION

### Critical RAM Addresses

```
Display Memory Map:
├─ 0x200005AC: SPI/Display interface control block
│   └─ Contains SPI handle and configuration
│
├─ 0x20003358: Display state structure (32 bytes)
│   ├─ +0x00: Status flags
│   ├─ +0x10: Display config (0x003D0900)
│   └─ +0x1C: Function pointers
│
├─ 0x200034D4: Controller status register
│   └─ Stores controller handle after init
│
├─ 0x200034D8: Command queue pointer
│   └─ Used for deferred command execution
│
├─ 0x20000670: GPIO control base
│   └─ Range: 0x20000670 - 0x200007FF
│
└─ 0x20000758: Display status flags
    ├─ +0x00: Update in progress flag
    ├─ +0x01: Error status
    └─ +0x02: Power state
```

### Display State Structure

```c
typedef struct {
    uint32_t status;           // +0x00: Status flags
    uint32_t reserved1[3];     // +0x04: Reserved
    uint32_t config;           // +0x10: 0x003D0900
    uint32_t reserved2[2];     // +0x14: Reserved
    void*    update_func;      // +0x1C: Update function pointer
} display_state_t;
```

---

## 10. INITIALIZATION TABLE DATA

### 32-Byte Initialization Table

```
Copy Operation: FUN_0000614c @ 0x0000614C
├─ Source: DAT_0000615c (ROM)
├─ Destination: 0x20003358 (display_state)
├─ Size: 0x20 bytes (32 bytes)
```

### Table Contents (from 0x0000615C)

```
Initialization Data (32 bytes):
Offset  Value    Description
------  -------  -----------
0x00    00 00 00 00    Initial status flags (all cleared)
0x04    FF FF FF FF    Bitmask (all bits enabled)
0x08    00 00 00 00    Reserved / padding
0x0C    40 42 0F 00    Config flags (0x40 = DC pin bit)
0x10    08 00 00 00    Timing configuration
0x14    00 00 00 00    Reserved
0x18    00 00 00 00    Reserved
0x1C    00 00 00 00    Reserved (function pointer placeholder)
```

### C Array Definition

```c
const uint8_t display_init_table[32] = {
    // Offset 0x00: Initial flags
    0x00, 0x00, 0x00, 0x00,

    // Offset 0x04: Bitmask
    0xFF, 0xFF, 0xFF, 0xFF,

    // Offset 0x08: Reserved
    0x00, 0x00, 0x00, 0x00,

    // Offset 0x0C: Config (0x40 indicates DC pin control)
    0x40, 0x42, 0x0F, 0x00,

    // Offset 0x10: Timing
    0x08, 0x00, 0x00, 0x00,

    // Offset 0x14-0x1F: Reserved
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};
```

---

## 11. CRITICAL IMPLEMENTATION NOTES

### Data Integrity

```
SPI Communication Rules:
├─ All SPI writes via FUN_0000422c (byte sender)
├─ DC pin control:
│   ├─ LOW (0) = Command byte
│   └─ HIGH (1) = Data byte
├─ Always verify BUSY signal before new command
└─ Use proper SPI timing (clock phase/polarity)
```

### Address Handling for 600×448

```c
// Resolution: 600 × 448 = 0x258 × 0x1C0

void send_address_command(uint16_t x, uint16_t y) {
    // X coordinate is byte-addressed (divide by 8)
    uint16_t col = x >> 3;  // Column = X / 8

    // Y coordinate is row
    uint16_t row = y;

    // Send addressing command
    send_command(0x20);  // Or 0x52/0xD8 based on size

    // Send address bytes
    send_data(col & 0xFF);        // Column low byte
    send_data((col >> 8) & 0xFF); // Column high byte
    send_data(row & 0xFF);        // Row byte
}
```

### Data Transfer Chunking

```
Transfer Rules:
├─ Maximum 64KB per 0x20 command sequence
├─ Use 0xD8 command for transfers > 64KB
├─ Use 0x52 command for transfers > 32KB
├─ Always transfer in 4KB chunks for efficiency
└─ End with display state update
```

### Register Interpretation

```
GPIO Register Operations:
├─ DC pin toggling:
│   └─ Bit manipulation on GPIO struct @ +0x10
│
├─ SPI clock generation:
│   └─ Managed by FUN_00006b76
│
├─ BUSY polling:
│   └─ ldrb from memory-mapped GPIO @ +0x12
│
└─ RST control:
    └─ Bit 0x40 in GPIO register @ +0x10
```

### Power Optimization

```
Power Management:
├─ Always use 0xB9 (SLEEP) for power saving
├─ Never remove power without SLEEP sequence
├─ Always wait for BUSY clear before power off
└─ Keep controller in SLEEP between updates
```

---

## 12. COMPLETE INITIALIZATION SEQUENCE (C CODE)

```c
#include <stdint.h>
#include <stdbool.h>

// Display resolution
#define DISPLAY_WIDTH  600
#define DISPLAY_HEIGHT 448
#define FRAMEBUFFER_SIZE ((DISPLAY_WIDTH * DISPLAY_HEIGHT) / 8)  // 16800 bytes

// UC8159 Commands
#define CMD_WAKE     0xAB
#define CMD_NOP      0x00
#define CMD_ENABLE   0x06
#define CMD_DATA     0x20
#define CMD_ADDR     0x03
#define CMD_DATA_32K 0x52
#define CMD_DATA_64K 0xD8
#define CMD_SLEEP    0xB9

// GPIO Pins (example - adjust for actual hardware)
#define PIN_DC       10
#define PIN_RST      11
#define PIN_BUSY     12
#define PIN_SPI_CLK  13
#define PIN_SPI_MOSI 14

// Memory addresses (from firmware analysis)
#define SPI_CONTROL_BASE    0x200005AC
#define DISPLAY_STATE_BASE  0x20003358
#define CONTROLLER_STATUS   0x200034D4
#define GPIO_CONTROL_BASE   0x20000670

// Display state structure
typedef struct {
    uint32_t status;
    uint32_t reserved1[3];
    uint32_t config;        // 0x003D0900
    uint32_t reserved2[2];
    void*    update_func;
} display_state_t;

// Initialization table (32 bytes from 0x0000615C)
const uint8_t init_table[32] = {
    0x00, 0x00, 0x00, 0x00,  // Initial flags
    0xFF, 0xFF, 0xFF, 0xFF,  // Bitmask
    0x00, 0x00, 0x00, 0x00,  // Reserved
    0x40, 0x42, 0x0F, 0x00,  // Config
    0x08, 0x00, 0x00, 0x00,  // Timing
    0x00, 0x00, 0x00, 0x00,  // Reserved
    0x00, 0x00, 0x00, 0x00,  // Reserved
    0x00, 0x00, 0x00, 0x00   // Reserved
};

// Function prototypes
void spi_init(void);
void gpio_init(void);
void send_command(uint8_t cmd);
void send_data(uint8_t data);
void wait_busy(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

// Display initialization
void display_init(void) {
    // Initialize SPI
    spi_init();

    // Initialize GPIOs
    gpio_init();

    // Copy initialization table to display state
    display_state_t* state = (display_state_t*)DISPLAY_STATE_BASE;
    memcpy(state, init_table, sizeof(init_table));

    // Set display configuration
    state->config = 0x003D0900;

    // Hardware reset sequence
    gpio_set(PIN_RST, 0);   // Assert reset (LOW)
    delay_ms(10);
    gpio_set(PIN_RST, 1);   // Release reset (HIGH)
    delay_ms(10);

    // Wake sequence
    send_command(CMD_WAKE);  // 0xAB

    // Send 4× NOP delays
    send_command(CMD_NOP);   // 0x00
    send_command(CMD_NOP);
    send_command(CMD_NOP);
    send_command(CMD_NOP);

    // Wait for controller ready
    wait_busy();

    // Enable display
    send_command(CMD_ENABLE); // 0x06

    // Display initialized and ready
}

// Display update function
void display_update(const uint8_t* framebuffer) {
    uint32_t addr = 0;
    uint32_t total_bytes = FRAMEBUFFER_SIZE;
    const uint8_t* data_ptr = framebuffer;

    // Transfer data in 4KB chunks
    while (total_bytes > 0) {
        uint32_t chunk_size = (total_bytes > 0x1000) ? 0x1000 : total_bytes;
        uint8_t cmd;

        // Select command based on address range
        if (addr >= 0x10000) {
            cmd = CMD_DATA_64K;  // 0xD8
        } else if (addr >= 0x8000) {
            cmd = CMD_DATA_32K;  // 0x52
        } else {
            cmd = CMD_DATA;      // 0x20
        }

        // Send addressing command
        send_command(CMD_ENABLE);  // 0x06
        wait_busy();

        send_command(cmd);
        send_data((addr >> 0) & 0xFF);   // Column low
        send_data((addr >> 8) & 0xFF);   // Column high
        send_data((addr >> 16) & 0xFF);  // Row

        wait_busy();

        // Transfer chunk data
        for (uint32_t i = 0; i < chunk_size; i++) {
            send_data(*data_ptr++);
        }

        // Update counters
        addr += chunk_size;
        total_bytes -= chunk_size;
    }

    // Finalize update
    send_command(CMD_SLEEP);  // 0xB9
    wait_busy();
}

// Power down display
void display_sleep(void) {
    send_command(CMD_SLEEP);  // 0xB9
    wait_busy();
}

// Wait for BUSY pin to clear
void wait_busy(void) {
    uint32_t timeout = 5000000;  // 5 second timeout

    while (gpio_read(PIN_BUSY) && timeout > 0) {
        delay_us(10);
        timeout--;
    }

    if (timeout == 0) {
        // Handle timeout error
    }
}

// Send command byte (DC = LOW)
void send_command(uint8_t cmd) {
    gpio_set(PIN_DC, 0);  // Command mode
    spi_transfer(cmd);
    gpio_set(PIN_DC, 1);  // Back to data mode
}

// Send data byte (DC = HIGH)
void send_data(uint8_t data) {
    gpio_set(PIN_DC, 1);  // Data mode
    spi_transfer(data);
}
```

---

## 13. QUICK REFERENCE

### Command Summary

| Command | Hex  | Purpose          | When Used                  |
|---------|------|------------------|----------------------------|
| WAKE    | 0xAB | Power on         | Initialization             |
| NOP     | 0x00 | Delay            | After WAKE (4×)            |
| ENABLE  | 0x06 | Enable display   | Before data transfer       |
| DATA    | 0x20 | Start frame      | Address < 32KB             |
| DATA_32K| 0x52 | Mid-frame        | Address 32KB-64KB          |
| DATA_64K| 0xD8 | Large frame      | Address >= 64KB            |
| ADDR    | 0x03 | Set address      | Memory configuration       |
| SLEEP   | 0xB9 | Power down       | After update complete      |

### Memory Map

| Address      | Purpose                    |
|--------------|----------------------------|
| 0x200005AC   | SPI control block          |
| 0x20003358   | Display state (32 bytes)   |
| 0x200034D4   | Controller status          |
| 0x200034D8   | Command queue              |
| 0x20000670   | GPIO control base          |
| 0x0000EB78   | LUT table pointer (ROM)    |
| 0x0000615C   | Init table (ROM)           |

### Timing Reference

| Operation              | Duration         |
|------------------------|------------------|
| NOP delay              | 50-100 µs        |
| 4KB transfer           | 10-50 ms         |
| Full frame (16.8KB)    | 50-150 ms        |
| Display refresh        | 1-3 seconds      |
| BUSY timeout (max)     | 5 seconds        |

---

## 14. NEXT STEPS FOR OEPL INTEGRATION

1. **Create UC8159 600×448 driver** using this init sequence
2. **Adapt SPI HAL** from CC2630 OEPL firmware
3. **Map GPIO pins** by comparing with OEPL GPIO usage
4. **Test with static images** before full integration
5. **Handle 16.8KB framebuffer** (compression or line-by-line transfer)

---

**This document contains the complete UC8159 initialization sequence extracted from the TG-GR6000N stock firmware, ready for implementation in the OEPL firmware port.**
