# Servo board — plan for a 4-servo remote on a 12 V input

Date: 2026-11-15 · **A plan, not a build. Nothing here has been wired.** Part numbers are
suggestions to check against their datasheets, not tested parts. Measured figures are marked
and come from `docs/07-servo-s51.md` and the bench on 2026-11-15.

The board carries a XIAO ESP32C3 running `firmware/jaw/` and up to **4 servos** (channels 1..4 on
D1, D2, D3, D4). It is fed from a **12 V power brick or the TalentCell 12 V battery**, whichever
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
   └─ 5 V buck converter, >= 3 A ──┬── servo bus: 470–1000 µF bulk cap (the only cap on the servo side) ──> 4 servo headers (+)
                                   │
                                   └── Schottky (1N5819) ── XIAO 5V pin (+ 100 µF at the pin)

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
| 1 | orange, signal | XIAO pin (table below), nothing else on the line |
| 2 | red, +5 V | the servo bus (after the bulk cap) |
| 3 | brown, GND | the GND star point |

| Channel | XIAO silkscreen | GPIO |
|---|---|---|
| 1 | D1 | GPIO3 |
| 2 | D2 | GPIO4 |
| 3 | D3 | GPIO5 |
| 4 | D4 | GPIO6 |
| *never* | D0, D8, D9 | GPIO2, 8, 9 — strapping, see CLAUDE.md |

3.3 V signals drive the S51 directly (measured); no level shifter. A 100–330 Ω series resistor on
each signal is optional protection. There are **no per-servo pull-downs or caps**: the pull-downs did not stop the boot twitch when tried, and the one bulk cap serves all four servos.

D7 (GPIO20) and D10 (GPIO10) are the two sensor inputs (§7), on the opposite edge of the XIAO from the
servos. D7 is the UART receive pin and is free because the board talks over USB. D5 and D6 are unused (D6 is
the UART transmit pin and prints boot messages).

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
| 3 | Schottky, XIAO 5V feed | 1N5819 (DO-41) | 1 | **yes** (owned) | Seeed requires it on the 5V pin |
| 4 | Reverse-polarity diode | 1N5822 (3 A, 40 V); SB340 or 1N5823 also fit | 1 | **buying** | in series at the 12 V input; costs ~0.4 V, harmless into a buck. A 1N5819 (1 A) would run at its limit with all four servos stalled |
| 5 | TVS diode | **P6KE15A** (through-hole, unidirectional, no "CA" suffix) | 1 | **buying** | across the buck's IN+ and IN− pads, band to +. The SMD SMBJ15A is the same part electrically |
| 6 | Resettable fuse | 16 V plug-in PTC kit (0.5–10 A); use the **2 A** | 1 | **buying** (kit) | first part on the 12 V input. 16 V is fine for 12–12.6 V; check the brick's no-load voltage stays under 16 V. Worst-case draw on this side is about 1 A |
| 7 | Bulk cap, servo bus | 1000 µF, 16 V electrolytic | 1 | **[bin?]** | at the buck output; 470 µF also acceptable |
| 8 | Input cap | 100 µF, 25 V electrolytic | 1 | ? | at the buck input |
| 9 | XIAO feed cap | 100 µF, 10 V or higher | 1 | ? | at the XIAO 5V pin, after the Schottky; it only has to carry the XIAO (~100 mA, radio bursts higher) through short dips |
| 10 | Ceramic cap | 100 nF, 50 V | 1 | ? | one at the XIAO 5V pin |
| 11 | *(removed: signal pull-downs)* | | 0 | | not needed |
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

## 7. Two sensor ports (D7, D10)

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

Bench test without the circuit: the pins idle HIGH, so touching a wire from D7 or D10 to GND
looks like motion.

Parts for the two ports (copied from the SFX Box's list, per port): 1 × 3.5 mm stereo jack, 1 ×
2N3904 (MMBT3904 or BC547 also work), 1 × 1 kΩ, 1 × 100 kΩ, 1 × 22 Ω, 1 × 4.7 µF, 1 × 100 nF.

## 8. Layout on the SB400-style board

![Board layout](../images/servo-board-layout.png)

**The 12 V jack and the four servo headers are on a second board** (`images/servo-board-connectors.png`), the one that mounts in the box wall. Every
rounded tag in the layout (`12V+`, `S1`, `+5V` and so on) is a wire to the tag of the same name there. The two sensor
connectors are on the main board, so `J1.T` and `J1.R` are just wires between the connector and its sensor block.

![Connector board](../images/servo-board-connectors.png)

**The board, from the photos of both sides** (a BusBoard SB400-style board):

- **Three continuous strips down each side:** two bus strips close together (A outer, B), a gap, then a third (C)
  right next to the holes. They are separate nets, so they can be used for different things.
- **Every row is split into three islands: 5 holes | 2 holes | 5 holes.** The 2-hole island in the middle is its own
  net (drawn as a lighter pad in the layout). The XIAO's pins land one in each 5-hole island, so its body sits over the
  middle one.
- About 30 rows along the strips. The layout needs **26**, so the board can be cut after row 26 (a couple of rows off the end)
  and the offcut used for the connector board. Cut through a gap in the copper, not along a strip.

**How I used the strips (both sides, mirrored):** A = **+5 V**; B and C = **GND**, tied together with a short wire at
row 1 so every ground connection has a strip right beside the holes. The 1000 µF sits across A and B, which are
adjacent. **Each side is fed straight from the buck:** the buck has two OUT+ and two OUT− pads, so one pair goes to the left
strips and the other pair to the right strips, and no wire bridges the two sides. The `+5V` and `GND` tags to the
connector board leave from the right-hand strips at the top, so the heavy servo current never runs down a strip.

**Why the XIAO is turned so the USB end points at the bottom:** the four servo pins D1–D4 are on the right edge
of the XIAO and the sensor pins D7 and D10 are on the left. The sensor circuit for D7 sits in the left island and the
one for D10 in the right island, as mirror images, so the two blocks are built the same way and each collector wire is
short. Rotate the whole board 180° in the box if the USB end should face elsewhere.

**XIAO position:** the XIAO is 7 columns wide. Its left pins are in column 4 of the left island and its right pins in column 10 (columns
1–5 left, 6–7 middle, 8–12 right), rows 14–20. That leaves **three free holes on the left** (columns 1–3, where the
power parts are) and **two on the right** (columns 11–12). The sensor blocks sit above it in rows 3–14. The two
sensor connectors (3-pin, on the board) are below it in rows 23–25.

| Rows | Left pin | Right pin |
|---|---|---|
| 14 | **D7** sensor 1 | D6 (unused) |
| 15 | D8 (avoid) | D5 (unused) |
| 16 | D9 (avoid) | **D4** servo 4 |
| 17 | **D10** sensor 2 | **D3** servo 3 |
| 18 | 3V3 (unused) | **D2** servo 2 |
| 19 | GND | **D1** servo 1 |
| 20 | 5V | D0 (avoid) |

### Hole list (row, column; the left island is columns 1–5, the right island 8–12; strips are `A`, `B`, `C`, left or right)

**12 V side, off the board.** The 12 V parts sit above the buck module and the 5 V side below it, so the
buck is the only thing between them: barrel jack + → PTC fuse → 1N5822 (band toward the buck) → buck IN+,
with the 100 µF 25 V and the TVS (P6KE15A, band to +) from that node to GND, and jack GND → buck IN−. Build
it on the buck's own bit of board or wire it point to point. Nothing from this chain goes on the SB400 board.

**Strips and buck feed**

| Part | From | To |
|---|---|---|
| Buck OUT+ (first pad) | left A, row 1 | |
| Buck OUT− (first pad) | left B, row 1 | |
| Buck OUT+ (second pad) | right A, row 1 | |
| Buck OUT− (second pad) | right B, row 1 | |
| B-to-C ground jumpers (2 short wires) | left B to left C, row 1 | right B to right C, row 1 |
| 1000 µF 10 V+ (+ to A) | right A, row 13 | right B, row 13 |
| Tag `GND` (20 AWG) | right C, row 1 | |
| Tag `+5V` (20 AWG) | right A, row 2 | |

Four stalled servos draw about 1.9 A. The two 20 AWG tag wires take that current from the right-hand strips right
where the buck feeds them, which is why nothing heavy runs down a strip. A GND wire between left C and right C is no
longer needed; if you already fitted one, it does no harm.

**Sensor circuits (two mirror-image blocks, rows 3–14).** The left block serves sensor 1 and goes to **D7**; the
right block serves sensor 2 and goes to **D10**. Columns are counted from the block's own side: "k1" is the hole
next to strip C, k5 the hole nearest the middle island. Lead spacing as built: the caps are three holes apart (+, one
empty hole, −), the resistors four holes apart, the transistor in three consecutive rows. Rows: **3 feed, 6 tip,
7 ring, 8 GND and emitter, 11 base**; the collector is row 14 on the left block and row 12 on the right one. The dashed lines in the drawing are the copper that joins holes
in a row, not wires. Column k4 is left empty between the two filter caps so two barrels fit side by side.

| Part | From | To |
|---|---|---|
| +5 V feed wire | A, row 3 | row 3, k2 |
| 22 Ω | row 3, k2 | row 6, k2 |
| 4.7 µF (+ to row 6), a barrel | row 6, k5 | row 8, k5 |
| 100 nF (+ to row 6 if it is an electrolytic) | row 6, k3 | row 8, k3 |
| 1 kΩ | row 7, k2 | row 11, k2 |
| 100 kΩ | row 8, k4 | row 11, k4 |
| GND jumper (caps, 100 kΩ and the emitter, which shares the hole) | C, row 8 | row 8, k1 |
| 2N3904 in k1 (both blocks the same way: **flat face toward the left edge of the board**, legs spread so E, B, C land in the rows below) | left: emitter row 8, base row 11, collector row 14. Right: emitter row 8, base row 11, collector row 12 | |
| Tag `J1.T` / `J2.T` | row 6, k1 | |
| Tag `J1.R` / `J2.R` | row 7, k1 | |
| Collector, left block | no wire: the collector leg sits in row 14, the same row as **D7** | |
| Collector wire, right block (insulated) | row 12, col 12, left along row 12 to left col 3 | left row 17, col 3 (beside **D10**) |

**Sensor connectors, on the board.** Two 3-pin connectors in rows 23–25, wired like the SFX Box's sensor board. The
left one (`J1`, sensor 1) has its pins in column 2, top to bottom **Sleeve, Tip, Ring**. The right one (`J2`, sensor 2)
has its pins in column 11, top to bottom **Ring, Tip, Sleeve**.

| Pin | Hole J1 (col 2) | Hole J2 (col 11) | Goes to |
|---|---|---|---|
| Sleeve | row 23 | row 25 | GND: short wire to C (left C / right C) on the same row |
| Tip | row 24 | row 24 | tag `J1.T` / `J2.T` (wire to row 6, k1 of that block) |
| Ring | row 25 | row 23 | tag `J1.R` / `J2.R` (wire to row 7, k1 of that block) |

**XIAO power and servos**

| Part | From | To |
|---|---|---|
| GND jumper (XIAO GND pin is row 19) | left row 19, col 1 | left C, row 19 |
| 100 µF (+ to row 20), a barrel, shares the row 19 jumper hole | left row 20, col 1 | left row 19, col 1 |
| 100 nF (+ to row 20 if it is an electrolytic) | left row 20, col 3 | left row 19, col 3 |
| 1N5819, lying along row 21 (band to the XIAO) | anode left A, row 21 | cathode left row 21, col 3 |
| 5 V hop (the 5 V pin and the cap + legs are row 20, the diode cathode is row 21) | left row 21, col 3 | left row 20, col 3 |
| Tags `S1`, `S2`, `S3`, `S4` | right rows 19 / 18 / 17 / 16, col 11 | |

**Wires between the two boards (6 plus the 12 V pair).** `S1`–`S4`, `+5V` and `GND` (20 AWG, they carry every servo's current). `12V+` and `12V−` go from the jack to the 12 V chain
above the buck, not to the main board. On the connector board the four servo + pins join one +5V bus, and the
servo − pins join one GND bus.

**Soldering order that keeps things reachable:** the buck feed wires and strip jumpers, then the two sensor blocks, then the diode and caps beside the XIAO, then the XIAO last (it covers the
holes under it). Meter each rail for continuity to the far end, and meter across the channel on a spare row
to confirm the halves are isolated, before the XIAO goes in.

**Mounting the buck module.** The MP1584EN modules have four pads (IN+, IN−, OUT+, OUT−) and no pins, and
they don't land on a 0.1" grid, which is why they won't drop into a perfboard. The usual way is simply wires:
tin each pad, solder a short 22 AWG wire (red for +, black for −), add a dab of hot glue over the joints as strain
relief, and stick the module down with double-sided foam tape. Set the output to 5.0 V with a meter
*before* connecting anything to it.

**Keep the buck module off the board, a few cm from the XIAO.** Its inductor is the noisiest thing in the box.
