#!/bin/bash
# Flash and verify firmware on TG-GR6000N via UART bootloader
#
# Wiring (pogo jig):
#   Reset  → UART adapter RTS
#   TXD    → UART adapter RX  (crossover)
#   RXD    → UART adapter TX  (crossover)
#   GND    → UART adapter GND
#   BAT    → 3.3V supply
#   TI_DN  → Hold to GND the entire time tag is on jig
#
# Usage: Hold TI_DN to GND, run this script, keep holding until it finishes.
# cc2538-bsl flashes with erase + write + CRC32 verify, then we do a quick
# readback of the vector table as a spot-check.

set -e

FIRMWARE="${1:-binaries/Tag_FW_CC2630_TG-GR6000N.bin}"
SERIAL_PORT="${2:-/dev/ttyUSB0}"
BSL_TOOL="python3 $HOME/Code/cc2538-bsl/cc2538_bsl/cc2538_bsl.py"
READBACK="/tmp/flash_readback.bin"

if [ ! -f "$FIRMWARE" ]; then
    echo "ERROR: Firmware file not found: $FIRMWARE"
    exit 1
fi

if [ ! -c "$SERIAL_PORT" ]; then
    echo "ERROR: Serial port not found: $SERIAL_PORT"
    exit 1
fi

echo "=== TG-GR6000N Flash & Verify ==="
echo "Firmware: $FIRMWARE"
echo "Serial:   $SERIAL_PORT"
echo ""

# Step 1: Flash (erase + write + CRC32 verify)
echo "[1/2] Flashing (erase, write, CRC verify)..."
$BSL_TOOL \
    -p "$SERIAL_PORT" \
    -b 500000 \
    --bootloader-invert-lines \
    -e -w -v \
    "$FIRMWARE"

echo ""

# Step 2: Quick readback spot-check (first 256 bytes — vector table + startup code)
echo -n "[2/2] Readback spot-check... "
$BSL_TOOL \
    -p "$SERIAL_PORT" \
    -b 500000 \
    --bootloader-invert-lines \
    -r -l 256 -a 0 \
    "$READBACK" > /dev/null 2>&1

if cmp -n 256 "$READBACK" "$FIRMWARE" 2>/dev/null; then
    echo "OK"
    # Show the IEEE address (unique per tag)
    ADDR=$($BSL_TOOL -p "$SERIAL_PORT" -b 500000 --bootloader-invert-lines -r -l 1 -a 0 "$READBACK" 2>&1 | grep "IEEE Address" | head -1)
    [ -n "$ADDR" ] && echo "  $ADDR"
    echo ""
    echo "=== PASS ==="
else
    echo "FAILED — vector table mismatch!"
    echo ""
    echo "=== FAIL ==="
    exit 1
fi
