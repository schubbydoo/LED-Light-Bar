# RGB LED Box — Design Spec

> **⚠ Superseded 2026-09-04 for the LED and power sections.** The requirement changed to
> *battery powered, no extension cord*. The salvaged panel's ~9.6 V strings are a poor match
> for a battery, so the build moved to a 5 V addressable strip. See
> **`docs/02-build-and-power.md`**. The 433 MHz link design and the salvage analysis below
> are still current, and Section 3b still applies if this panel is ever used on a wall wart.

**Goal:** Reuse the RGB LED panel salvaged from a fog machine to build a standalone,
12&nbsp;VDC-only effect box that SFX_box commands over 433&nbsp;MHz.

**Status:** Draft v1 — *bench measurement in Section 6 must be done before ordering parts.*
Date: 2026-09-04

---

## 1. Inventory — what came out of the fog machine

| Item | Markings | Notes |
|---|---|---|
| RGB LED panel | `MD-6LED-A`, `A7721Z` | Frame-shaped PCB, two arms of 3 RGB emitters each (6 total). Pads at each arm end: `+`, `R`, `G`, `B`. Common anode. |
| Driver brick | `LED RGB DRIVER  MODEL: CLK-10` | IN: AC 85–265&nbsp;V. OUT: DC 9–11&nbsp;V, 300&nbsp;mA ±5%. `RF 433.92M`. `POWER RGB 9W`. Output wiring: **BLACK = V+, RED = R−, GREEN = G−, BLUE = B−** (confirms common anode). |
| RF daughterboard | `CLK-RF433A` | Piggybacks on the driver PCB. 6.7458&nbsp;MHz crystal (the standard 433.92&nbsp;MHz superhet LO part) + 8-pin SOIC receiver. Silkscreened pads: `5V`, `DATA`, GND. |
| Remote | 24-key "RF Wireless" | Colors, W, brightness ±, SMOOTH/FADE/STROBE/FLASH, ON/OFF. |

### Reuse verdict

- **LED panel — keep.** This is the whole point of the exercise.
- **Driver brick — scrap.** It is an isolated offline flyback (mains-referenced primary,
  visible transformer and Y-cap). There is no sane way to run it from 12&nbsp;VDC:
  85&nbsp;V is the lower limit of its input rectifier, and back-feeding the secondary
  bypasses the housekeeping supply that runs its MCU. It also violates the
  "no high voltage in this box" requirement by definition. Keep the plastic shell if
  it's a convenient size; bin the guts.
  **Safety:** if you do open or probe it further, leave it unplugged for several minutes
  first — the primary bulk cap holds mains voltage after disconnect.
- **`CLK-RF433A` daughterboard — technically reusable, but don't bother.** It is a plain
  OOK superhet receiver: 5&nbsp;V in, raw demodulated bitstream out on `DATA`. All the
  protocol decoding happened on the driver's MCU, not here. That's actually the right
  shape for our design — but it sits on the driver's secondary side with unknown supply
  decoupling and no datasheet, and a known-good **RXB6** module costs about \$3.
  Use a new RXB6; keep this board in the parts bin as a spare.
- **Remote — set aside.** We're using our own protocol (see Section 5), so the remote's
  codes are irrelevant. If you ever want it back, it's a firmware-only change: capture the
  key codes with the RXB6 and add a second decoder.

---

## 2. Architecture

```
 12 VDC in ──┬─ fuse 1 A ──┬─ reverse-polarity P-FET ──┬─ +12V rail
 (2.1 mm     │             │                           │
  barrel,    │          470 µF                         ├─ LED panel anode (+)
  center +)  │           25 V                          │
             └─ SPST switch                            ├─ CC sink R ──┐
                                                       ├─ CC sink G ──┤ PWM from ESP32
                                                       └─ CC sink B ──┘
                                                       │
                        buck (MP1584/LM2596) ──> +5 V ──┼─ ESP32 DevKit (VIN)
                                                        └─ RXB6 receiver (VCC)
                                                             DATA ─ divider ─> ESP32 GPIO
```

Everything runs off one 12&nbsp;V rail. No mains inside the box.

---

## 3. LED drive — why constant current, not resistors

The old driver's 9–11&nbsp;V compliance range is the tell: each color is almost certainly
**3 dies in series × 2 parallel strings** (3 emitters per arm, 2 arms). Three blue or green
dies in series is ≈9.6&nbsp;V; three red is ≈6.6&nbsp;V.

On a 12&nbsp;V rail that leaves only **~2.4&nbsp;V of headroom on blue/green**. A plain
series resistor there is a bad idea: a ±0.3&nbsp;V forward-voltage spread or a 20&nbsp;°C
temperature rise swings the current by tens of percent, and color balance drifts as the box
warms up. A current sink fixes the current regardless.

### Per-channel current sink (×3, one each for R, G, B)

Classic two-transistor sink plus a PWM shunt — four parts per channel, all through-hole:

| Ref | Part | Function |
|---|---|---|
| R1 | 10 kΩ | Gate pull-up from +12&nbsp;V |
| M1 | IRLZ44N or IRLB8721 (logic-level N-MOSFET, TO-220) | Pass element. Drain → LED cathode pad (R/G/B), source → Rs |
| Q1 | 2N3904 | Regulator. Base → M1 source, emitter → GND, collector → M1 gate |
| Rs | see below, 1/4 W | Sets the current: **I ≈ 0.65 V / Rs** |
| Q2 | 2N3904 | PWM shunt. Collector → M1 gate, emitter → GND, base → 1 kΩ → ESP32 GPIO |

**Rs starting values** (revise after the bench test in Section 6):

| Target total channel current | Rs | Rs dissipation |
|---|---|---|
| 100 mA | 6.8 Ω | 0.07 W |
| 140 mA | 4.7 Ω | 0.09 W |
| 200 mA | 3.3 Ω | 0.13 W |

**M1 dissipation** at 140&nbsp;mA: red channel is the hot one,
(12 − 6.6 − 0.65) × 0.14 ≈ **0.66 W** — a TO-220 with a small clip-on heatsink is fine.
Blue/green ≈ 0.25&nbsp;W each.

**PWM logic is inverted:** GPIO high turns Q2 on, which pulls M1's gate down and turns the
channel *off*. Either invert duty in firmware or use the ESP32 LEDC channel's invert option.

**⚠ Headroom decision gate.** If the bench test shows blue/green string Vf above ~10.5&nbsp;V,
the 12&nbsp;V rail leaves M1 less than ~1&nbsp;V of Vds and it will fall out of regulation.
Two outs: (a) move to a 15&nbsp;V supply (still low voltage, still safe), or (b) cut the
panel's series traces and rewire each color as 2 series × 3 parallel. Decide *after* measuring.

---

## 4. Controller — ESP32

- **Board:** ESP32-WROOM-32 DevKitC. Feed the buck's 5&nbsp;V into `VIN` (not `3V3`).
- **PWM:** LEDC peripheral, **1 kHz, 10-bit** on three GPIOs. 1&nbsp;kHz is well above
  flicker perception, easy on the linear sinks, and won't beat with camera shutters at
  common frame rates. If you plan to film the prop, 2–4&nbsp;kHz is safer still.
- **Gamma:** apply a γ≈2.2 lookup before writing duty, otherwise fades bunch up at the
  bottom of the range and look stepped.
- **Suggested pins:** R=GPIO25, G=GPIO26, B=GPIO27, RXB6 DATA=GPIO14, status LED=GPIO2.
  Avoid GPIO 6–11 (flash) and the strapping pins 0/2/12/15 for the RF data line.
- **Future upside:** WiFi/ESP-NOW is already on the board if 433 ever proves too flaky —
  no hardware change needed.

---

## 5. RF link — 433 MHz, own protocol

**Receiver (this box):** RXB6 superheterodyne module. Power at 5&nbsp;V for best sensitivity;
its DATA output is then 5&nbsp;V logic, so level-shift into the ESP32 with a
**10 kΩ / 20 kΩ divider** (or a 1 kΩ + 2 kΩ). Do not feed 5&nbsp;V straight into a GPIO.

**Transmitter (SFX_box):** FS1000A / STX882. These get noticeably more range at 12&nbsp;V
than at 5&nbsp;V — worth doing if SFX_box has a 12&nbsp;V rail.

**Antennas:** 17.3&nbsp;cm of straight solid wire on each end (quarter wave at 433.92&nbsp;MHz).
Route it away from the buck converter and out of any metal enclosure — the single biggest
range killer on these builds is a coiled antenna sitting next to a switching regulator.

**Library:** RadioHead `RH_ASK` at 2000&nbsp;bps is the path of least resistance and works on
ESP32. OOK has no ACK, so **send every command 3–5 times** with ~20&nbsp;ms spacing.

**Suggested packet** (8 bytes, fixed length, easy to eyeball on a scope):

```
[0] 0xA5    magic
[1] boxID   1..255  (lets SFX_box address several boxes)
[2] cmd     0x01 SET_RGB | 0x02 FADE_TO | 0x03 STROBE | 0x04 FLICKER | 0x00 OFF
[3] R       0..255
[4] G       0..255
[5] B       0..255
[6] param   fade time (×10 ms) / strobe rate — command-specific
[7] csum    XOR of bytes 0..6
```

Include a **failsafe**: if no valid packet for N seconds during a show, hold last state
(don't blank) — a dropped packet shouldn't kill the effect mid-scene.

**Coexistence note:** your Traffic Light Controller is also a field-deployed wireless product.
If it's on 433&nbsp;MHz, check that the two don't collide — the `boxID` byte and the magic
header make cross-talk harmless, but a busy channel still costs you range.

---

## 6. Bench test — DO THIS FIRST

> **See `docs/archive/bench-test-procedure.md`** for the full step-by-step version using a 12 V battery,
> a DMM and resistors (no bench supply required), including a recording sheet.

Nothing above is safe to order parts against until the panel is characterized.
Roughly 30 minutes with a DMM and a current-limited bench supply.

1. **Confirm common anode.** Continuity between the two arms' `+` pads → they're bussed.
   Diode-test mode from `+` to each of `R`, `G`, `B`. A reading well above the meter's
   single-junction range (or no reading at all — many DMMs top out around 2&nbsp;V) means
   multiple dies in series, which is what we expect.
2. **Find the string topology and Vf.** Current-limited supply, **limit set to 30&nbsp;mA**.
   `+` pad to supply positive, one color pad to supply negative. Raise voltage slowly from
   0 until the supply enters current limit. Record the voltage — that's Vf at 30&nbsp;mA.
   Repeat for all three colors.
   - Vf ≈ 6–7&nbsp;V on red and 9–10&nbsp;V on blue/green ⇒ 3-series confirmed.
   - Vf ≈ 2&nbsp;V / 3&nbsp;V ⇒ all parallel, and a 12&nbsp;V rail needs much bigger
     ballast resistance. Different design; tell me and I'll revise.
3. **Sweep current.** Record Vf at 50, 100, 150, 200&nbsp;mA per color. Stop early if any
   emitter looks strained or the PCB gets hot to the touch.
4. **Thermal soak.** Run the brightest color at your intended current for 10 minutes.
   Measure PCB temperature near an emitter. Above ~60&nbsp;°C, back the current off.
5. **Pick the operating point** — brightness you actually want, not maximum. These were
   driven at 300&nbsp;mA *total* by the original brick, which is a reasonable ceiling.
6. **Write the numbers into Section 3's table** and we finalize Rs, rail voltage, and heatsinking.

---

## 7. Bill of materials (provisional)

| Qty | Part | Notes |
|---|---|---|
| 1 | ESP32-WROOM-32 DevKitC | Controller |
| 1 | RXB6 433.92 MHz superhet receiver | Plus 17.3 cm wire antenna |
| 1 | MP1584EN or LM2596 buck module | 12 V → 5 V, set with the trimmer *before* connecting the ESP32 |
| 3 | IRLZ44N or IRLB8721 | Logic-level N-MOSFET, TO-220 |
| 1 | Small clip-on TO-220 heatsink | For the red channel FET |
| 4 | 2N3904 | 3 regulators + spares |
| 3 | Rs, value TBD after bench test | 1/4 W metal film; parallel two for odd values |
| 3 | 10 kΩ 1/4 W | Gate pull-ups |
| 3 | 1 kΩ 1/4 W | PWM base resistors |
| 1 | 10 kΩ + 20 kΩ | RXB6 DATA level shifter |
| 1 | 470 µF 25 V electrolytic | Bulk on the 12 V rail |
| 2 | 0.1 µF ceramic | Decoupling at the buck and RXB6 |
| 1 | 1 A slow-blow fuse + holder | |
| 1 | SI2301 / IRF9540 P-MOSFET | Reverse-polarity protection (a 1N5819 Schottky also works, costs 0.4 V) |
| 1 | 2.1 mm barrel jack, panel mount | Center positive |
| 1 | 12 V 2 A wall supply | Regulated, not a wall wart with a transformer |
| 1 | SPST panel switch | |
| 1 | Project enclosure | Non-metal, or bring the antenna outside |
| — | Diffuser | Frosted acrylic or a sheet of 3D-printed white PETG over the panel |

---

## 8. Open items

- Bench numbers from Section 6 → finalizes Rs, rail voltage, heatsinking.
- Does SFX_box already have a 433 TX and a packet format from the TLC work? If so, this box
  should adopt it rather than invent a second one.
- Enclosure form factor and how the frame-shaped panel mounts — the PCB has a hollow center,
  which might be worth designing around (backlit ring, something visible through the middle).
- Whether you want a local override — a button on the box for bench testing without SFX_box.

---

## 3b. Off-the-shelf driver options (added 2026-09-04)

### Important constraint: the panel is COMMON ANODE

This rules out the popular cheap buck LED-driver modules — **PT4115 / AL8805 / RCD-24** and
friends. They all put their current-sense resistor on the *anode* side, so each channel needs
independent access to both ends of its LED string. With three colors sharing one anode node,
current from one channel's sense resistor leaks into the other channels' strings and none of
them regulate. (This is a well-known dead end — see the Arduino forum thread linked at the
bottom of this file.)

Anything that drives this panel must be a **low-side current sink** or a **low-side switch**.

### Option A — TLC5947 breakout (Adafruit) — "buy one part and it's done"

24 constant-current *sinks*, LED supply 5–30 V, 12-bit PWM generated on-chip, SPI from the
ESP32, and a single reference resistor sets the current for all channels.

- Factory-set to 15 mA/channel, 30 mA/channel max. **Gang 8 channels per color** (tie 8
  outputs together) → up to 240 mA per color. Standard, accepted practice.
- Uses one SPI port instead of three ESP32 PWM channels, and the driver handles supply
  fluctuation automatically.
- **Caveat — heat.** The sinks are linear, so the headroom voltage is burned inside the chip.
  On a 12 V rail the red string (~6.6 V) leaves 5.4 V × 0.24 A ≈ **1.3 W** in one TSSOP.
  Fix: put a **small series resistor in the red leg only** to drop ~4 V outside the chip
  (≈16 Ω, 1 W at 240 mA). Green and blue need nothing.

### Option B — 3-channel logic-level MOSFET board + one ballast resistor per color

The cheapest path (~$8 for a 4-channel MOSFET breakout) and honestly fine for a prop.
ESP32 generates the PWM directly; the board is just three low-side switches.

- Weakness: resistor ballast is temperature-sensitive when the blue/green strings sit at
  ~9.6 V on a 12 V rail — only ~2.4 V across the resistor, so Vf drift moves the current a lot.
- **This weakness largely disappears at 15 V.** The requirement was *no mains inside the box*,
  and 15 V (or 24 V) DC is just as safe as 12 V. At 15 V there's ~5.4 V across the blue
  ballast, the resistor dominates the string, and the current stops wandering as the panel heats.
  Cost is a bit more heat in the resistors — a fair trade for far fewer parts.

### Option C — discrete current sinks (Section 3 above)

~$1.50 per channel, fully constant-current, works at 12 V. Only worth the parts count if
Options A and B both come up short after measurement.

### Recommendation

**Option B on a 15 V rail** unless the bench numbers show unusually wide Vf spread between the
two arms, in which case **Option A**.

---

## 0. Buy this first: an adjustable CC/CV supply

Every number in Section 6 depends on limiting current safely, and a current-limited supply
*is* the test — set the limit to 50 mA, dial the voltage up from zero, and read Vf and current
straight off the display. No resistors, no math, and nothing can be damaged because the limit
holds.

| Tier | What | Notes |
|---|---|---|
| ~$10–15 | "XL4015 CC/CV buck module with display" fed from a wall brick | Two pots: output voltage and current limit. **Buck only** — output is always below input, so feed it from a **19 V laptop brick** if you have one, so you can reach above 12 V when needed. |
| ~$50–80 | Korad KA3005P, Wanptek/Kungber 30 V 5 A, Riden RD6006 | A real bench supply. Given how much prop electronics you do, this is the highest-leverage tool purchase on the list — it answers every future LED, servo and Pi-power question in 30 seconds. |

With one of these, `docs/archive/bench-test-procedure.md` collapses to: set current limit, raise voltage
until the limit engages, record the voltage. The battery-and-resistor method in that file
stays as the no-purchase fallback.

---

## References

- PT4115 vs. common-anode RGB — <https://forum.arduino.cc/t/driving-common-anode-rgb-led-with-pt4115/420972>
- TLC5947 / TLC59711 power and LED wiring — <https://learn.adafruit.com/tlc5947-tlc59711-pwm-led-driver-breakout/power-and-leds>
