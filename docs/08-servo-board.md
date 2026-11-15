# Servo board — plan for a 4-servo remote on a 12 V input

Date: 2026-11-15 · **A plan, not a build. Nothing here has been wired.** Part numbers are
suggestions to check against their datasheets, not tested parts. Measured figures are marked
and come from `docs/07-servo-s51.md` and the bench on 2026-11-15.

The board carries a XIAO ESP32C3 running `firmware/jaw/` and up to **4 servos** (channels 1..4 on
D10, D1, D2, D3). It is fed from a **12 V power brick or the TalentCell 12 V battery**, whichever
is available, and makes its own 5 V.

---

## 1. Power budget

| Load | At 5 V | Basis |
|---|---|---|
| XIAO, radio on, idle | **94 mA** | measured 2026-11-15, meter in series with the 5V pin |
| Servo holding | ~3 mA each | measured (S51, no load) |
| Servo moving, free lever | ~61 mA each | measured, MAX mode (S51) |
| Servo stalled | **459 mA** each | measured, lever held by hand (S51) |

- Typical show: the XIAO plus a few servos moving, about **0.2–0.4 A at 5 V**.
- Worst case, all four stalled: about **1.8 A + 0.1 A**. Set `loDeg`/`hiDeg` so this does not
  happen, but size the supply for it anyway: **a 5 V converter rated for at least 3 A.**
- From the 12 V source that is roughly **0.1 A typical and 0.2 A worst case at 12 V**
  (5 V × current ÷ 12 V ÷ ~0.85 efficiency; not measured).

## 2. Power path

```
 12 V in (barrel jack or screw terminal)
   │
   ├─ fuse (~2 A, resettable) ─ reverse-polarity protection ─ TVS across the input
   │
   └─ 5 V buck converter, >= 3 A ──┬── servo bus: 470–1000 µF bulk cap ──> 4 servo headers (+)
                                   │
                                   └── Schottky (1N5819) ── XIAO 5V pin (+ 10–47 µF at the pin)

 GND: one star point. Servo ground returns and the XIAO GND meet there; the buck's GND too.
```

- **Servo power never passes through the XIAO.** The XIAO has its own diode-fed tap so a servo
  starting or stalling cannot reset it. Seeed requires the series Schottky on the 5V pin.
- **Converter:** pick one with a wide input (a 12 V battery reads about 12.6 V full and a brick
  may sit above 12 V unloaded), for example a Pololu D36V28F5-class 5 V, 3.2 A step-down
  (6–50 V in). Check the datasheet. Cheap LM2596-class modules work but are noisy.
- **Keep the converter away from the XIAO's antenna.** Its inductor radiates, and the link is the
  whole point.
- **Stay at 5 V.** The S51 allows 6 V but `docs/07` says not to run it there on a guess.
- **Input protection is for a borrowed prop.** Bricks and batteries are both easy to connect
  backwards, and a brick can spike. Use a series diode or P-channel MOSFET for polarity, a TVS
  (SMBJ15A-class) across the input, and a ~2 A resettable fuse. **Meter the brick's polarity**
  before the first connection: barrel-jack centre polarity is not universal.

## 3. Servo connectors and signal

One **3-pin 0.1" male header per servo**, in the S51's own order so the servo plugs straight in
(no crossed wires, unlike the JST adapter in `docs/07` §8):

| Header pin | Wire (S51) | Goes to |
|---|---|---|
| 1 | orange, signal | XIAO pin (table below), 10 kΩ to GND |
| 2 | red, +5 V | the servo bus (after the bulk cap) |
| 3 | brown, GND | the GND star point |

| Channel | XIAO silkscreen | GPIO |
|---|---|---|
| 1 | D10 | GPIO10 |
| 2 | D1 | GPIO3 |
| 3 | D2 | GPIO4 |
| 4 | D3 | GPIO5 |
| *never* | D0, D8, D9 | GPIO2, 8, 9 — strapping, see CLAUDE.md |

3.3 V signals drive the S51 directly (measured); no level shifter. A 100–330 Ω series resistor on
each signal is optional protection. Add a **100 nF cap across each header's + and GND**.

Spare pins: D4, D5 (GPIO6, 7) are free for a motion sensor or two more servos if the firmware
cap is raised from 4. D6/D7 are the UART pins.

## 4. Before first power

1. Power from the brick with the **XIAO and servos unplugged**; meter 5 V on the servo bus and
   at the XIAO's diode output (about 0.3 V lower), and 0 V on every signal pin.
2. Plug the XIAO in alone and check it comes up and answers on the radio.
3. Plug servos in one at a time. Meter the signal pin on each header with `jaw.ino` running.
4. Confirm with the meter that the brick's centre-pin polarity matches the board's before use.

## 5. Still unknown

- Whether the TalentCell's 12 V output keeps running at this small a draw (around 0.1 A at 12 V);
  some batteries cut off at low current. Not measured.
- The converter's behaviour under a four-servo stall, and the bus voltage at the servos then.
- Real runtime on the battery.
