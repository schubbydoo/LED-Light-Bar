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

> ### ⚠ Open: the fixture is still specified at 14", the strip is not
>
> As of **2026-09-12** the electrical and firmware sides light the whole ~1 m (3.2 ft) strip —
> 144 pixels. Everything mechanical in this document still describes the 14" version: the channel
> length, the diffuser, and "leaning on the teeth" as the mounting. Whoever mounts the full metre
> owns that decision and should correct this section afterwards, because the diagram above is the
> only place the fixture is written down.
>
> **The diffuser is now flexible rather than the rigid 45° channel**, which is what made the full
> metre mountable at all — it contours to the prop instead of demanding a straight 14" seat. That
> also changes what this row has to specify: a *length* that a season can pick, not a fixture cut
> once. The firmware already treats it that way — `len <inches>` sets how much of the strip is
> lit, and `ends` proves it against the physical bar — so the mechanical side is free to use as
> much or as little of the metre as a prop wants, and only this document is behind.
>
> ⚠ **The channel was also the heatsink.** A flexible diffuser is not one, and the strip is rated
> to +40 °C. Night-prop-only already covered the ambient case; running long stretches at high
> `brightness` with no aluminium behind the strip has not been checked, and `set maxMA` is the
> lever if it turns out to matter.
>
> Two things to settle while doing it. **The tether carries ~0.8 A measured** (~1.9 A only if
> someone runs the fire at full brightness), so don't go thinner than 20 AWG
> (`02-build-and-power.md`). And **a full metre includes the strip's factory solder joint
> at 50 cm**, around pixel 72, which the 14" cut deliberately avoided — it is the one mechanical
> discontinuity in the run and the first suspect if the far half ever misbehaves.

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
| 1 | **WS2812B 5 V, 144 LED/m, IP65, 1 m** — the **whole metre, 144 LEDs**, since 2026-09-12 (was cut ~14" ≈ 50) | [link](https://www.amazon.com/s?k=BTF-LIGHTING+WS2812B+144+LEDs+per+meter+5V+IP65+1m) | $18–25 |
| 2 | **45° corner-mount aluminum LED channel + frosted diffuser**, 1 m, cut to ~14" — ⚠ **see the note below: the strip is now the full metre and this row still describes a 14" fixture** | [link](https://www.amazon.com/s?k=45+degree+corner+aluminum+LED+channel+frosted+diffuser+1m) | $13–18 |
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

**Runtime, at 144 pixels — measured.** The 1.256 A figure was the 50-pixel run on the SP002E. On
this build, with all 144 lit and the fire at `brightness 110`, a USB meter reads **5.05 V /
~0.80 A ≈ 4 W** for the whole thing, XIAO included. The 5 V rail is rated 12000 mAh (~60 Wh); call
it **~52 Wh usable**, which gives **~13 h** — barely down from ~17 h at 50 pixels, because a fire
is amber at partial heat rather than white. Comfortably more than the 20,000 mAh phone bank this
replaces, and a 5–6 hour night is covered about twice over. *An earlier revision of this paragraph
predicted ~6.5 h from W/LED arithmetic and told you the margin was one night; measuring it showed
that was pessimistic by 2×.* Full numbers in `02-build-and-power.md`. It still kills a whole
category of problem:

**No auto-shutoff to design around.** These packs are built for continuous low-draw loads — CCTV
cameras, LED strips — and switch on and off with a physical button rather than by sensing load. So
the trickle-mode hunt and the firmware idle-floor hack are both off the table. *Verify it on the
bench with the USB power meter anyway* — run the box at a dim idle for half an hour and confirm it
doesn't drop out. Cheap certainty.

**⚠ The one constraint: 2 A on the USB output — with more room than expected.** The fire at
`brightness 110` *measures* ~0.80 A, about 40 % of the ceiling, so the 1500 mA cap remains a
distant guard rail rather than something in the effect's path — `pwr` has never once reported
`LIMITING`. At full brightness the same fire would want ~1.9 A, and
`fill_solid(CRGB::White)` on 144 WS2812Bs is **8.6 A** — four times what this supply can deliver.
Guard it in firmware rather than by remembering:

```cpp
FastLED.setMaxPowerInVoltsAndMilliamps(5, 1500);   // FastLED scales brightness to fit
```

**1500, not 1700.** The cap governs only the LEDs, so the remaining 500 mA of the 2 A ceiling
has to cover the XIAO (~100 mA) plus converter tolerance. See `02-build-and-power.md`.

That holds the LEDs to 1500 mA no matter what the effect asks for, including during a careless
test — about 1.6 A at the wall socket once the XIAO is counted. It is also live as
`set maxMA <mA>`, because with a USB meter in hand the cap is a number you set by measurement.
Ask the firmware what it is drawing with `./send.sh bar pwr`; `LIMITING` means the cap and not
`brightness` is setting the level of what you are looking at.

**If you want more headroom, this is now the live option rather than a footnote.** The 12 V output
is rated 3 A (36 W), so a $8 5 V/3 A buck module off that rail retires the 2 A ceiling and takes
the full-brightness fire (~3.5 A) with it. Not needed to run the approved look at 144 pixels —
that fits, with ~20 % to spare — but it is the answer if the whole strip ever wants to be
brighter, and the only answer: raising `maxMA` past what the supply delivers buys a brownout, and
a brownout part-way along a WS2812B run reads as random colour rather than as dimming.

**Where it lives:** it's roughly 4.5 × 2.5 × 1 inches, so it stays outside the mouth in the tiki
body or base, on the two-wire tether — exactly as planned. Its USB-A output pairs directly with
the pigtail already on the list.

---

## Why these specifics

**144 LED/m instead of 60.** The throw is only **5 inches**. At that distance 60/m spacing
(16.7 mm) can show as a row of distinct hot spots on the back of the mouth; 144/m (7 mm) blends
into a continuous wash. It also keeps the LED *count* high — 50 in the original 14" run, 144 over
the full metre — which is what the wandering-bright-zone effect needs to look like fire rather
than one blob pulsing. The frosted diffuser does most of the blending work, but at this throw
distance density genuinely helps.

At the full metre the density decision is what makes the extension free in effect terms:
`spaceScale` and `flareWidth` are per-pixel, so a noise feature stays the same *physical* size
and the longer bar simply gets more of them — a fire three times as long, not three features
stretched over three times the length.

*Cheaper fallback:* 60/m gives ~21 LEDs in 14", or ~60 over a metre. It will work — coarser
effect, less smooth. If you want to save $10 and don't mind, it's a legitimate choice.

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
