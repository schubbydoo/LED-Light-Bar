# LED Light Bar

A portable, battery-powered light bar whose ambience is driven by speech, so that a simulated
fire appears to speak the words of a greeting.

![Power distribution](images/power-distribution.png)

## What it is

A ~14" strip of 50 addressable pixels in an aluminium channel, run by a XIAO ESP32C3 on a
battery. It renders a **named ambience** — `fire`, `water`, `storm` — continuously and
unsupervised. When the [SFX Box](#the-sfx-box) plays a greeting, the speech modulates that
ambience in time with the words.

For Halloween 2026 it is placed in the mouth of a borrowed tiki: firelight on the back of the
throat, and when a visitor triggers the prop, the fire speaks to them.

## Design in brief

**The box owns the speech; the bar owns the aesthetics.** The SFX Box sends an ambience name,
a pre-computed speech track, a cue at actual playback start, and a position update every couple
of seconds. It never sends colours. The bar excites its ambience with two scalars —
`excitation` and `onset` — which each ambience interprets in its own visual terms.

That separation is what makes the capability generic: the same two numbers drive a servo jaw on
another prop. See `sfx-box/spec-addendum-5.7A-speech-driven-channels.md`.

## Documentation

| | |
|---|---|
| [`docs/01-effect-design.md`](docs/01-effect-design.md) | The fire model, speech modulation, and the failure modes that make an LED strip look like an LED strip |
| [`docs/02-build-and-power.md`](docs/02-build-and-power.md) | Electrical ground truth — power, signal, both diagrams, strip specs, mechanical constraints |
| [`docs/03-perfboard-layout.md`](docs/03-perfboard-layout.md) | The electronics pod: layout rules, wiring list, staged assembly and test order |
| [`docs/04-rf-link.md`](docs/04-rf-link.md) | ESP-NOW: why not WiFi or 433, the SFX Box bridge wiring, and what crosses the link |
| [`docs/05-parts.md`](docs/05-parts.md) | What was bought and why |
| [`docs/archive/`](docs/archive/) | Superseded work, kept because the reasoning is still useful |

## Status

**Hardware verified and wired. Firmware not started.**

The strip was smoke-tested on its bundled SP002E controller: all 50 pixels light, and the power
path through the single JST is proven at **1.256 A measured** — about 8 hours of runtime at that
draw, comfortably more at fire brightness. Wire colours were metered to the silkscreened pads,
and the light-bar wiring was completed 2026-09-09.

Nothing downstream of the XIAO's data pin has been exercised yet, since no code exists. Next
step is firmware — start from `docs/01-effect-design.md`, and make the first thing that runs a
single-pixel test rather than the fire renderer.

## The SFX Box

A separate, private project: a general-purpose Raspberry Pi effects controller that plays the
audio, watches the sensor, and drives this bar. It is **not vendored here**. `sfx-box/` contains
only the architectural proposal that project needs in order to host this capability.

## Licence

Personal project. No licence granted.
