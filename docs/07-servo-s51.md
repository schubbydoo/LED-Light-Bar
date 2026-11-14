# Smraza S51 micro servo (9 g) — reference

Date: 2026-11-14 · **Source: the seller's product listing (two images, transcribed). Every
number in §1–§4 is the datasheet's claim or worked out from it, not a measurement.** §5 holds
what has been measured on this servo since, and §6 what is still open.

This file is written to be reused: it is the S51's reference sheet, not part of the jaw design.
Another project that has an S51 can link or copy it as it stands.

---

## 1. Wiring

| Wire colour | Function | Goes to |
|---|---|---|
| **Orange** | Signal | a GPIO (on the jaw: XIAO **D10**) |
| **Red** | Positive (+) | the servo's own 5 V supply |
| **Brown** | Negative (−) | supply GND, **and** the controller's GND |

3-pin 0.1" female housing, signal / + / − in that order, orange on the signal end. This matches
the colours the listing draws.

**Colours on this build were not metered.** On the LED strip the wire colours did not match its
manual, and 5 V on the wrong pad killed a pixel. Meter the connector against the supply before
first power.

## 2. Electrical and mechanical (datasheet)

| | |
|---|---|
| Operating voltage | **4.8 – 6.0 V** — a 5 V rail is in range; there is no headroom to go lower |
| No-load speed | **0.09 ± 0.01 s / 60°** at 4.8 V · **0.08 ± 0.01 s / 60°** at 6 V |
| Stall torque | **1.4 kg·cm** at 4.8 V · **1.9 kg·cm** at 6 V |
| Travel | **180°** |
| Dead band | **≤ 5 µs** — the listing's "zero jitter" panel sets this against a "typical" 7 µs servo. Marketing, not a test |
| Operating temperature | −25 to +70 °C |
| Weight | 9 g ± 5 % |

### Dimensions (from the listing's drawings)

| | |
|---|---|
| Body footprint | **22.6 × 12 mm** (0.89 × 0.47 in) |
| Length across the mounting ears | **32 mm** (1.26 in) |
| Overall height | **30 mm** (1.18 in), to the top of the output shaft |
| Other heights marked on the side view | 18, 22.4 and 26.8 mm — the drawing does not say which feature each one reaches, so check on the part before designing a mount around any of them |
| Lead | **250 mm** (9.84 in), 3-pin female connector |

Horns in the box: a double arm **34 mm** long, a single arm **32 mm**, and a round disc
**20 mm** across. Their lengths are what set the jaw's travel against the linkage.

## 3. Control signal (datasheet)

Hobby PWM: one positive pulse per frame, and **the pulse width is the angle.**

| Pulse width | Angle |
|---|---|
| 0.5 ms | 0° |
| 1.0 ms | 45° |
| 1.5 ms | 90° |
| 2.0 ms | 135° |
| 2.5 ms | 180° |

Linear: **0.09° per µs**, i.e. 180° over 2000 µs. The listing quotes the range per controller:

| Controller | Pulse range | Travel |
|---|---|---|
| RC receiver | 1.0 – 2.0 ms | ≈ 120° |
| Robot arm | 1.0 – 2.0 ms | ≈ 120° |
| Arduino / Raspberry Pi | 0.5 – 2.5 ms | ≈ 180° |

So **the full 180° needs the wide 0.5–2.5 ms range**; a 1.0–2.0 ms signal reaches only ~120° of it.

The frame rate is **not stated** on the listing. 50 Hz (20 ms) is the convention for hobby servos.

**A dead band this small means the pulse itself must be clean.** A few µs of timing jitter in the
command is as large as the dead band, so the servo will chase it and the jaw will buzz. Generate
the pulse with the ESP32's hardware PWM (LEDC), not a software delay loop or a timer interrupt
that WiFi and ESP-NOW can disturb. This is a design note, not a measurement.

## 4. Figures worked out from the above (calculated, not measured)

- No-load slew: 60° in 90 ms at 4.8 V → **≈ 670 °/s**; 60° in 80 ms at 6 V → ≈ 750 °/s. A full
  180° sweep is **≈ 270 ms** at 4.8 V.
- At a 50 Hz command rate the servo can follow **≈ 13° per frame** at best, with no load. That is
  the ceiling for a slew limit; the real figure on a linkage will be lower.
- Dead band 5 µs ≈ **0.45°** of travel.

These are the no-load numbers. A jaw has a load.

## 5. Measured on this servo

**2026-11-14, `firmware/jawcheck/`, horn off, nothing on the output, 5 V.** Current read on a USB
power meter in line with the servo supply, so it averages and **misses short spikes**. The servo
was powered from a **breadboard 5 V / 3.3 V power module, fed over USB from a 2 A wall adapter**.
The module's rail voltage under load was not measured, and the adapter's 2 A is its rating, not a
measurement.

| Condition | Measured | Note |
|---|---|---|
| Powered, **no pulse** (floating signal, then no firmware) | 24–32 mA, swinging, no movement | not an idle figure — nothing was commanded |
| **Holding 1500 µs**, no load | **24–28 mA** | steady, no twitch |
| **Slow sweep**, 1000–2000 µs, no load | **peak 167 mA** | 20 µs per 40 ms (≈ 45°/s). Two sweeps, same result. Motion judged by eye as normal |

**Signal level: 3.3 V works.** The servo followed the XIAO's 3.3 V pulse on D10 with no level
shifter, at centre and through the sweep. So the jaw plan's "do not buy the SN74AHCT125N yet" holds.

So a slow, unloaded move peaks at about **6 times the holding current**. A faster move, a load, or
a stall will read higher, and **stall current is still unmeasured.** Nothing here was loaded.

## 6. What the listing does not say — still to measure

| Missing | Why it matters | Where it's measured |
|---|---|---|
| **Stall current** (the idle and slow-sweep figures are above) | decides the jaw's 5 V source, bulk cap and brownout risk | `docs/06-jaw-plan.md` §6 |
| Moving current at speed, and on a linkage | sizing the supply | same |
| Start-up inrush | a USB meter averages and will miss it, so watch for resets | same |
| Real end-stop positions | the 0.5 / 2.5 ms ends may sit past the mechanical stops, and driving into one stalls the servo | `firmware/jawcheck/` — widen the range gradually from the middle |
| Frame rate it accepts | not stated | same |

**A servo driven past its stop draws stall current until told to move.** Start narrow, around
1.5 ms, and widen the range while watching a power meter.

## 7. Using it on this project

- Jaw plan: `docs/06-jaw-plan.md`. 5 V only; **do not run it at 6 V on a guess**, though the
  datasheet allows it.
- Signal on **D10**, with a 10 kΩ pull-down so it does not twitch while the XIAO boots.
- Power does not come from the XIAO or the SFX Box's USB.
