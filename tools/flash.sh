#!/bin/sh
# Build and flash: tools/flash.sh   (PORT=/dev/cu.usbmodemXXX to override the port)
cd "$(dirname "$0")/.."
. tools/idf_env.sh
# with several boards connected the port must be given: never guess
if [ -z "$PORT" ]; then
    n=$(ls /dev/cu.usbmodem* 2>/dev/null | wc -l)
    if [ "$n" -ne 1 ]; then
        echo "$n USB serial ports, set PORT=/dev/cu.usbmodemXXX"
        exit 1
    fi
    PORT=$(ls /dev/cu.usbmodem*)
fi
BUILD=${BUILD:-build}
if ! ninja -C "$BUILD" > "$BUILD/ninja.log" 2>&1; then
    grep -E "error|undefined reference" "$BUILD/ninja.log" | grep -v "^/.*-c /" | sort -u | head -30
    echo "BUILD FAILED"
    exit 1
fi
grep "binary size" "$BUILD/ninja.log"
# the serial logger must release the port (restart it afterwards)
pkill -f serial_daemon.py && sleep 1
cd "$BUILD"
python -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before default_reset --after watchdog_reset write_flash @flash_args 2>&1 | grep -E "Wrote|error|Error" || true
# the flash asset image (tools/mkassets.py), when given: tools/flash.sh assets
if [ "$1" = "assets" ]; then
    python -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before default_reset --after watchdog_reset write_flash --flash_size 16MB 0x1d0000 ../gamedata/assets.bin 2>&1 | grep -E "Wrote|error|Error" || true
fi
