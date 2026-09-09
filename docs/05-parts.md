# Shopping List — Tiki Fire Bar
### Revised 2026-09-04 for the tiki-mouth form factor

**Enclosure envelope:** mouth 15" wide × 5" deep (back of teeth → back of mouth) × 3" from floor
of mouth to teeth. The unit leans against the back of the teeth and throws light onto the back of
the mouth.

Search links rather than ASINs — listings churn. **[bin?]** = probably already in your bins.

---

## The form factor changed the design

It isn't a box. **It's a slim light bar, and the battery lives somewhere else.**

A 20,000 mAh power bank is roughly 6 × 3 × 1 inches — it was never going to fit in that mouth.
So split the build:

```
   ┌─ in the mouth ─────────────────────────────┐      ┌─ hidden in the tiki body/base ─┐
   │  ~14" aluminum channel                     │      │                                │
   │  + WS2812B strip + XIAO + level shifter    │◄─────┤  20,000 mAh power bank         │
   │  ~14" × 0.7" × 0.7", leaning on the teeth  │ 5V,  │                                │
   └────────────────────────────────────────────┘ GND  └────────────────────────────────┘
```

Only **two wires** cross the gap. The XIAO is 21 × 17.5 mm — smaller than a postage stamp — so it
rides at one end of the channel and the data run to the strip stays a couple of inches, which is
exactly what you want for signal integrity.

---

## Already on hand — do not buy

2× XIAO ESP32C3 · TX118SA-4 / RX480E-4 433 pair · motion detector (Pi side)
· **TalentCell YB1206000-USB battery pack** — see below, this replaces the power bank

---

## Buy

| # | Item | Search | ~Price |
|---|---|---|---|
| 1 | **WS2812B 5 V, 144 LED/m, IP65, 1 m** — cut ~14" ≈ 50 LEDs | [link](https://www.amazon.com/s?k=BTF-LIGHTING+WS2812B+144+LEDs+per+meter+5V+IP65+1m) | $18–25 |
| 2 | **45° corner-mount aluminum LED channel + frosted diffuser**, 1 m, cut to ~14" | [link](https://www.amazon.com/s?k=45+degree+corner+aluminum+LED+channel+frosted+diffuser+1m) | $13–18 |
| 3 | **SN74AHCT125N, DIP-14** level shifter | [link](https://www.amazon.com/s?k=SN74AHCT125N+DIP+14+quad+level+shifter) | $8 |
| 5 | **USB-A male to bare wire / screw terminal** | [link](https://www.amazon.com/s?k=USB+2.0+type+A+male+plug+to+screw+terminal+adapter) | $8 |
| 6 | **USB power meter** | [link](https://www.amazon.com/s?k=USB+power+meter+voltage+current+tester) | $10–14 |
| 7 | **18–20 AWG two-conductor wire, 6 ft** — the tether | [link](https://www.amazon.com/s?k=20+AWG+2+conductor+stranded+wire+red+black) | $10 |
| 8 | 1000 µF 16 V electrolytics **[bin?]** | [link](https://www.amazon.com/s?k=electrolytic+capacitor+assortment+kit+1000uF+16V) | $12 |
| 9 | 330–470 Ω resistors **[bin?]** | [link](https://www.amazon.com/s?k=1%2F4W+metal+film+resistor+assortment+kit) | $12 |
| 10 | 22 AWG silicone hookup wire **[bin?]** | [link](https://www.amazon.com/s?k=22+AWG+silicone+stranded+hookup+wire+kit+6+colors) | $16 |

**Core: ~$60–85.**

---

## Battery: TalentCell YB1206000-USB — yes, and it's better than a phone power bank

**Specs:** 12 V / 6000 mAh DC out (9–12.6 V, 3 A max) **and 5 V / 12000 mAh USB out at 2 A**.
~72 Wh nominal. Charges from its own 12.6 V 1 A charger in about 6 hours. Physical power button.

**Runtime:** measured draw is **1.256 A at 5 V (6.3 W)** at peak, and the fire effect averages
below that. The 5 V rail is rated 12000 mAh (~60 Wh); call it **~52 Wh usable**, which gives
**~8 h at the measured peak** and **~11 h at typical fire brightness** — comfortably more than
the 20,000 mAh phone bank this replaces, and it kills a whole category of problem:

**No auto-shutoff to design around.** These packs are built for continuous low-draw loads — CCTV
cameras, LED strips — and switch on and off with a physical button rather than by sensing load. So
the trickle-mode hunt and the firmware idle-floor hack are both off the table. *Verify it on the
bench with the USB power meter anyway* — run the box at a dim idle for half an hour and confirm it
doesn't drop out. Cheap certainty.

**⚠ The one constraint: 2 A on the USB output.** At 50 LEDs the measured peak is 1.256 A, so
there's headroom — but `fill_solid(CRGB::White)` at full brightness on 50 WS2812Bs is **3 A** and
would trip it. Guard it in firmware rather than by remembering:

```cpp
FastLED.setMaxPowerInVoltsAndMilliamps(5, 1500);   // FastLED scales brightness to fit
```

**1500, not 1700.** The cap governs only the LEDs, so the remaining 500 mA of the 2 A ceiling
has to cover the XIAO (~100 mA) plus converter tolerance. See `02-build-and-power.md`.

That caps the draw at 1.7 A no matter what the effect asks for, including during a careless test.

**If you ever want more headroom**, the 12 V output is rated 3 A (36 W) — a $8 5 V/3 A buck module
off that rail would remove the ceiling entirely. Not needed for this build.

**Where it lives:** it's roughly 4.5 × 2.5 × 1 inches, so it stays outside the mouth in the tiki
body or base, on the two-wire tether — exactly as planned. Its USB-A output pairs directly with
the pigtail already on the list.

---

## Why these specifics

**144 LED/m instead of 60.** The throw is only **5 inches**. At that distance 60/m spacing
(16.7 mm) can show as a row of distinct hot spots on the back of the mouth; 144/m (7 mm) blends
into a continuous wash. It also keeps the LED *count* around 50 in a 14" run, which is what the
wandering-bright-zone effect needs to look like fire rather than one blob pulsing. The frosted
diffuser does most of the blending work, but at this throw distance density genuinely helps.

*Cheaper fallback:* 60/m gives ~21 LEDs in 14". It will work — coarser effect, less smooth. If
you want to save $10 and don't mind, it's a legitimate choice.

**45° corner-mount channel.** This is the part that solves your geometry. It sits flat in the
bottom-back of the mouth and emits at 45° up and back — exactly the angle to wash the throat from
a unit leaning on the teeth. It also **hides the strip from anyone looking in over the teeth**,
which matters: see a pixel and the illusion is gone. And the aluminum is your heatsink, which
you'll want, because a sealed tiki mouth has no airflow and you're putting ~3 W in there.

**Two-conductor tether.** 5 V and ground only, from the bank to the bar. Keep it 18–20 AWG so the
voltage drop over six feet at ~1 A stays small — thin wire here shows up as the far end of the
strip shifting red.

**No JST connectors this time** — with the bar this small, solder direct and seal it. Fewer
things to corrode in an outdoor tiki mouth in Florida.

---

## Layout inside the mouth

Run the strip **horizontally along the bottom, aimed up and back**. Two reasons:

- The bright zone then wanders **left–right across the throat**, which reads as flame movement.
- The **vertical** gradient comes free from the throw — bright at the bottom, falling off toward
  the top of the mouth. That's exactly how a fire lights a cavity, and you get it from geometry
  rather than code.

Leave the channel a little short of the full 15" so it isn't visible at the corners of the mouth.

**Check before you build:** stand at the height a trick-or-treater will and look in over the
teeth. If you can see the LEDs directly, add a small opaque lip on the viewer side of the channel.

---

## Open item

**Moisture.** Outdoor tiki, Florida, late October. IP65 strip is right, and pot or heat-shrink the
XIAO end. A mouth is a water trap — give the cavity a drain path if the tiki doesn't have one.

**Audio — resolved.** The greeting comes from the SFX_box over speakers placed behind and to the
sides of the tiki. That works: with sound and light arriving from roughly the same direction, the
visual capture effect ("ventriloquism") binds the voice to the moving light, and viewers will
attribute the speech to the mouth. The only thing that would break it is a speaker the audience
walks directly past — close proximity to one source overrides the binding. Keep them behind the
tiki line, out of the approach path.
