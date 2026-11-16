# Servo board — plan for a 4-servo remote on a 12 V input

Date: 2026-11-15 · **A plan, not a build. Nothing here has been wired.** Part numbers are
suggestions to check against their datasheets, not tested parts. Measured figures are marked
and come from `docs/07-servo-s51.md` and the bench on 2026-11-15.

The board carries a XIAO ESP32C3 running `firmware/jaw/` and up to **4 servos** (channels 1..4 on
D10, D1, D2, D3). It is fed from a **12 V power brick or the TalentCell 12 V battery**, whichever
is available, and makes its own 5 V.

---

![Servo board wiring](../images/servo-board.png)

*`../images/servo-board.svg` is the source. Pin order on the XIAO is simplified; use its silkscreen.*

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

D4 and D5 (GPIO6, 7) are the two sensor inputs (§7). D6/D7 are the UART pins and are left unused:
the chip prints boot messages on one of them.

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

---

## 6. Parts list

**Inventory:** no personal inventory file turned up under `/home/shschubert`. The "On hand?" column
uses only what two project files say: **OWNED** is from the SFX Box BOM
(`SFX box/reference/sfx-box-bom-v3.xlsx`), **[bin?]** is `docs/05-parts.md`'s own guess, and
**?** means no file says. Check it against your shelf.

**Part numbers are from memory and are suggestions. Check each against its datasheet or listing
before ordering.**

| # | Part | Part number / spec | Qty | On hand? | Notes |
|---|---|---|---|---|---|
| 1 | Controller | Seeed XIAO ESP32C3 (SKU 113991054) | 1 | **yes** (2 per `05-parts.md`; one is the bar) | the jaw board already runs `jaw.ino` |
| 2 | 5 V buck converter | **MP1584EN module** (the EBOOT 6-pack) | 1 | **OWNED** (SFX BOM) | set to 5.0 V *before* connecting anything. Fine for a typical show. Marginal for four servos stalling at once (~1.9 A): these are rated 3 A peak but run hot near that |
| 2b | *or* a stronger buck | Pololu D36V28F5, 5 V 3.2 A step-down, 6–50 V in (Pololu #3782) | 1 | ? | the safe choice if you want the all-stalled worst case covered |
| 3 | Schottky, XIAO 5V feed | 1N5819 (DO-41) | 1 | ? (bar board uses one) | Seeed requires it on the 5V pin |
| 4 | Reverse-polarity diode | 1N5822 (3 A, 40 V) or SS54 | 1 | ? | in series at the 12 V input; costs ~0.4 V, harmless into a buck |
| 5 | TVS diode | SMBJ15A | 1 | ? | across the 12 V input, after the fuse |
| 6 | Resettable fuse | Bourns MF-R200 (2 A hold) | 1 | ? | on the 12 V input. Check its voltage rating against the 12.6 V battery |
| 7 | Bulk cap, servo bus | 1000 µF, 16 V electrolytic | 1 | **[bin?]** | at the buck output; 470 µF also acceptable |
| 8 | Input cap | 100 µF, 25 V electrolytic | 1 | ? | at the buck input |
| 9 | XIAO feed cap | 10–47 µF, 10 V | 1 | ? | at the XIAO 5V pin |
| 10 | Ceramic caps | 100 nF, 50 V | 5 | ? | one across each servo header's + and GND, one at the XIAO |
| 11 | Signal pull-downs | 10 kΩ, 1/4 W | 4 | **[bin?]** | one per signal pin, to GND |
| 12 | Signal series resistors (optional) | 100–330 Ω, 1/4 W | 4 | **[bin?]** | optional protection |
| 13 | Servo headers | 3-pin 2.54 mm male header (break from a 40-pin strip) | 4 | ? | signal, +5 V, GND, in the S51's own order |
| 14 | XIAO sockets | 7-pin 2.54 mm female header | 2 | ? | so the XIAO can be removed and reflashed |
| 15 | Power input | 5.5 × 2.1 mm barrel jack, panel or PCB mount, plus a 2-pin 5.08 mm screw terminal | 1 each | **OWNED** (barrel jacks and screw terminals, SFX BOM) | **meter the brick's polarity**; centre-positive is common, not universal |
| 16 | Board | perfboard, the size used for the bar | 1 | ? | |
| 17 | Wire | 20 AWG for the power bus, 22 AWG silicone for signals | — | **[bin?]** (22 AWG) | |
| 18 | Servos | Smraza S51, 9 g | 2–4 | **yes** (2 on the bench) | |
| 19 | Motion sensor (optional) | HC-SR501 PIR or VL53L1X ToF | 1 | **OWNED** (VL53L1X, SFX BOM); HC-SR501 listed as optional stock | the box handles triggers today; this is only for a standalone board |
| 19b | Sensor port parts (×2) | 3.5 mm stereo jack, 2N3904, 1 kΩ, 100 kΩ, 22 Ω, 4.7 µF, 100 nF | 2 sets | **?** (the SFX Box's ports use the same parts) | see §7 |
| 20 | 12 V sources | 12 V brick (5.5 × 2.1 mm) · TalentCell YB1206000 12 V battery | 1 each | **yes** (TalentCell, `05-parts.md`); the brick **OWNED** per the SFX BOM | the box's 12 V PSU is the same class of brick |

**Not a part, but needed:** a DMM for the first-power checks in §4 (you have the DM6000AR).

---

## 7. Two sensor ports (D4, D5)

Same port as the SFX Box's (`SFX box/sfx-box-wiring.md`, "Sensor daughter board"), so the same
sensor plugs into either: **3.5 mm stereo jack, Tip = +5 V, Ring = signal, Sleeve = GND.** A Neumi
motion sensor (3.3 V output on motion) or a plain push button wired Ring to Tip both work.

Per port: the 22 Ω from the 5 V bus feeds Tip (absorbing the momentary short as a plug seats,
with 4.7 µF + 100 nF at that node); Ring goes through 1 kΩ to the base of an NPN (2N3904), with
100 kΩ from the base to GND; the emitter is GND; the collector goes to the XIAO pin, whose
**internal pull-up** is on. The stage level-shifts and **inverts: motion pulls the pin LOW.**

**Firmware:** `firmware/jaw/jaw.ino` debounces both inputs (30 ms) and announces each change
unasked over the radio:

    ev sensor <n> <0|1> <seq>        1 = active (pin LOW), 0 = released

Each event goes out 3 times, 40 ms apart, with the same `seq`; the receiver ignores a repeat of
the same (n, state, seq). The bridge needs no change: it already prints anything a prop says,
tagged `jaw>`. `sens` reads the two inputs back (`sens 1=0 2=0`) and `st` carries `sens=00`.
**Not done:** the SFX Box does not yet read unsolicited `ev` lines or offer them as triggers.

Bench test without the circuit: the pins idle HIGH, so touching a wire from D4 or D5 to GND
looks like motion.

Parts for the two ports (copied from the SFX Box's list, per port): 1 × 3.5 mm stereo jack, 1 ×
2N3904 (MMBT3904 or BC547 also work), 1 × 1 kΩ, 1 × 100 kΩ, 1 × 22 Ω, 1 × 4.7 µF, 1 × 100 nF.
