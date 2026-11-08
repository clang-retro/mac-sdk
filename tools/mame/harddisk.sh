#!/bin/sh
# Boot a SCSI hard disk image in MAME and save a screenshot.
#
#   harddisk.sh MACHINE DISK.raw OUT.png SECONDS
#
# For System 7.0 and later, which do not fit on an 800K floppy. Make the
# disk image with your System on it and your app as the startup
# application (or in the Startup Items folder). MAME_ROMS names the
# folder of ROM sets (default ./roms) and MAME the emulator (default mame).
set -e
[ $# -eq 4 ] || { sed -n '2,9p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
DISK=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
echo "return { {$4, 'shot'} }" > "$W/steps.lua"
MACSTEPS="$W/steps.lua" timeout 900 "${MAME:-mame}" "$1" \
  -rompath "${MAME_ROMS:-$PWD/roms}" -hard1 "$DISK" \
  -video none -sound none -nothrottle -seconds_to_run $(($4 + 1)) \
  -snapshot_directory "$W/snap" -cfg_directory "$W/cfg" -nvram_directory "$W/nv" \
  -autoboot_script "$HERE/drive.lua" -skip_gameinfo >/dev/null 2>&1 || true
SHOT=$(ls "$W"/snap/*/*.png 2>/dev/null | head -1)
[ -n "$SHOT" ] || { echo "no screenshot" >&2; exit 1; }
cp "$SHOT" "$3"
