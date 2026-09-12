# Perfboard Layout — electronics pod

Everything except the strip and the battery lives on one small board that rides at one end of the
aluminium channel, behind or under it, hidden from the viewer.

**Target size: ~50 × 30 mm (2.0" × 1.2"), a 20 × 12 hole board.** That's comfortable — you could
do it in 40 × 25 mm if space is tighter, but there's no reward for cramming it.

---

## What's on it

| Part | Footprint | Notes |
|---|---|---|
| XIAO ESP32C3 | 7 × 7 holes | **On a female header, not soldered down** — see below |
| SN74AHCT125N | 7 × 3 holes | **In a DIP-14 socket** |
| 0.1 µF ceramic | 2 holes | At the socket's pins 14/7 |
| 1000 µF 16 V | 2 holes, ~10 mm dia | At the output end |
| 470 Ω | 3–4 holes | In the data line, output side |
| 1N5819 Schottky | 3–4 holes | In the XIAO's 5 V feed. **As built** — confirmed on the board 2026-09-10 |
| Tether pads | 2 | Plus a strain-relief hole |
| JST SM 3-pin | 3 | Out to the strip |

---

## Zone map

Power enters one short edge, signal leaves the other. Nothing crosses.

```
   ┌─── this edge faces OUT of the mouth (USB-C reachable) ───┐
   │                                                          │
   │   ┌─────────────────────────┐                            │
   │   │   XIAO ESP32C3          │      u.FL antenna cable ──►│
   │   │   (on female headers)   │                            │
   │   └──┬────┬─────────┬───────┘                            │
   │      │    │         │ D10                                │
   │   5V │ GND│         ▼                                    │
   │      │    │    ┌──────────┐                              │
   │   ◄─▷|    │    │ AHCT125  │  ‖ 0.1µF                      │
   │   1N5819  │    │  socket  │                              │
   │      │    │    └────┬─────┘                              │
   │ ═════╪════╪═════════╪═══════════════════════  +5 V BUS   │
   │ ═════╪════╪═════════╪═══════════════════════  GND  BUS   │
   │      │    │         │                                    │
   │   ┌──┴────┴──┐   ┌──┴──┐         ┌────────┐              │
   │   │ TETHER   │   │470 Ω│         │ 1000µF │              │
   │   │  IN pads │   └──┬──┘         └────────┘              │
   │   └──────────┘      └────────────► JST SM 3-pin OUT      │
   │        ▲                                ▲                │
   └────────┼────────────────────────────────┼────────────────┘
        from battery                    to LED strip
```

---

## The five rules that matter

**1. Two fat bus wires, not traces.** Perfboard has no copper pours. Lay **bare 20 AWG solid
wire** along one full row for +5 V and another for GND, soldered at every few holes. Everything
taps off those. This is what carries 1.2 A without the board's own resistance eating your voltage.

**2. One star point, at the tether pads.** The +5 V bus starts where the tether lands, and the
three loads — strip, buffer, XIAO — each tap the bus separately. Never daisy-chain
XIAO → buffer → strip; strip current must not flow through a logic ground path.

**3. Keep the strip's power and the strip's data on opposite halves.** 1.5 A switching at video
rates next to a data line is how you get flicker that looks like a code bug — and at 144 pixels
that is 1.5 A rather than the 1.2 A this rule was written for. Power on the bus
rows, data across the middle, and cross them at right angles if you must cross at all.

**4. The 1000 µF sits at the output connector**, not at the tether input. Its job is absorbing
inrush when 144 pixels jump at once — it can only do that from the strip side, and the step it
has to absorb is three times what it was sized against.

**5. 0.1 µF physically touching the socket's pins 14 and 7.** Not "somewhere on the 5 V bus."
Decoupling caps only work when they're inches of wire closer than the bulk cap.

---

## Two things to socket, and why

**Put the XIAO on female headers**, don't solder it flat. You'll want to pull it for reflashing on
the bench, and if you ever kill one you swap it in thirty seconds instead of desoldering 14 pads
in a tiki mouth. Two 7-pin female strips on 0.6" centres.

**Put the AHCT125 in a DIP-14 socket.** Twenty cents, and it's the part most likely to take a hit
if something goes wrong on the strip side.

Orient the board so the **XIAO's USB-C faces out of the mouth** — you will reflash this thing more
times than you expect, and you don't want to disassemble the tiki each time.

---

## Wiring list

| From | To |
|---|---|
| Tether **red** | +5 V bus (star point) |
| Tether **black** | GND bus (star point) |
| +5 V bus | 1N5819 anode |
| 1N5819 cathode | XIAO **5V** pin |
| +5 V bus | socket **pin 14** |
| +5 V bus | 1000 µF **+**, and JST **+5 V wire (brown on this strip)** |
| GND bus | XIAO **GND**, socket **pin 7**, 1000 µF **−**, JST **GND wire (white)** |
| Socket **pin 1** (1OE) | GND bus |
| Socket **pins 4, 10, 13** | +5 V bus |
| Socket **pins 5, 9, 12** | GND bus |
| XIAO **D10** | socket **pin 2** |
| Socket **pin 3** | 470 Ω → JST **DIN wire (green)** |
| 0.1 µF | across socket pins 14 ↔ 7 |

### ⚠ Wire colours on this strip do NOT match the manual

The manual says the power wire is **red**. On the strip that actually shipped it is **brown**.

| Function | Manual says | This strip has |
|---|---|---|
| +5 V | red | **brown** |
| DIN | green | green |
| GND | white | **white** — not black |

**Two conventions in one build:** your tether is red/black, the strip's JST loom is brown/green/
white. On the board a red wire and a brown wire both land on +5 V, and a black wire and a white
wire both land on GND. Both correct — don't "fix" one to match the other.

**Do not wire this by colour. Verify with a meter first.**

The PCB silkscreen is the authority — the strip's pads are marked `+5V`, `DIN` (or `DI`) and
`GND`. Put the meter in continuity mode, one probe on a JST wire, the other on each pad in turn,
and write down what you find. Two minutes, and it's the difference between a working strip and a
dead one: 5 V onto the DIN pad kills the first pixel instantly, and reversed supply kills the
whole run.

Do this **before** the smoke test, not after. **✅ Done 2026-09-09** — metered and confirmed.

### The JST carries everything — one connector

All three conductors go through the single 3-pin JST. There is **no separate power lead** to the
strip; trim the spare power pigtail off and heat-shrink those two wires individually.

| JST pin | Wire on this strip | Carries | From |
|---|---|---|---|
| 1 | **brown** | **+5 V, ~1.5 A** | +5 V bus (star point) |
| 2 | green | DIN, 5 V logic | socket pin 3 → 470 Ω |
| 3 | white | **GND return, ~1.5 A** | GND bus |

*(Pin order above is the working assumption — confirm it against the silkscreen with the meter.)*

JST SM is rated **3 A per contact**. At 50 pixels the measured 1.256 A sat at 42 % of rating; at
**144 pixels the estimate is ~1.5 A, or 50 %**, and the 1500 mA FastLED cap is what holds it
there rather than headroom doing it. The connector is still comfortable — the cap is the reason. The vendor's own SP002E controller powers the strip the same way.

Because two of these contacts now carry the full strip current, **run them from the bus wires
directly** — don't tap them off a shared leg with the XIAO or the buffer. And keep the 1000 µF
immediately adjacent to this connector; that's why it lives at this end of the board.

---

## Mechanical

**Strain-relieve the tether.** Thread both wires down through one hole and back up through the
next before soldering, or drill a hole for a zip tie. The classic perfboard death is a wire
flexing until the pad lifts — and yours lives in a mouth that gets handled.

**Leave the u.FL cable a clear exit** with a little slack and its own tie-down. That connector is
delicate and it's the one part you cannot re-solder.

**Add two test pads** — a bare +5 V and a bare GND with nothing else on them — so you can probe
with the meter without clipping onto a component leg.

**Coat it after testing.** Conformal coating or clear acrylic over the solder side, masking the
socket contacts, the headers, and the JST. Same coating you're using on the strip's solder joints.

**Print a shell for it.** A simple two-piece box that clips to the end of the channel, with
cutouts for USB-C, the JST, the tether, and the antenna. Keeps it off the wet floor of the mouth
and gives the tether something to anchor to.

---

## Assembly and test order

Build it in stages and power up between each — finding a fault on a board with four parts is much
easier than on a board with nine.

1. ~~**Smoke-test the strip with the SP002E first.**~~ **✅ Done 2026-09-09** — all 50 pixels lit, power path proven. Start at step 2. *(That test was the 50-pixel run; the build now lights all 144, and neither the far half nor its mid-run solder joint has been through it. Worth repeating on the full strip — the SP002E drives 600.)*
2. Solder the bus wires and the tether pads. **Power up with nothing else fitted** and confirm
   5 V on the bus with the meter, correct polarity.
3. Add the socket, decoupling cap, and the 5 V/GND legs. Power up. **Confirm 5 V on socket pin 14
   and 0 V on pin 7 before inserting the chip.** A reversed socket kills the chip instantly.
4. Insert the AHCT125. Add the diode and the XIAO headers.
5. Fit the XIAO, check **4.6–4.7 V** on its 5V pin (5 V minus the Schottky drop).
6. Add the 470 Ω, the 1000 µF, and the JST. Connect the strip and run a single-pixel test before
   anything more ambitious.

---

## Breadboard first

Before any of this is soldered, the same circuit gets built on a breadboard. The full
tick-list — **10 to +5 V, 10 to GND, 3 signal, 3 deliberately unconnected** — is in
[`02-build-and-power.md`](02-build-and-power.md#breadboard-connection-checklist) and printed at
the bottom of `../images/power-distribution.png`.

Two things carry over from the breadboard stage to this board:

- **The strip's current never crosses the breadboard rails.** Spring contacts are ~1 A each;
  1.26 A through them gives voltage drop, warm rails, and colours that drift down the strip.
  The JST's brown and white come straight off the tether junction. Only the XIAO, U1 and the
  two caps sit on the board.
- **Every pin on both diagrams is colour-coded by function** — red for +5 V, black for GND,
  green for data, hollow for not connected — on the pin's rectangle and on its number. Check a
  wired board by eye: wire colour should match pin colour.
- **The XIAO's silkscreen is on the underside.** Face-up on the board, the right-hand pads read
  **5V, GND, 3V3, D10, D9, D8, D7** downward from the USB-C end, and the left-hand pads read
  D0 through D6. Both diagrams are drawn that way now — top-down with the USB-C at the top,
  matching how the SN74AHCT125N is drawn.
