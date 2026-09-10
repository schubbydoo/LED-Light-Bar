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

case "$target" in
    bar)    mac="$MAC_BAR" ;;
    bridge) mac="$MAC_BRIDGE" ;;
    *)      echo "usage: ./send.sh {bar|bridge} <command>" >&2; exit 2 ;;
esac
[[ $# -gt 0 ]] || { echo "nothing to send" >&2; exit 2; }
port="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_${mac}-if00"

# Every command in ONE connection. Each new connection to the bar reboots it —
# the C3's native USB-JTAG resets the chip on connect — so a second invocation
# would find the compiled defaults again. Over the radio this does not apply.
printf -v _args '%q ' "$@"
ssh -i "$BOX_KEY" "$BOX" "$PY - '$port' $_args" <<'PY'
import sys, time, serial

port, cmds = sys.argv[1], sys.argv[2:]
# Set DTR and RTS low BEFORE opening. pyserial asserts them during open(), and
# on the C3's native USB that pulses the board into a reset — so clearing them
# afterwards is too late, the reboot has already happened.
#
# This mattered more than it sounds. Every `send.sh bar "set ..."` was resetting
# the fire to its compiled defaults and then applying one value, so only ever one
# setting was in force and the live-tuning loop did not actually work.
p = serial.Serial()
p.port = port
p.baudrate = 115200
p.timeout = 0.2
p.dtr = False
p.rts = False
p.open()

# Let the board finish rebooting BEFORE writing anything.
#
# Opening the port resets the C3, and a command written into that window sits in
# a buffer until some later session drains it. The symptom is replies that lag
# one command behind — you ask for the state and get the ack for whatever you
# sent last time, which reads as the link being slow rather than as the command
# never having been delivered. Wait for the boot, then clear whatever the boot
# printed, then send.
time.sleep(1.2)
p.reset_input_buffer()

for c in cmds:
    p.write((c + "\n").encode())
    p.flush()
    time.sleep(0.35)

# Read long enough for a round trip out to the bar and back, not just for the
# bridge's own acknowledgement.
t0 = time.time()
while time.time() - t0 < 2.0:
    line = p.readline()
    if not line:
        continue
    s = line.decode("utf-8", "replace").rstrip()
    if s.startswith(("src/", "ESP-ROM", "rst:", "load:", "entry ")):
        continue
    print(s, flush=True)
p.close()
PY
