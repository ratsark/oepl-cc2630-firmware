#!/bin/bash
# Control the D/L (Download) pin on the TG-GR6000N via Raspberry Pi GPIO 17
#
# The D/L pin controls bootloader entry on the CC2630:
#   LOW  = Enter UART bootloader on next power cycle/reset
#   HIGH = Boot application normally
#
# Uses libgpiod (gpioset/gpioget) which works on RPi 5.
#
# Usage:
#   ./dl_pin.sh low    # Pull D/L low (bootloader mode)
#   ./dl_pin.sh high   # Release D/L high (normal boot)
#   ./dl_pin.sh status # Show current state

GPIO_CHIP="gpiochip0"
GPIO_PIN=17

case "$1" in
    low)
        # Drive D/L pin low - CC2630 will enter bootloader on next reset
        gpioset -c "$GPIO_CHIP" "$GPIO_PIN"=0 &
        GPIOSET_PID=$!
        echo "$GPIOSET_PID" > /tmp/dl_pin_gpioset.pid
        echo "D/L pin LOW (PID $GPIOSET_PID) - device will enter bootloader on reset"
        ;;
    high)
        # Release D/L pin (kill the gpioset process to release the line)
        if [ -f /tmp/dl_pin_gpioset.pid ]; then
            PID=$(cat /tmp/dl_pin_gpioset.pid)
            kill "$PID" 2>/dev/null
            rm -f /tmp/dl_pin_gpioset.pid
            echo "D/L pin released (was PID $PID) - device will boot normally"
        else
            echo "D/L pin not actively driven (no gpioset process found)"
        fi
        ;;
    status)
        if [ -f /tmp/dl_pin_gpioset.pid ]; then
            PID=$(cat /tmp/dl_pin_gpioset.pid)
            if kill -0 "$PID" 2>/dev/null; then
                echo "GPIO $GPIO_PIN: actively driven LOW (PID $PID)"
            else
                rm -f /tmp/dl_pin_gpioset.pid
                echo "GPIO $GPIO_PIN: not driven (stale PID file cleaned up)"
            fi
        else
            echo "GPIO $GPIO_PIN: not driven"
        fi
        ;;
    *)
        echo "Usage: $0 {low|high|status}"
        echo "  low    - Pull D/L low for bootloader entry"
        echo "  high   - Release D/L for normal boot"
        echo "  status - Show current GPIO state"
        exit 1
        ;;
esac
