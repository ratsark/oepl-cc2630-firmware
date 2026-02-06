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

# TI SimpleLink SDK paths
SDK_ROOT ?= $(HOME)/Code/ti/simplelink_cc13xx_cc26xx_sdk_8_32_00_07
DRIVERLIB = $(SDK_ROOT)/source/ti/devices/cc13x1_cc26x1/driverlib
DEVICE_DIR = $(SDK_ROOT)/source/ti/devices/cc13x1_cc26x1

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
	$(FIRMWARE_DIR)/main.c \
	$(FIRMWARE_DIR)/oepl_app.c \
	$(FIRMWARE_DIR)/oepl_radio_cc2630.c \
	$(FIRMWARE_DIR)/oepl_hw_abstraction_cc2630.c \
	$(FIRMWARE_DIR)/oepl_nvm_cc2630.c \
	$(DRIVERS_DIR)/oepl_display_driver_uc8159_600x448.c \
	$(DRIVERS_DIR)/oepl_display_driver_common_cc2630.c \
	$(FIRMWARE_DIR)/oepl_compression.c

# Include paths
INCLUDES = \
	-I$(FIRMWARE_DIR) \
	-I$(DRIVERS_DIR) \
	-I$(CONFIG_DIR) \
	-I$(FIRMWARE_DIR)/shared \
	-I$(DRIVERLIB) \
	-I$(DEVICE_DIR) \
	-I$(SDK_ROOT)/source \
	-I$(SDK_ROOT)/kernel/tirtos/packages

# Defines
DEFINES = \
	-DDeviceFamily_CC26X1 \
	-DCC2630 \
	-DOEPL_TARGET_CC2630 \
	-DOEPL_DISPLAY_UC8159_600X448 \
	-DSIMPLELINK_SDK_AVAILABLE

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
	$(DRIVERLIB)/bin/gcc/driverlib.lib \
	-lm \
	-lc \
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
# Note: D/L pin on RPi GPIO 17 must be pulled low first
SERIAL_PORT ?= /dev/ttyUSB0
program: $(BIN_DIR)/$(PROJECT).bin
	@echo "Programming via UART bootloader..."
	@echo "Make sure D/L pin (RPi GPIO 17) is LOW for bootloader entry"
	cc2538-bsl -p $(SERIAL_PORT) -e -w -v $<

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
