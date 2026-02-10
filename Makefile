# Makefile for OpenEPaperLink CC2630 Firmware (TG-GR6000N)

# Project name
PROJECT = Tag_FW_CC2630_TG-GR6000N

# Toolchain
TOOLCHAIN_PREFIX = arm-none-eabi-
CC = $(TOOLCHAIN_PREFIX)gcc
CXX = $(TOOLCHAIN_PREFIX)g++
AS = $(TOOLCHAIN_PREFIX)as
LD = $(TOOLCHAIN_PREFIX)ld
OBJCOPY = $(TOOLCHAIN_PREFIX)objcopy
SIZE = $(TOOLCHAIN_PREFIX)size

# CC26x0 driverlib paths (correct for CC2630)
CC26X0_DIR ?= $(HOME)/Code/ti/cc26x0
CC26X0_DRIVERLIB = $(CC26X0_DIR)/driverlib
CC26X0_INC = $(CC26X0_DIR)/inc

# Directories
FIRMWARE_DIR = firmware
DRIVERS_DIR = $(FIRMWARE_DIR)/drivers
CONFIG_DIR = $(FIRMWARE_DIR)/config
BUILD_DIR = build
BIN_DIR = binaries

# Target chip
DEVICE = CC2630F128

# Source files
SOURCES = \
	$(FIRMWARE_DIR)/startup_cc2630.c \
	$(FIRMWARE_DIR)/ccfg.c \
	$(FIRMWARE_DIR)/rtt.c \
	$(FIRMWARE_DIR)/oepl_rf_cc2630.c \
	$(FIRMWARE_DIR)/oepl_radio_cc2630.c \
	$(FIRMWARE_DIR)/oepl_hw_abstraction_cc2630.c \
	$(DRIVERS_DIR)/oepl_display_driver_uc8159_600x448.c \
	$(FIRMWARE_DIR)/splash.c \
	$(FIRMWARE_DIR)/main.c

# Include paths
INCLUDES = \
	-I$(FIRMWARE_DIR) \
	-I$(DRIVERS_DIR) \
	-I$(CONFIG_DIR) \
	-I$(FIRMWARE_DIR)/shared \
	-I$(CC26X0_INC) \
	-I$(CC26X0_DRIVERLIB)

# Defines
DEFINES = \
	-DCC2630 \
	-DOEPL_TARGET_CC2630 \
	-DOEPL_DISPLAY_UC8159_600X448

# Compiler flags
CFLAGS = \
	-mcpu=cortex-m3 \
	-mthumb \
	-march=armv7-m \
	-std=c11 \
	-Wall \
	-Wextra \
	-Os \
	-ffunction-sections \
	-fdata-sections \
	-g \
	$(DEFINES) \
	$(INCLUDES)

# C++ flags (for compression.cpp)
CXXFLAGS = \
	-mcpu=cortex-m3 \
	-mthumb \
	-march=armv7-m \
	-std=c++11 \
	-Wall \
	-Wextra \
	-Os \
	-ffunction-sections \
	-fdata-sections \
	-fno-exceptions \
	-fno-rtti \
	-g \
	$(DEFINES) \
	$(INCLUDES)

# Linker flags
LDFLAGS = \
	-mcpu=cortex-m3 \
	-mthumb \
	-march=armv7-m \
	-nostartfiles \
	-Wl,--gc-sections \
	-Wl,-Map=$(BUILD_DIR)/$(PROJECT).map \
	-T $(FIRMWARE_DIR)/cc2630f128.lds

# Libraries
LIBS = \
	$(CC26X0_DRIVERLIB)/bin/gcc/driverlib.lib \
	-lgcc \
	-lnosys

# Object files
OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(filter %.c,$(SOURCES)))
OBJECTS += $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(SOURCES)))

# Default target
all: $(BIN_DIR)/$(PROJECT).bin size

# Create directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/$(FIRMWARE_DIR)
	mkdir -p $(BUILD_DIR)/$(DRIVERS_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Compile C files
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@echo "CC $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile C++ files
$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	@echo "CXX $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Link
$(BUILD_DIR)/$(PROJECT).elf: $(OBJECTS)
	@echo "LD $@"
	@$(CC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@

# Create binary (with CCFG at end - will be 128KB)
$(BIN_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).elf | $(BIN_DIR)
	@echo "OBJCOPY $@"
	@$(OBJCOPY) -O binary $< $@
	@echo "Firmware binary created: $@"

# Show size
size: $(BUILD_DIR)/$(PROJECT).elf
	@echo ""
	@echo "===== Firmware Size ====="
	@$(SIZE) $<
	@echo ""
	@echo "CC2630F128 Limits:"
	@echo "  Flash: 128KB (131072 bytes)"
	@echo "  RAM:   20KB  (20480 bytes)"
	@echo ""

# Program via UART bootloader
# Automates D/L pin control via RPi GPIO 17
SERIAL_PORT ?= /dev/ttyUSB0
BSL_TOOL ?= python3 $(HOME)/Code/cc2538-bsl/cc2538_bsl/cc2538_bsl.py
DL_PIN_SCRIPT = tools/dl_pin.sh

program: $(BIN_DIR)/$(PROJECT).bin
	@echo "=== Programming via UART bootloader ==="
	@echo "Step 1: Pull D/L pin LOW (enter bootloader on next reset)..."
	@bash $(DL_PIN_SCRIPT) low
	@sleep 0.5
	@echo "Step 2: Flashing firmware..."
	$(BSL_TOOL) -p $(SERIAL_PORT) -e -w -v $<
	@echo "Step 3: Release D/L pin (normal boot)..."
	@bash $(DL_PIN_SCRIPT) high
	@echo "=== Done! Device should now boot the new firmware ==="
	@echo "Connect serial terminal: screen $(SERIAL_PORT) 115200"

# Program via JTAG (OpenOCD + J-Link)
jtag-flash: $(BIN_DIR)/$(PROJECT).bin
	@echo "Programming via JTAG..."
	openocd -f interface/jlink.cfg -c "transport select jtag" \
		-f target/ti_cc26x0.cfg \
		-c "init; halt; flash write_image erase $< 0x00000000; verify_image $< 0x00000000; reset run; shutdown"

# Start OpenOCD debug server (connect GDB to localhost:3333)
debug-server:
	openocd -f interface/jlink.cfg -c "transport select jtag" -f target/ti_cc26x0.cfg

# Connect GDB to running debug server
debug: $(BUILD_DIR)/$(PROJECT).elf
	gdb-multiarch -ex "target remote :3333" -ex "monitor halt" $<

# Clean
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Help
help:
	@echo "OpenEPaperLink CC2630 Firmware Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build firmware (default)"
	@echo "  clean        - Remove build artifacts"
	@echo "  program      - Program via UART bootloader (D/L pin must be low)"
	@echo "  jtag-flash   - Program via JTAG (J-Link + OpenOCD)"
	@echo "  debug-server - Start OpenOCD GDB server on :3333"
	@echo "  debug        - Connect GDB to debug server"
	@echo "  size         - Show firmware size"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Configuration:"
	@echo "  SDK_ROOT    - Path to TI SimpleLink SDK (default: ~/Code/ti/...)"
	@echo "  SERIAL_PORT - UART device (default: /dev/ttyUSB0)"
	@echo ""
	@echo "JTAG Debugging:"
	@echo "  1. make debug-server   (in one terminal)"
	@echo "  2. make debug          (in another terminal)"

.PHONY: all clean program jtag-flash debug-server debug size help
