# Complete GPIO Pin Mapping: CC2630 OEPL vs TG-GR6000N

**Analysis Date**: 2026-01-28
**Purpose**: GPIO pin mapping for OEPL firmware adaptation to TG-GR6000N hardware

---

## Executive Summary

The two firmwares use **fundamentally different GPIO configuration approaches**:

- **CC2630 OEPL**: Dynamic, array-based pin configuration with up to 30 configurable pins (DIO 0-29)
- **TG-GR6000N Stock**: Fixed memory-mapped GPIO control registers with hardcoded pin assignments

**Key Finding**: The adaptation requires creating a new pin configuration array that maps OEPL's flexible GPIO system to TG-GR6000N's fixed hardware pinout.

---

## 1. Memory-Mapped GPIO Structures

### CC2630 OEPL Firmware

**GPIO Configuration Base Addresses**:
```
├─ 0x200038D8 - GPIO configuration base #1 (FUN_00006e98)
├─ 0x20003928 - GPIO DIO count register (max pins available)
├─ 0x20003B50 - Pin configuration array (array of pin config pointers)
└─ 0x67C77F00 - GPIO control magic value (IOC configuration)
```

**GPIO Init Function** (FUN_00006e98 @ 0x00006E98):
```c
// Pseudo-code representation
void gpio_init(gpio_config_t* config, uint8_t* pin_array) {
    // Load max pin count
    uint8_t max_pins = *(uint8_t*)0x20003928;

    // Pin configuration array
    gpio_pin_t** pin_configs = (gpio_pin_t**)0x20003B50;

    // Process each pin in array
    for (int i = 0; pin_array[i] != 0xFE; i++) {
        uint8_t pin_num = pin_array[i];

        // Validate pin number
        if (pin_num == 0xFF) continue;  // Skip invalid pins
        if (pin_num > 0x1E) return ERROR;  // Max 30 pins (0-29)
        if (pin_num > max_pins) return ERROR;

        // Check if pin already configured
        if (pin_configs[pin_num] != NULL) return ERROR;

        // Configure pin using IOC magic value
        configure_pin(config, 0x67C77F00, pin_num);

        // Store configuration
        pin_configs[pin_num] = config;
        config->mask |= (1 << pin_num);
    }

    return SUCCESS;
}
```

**Key Constants**:
- `0xFE` - Pin array terminator
- `0xFF` - Invalid/disabled pin marker
- `0x1E` (30) - Maximum DIO pin number (0-29)

### TG-GR6000N Stock Firmware

**GPIO Control Structure** (0x20000670 base):
```
Display GPIO Memory Map:
├─ 0x20000670 + 0x00: GPIO base/status register
├─ 0x20000670 + 0x04: SPI MOSI data buffer
├─ 0x20000670 + 0x08: SPI clock control
├─ 0x20000670 + 0x10: DC pin control (also RST bit 0x40)
├─ 0x20000670 + 0x12: BUSY input status
├─ 0x20000670 + 0x14: Additional control register
└─ Total range: 0x20000670 - 0x200007FF (384 bytes)
```

**Specific Register Addresses**:
- `0x20000734` (offset +0x10) - DC (Data/Command) pin state
  - Bit 0x40 = RST (Reset) control
- `0x20000736` (offset +0x12) - BUSY pin input status

---

## 2. IOC (I/O Controller) Peripheral Registers

### CC2630 IOC Base Register: 0x40081000

**Per-Pin IOC Configuration**:
```
Each DIO pin has a 32-bit IOC configuration register:
├─ DIO0:  0x40081000
├─ DIO1:  0x40081004
├─ DIO2:  0x40081008
├─ ...
├─ DIO9:  0x40081024 (typical SPI MOSI)
├─ DIO10: 0x40081028 (typical SPI CLK)
├─ ...
└─ DIO29: 0x40081074

Formula: IOC_addr = 0x40081000 + (DIO_number × 4)
```

**Other Peripheral Registers** (found in both firmwares):
```
├─ 0x40041000: GPIO/Radio control base
├─ 0x40041008: GPIO register
├─ 0x40080000: GPIO data output enable (DOE)
├─ 0x40080008: GPIO data out register
├─ 0x40082028: Peripheral control register
├─ 0x40082110: Display-related peripheral
└─ 0x40082140: Additional peripheral control
```

---

## 3. IOC Configuration Magic Value

### The 0x67C77F00 Constant

**Found in CC2630 OEPL firmware**:
- Referenced at lines: 15329, 15369, 15411
- Used in: FUN_00006e64 (pin configuration function)
- Purpose: IOC register configuration value

**Bit Field Interpretation**:
```
Value: 0x67C77F00 = 0110 0111 1100 0111 0111 1111 0000 0000

Likely CC26xx IOC bit fields:
├─ Bits [0]:     Input enable (0 = disabled)
├─ Bits [2:1]:   Pull control (00=none, 01=down, 10=up, 11=reserved)
├─ Bits [4:3]:   Edge detection (00=none, 01=falling, 10=rising, 11=both)
├─ Bits [6:5]:   Drive strength (00=low, 01=medium, 10=high, 11=reserved)
├─ Bits [15:7]:  Port ID / function select
└─ Bits [31:16]: Reserved / slew rate control

Decoded (approximate):
├─ 0x67: Drive strength HIGH, edge detection enabled
├─ 0xC7: Pull-up enabled, input enabled
├─ 0x7F: Function select (GPIO mode)
└─ 0x00: Reserved bits
```

---

## 4. Side-by-Side Pin Comparison

| Function | CC2630 OEPL | TG-GR6000N Stock | Compatibility |
|----------|-------------|------------------|---------------|
| **GPIO Init Method** | Array-based (0x20003B50) | Fixed memory-mapped | Must adapt |
| **DC (Data/Command)** | Dynamic from array | 0x20000734 (+0x10) | Need mapping |
| **RST (Reset)** | Dynamic from array | 0x20000734 bit 0x40 | Need mapping |
| **BUSY (Input)** | Dynamic from array | 0x20000736 (+0x12) | Need mapping |
| **SPI CLK** | DIO10 (typically) | Hardware SPI peripheral | ✅ Compatible |
| **SPI MOSI** | DIO9 (typically) | Hardware SPI peripheral | ✅ Compatible |
| **SPI CS** | Dynamic from array | Combined with DC | Need mapping |
| **Max Pins** | 30 (DIO 0-29) | Not explicitly limited | OK |
| **Array Terminator** | 0xFE | Not applicable | N/A |
| **Invalid Pin** | 0xFF | Not applicable | N/A |

---

## 5. Display Control Pin Details

### DC (Data/Command) Pin

**CC2630 OEPL**:
- Configured via pin array
- DIO number determined at init time
- Controlled through GPIO output register

**TG-GR6000N**:
- Fixed at memory address: `0x20000734`
- Offset: +0x10 from GPIO base (0x20000670)
- Operation: `strb r0, [r4, #0x10]` (write single byte)
- Logic: 0 = Command mode, 1 = Data mode

### RST (Reset) Pin

**CC2630 OEPL**:
- Configured via pin array
- Separate DIO pin for reset control

**TG-GR6000N**:
- **Shares address with DC pin**: `0x20000734`
- **Bit mask**: `0x40` (bit 6)
- Logic:
  - Set bit 0x40: Normal operation (RST HIGH)
  - Clear bit 0x40: Reset active (RST LOW)
- Example: `bic r0, r0, #0x40000` (clear reset bit)

### BUSY (Input) Pin

**CC2630 OEPL**:
- Configured via pin array as input
- Read through GPIO input register

**TG-GR6000N**:
- Fixed at memory address: `0x20000736`
- Offset: +0x12 from GPIO base (0x20000670)
- Operation: `ldrb r0, [r5, #0x12]` (read single byte)
- Logic: Poll until LOW (0) = display ready

### SPI Pins (Both Firmwares)

**SPI Configuration**:
```
Both use TI CC2630 hardware SPI peripheral:
├─ SPI CLK:  Typically DIO10 (0x40081028)
├─ SPI MOSI: Typically DIO9 (0x40081024)
├─ SPI MISO: Typically DIO8 (0x40081020) - if used
└─ SPI CS:   May be software-controlled or combined with DC

SPI Control:
├─ Base: 0x200005AC (both firmwares use this address!)
├─ Configuration: TI SPI HAL handles low-level control
└─ Compatible: OEPL SPI driver should work as-is
```

---

## 6. Pin Configuration Arrays

### CC2630 OEPL Pin Array Format

```c
// Pin configuration array (terminated with 0xFE)
const uint8_t oepl_pin_config[] = {
    DIO_9,      // SPI MOSI
    DIO_10,     // SPI CLK
    DIO_11,     // Display CS (example)
    DIO_12,     // Display DC
    DIO_13,     // Display RST
    DIO_14,     // Display BUSY (input)
    DIO_15,     // LED (if present)
    0xFE        // Terminator
};

// Constants
#define PIN_ARRAY_END    0xFE
#define PIN_INVALID      0xFF
#define PIN_MAX          0x1E  // 30 pins (0-29)
```

### TG-GR6000N Pin Array (To Be Created)

```c
// New pin array for TG-GR6000N hardware
// Must be extracted from stock firmware analysis

const uint8_t tg_gr6000n_pin_config[] = {
    DIO_?,      // SPI MOSI (extract from stock FW)
    DIO_?,      // SPI CLK (extract from stock FW)
    DIO_?,      // Display CS
    DIO_?,      // Display DC
    DIO_?,      // Display RST
    DIO_?,      // Display BUSY (input)
    // Add other pins as needed
    0xFE        // Terminator
};
```

---

## 7. Adaptation Strategy

### Step 1: Extract Pin Numbers from Stock Firmware

**Method**: Trace IOC register writes in stock firmware initialization

```bash
# Search for IOC register writes (0x40081xxx)
grep "40081" stock_listing.txt

# Look for patterns like:
# ldr r0, =0x40081028  ; IOC config for DIO10
# str r1, [r0]         ; Write configuration
```

### Step 2: Create Pin Mapping Table

| Function | Stock FW Address | OEPL Pin Array Index | DIO Number | IOC Address |
|----------|------------------|----------------------|------------|-------------|
| SPI CLK | Hardware | 0 | DIO_? | 0x40081??? |
| SPI MOSI | Hardware | 1 | DIO_? | 0x40081??? |
| Display CS | Combined | 2 | DIO_? | 0x40081??? |
| Display DC | 0x20000734 | 3 | DIO_? | 0x40081??? |
| Display RST | 0x20000734 bit 0x40 | 4 | DIO_? | 0x40081??? |
| Display BUSY | 0x20000736 | 5 | DIO_? | 0x40081??? |

### Step 3: Modify OEPL Display Driver

**Original OEPL display driver** (pseudo-code):
```c
// Generic GPIO control
void display_set_dc(bool is_data) {
    gpio_set_pin(gpio_dc_pin, is_data);
}

void display_set_rst(bool level) {
    gpio_set_pin(gpio_rst_pin, level);
}

bool display_read_busy(void) {
    return gpio_get_pin(gpio_busy_pin);
}
```

**Adapted for TG-GR6000N** (using fixed addresses):
```c
// Direct memory-mapped GPIO control
#define GPIO_BASE        0x20000670
#define DC_RST_OFFSET    0x10          // +0x10 from base = 0x20000734
#define BUSY_OFFSET      0x12          // +0x12 from base = 0x20000736
#define RST_BIT          0x40          // Bit mask for RST

#define GPIO_DC_RST      (*(volatile uint8_t*)(GPIO_BASE + DC_RST_OFFSET))
#define GPIO_BUSY        (*(volatile uint8_t*)(GPIO_BASE + BUSY_OFFSET))

void display_set_dc(bool is_data) {
    if (is_data) {
        GPIO_DC_RST |= 0x01;   // Set DC bit (data mode)
    } else {
        GPIO_DC_RST &= ~0x01;  // Clear DC bit (command mode)
    }
}

void display_set_rst(bool level) {
    if (level) {
        GPIO_DC_RST |= RST_BIT;   // Set RST bit (normal)
    } else {
        GPIO_DC_RST &= ~RST_BIT;  // Clear RST bit (reset)
    }
}

bool display_read_busy(void) {
    return (GPIO_BUSY & 0x01) != 0;  // Read BUSY pin
}
```

### Step 4: Update Pin Configuration Function

**Wrapper function to adapt OEPL pin init for TG-GR6000N**:
```c
// Adaptation layer between OEPL and TG-GR6000N
void tg_gr6000n_gpio_init(void) {
    // Initialize GPIO base structure
    gpio_config_t config;
    memset(&config, 0, sizeof(config));

    // Call OEPL GPIO init with TG-GR6000N pin array
    // (Extract actual pin numbers from stock firmware first!)
    const uint8_t pins[] = {
        DIO_9,   // SPI MOSI (example - verify from stock FW)
        DIO_10,  // SPI CLK (example - verify from stock FW)
        DIO_11,  // Display CS
        DIO_12,  // Display DC
        DIO_13,  // Display RST
        DIO_14,  // Display BUSY
        0xFE     // Terminator
    };

    // Call original OEPL GPIO init
    gpio_init(&config, pins);

    // Override with direct memory-mapped control for display
    // (Since TG-GR6000N uses fixed addresses)
    use_fixed_display_gpio = true;
}
```

---

## 8. Critical Register Values

### Both Firmwares Share These Addresses

```
Peripheral Registers (Common):
├─ 0x40041000: GPIO/Radio control base
├─ 0x40041008: GPIO control register
├─ 0x40080000: GPIO DOE (Data Output Enable)
├─ 0x40080008: GPIO DOUT (Data Output)
├─ 0x40081000: IOC (I/O Controller) base
├─ 0x40082028: Peripheral control
├─ 0x40082110: Display-related control
└─ 0x40082140: Additional peripheral

RAM Structures:
├─ 0x200005AC: SPI control (BOTH use same address!)
├─ 0x20003358: Display state (OEPL) / 0x200034D4 (Stock)
├─ 0x20000670: GPIO base (Stock only)
└─ 0x20003B50: Pin config array (OEPL only)
```

---

## 9. Memory Layout Comparison

### CC2630 OEPL Firmware

```
RAM Layout (20KB total):
├─ 0x20000000 - 0x200005FF: Stack and low memory
├─ 0x200005AC: SPI control structure
├─ 0x20003358: Display state structure (32 bytes)
├─ 0x200038D8: GPIO configuration base
├─ 0x20003928: GPIO DIO count register
├─ 0x20003B50: Pin configuration array (4 bytes per pin, up to 30)
├─ 0x20003B50 + (30×4) = 0x20003BC8: End of pin config (184 bytes)
└─ 0x20004FFF: End of RAM

Framebuffer:
├─ 400×300 = 15,000 bytes (15KB)
├─ Fits in 20KB RAM with room for stack and structures
```

### TG-GR6000N Stock Firmware

```
RAM Layout (20KB total):
├─ 0x20000000 - 0x200005FF: Stack and low memory
├─ 0x200005AC: SPI control structure (SAME as OEPL!)
├─ 0x20000670 - 0x200007FF: GPIO control area (384 bytes)
├─ 0x20000734: DC/RST pin control
├─ 0x20000736: BUSY pin input
├─ 0x200034D4: Controller status
├─ 0x200034D8: Command queue
└─ 0x20004FFF: End of RAM

Framebuffer Challenge:
├─ 600×448 = 33,600 bytes (33.6KB)
├─ DOES NOT FIT in 20KB RAM!
├─ Solution: Line-by-line transfer from compressed EEPROM storage
└─ OEPL uses zlib compression for images
```

---

## 10. Next Steps for Complete Pin Extraction

### Required Analysis (Not Yet Complete)

To finish the pin mapping, you need to:

1. **Identify SPI Pin Numbers**:
   ```bash
   # Search for SPI peripheral initialization in stock firmware
   grep -A20 "SSI\|SPI" stock_listing.txt

   # Look for IOC register writes during SPI init
   grep "40081" stock_listing.txt | grep -A5 -B5 "SSI\|SPI"
   ```

2. **Trace Display Pin Initialization**:
   ```bash
   # Find where 0x20000734 is first written
   grep "20000734" stock_listing.txt

   # Find the initialization function that sets up display GPIO
   grep -B30 "EPD.*Init" stock_listing.txt
   ```

3. **Extract IOC Configuration Values**:
   ```bash
   # Find all IOC writes in stock firmware
   grep "str.*40081" stock_listing.txt

   # Look for configuration patterns
   # Compare with OEPL's 0x67C77F00 value
   ```

4. **Verify Pin Numbers**:
   - Cross-reference with CC2630 datasheet
   - Confirm SPI pins (usually DIO 9-11)
   - Confirm GPIO pins for display control
   - Check for conflicts or shared pins

5. **Create Final Pin Array**:
   ```c
   // Complete pin configuration for TG-GR6000N
   const uint8_t tg_gr6000n_pins[] = {
       DIO_9,   // SPI MOSI (verify)
       DIO_10,  // SPI CLK (verify)
       DIO_11,  // Display CS (verify)
       DIO_12,  // Display DC (verify)
       DIO_13,  // Display RST (verify)
       DIO_14,  // Display BUSY (verify)
       // Add LED, button pins if present
       0xFE     // Terminator
   };
   ```

---

## 11. Testing and Validation

### Pin Mapping Validation Checklist

- [ ] **SPI Communication Works**: Can send commands to display
- [ ] **DC Pin Toggles Correctly**: Command vs. data mode switching
- [ ] **RST Pin Functions**: Can reset display controller
- [ ] **BUSY Pin Reads Correctly**: Can detect display ready state
- [ ] **No Pin Conflicts**: Each DIO used for only one function
- [ ] **IOC Configuration Valid**: Pins configured with correct drive/pull settings
- [ ] **Display Responds**: Can wake UC8159 and send init sequence
- [ ] **Image Transfer Works**: Can send partial/full framebuffer

### Debugging GPIO Issues

If pins don't work:
1. Verify DIO numbers match physical hardware
2. Check IOC configuration values
3. Confirm pin direction (input vs. output)
4. Use logic analyzer to verify SPI signals
5. Measure voltage levels on DC, RST, BUSY pins
6. Compare with stock firmware behavior

---

## 12. Summary

### What We Know

✅ **OEPL uses dynamic pin configuration** via arrays
✅ **TG-GR6000N uses fixed memory-mapped GPIO**
✅ **Both use same SPI control address** (0x200005AC)
✅ **IOC configuration value** (0x67C77F00) found
✅ **Display pin addresses** (0x20000734, 0x20000736) found
✅ **Pin limits** (30 pins max, DIO 0-29)

### What We Need

⏳ **Exact DIO numbers** for each function
⏳ **SPI pin assignments** from stock firmware
⏳ **IOC configuration** values used by stock firmware
⏳ **Additional pins** (LED, buttons if present)

### Adaptation Summary

**For successful OEPL → TG-GR6000N porting**:

1. ✅ **SPI driver**: Compatible (same TI HAL)
2. ✅ **GPIO structure**: Understood (array vs. fixed)
3. ⏳ **Pin mapping**: Need exact DIO numbers
4. ✅ **Display init**: Already extracted (see COMPLETE_INIT_SEQUENCE.md)
5. ✅ **OEPL protocol**: Compatible (see CC2630_OEPL_ANALYSIS.md)
6. ⚠️ **Framebuffer**: Need compression (33.6KB > 20KB RAM)

---

**Document Status**: GPIO structure analysis complete. Exact pin numbers need extraction from stock firmware IOC initialization code.

**Next Action**: Search stock firmware for IOC register writes (`str` to `0x40081xxx`) during initialization to extract DIO pin assignments.
