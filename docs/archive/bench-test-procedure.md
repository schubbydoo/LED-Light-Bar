# Bench Test Procedure — MD-6LED-A panel
### Using only a 12 V battery, a DMM, and resistors

The goal is three numbers per color: **how many LEDs are in series (Vf), how much current
the panel wants, and how hot it gets.** Everything in the design doc's Section 3 depends on them.

You don't need a current-limited supply. A resistor in series *is* the current limit, and the
voltage across that resistor *is* your ammeter — **I = V_resistor / R**. Measuring voltage
across a known resistor is more accurate and far safer than putting the DMM in current mode,
where a slip blows the meter's fuse.

---

## ⚠ Before you start

A 12 V battery is a near-infinite current source at these scales. A direct slip across the
LED pads will kill all six emitters instantly and silently.

- **Never close the circuit without a resistor in it.** Build the whole chain, then connect
  the battery lead last.
- Put a **1 A inline fuse** in the battery positive lead if you have one. Cheap insurance.
- Use alligator clip leads, not hand-held probes, so nothing slips.
- Work on a non-conductive surface. The panel's back is bare copper in places.

---

## Step 0 — Measure the battery

DMM to **DC volts**, probes on the battery terminals.

**Record: V_bat = ________ V**

If it's a car/SLA battery sitting on a charger it'll read ~13.8 V — take it off the charger
and let it settle for a few minutes. Every calculation below uses this number, and it will
sag slightly under load, so re-check it once during the test.

---

## Step 1 — Confirm the two arms are in parallel

DMM to **continuity** (beeper).

| Probe A | Probe B | Expect |
|---|---|---|
| `+` pad, arm 1 | `+` pad, arm 2 | **beep** — anodes are bussed |
| `R` pad, arm 1 | `R` pad, arm 2 | **beep** |
| `G` pad, arm 1 | `G` pad, arm 2 | **beep** |
| `B` pad, arm 1 | `B` pad, arm 2 | **beep** |
| `+` pad | any of `R`/`G`/`B` | **no beep** (LEDs block at the meter's low test voltage) |
| `R` pad | `G` pad | **no beep** |

**If all four beep:** the arms are in parallel on a common 4-wire bus. Good — this is the
expected case, and you can test the whole panel at once from either arm's pads.

**If they don't beep:** the arms may be wired in series through the frame traces, which would
put six LEDs in a string (~19 V for blue) and 12 V will never light them. Stop and tell me —
we'll change the plan. You can still run Steps 2–4 on each arm separately.

**Record: arms parallel? Y / N**

---

## Step 2 — Confirm polarity and pad labels

DMM to **diode test** (the ⇉| symbol).

**Red probe on the `+` pad, black probe on `R`.** Then repeat for `G` and `B`.

- A reading of roughly **1.6–2.0 V** on red or **2.5–3.2 V** on green/blue means the meter
  pushed through **one** die — so that color is a single LED deep.
- **`OL` / no reading** means more than one junction in series. **This is the expected and
  good result** — most DMMs only source ~2–3 V in diode mode, not enough for three LEDs.
  It is not a fault.
- If you get a reading with the probes *reversed* (red on the color pad), the panel is
  common *cathode*, not common anode. Tell me — that changes the driver design.

**Record: `+`→R = ______   `+`→G = ______   `+`→B = ______**

Some meters will faintly light a single die during this test. Do it in a dim room and watch.

---

## Step 3 — The main test: Vf and current, one color at a time

### 3a. Measure your resistors first

Resistors are ±5%. DMM to **ohms**, measure each one you'll use and write the *actual* value
on a bit of tape. Use those measured values in the math, not the color code.

### 3b. Wire it up

```
  battery +  ──[ 1 A fuse ]── clip ──> panel `+` pad

  panel `R` pad ── clip ──[ R_ballast ]── clip ──> battery −
```

The ballast goes in the **ground leg**. That puts one end of it at battery negative, so you
can leave the black DMM probe clipped to battery − for the whole test.

**Start with R_ballast = 470 Ω.** Two 1 kΩ ¼ W resistors twisted in parallel (= 500 Ω) works
and shares the heat. Worst case at this value is about 22 mA — safe even if the panel turns
out to be a single LED deep.

Connect the battery lead **last**.

### 3c. Take two readings

DMM to **DC volts**.

1. **Across the ballast resistor** (from the panel-side end of the resistor to battery −).
   Call it **V_R**. → **I = V_R / R_measured**
2. **Across the LED string** (from the `+` pad to the `R` pad).
   Call it **V_f**.

Sanity check: **V_f + V_R should equal V_bat**, within a few tenths. If it doesn't, a clip
lead is loose or you're probing the wrong nodes.

### 3d. Read the answer

At 470 Ω, with V_bat ≈ 12.6 V:

| V_f you measure | What it means |
|---|---|
| **~6.0–7.0 V** on red, **~9.0–10.5 V** on green/blue | **3 dies in series.** Expected case. The design doc's plan holds. |
| **~4.0–4.5 V** red, **~6.0–6.8 V** green/blue | 2 in series. Even better — more headroom on 12 V. |
| **~1.8–2.1 V** red, **~2.8–3.2 V** green/blue | All parallel. 12 V leaves a lot to burn off; tell me and I'll revise. |
| **> 11 V** and barely lit | 4+ in series, or the arms are in series. Stop, tell me. |

### 3e. Now walk the current up

Swap in smaller ballast resistors and re-read both voltages each time. **Compute the next
resistor from what you just measured**, don't just grab the next one down:

> **R_next = (V_bat − V_f) / I_target**

Because V_f climbs as current rises, you'll always land a little *under* your target. That's
the safe direction.

Rough starting guide, assuming 3-series and V_bat = 12.6 V:

| R_ballast | ≈ I on **red** (ΔV ≈ 6.0 V) | ≈ I on **green/blue** (ΔV ≈ 3.0 V) |
|---|---|---|
| 470 Ω | 13 mA | 6 mA |
| 220 Ω | 27 mA | 14 mA |
| 100 Ω | 60 mA | 30 mA |
| 47 Ω | 128 mA | 64 mA |
| 33 Ω | 180 mA | 91 mA |
| 22 Ω | 270 mA ⚠ | 136 mA |
| 15 Ω | 400 mA ⛔ | 200 mA |

**Note that red needs a much larger resistor than green/blue for the same current** — its
string voltage is ~3 V lower, so there's twice the voltage across the ballast. Don't reuse
the same resistor across colors without recomputing.

**Watch resistor wattage.** P = V_R × I. At 47 Ω on red that's 6.0 V × 0.128 A ≈ **0.77 W** —
well past a ¼ W part. Either gang resistors in parallel (four 220 Ω ¼ W in parallel = 55 Ω
and 1 W total), or just make the connection for **2–3 seconds**, take the reading, and
disconnect. A ¼ W resistor survives 1 W briefly; it will not survive it for a minute.

**Stop climbing when** any of: you reach ~300 mA total (what the original brick delivered),
the light stops getting visibly brighter for more current, an emitter looks strained or
yellowish, or the PCB is uncomfortable to touch.

### 3f. Repeat for green and blue

Same procedure, same starting resistor. Green and blue will draw roughly half the current of
red at any given resistor value.

---

## Step 4 — Thermal soak

Pick your intended operating current — this should be the brightness you actually *want*,
not the maximum the panel survives. Wire all three colors at once, each through its own
ballast resistor sized for that current (white output). Leave it running **10 minutes**.

Then check the PCB temperature next to an emitter, by touch or IR thermometer:

- **Comfortable to hold (< ~50 °C):** good, you have margin.
- **Too hot to hold for 5 seconds (> ~60 °C):** back the current down 25% and re-soak.

Also watch for color shift over those 10 minutes — if white drifts noticeably warm or cool as
it heats, that's the forward-voltage drift that makes constant-current drive worth the parts.

---

## Recording sheet

```
V_bat (rested) = ________ V        Arms in parallel?  Y / N
Diode test  +→R ______   +→G ______   +→B ______      Common anode? Y / N

RED
  R_ballast (measured)   V_R      I = V_R/R      V_f
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V

GREEN
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V

BLUE
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V
  ______ Ω               ____ V   ______ mA      ______ V

Chosen operating point:  R ____ mA   G ____ mA   B ____ mA
10-min soak temperature: ________ °C     Color drift observed? ________
```

---

## What I do with the results

- **V_f at your chosen current** → sets whether a 12 V rail has enough headroom for the
  MOSFET current sinks, or whether we go to 15 V. The blue/green number is the critical one:
  above about **10.5 V** and 12 V won't work.
- **Chosen current** → sets R_sense in each sink (R_sense = 0.65 / I) and the MOSFET
  heatsinking.
- **Soak temperature** → tells us whether the panel needs to be mounted to something with
  thermal mass inside the box.

Send me the filled-in sheet and I'll finalize Section 3 of the design doc.
