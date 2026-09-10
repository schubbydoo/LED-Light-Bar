#!/usr/bin/env bash
#
# Build a sketch and flash it to one of the two XIAOs.
#
# Both boards live on the SFX Box's USB, not on a PC. That is not where this
# project expected them, and it turned out better: the box is a Linux machine
# with a shell on it, so building and flashing need no human in the loop. The
# only step that needs a person is looking at the strip.
#
# Build happens here, flashing happens on the box. The box gets `esptool` in its
# own venv and nothing else — a 1 GB Arduino toolchain does not belong on a
# machine whose day job is running a prop.
#
#   ./flash.sh bringup bar        build firmware/bringup, flash the light bar
#   ./flash.sh bridge bridge      build firmware/bridge,  flash the radio
#   ./flash.sh bringup bar full   full-chip image (bootloader + partitions + app)
#
# The third argument is rarely wanted. An app-only write is ~430 kB and a few
# seconds; a full image is 4 MB. Use `full` for a board that has never been
# flashed, or one whose partition table changed.
#
# --------------------------------------------------------------------------
# WHY THE BOARDS ARE ADDRESSED BY MAC
#
# Two identical XIAOs on one machine enumerate as ttyACM0 and ttyACM1 in
# whatever order they were plugged in — so a script that hard-codes ttyACM0
# flashes a different board depending on what happened at boot. Fortunately the
# ESP32-C3's USB descriptor carries its MAC, so /dev/serial/by-id/ gives each
# board a name that is stable across replugging, renumbering and reboots.
#
# Read a board's MAC with:
#     esptool --port <dev> --after no-reset read-mac
# --------------------------------------------------------------------------

set -euo pipefail

BOX="${SFX_BOX:-admin@sfx.local}"
BOX_KEY="${SFX_BOX_KEY:-$HOME/.ssh/sfxbox-dev}"
ESPTOOL="${SFX_ESPTOOL:-\$HOME/esptool-venv/bin/esptool}"
REMOTE_DIR="lightbar-fw"

# The two boards, by MAC. Replace a value here if a board is swapped out — that
# is the only place either address appears.
MAC_BAR="F0:9E:9E:B2:A8:28"
MAC_BRIDGE="F0:9E:9E:B2:3D:38"

FQBN="esp32:esp32:XIAO_ESP32C3"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

sketch="${1:-}"
target="${2:-}"
mode="${3:-app}"

if [[ -z "$sketch" || -z "$target" ]]; then
    sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
[[ -d "$HERE/$sketch" ]] || { echo "no such sketch: firmware/$sketch" >&2; exit 2; }

case "$target" in
    bar)    mac="$MAC_BAR" ;;
    bridge) mac="$MAC_BRIDGE" ;;
    *)      echo "target must be 'bar' or 'bridge', not '$target'" >&2; exit 2 ;;
esac
port="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_${mac}-if00"

command -v arduino-cli >/dev/null || {
    echo "arduino-cli not on PATH. See firmware/README.md." >&2; exit 1; }

build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT

echo "==> building firmware/$sketch"
arduino-cli compile -b "$FQBN" --output-dir "$build" "$HERE/$sketch"

if [[ "$mode" == "full" ]]; then
    image="$build/$sketch.ino.merged.bin"; offset="0x0"
else
    image="$build/$sketch.ino.bin";        offset="0x10000"
fi
[[ -f "$image" ]] || { echo "expected artifact missing: $image" >&2; exit 1; }

echo "==> shipping $(du -h "$image" | cut -f1) to $BOX"
ssh -i "$BOX_KEY" "$BOX" "mkdir -p ~/$REMOTE_DIR"
# Retry once. A 1.1 MB transfer failed with "Connection closed" exactly once
# and the flash silently did not happen — the board kept running its old
# firmware while everything downstream looked like a code fault. `set -e` DID
# abort this script correctly; what hid it was piping the script through
# `tail`, because a pipeline's exit status is the LAST command's.
# Do not pipe this script. Redirect it.
scp -q -i "$BOX_KEY" "$image" "$BOX:~/$REMOTE_DIR/$(basename "$image")" || {
    echo "==> transfer failed, retrying once" >&2
    sleep 2
    scp -q -i "$BOX_KEY" "$image" "$BOX:~/$REMOTE_DIR/$(basename "$image")"
}

echo "==> flashing $target ($mac) at $offset"
ssh -i "$BOX_KEY" "$BOX" \
    "$ESPTOOL --port '$port' --chip esp32c3 --after hard-reset \
     write-flash $offset ~/$REMOTE_DIR/$(basename "$image")"

echo "==> done. Watch it with:  ./monitor.sh $target"
