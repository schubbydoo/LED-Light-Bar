#!/usr/bin/env bash
#
# Watch one board's serial output.
#
#   ./monitor.sh bar             follow until interrupted
#   ./monitor.sh bar 30          stop after 30 seconds
#   ./monitor.sh bar 30 raw      keep the boot ROM and FastLED chatter
#
# Opening the port resets the board, so what you see always starts from its
# banner. That is usually what you want — the stages restart with you.
#
# By default the ESP boot ROM lines and FastLED's driver logging are filtered
# out. They matter exactly twice: when the board is crash-looping (the `rst:`
# reason changes) and when the RMT driver fails to claim the pin. Pass `raw`
# then.

set -euo pipefail

BOX="${SFX_BOX:-admin@sfx.local}"
BOX_KEY="${SFX_BOX_KEY:-$HOME/.ssh/sfxbox-dev}"
PY="${SFX_ESPTOOL_PY:-\$HOME/esptool-venv/bin/python}"

MAC_BAR="F0:9E:9E:B2:A8:28"
MAC_BRIDGE="F0:9E:9E:B2:3D:38"

target="${1:-}"
secs="${2:-0}"
raw="${3:-}"

case "$target" in
    bar)    mac="$MAC_BAR" ;;
    bridge) mac="$MAC_BRIDGE" ;;
    *)      echo "usage: ./monitor.sh {bar|bridge} [seconds] [raw]" >&2; exit 2 ;;
esac
port="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_${mac}-if00"

ssh -i "$BOX_KEY" "$BOX" "$PY - '$port' '$secs' '$raw'" <<'PY'
import sys, time, serial

port, secs, raw = sys.argv[1], float(sys.argv[2]), sys.argv[3] == "raw"
NOISE = ("src/", "ESP-ROM", "Build:", "rst:", "Saved PC", "SPIWP", "mode:",
         "load:", "entry ", "clk_drv", "configsip")

p = serial.Serial(port, 115200, timeout=1)
# Leave the control lines alone. Asserting them on a C3's native USB can drop
# the board into the download stub, which looks like a hang.
p.setDTR(False)
p.setRTS(False)

t0 = time.time()
try:
    while secs <= 0 or time.time() - t0 < secs:
        line = p.readline()
        if not line:
            continue
        s = line.decode("utf-8", "replace").rstrip()
        if not raw and s.startswith(NOISE):
            continue
        print(f"{time.time() - t0:6.1f}s  {s}", flush=True)
except KeyboardInterrupt:
    pass
finally:
    p.close()
PY
