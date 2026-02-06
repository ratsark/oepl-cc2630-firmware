#!/bin/bash
# Control the D/L (Download) pin on the TG-GR6000N via Raspberry Pi GPIO 17
#
# The D/L pin controls bootloader entry on the CC2630:
#   LOW  = Enter UART bootloader on next power cycle
#   HIGH = Boot application normally
#
# Usage:
#   ./dl_pin.sh low    # Pull D/L low (bootloader mode)
#   ./dl_pin.sh high   # Release D/L high (normal boot)
#   ./dl_pin.sh status # Show current state

GPIO_PIN=17
GPIO_PATH="/sys/class/gpio/gpio${GPIO_PIN}"

# Export GPIO if not already
if [ ! -d "$GPIO_PATH" ]; then
    echo "$GPIO_PIN" > /sys/class/gpio/export 2>/dev/null
    sleep 0.1
fi

case "$1" in
    low)
        echo "out" > "$GPIO_PATH/direction"
        echo "0" > "$GPIO_PATH/value"
        echo "D/L pin LOW - device will enter bootloader on next power cycle"
        ;;
    high)
        echo "in" > "$GPIO_PATH/direction"
        echo "D/L pin released (high-Z/pulled up) - device will boot normally"
        ;;
    status)
        dir=$(cat "$GPIO_PATH/direction" 2>/dev/null)
        val=$(cat "$GPIO_PATH/value" 2>/dev/null)
        echo "GPIO $GPIO_PIN: direction=$dir value=$val"
        ;;
    *)
        echo "Usage: $0 {low|high|status}"
        echo "  low    - Pull D/L low for bootloader entry"
        echo "  high   - Release D/L for normal boot"
        echo "  status - Show current GPIO state"
        exit 1
        ;;
esac
