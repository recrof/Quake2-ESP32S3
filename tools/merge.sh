#!/bin/sh
# Build and merge the bootloader, partition table and firmware into one image to flash at 0x0:
# [BUILD=build-tdeck] tools/merge.sh [out.bin]   (default dist/quake2-<build dir>.bin)
# The flash asset image is not included: it is made from the retail PAK files (not
# redistributable), see tools/mkassets.py.
cd "$(dirname "$0")/.."
. tools/idf_env.sh
BUILD=${BUILD:-build}
if ! ninja -C "$BUILD" > "$BUILD/ninja.log" 2>&1; then
    grep -E "error|undefined reference" "$BUILD/ninja.log" | grep -v "^/.*-c /" | sort -u | head -30
    echo "BUILD FAILED"
    exit 1
fi
OUT=${1:-dist/quake2-$(basename "$BUILD").bin}
mkdir -p "$(dirname "$OUT")"
OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
cd "$BUILD"
python -m esptool --chip esp32s3 merge_bin -o "$OUT" @flash_args 2>&1 | grep -iE "wrote|error" || true
ls -l "$OUT"
