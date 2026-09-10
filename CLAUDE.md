# LED Light Bar — project guide for Claude Code

A portable, battery-powered **speech-driven light bar**. A strip of addressable pixels runs a
named ambience — `fire`, `water`, `storm` — and a greeting played by the **SFX Box** modulates
that ambience so the light appears to *speak the words*.

This season it sits in the mouth of a borrowed tiki and burns like a fire in a cave. It is not
a tiki accessory. **The prop is a venue the device visits for a season.**

## Read first, always

- `docs/01-effect-design.md` — what the thing is supposed to look like, and why. The four-layer
  fire model, how speech modulates it, and the specific mistakes that make an LED strip announce
  itself as an LED strip. **Read before writing any rendering code.**
- `docs/02-build-and-power.md` — the electrical ground truth. Power distribution, the signal
  path, both wiring diagrams, confirmed strip specs from its manual, and the borrowed-prop
  mechanical constraints.
- `docs/04-rf-link.md` — the ESP-NOW link, why it beats WiFi *and* 433 here, the bridge wiring
  on the SFX Box side, and the message set that crosses it.
- `sfx-box/spec-addendum-5.7A-speech-driven-channels.md` — the architecture. How this capability
  fits the SFX Box platform, and why speech-sync is a **rendering** capability rather than a
  motion one.

If a doc and the code disagree, the doc is probably right and the code has a bug. These were
written while measuring real parts.

## The design in one paragraph

The SFX Box owns the **speech**; the light bar owns the **aesthetics**. The box sends a named
ambience, a pre-computed speech track, a cue at actual playback start, and a position update
every couple of seconds. It never sends colours. The bar renders whatever ambience it was told
to run and excites it with two scalars — `excitation` and `onset` — which each ambience
interprets in its own visual terms. A jaw renderer on another prop gets the identical two
numbers and opens a mouth with them.

## Hardware, as built

| | |
|---|---|
| Controller | Seeed **XIAO ESP32C3** in the bar; a second one on the Pi's USB as an ESP-NOW bridge |
| Strip | **WS2812B, 144 LED/m**, cut to ~14" ≈ **50 pixels**, black PCB, IP30 |
| Optics | 45° corner-mount aluminium channel + frosted diffuser (also the heatsink) |
| Level shift | **SN74AHCT125N** — the T is not optional |
| Power | **TalentCell YB1206000-USB**, 5 V USB out, **2 A ceiling** |
| Link | **ESP-NOW** — no access point, no association, no router |
| Data pin | **D10** (GPIO10) → SN74AHCT125N pin 2 → pin 3 → 470 Ω → strip DIN |

## Traps that have already cost time

- **XIAO strapping pins.** ESP32-C3's GPIO2/8/9 are the board's silkscreen **D0, D8, D9**. A
  signal that idles LOW on any of them stops the board booting and looks like a dead XIAO.
  Pin map is in `docs/03-perfboard-layout.md`.
- **Strip wire colours do not match its manual.** +5 V is **brown**, not red. GND is **white**,
  not black. Meter to the silkscreened pads before connecting anything — 5 V on the DIN pad
  kills the first pixel instantly.
- **Two colour conventions coexist.** The tether is red/black; the strip's JST loom is
  brown/green/white. A red wire and a brown wire both land on +5 V. Don't "fix" one.
- **The 2 A supply ceiling is real, and still is.** Measured draw is **1.256 A** (peak across
  every SP002E test pattern, 2026-09-09) — comfortable. But those patterns never lit all 50
  pixels white at once, so the **3 A all-white case remains untested**. Guard it in firmware:
  `FastLED.setMaxPowerInVoltsAndMilliamps(5, 1500)` — 1500, not 1700, because the cap governs
  only the LEDs and the XIAO needs the rest.
- **Colour order is GRB.** FastLED's default for WS2812B, but confirmed rather than assumed.
- **Strip is rated −20 to +40 °C.** A sealed tiki mouth in Florida sun exceeds that with no
  power applied. Night prop only.

## Firmware — written 2026-09-10, tuning not started

**Both sketches live in this repo**, even though one of them runs on a board plugged into the
SFX Box's USB:

    firmware/bar/       the light bar — ambience renderer, ESP-NOW receiver
    firmware/bridge/    the SFX Box's radio — serial ⇄ ESP-NOW, ~40 lines

They are two ends of **one** protocol and have to version together. A copy of the bridge in
the SFX Box repo would drift the moment the message set changed, and that drift shows up as a
prop that half-works rather than as a build error. The SFX Box repo references this one and
vendors nothing; `sfx-box/README.md` says so from that side.

Plus three diagnostics that exist because the hardware had never been exercised: `metercheck/`
(strip disconnected, steady DC so a meter can prove the level shifter), `bringup/` (five staged
questions with the strip attached), `pixelcheck/` (whole-strip primaries and fixed markers, held
— for when `bringup` raises a question about one pixel or one channel).

**Both XIAOs live on the SFX Box's USB, not on a PC.** That was not the plan and it is better
than the plan: the box is a Linux machine with a shell on it, so `flash.sh`, `monitor.sh` and
`send.sh` put nothing between an edit and a running board. The only step that needs a person is
looking at the strip. Neither script addresses a board by `ttyACM` number — two identical boards
enumerate in plug order, so they go by MAC through `/dev/serial/by-id/`. See `firmware/README.md`.

**`send.sh` is how the fire gets tuned.** Thirteen parameters, all judged by eye, and a compile
is 60-90s — reflashing to try a number destroys the comparison you are holding in your head.
`speak <seconds>` runs a synthetic envelope, so attack, release and gain can be tuned **before
ESP-NOW exists**; the radio then becomes a transport change against a renderer already known
good, rather than two unknowns at once. Nothing sent is persisted, deliberately.

## Firmware notes

- **FastLED 3.6 or newer** — earlier versions predate ESP32-C3 support. `Adafruit_NeoPixel` and
  `NeoPixelBus` are working fallbacks.
- The C3 drives the strip from the **RMT peripheral**, so timing is hardware and single-core is
  not a problem.
- **Rest state is the idle ambience, not black.** For a fire, dark reads as broken. And because
  the prop is borrowed, the device must also be inert when the battery is pulled — no retained
  state that surprises the owner in March.
- **One clock.** All timing derives from the box's playback handle, never from a local
  `millis()` started independently. This is the single bug the SFX Box platform keeps removing.

## Where the build actually is

**Hardware: verified, and the wiring is done.** The strip was smoke-tested on its bundled
SP002E controller — all 50 pixels light, and the power path through the single JST is proven at
**1.256 A measured**, with no firmware involved. Wire colours were metered to the silkscreened
pads and match what the diagrams show. Steve reports the light-bar wiring complete as of
2026-09-09. That gate is closed; don't re-open it, and don't propose wiring changes.

What the wiring being done does *not* mean: nothing downstream of the XIAO's D10 pin has ever
been exercised, because no firmware exists. The **first** thing to run on this board is a
single-pixel test, not the fire renderer. If that first test shows nothing at all, the highest-
prior suspect is **pin 1 (1OE) not actually at GND** — the buffer's output sits high-impedance
and everything looks correct.

**Firmware: written, running, untuned.** `bar/` implements docs/01's four-layer model — ember
floor, whole-strip breath, correlated spatial noise, Poisson flares — with colour a pure function
of heat and gamma applied last.

**The structural decision, which is not negotiable:**

    heat[i] = clamp(EMBER_FLOOR, (noise[i] + flare[i]) * breath * (1 + speechGain*env), 1.0)

At `env == 0` that collapses *exactly* to the idle fire, so there is **no talking mode**, no
crossfade, and no state to get wrong. Idle is speech with a zero envelope. Anyone tempted to add
a second renderer and blend between them should read docs/01 §4 first: that change turns a fire
that speaks into a lamp on a dimmer, and no parameter recovers it.

What has NOT happened: nobody has judged how it looks. Per docs/01 §10, that has to be in real
darkness, at real viewing distance, on the real surface — a bench under room lights will mislead
you completely. Tune `breathHz` first (the dominant cue) and `release` second (thermal inertia,
the most character-defining number).

### Reading the diagrams

All three sheets in `images/` use the same conventions, and they are the current, only version:

- **Every pin is colour-coded by function** — on the pin's rectangle *and* its number.
  **Red** = +5 V, **black** = GND, **green** = data, **hollow** = not connected.
- **The XIAO is drawn top-down with the USB-C at the top.** Right-hand pads read
  `5V GND 3V3 D10 D9 D8 D7` downward; left-hand pads read `D0`–`D6`. The module's silkscreen is
  on its **underside**.
- **Both signal pins of the AHCT125 are on the left edge** — pin 2 in, pin 3 out, adjacent.
  A DIP-14 numbers counter-clockwise from the notch. The output leg on the sheet loops back
  under the chip for that reason; it is one short jumper in reality.
- `docs/02-build-and-power.md` carries the same information as text tables, including a
  connection checklist: **10 to +5 V, 10 to GND, 3 signal, 3 deliberately unconnected.**

The useful consequence of the hardware being proven: **a fault from here is a code fault.**
If pixels misbehave once firmware exists, suspect the code, the GRB order, or a strapping pin
before suspecting the strip or the supply.

## Relationship to the SFX Box

The SFX Box is a separate, private repo — a general-purpose Raspberry Pi effects controller
(FastAPI web UI, mpv playback over JSON IPC, RF relay channels, media assets with derived
sidecars). **It is deliberately not vendored here.**

`sfx-box/` holds only what that project needs in order to understand this one: the spec
addendum proposing where this capability belongs in its architecture. Everything else about
the SFX Box lives in its own repo.

### What the box can already do — 2026-09-10

The box generates and serves **word timestamps** as a sidecar on any audio asset, at
`GET /media/timestamps/<asset_id>`. Entry types are `word`, `spacing` and `audio_event`, in one
shape whichever provider produced them, with a `version` field for a bar that cached a track.
Details and the JSON in `sfx-box/README.md`.

**That is not the fire's driver.** Word boundaries make a fire strobe rather than burn — the
argument is in `docs/01-effect-design.md` and the addendum. The fire wants `speech_envelope`,
which is **not built** on the box side. What timestamps give this project is the *other* half:
phrase structure, so the bar can visibly settle between phrases and flare harder on an
emphasised word. Plan on consuming both, not one.

Still missing on the box side, in the order this project needs them: the `speech_envelope`
sidecar, a `PIXEL` channel kind, the serial bridge, and cue emission bound to the playback
handle's *actual* start. Nothing yet consumes a track at show time on either side, so **the
one-clock rule has not been tested by anything** — it is still a rule, not a fact.
