#!/usr/bin/env bash
#
# Send one command line to a board and print what it says back.
#
#   ./send.sh bar show                 dump every live parameter
#   ./send.sh bar "set breath 1.4"     change one, see it immediately
#   ./send.sh bar "speak 4"            run a synthetic speech envelope
#   ./send.sh bar "env 0.8"            hold excitation, to judge the top end
#
# This exists because the fire has thirteen knobs and every one of them is
# judged by eye. Reflashing to try a number is a ninety-second round trip and
# breaks the thing that matters most — seeing the change happen against the
# version you just had in your head. Over a tuning session that is the whole
# difference between converging and guessing.
#
# Nothing sent here is persistent. The board comes up on its compiled defaults
# every time, deliberately: a light bar in someone else's prop must not retain
# state that surprises them in March. When a value is right, it goes in the
# sketch as a default and gets committed.

set -euo pipefail

BOX="${SFX_BOX:-admin@sfx.local}"
BOX_KEY="${SFX_BOX_KEY:-$HOME/.ssh/sfxbox-dev}"
PY="${SFX_ESPTOOL_PY:-\$HOME/esptool-venv/bin/python}"

MAC_BAR="F0:9E:9E:B2:A8:28"
MAC_BRIDGE="F0:9E:9E:B2:3D:38"

target="${1:-}"
shift || true
cmd="${*:-}"

case "$target" in
    bar)    mac="$MAC_BAR" ;;
    bridge) mac="$MAC_BRIDGE" ;;
    *)      echo "usage: ./send.sh {bar|bridge} <command>" >&2; exit 2 ;;
esac
[[ -n "$cmd" ]] || { echo "nothing to send" >&2; exit 2; }
port="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_${mac}-if00"

ssh -i "$BOX_KEY" "$BOX" "$PY - '$port' '$cmd'" <<'PY'
import sys, time, serial

port, cmd = sys.argv[1], sys.argv[2]
p = serial.Serial(port, 115200, timeout=0.2)
# Never assert the control lines. On the C3's native USB that can drop the chip
# into its download stub, which looks exactly like a hang.
p.setDTR(False)
p.setRTS(False)

p.reset_input_buffer()
p.write((cmd + "\n").encode())
p.flush()

# Read for a moment. Most replies are one line; `show` is a dozen.
t0 = time.time()
while time.time() - t0 < 1.5:
    line = p.readline()
    if not line:
        continue
    s = line.decode("utf-8", "replace").rstrip()
    if s.startswith(("src/", "ESP-ROM", "rst:", "load:", "entry ")):
        continue
    print(s, flush=True)
p.close()
PY
