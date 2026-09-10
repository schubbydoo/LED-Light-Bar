# firmware/

Two sketches, two ends of one radio link, versioned together — see the note in
`../CLAUDE.md` for why the bridge lives here rather than in the SFX Box's repo.

| | |
|---|---|
| `metercheck/` | Steady DC on D10 so a multimeter can prove the level shifter. **Run this before the strip is ever connected.** |
| `bringup/` | Five staged hardware questions, with the strip attached. |
| `pixelcheck/` | Whole-strip primaries and fixed markers, held. For when `bringup` raises a question about one pixel or one channel — it removes motion from the answer. |
| `bar/` | The light bar — the ambience renderer. **Tunable live over serial.** |
| `bridge/` | The SFX Box's radio — serial ⇄ ESP-NOW. **Working.** |

---

## Where the boards are

**Both XIAOs are plugged into the SFX Box's USB, not into a PC.** That was not
the original plan and it is better than the original plan: the box is a Linux
machine with a shell on it, so building, flashing and reading serial need no
human in the loop. The only step that needs a person is looking at the strip.

Two identical boards enumerate as `ttyACM0` and `ttyACM1` in whatever order they
were plugged in, so **nothing here addresses a board by `ttyACM` number** — that
flashes a different board depending on what happened at boot. The ESP32-C3's USB
descriptor carries its MAC, so `/dev/serial/by-id/` gives each one a name that
survives replugging, renumbering and reboots.

| Board | MAC | Why it matters |
|---|---|---|
| **bar** | `F0:9E:9E:B2:A8:28` | the renderer, in the light bar |
| **bridge** | `F0:9E:9E:B2:3D:38` | on the Pi's USB; **this is the peer address the bar transmits to** |

Read a board's MAC without flashing it:

```bash
esptool --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00 \
        --after no-reset read-mac
```

Both addresses appear in exactly one place each in `flash.sh` and `monitor.sh`.
Swap a board, change them there.

---

## Build and flash

Build happens on the workstation, flashing happens on the box. The box gets
`esptool` in its own venv and nothing else — a 1 GB Arduino toolchain does not
belong on a machine whose day job is running a prop.

```bash
./flash.sh bringup bar          # build firmware/bringup, flash the light bar
./flash.sh bridge bridge        # build firmware/bridge,  flash the radio
./flash.sh bringup bar full     # full-chip image, for a board never flashed before

./monitor.sh bar                # follow serial until interrupted
./monitor.sh bar 30             # ...for 30 seconds
./monitor.sh bar 30 raw         # ...keeping the boot ROM and FastLED chatter

./send.sh bridge "amb water"    # over the AIR — this is the one you want
./send.sh bridge st             # one-line state, answered over the air
./send.sh bar show              # every live parameter (resets the board, see below)
./send.sh bar "set breath 1.4"  # change one and watch it happen
./send.sh bar "speak 4"         # synthetic speech envelope, no radio needed
./send.sh bar "env 0.8"         # hold excitation, to judge the top end
```

### Tune over the RADIO, not over the bar's serial

**Opening the bar's serial port reboots it.** The C3's native USB-JTAG resets the
chip on connect, so every separate `send.sh bar "set ..."` applies one value to a
fresh set of compiled defaults and loses whatever came before. The live-tuning
loop does not work that way and never did.

Send through the bridge instead. That resets the *bridge*, which has no state
worth keeping, and the bar is never interrupted — so settings accumulate:

```
./send.sh bridge "set yellow 0.75"
./send.sh bridge "set brightness 90"
./send.sh bridge st
   bar> st amb=fire y=0.75 br=90 breath=1.10 rel=0.080 gain=1.20 env=0.00
```

`st` is the only reply that comes back over the air, and it exists because
`show` cannot: reading the bar's own serial to check what you just set reboots it
first, so the answer is always the defaults.

Several commands in one connection also works — `send.sh bar "set a 1" "set b 2"
st` — which is the workaround when the radio is not an option.

**A compile is 60-90 s, which is why `send.sh` exists.** The fire has thirteen
parameters and every one is judged by eye; reflashing to try a number breaks the
comparison you are holding in your head. Nothing sent is persisted — the board
boots on its compiled defaults every time, deliberately, because a bar that lives
in a borrowed prop must not retain state that surprises its owner in March. When
a number is right, it goes back into `bar.ino` and gets committed.

Note the two-minute foreground limit on tooling here: run `flash.sh` in the
background or it can be killed mid-build. It was, once — during compile, so
nothing was written, but a kill during the write would be worse.

An app-only write is ~430 kB and a few seconds; `full` is 4 MB. Use `full` for a
board that has never been flashed or whose partition table changed.

`SFX_BOX` and `SFX_BOX_KEY` override the box's address and key.

### Workstation setup

```bash
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
arduino-cli core update-index
arduino-cli core install esp32:esp32     # large — several minutes
arduino-cli lib install FastLED          # 3.6+; 3.10.5 is what these were built against
```

### On the box, once

```bash
python3 -m venv ~/esptool-venv
~/esptool-venv/bin/pip install esptool
```

A venv rather than `pip install --user`: Debian 13 enforces PEP 668 and a bare
`pip install` fails with *externally-managed-environment*. The same rule the SFX
Box's own `CLAUDE.md` states.

---

## Board settings that matter

FQBN `esp32:esp32:XIAO_ESP32C3`. Defaults are correct; two are worth knowing:

- **USB CDC On Boot — leave it Enabled** (it is the default for this board).
  Disabled sends `Serial` to the UART pins instead of USB, and every
  `Serial.println` in these sketches prints into the void while the board looks
  hung.
- **Flash 4 MB, partition scheme default.** The bring-up sketch uses 32 % of
  program space, so there is room for the fire renderer, ESP-NOW, and a LittleFS
  partition of envelope tracks later.

---

## Using the Arduino IDE instead

Arduino IDE is installed on the Windows side and can do all of this, with two
caveats. The sketches live in WSL, so open them by UNC path
(`\\wsl.localhost\Ubuntu\home\shschubert\LED-Light-Bar\firmware\...`) rather
than copying them into `Documents\Arduino` — a second copy is a copy that
drifts, which is the same argument that keeps the bridge sketch in this repo.
And the boards are on the Pi now, so the IDE would only see them if one were
moved back to the PC.

---

## Order of operations

The strip is the expensive part and it is unforgiving: 5 V on the DIN pad kills
pixel 0 instantly. So the chain is proven from the XIAO outward, with the strip
connected last.

1. **`metercheck`, strip disconnected.** Rails, then D10 held LOW and HIGH while
   you probe U1 pins 2 and 3. Pin 3 at ~5 V while pin 2 is at 3.3 V proves the
   whole level-shifter stage at once — the D10 wire, 1OE actually grounded, VCC
   actually 5 V, and the part actually being an HCT rather than an HC.
2. **`bringup`, strip connected.** One pixel, then colour order, then a walk down
   the run, then the ends, then the whole strip under load.
3. `bar` — the fire.

`metercheck` parks D10 as an input for its first ten seconds. Driving a pin into
an unpowered buffer pushes current through the input's clamp diode into a dead
VCC net, which TI rates at 20 mA and a bare GPIO can exceed. Small risk, free to
avoid: **power the TalentCell before or with USB, never after.**

## Status

**Both sketches compile and run on the bar board, 2026-09-10.** The board boots,
FastLED claims the RMT peripheral on GPIO 10 with correct WS2812 bit timing, and
`bringup` cycles all five stages without resetting — including the full-strip
amber load. But **no strip has been connected yet**, so what any of it *looks*
like is still unknown, and the load stage so far proves only that the sketch does
not reset the board on its own.

**bar/ and bridge/: empty.** Next.
