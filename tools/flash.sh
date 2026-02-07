#!/bin/bash
# Flash firmware to TG-GR6000N via UART bootloader
#
# This script coordinates:
# 1. Holding D/L pin low via RPi GPIO 17 (bootloader entry)
# 2. Running cc2538-bsl to flash (which resets the device via DTR)
# 3. Releasing D/L pin for normal boot
#
# The UART adapter has RST connected (to DTR), and D/L is on a separate
# wire to GPIO 17. cc2538-bsl will toggle DTR to reset the chip, which
# enters bootloader because we hold D/L low.

set -e

FIRMWARE="${1:-binaries/Tag_FW_CC2630_TG-GR6000N.bin}"
SERIAL_PORT="${2:-/dev/ttyUSB0}"
GPIO_CHIP="gpiochip0"
DL_GPIO=17
BSL_TOOL="python3 $HOME/Code/cc2538-bsl/cc2538_bsl/cc2538_bsl.py"

if [ ! -f "$FIRMWARE" ]; then
    echo "ERROR: Firmware file not found: $FIRMWARE"
    exit 1
fi

if [ ! -c "$SERIAL_PORT" ]; then
    echo "ERROR: Serial port not found: $SERIAL_PORT"
    exit 1
fi

echo "=== TG-GR6000N Flash Tool ==="
echo "Firmware: $FIRMWARE ($(stat -c %s "$FIRMWARE") bytes)"
echo "Serial:   $SERIAL_PORT"
echo ""

# Step 1: Kill any existing gpioset holding D/L
if [ -f /tmp/dl_pin_gpioset.pid ]; then
    OLD_PID=$(cat /tmp/dl_pin_gpioset.pid)
    kill "$OLD_PID" 2>/dev/null || true
    rm -f /tmp/dl_pin_gpioset.pid
fi

# Step 2: Hold D/L pin LOW
# gpioset blocks and holds the pin as long as the process is alive
echo "[1/4] Pulling D/L pin LOW (GPIO $DL_GPIO)..."
gpioset -c "$GPIO_CHIP" "$DL_GPIO"=0 &
GPIOSET_PID=$!
echo "$GPIOSET_PID" > /tmp/dl_pin_gpioset.pid
sleep 0.3

# Verify gpioset is running
if ! kill -0 "$GPIOSET_PID" 2>/dev/null; then
    echo "ERROR: Failed to drive D/L pin low"
    exit 1
fi
echo "  D/L pin LOW (PID $GPIOSET_PID)"

# Step 3: Flash firmware
# cc2538-bsl will toggle DTR to reset the device, which enters bootloader
# because D/L is being held low.
# Bootloader baud rate is 500000 by default but we set it explicitly.
echo "[2/4] Flashing firmware via cc2538-bsl..."
echo "  (cc2538-bsl will reset the device via DTR)"

$BSL_TOOL \
    -p "$SERIAL_PORT" \
    -b 500000 \
    -e -w -v \
    "$FIRMWARE"

FLASH_RESULT=$?

# Step 4: Release D/L pin
echo "[3/4] Releasing D/L pin..."
kill "$GPIOSET_PID" 2>/dev/null || true
rm -f /tmp/dl_pin_gpioset.pid
sleep 0.3
echo "  D/L pin released (high-Z)"

if [ $FLASH_RESULT -ne 0 ]; then
    echo ""
    echo "ERROR: Flashing failed (exit code $FLASH_RESULT)"
    echo ""
    echo "Troubleshooting:"
    echo "  - Is the UART adapter connected and powered?"
    echo "  - Is RST connected to the UART adapter's DTR?"
    echo "  - Try: screen $SERIAL_PORT 500000 to check bootloader response"
    echo "  - Make sure no other process has the serial port open"
    exit 1
fi

# Step 5: Reset the device to run new firmware
# cc2538-bsl should have already reset, but D/L was low then.
# We need one more reset with D/L high to boot the application.
echo "[4/4] Resetting device for normal boot..."
python3 -c "
import serial, time
s = serial.Serial('$SERIAL_PORT', 115200)
s.dtr = False  # RST high (deassert)
time.sleep(0.1)
s.dtr = True   # RST low (assert reset)
time.sleep(0.1)
s.dtr = False  # RST high (release reset)
s.close()
" 2>/dev/null || echo "  (Could not toggle DTR for reset - power cycle manually)"

echo ""
echo "=== Flash complete! ==="
echo "The device should now be running the new firmware."
echo "Monitor output: screen $SERIAL_PORT 115200"
echo ""
