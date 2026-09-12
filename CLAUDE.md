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
| Strip | **WS2812B, 144 LED/m**, the whole ~1 m (3.2 ft) = **144 pixels**, black PCB, IP30 |
| Length | **a setting, not a constant** — `len <inches>` / `set leds <n>`, 1..144; 144 is the buffer |
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
- **The 2 A supply ceiling is real, with comfortable headroom.** MEASURED at the supply
  2026-09-12, all 144 lit, fire at `brightness 110`: **~1.0–1.2 A at rest (~5.5 W)** for the whole
  build, XIAO included — around **55 %** of the ceiling and ~9 h of runtime. **During a greeting it
  falls to ~0.7–0.8 A**, because `blackout` darkens all 144 pixels between words; a busy night is
  cheaper than a quiet one. An earlier revision predicted ~1.5 A from the manual's 0.12 W/LED,
  which is **the figure for WHITE** — this fire never goes white. All-white at full brightness
  would still want ~8.6 A, which this supply cannot deliver. Guard it in firmware:
  `FastLED.setMaxPowerInVoltsAndMilliamps(5, 1500)` — 1500, not 1700, because the cap governs
  only the LEDs and the XIAO needs the rest. It is live as `set maxMA`.
- **The cap bites for WHOLE-STRIP modes, not for the fire.** Measured: the resting ambient fire is
  887–1185 mA with `allowed` equal to `brightness` on all 14 samples — never limited, so the look a
  greeting performs is never held down. But `motion breathe` samples 1400–1489 mA with `capped=1`
  about half the time, and a solid white lamp is pinned at the cap. Those modes drive every pixel
  to full together; the fire only ever drives some. **`speech` makes it draw LESS, not more** —
  427–987 mA during `speak`, because `blackout` darkens all 144 between words while a word flash
  brightens only a few. If a look ever seems to fight itself, read `capped=1` in `st`; the fix is a
  lower `brightness`, never a `maxMA` above what the supply delivers — a brownout part-way along a
  WS2812B run reads as random colour, not as dimming.
- **`pwr` agrees with a meter for the FIRE, and runs ~30 % high for WHITE.** Amber at partial heat:
  1.0–1.2 A measured against 0.89–1.19 A estimated. Solid white: the cap holds the model at 1500 mA
  while the meter reads **1.14 A**. `setCorrection(TypicalLEDStrip)` scales green to ~69 % and blue
  to ~94 %, and `calculate_unscaled_power_mW` does not know it happened — a red-and-amber fire
  barely touches the corrected channels, white uses all three. **The error is always in the safe
  direction.** And read the meter over TIME: one spot reading of 0.80 A, taken mid-breath, briefly
  had this file claiming the model ran high for everything.
- **A full metre includes the strip's factory solder joint at 50 cm** (around pixel 72), which
  the 14" cut avoided. It carries everything past it and is the first suspect if the far half of
  the strip drops out, dims, or shifts colour while the near half is clean.
- **The length is live and persisted, because the diffuser is flexible.** The rigid diffuser could
  only be the 14" it was cut to; the 2026 one contours, so the bar gets re-cut per prop and
  `numLeds` is a fact about the SEASON rather than the build. `len 24` (inches) or `set leds 88`,
  then `save`. **`ends` is the only verification** — first and last pixel, 15 s, nothing between:
  the renderer draws whatever length it is told and cannot notice it is wrong. `MAX_LEDS` (144)
  sizes the buffers and *is* a recompile; the whole buffer is clocked out at any length, so a
  shorter bar buys draw, not frame time.
- **`flareMeanS` is the one tuned value the longer strip invalidated.** Its rate is per *strip*,
  so 3.5 s judged by eye across 14" is a third of the crackle density across 39". `spaceScale`
  and `flareWidth` are per-*pixel* and needed nothing — the fire keeps its grain and gets more of
  it. Left at the approved number rather than silently retuned; `set flareMeanS 1.2` restores the
  old density, by eye, on the prop. Now that the length is a setting this cuts both ways: a
  SHORTER bar wants a LARGER `flareMeanS` to read the same, and nothing scales it for you.
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
good, rather than two unknowns at once. Values are kept only on an explicit `save` —
see **Persistence** below.

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
**1.256 A measured**, with no firmware involved. *That was the 50-pixel run: the build lights all
144 as of 2026-09-12, and neither the far half of the strip nor its mid-run solder joint has been
through that test. The SP002E drives 600 pixels, so repeating it on the full strip is still the
cheapest way to separate a bad strip from bad code.* Wire colours were metered to the silkscreened
pads and match what the diagrams show. Steve reports the light-bar wiring complete as of
2026-09-09. That gate is closed; don't re-open it, and don't propose wiring changes.

What the wiring being done does *not* mean: nothing downstream of the XIAO's D10 pin has ever
been exercised, because no firmware exists. The **first** thing to run on this board is a
single-pixel test, not the fire renderer. If that first test shows nothing at all, the highest-
prior suspect is **pin 1 (1OE) not actually at GND** — the buffer's output sits high-impedance
and everything looks correct.

**Firmware: written, running, and the effect is approved.** `bar/` implements docs/01's four-layer model — ember
floor, whole-strip breath, correlated spatial noise, Poisson flares — with colour a pure function
of heat and gamma applied last.

**The structural decision, which is not negotiable:**

    heat[i] = clamp(EMBER_FLOOR, (noise[i] + flare[i]) * breath * (1 + speechGain*env), 1.0)

At `env == 0` that collapses *exactly* to the idle fire, so there is **no talking mode**, no
crossfade, and no state to get wrong. Idle is speech with a zero envelope. Anyone tempted to add
a second renderer and blend between them should read docs/01 §4 first: that change turns a fire
that speaks into a lamp on a dimmer, and no parameter recovers it.

## The effect as approved, 2026-09-10

The greeting drives the bar end to end: envelope generated on the Pi, pushed over ESP-NOW,
cued against the playback handle. **Sync measured at ±55ms across a 41s greeting with no
accumulation** — spec asks for ~50ms.

**Speech is driven by WORD BOUNDARIES, not loudness.** This is the single most important
decision here and it was reached twice independently. An RMS envelope of the real greeting
asks for **13.5 flares/sec against a word rate of 1.28** — flicker, not speech. Separately,
the Zoltar build had to drive its servo jaw from word starts rather than syllables. And a
third argument settles it: that greeting opens with ~14s of drums, and band-limited RMS
**cannot tell them from voice** (level 76 vs 101, nearly as many onsets). A word is a word;
a drum is not.

The renderer needed no change for that. It steps whatever track it is given, so the BOX
decides what drives the fire by shaping the track — `envelope.word_track()` on that side.
Keep it that way: the bar must stay ignorant of speech.

**What drives it, as of 2026-09-11:** a **gated envelope** from the box —
loudness, but forced to zero outside a phrase, so the musical introduction is
dark while the voice keeps its syllable structure. Word boundaries alone were
binary and read as *"it just turns on and off to the words"*; raw loudness could
not tell 14s of drums from a voice. Word timings were only ever needed to **gate**
the loudness, not to replace it.

**What a word does, as approved 2026-09-10 (before the noise-phase fix):**

- red (`flashHue 0`) mixed at `flash 0.6` — the flame shows through, so it reads as *the fire
  going red* rather than *a red light turning on*
- red follows the word's **span**, not its attack. Triggering on the onset and decaying on a
  timer is a hit, and reads as flashing
- `blackout 0.6` between words — the fire dims to 40% in the gaps, punctuating them, while
  still leaving a low fire alive through the 14s intro. Full blackout made the intro read as
  a broken prop rather than a waiting one
- crackles OFF (`wordFlare 0`) — red plus a flare was two events for one word

**Idle is untouched by any of that.** Between greetings the fire breathes normally and the
blackout does not apply, so the bar never sits dark on its own.

## Still to judge

**The palette has never been assessed fairly.** Both earlier adjustments were made while the
fire was rendering in a third of its range (see the `inoise8` note below), and only ever
against a **white wall** — which docs/01 §2 names as the surface that will mislead you. Redo
it on matte, warm-toned card before touching the ramp again.

## Persistence — built and proven 2026-09-11

**The track survives a power cycle**, and so do tuned parameters. Verified on the board
with a real hard reset, not reasoned about: pushed clip 47295 / 2246 frames, reset over
esptool, `st` reported it back.

This stopped being a field-reliability nicety and became urgent the day the track's
volatility broke a whole afternoon of testing — every firmware reflash emptied it, and
three consecutive greetings played to a silently dark bar that looked exactly like a broken
effect. In the field a battery blip does the same thing, mid-evening.

Two stores, because the two kinds of state want opposite things:

- **Params → NVS, on an explicit `save` only.** Tuning is dozens of nudges and each one
  would otherwise be a flash write. Explicit also matches how a look is actually arrived
  at: try a value, watch the strip, decide. `forget` returns to compiled defaults and drops
  the track.
- **Track → LittleFS, automatically** the moment a load succeeds. The box pushes one rarely
  and never during a show, so there is no wear question, and nobody should have to remember
  to save the thing that makes the prop work.

Params are **one blob with a version AND a size**, not a key per parameter. Twenty-odd
names would drift from the struct the first time one was renamed, and a silently-missing
key reads as zero — which for `emberFloor` or `breathHz` is a dead-looking fire rather than
an error. If either differs, the compiled defaults win.

**`st` reports the loaded CLIP as well as the frame count**, and that is a consequence of
persistence rather than a nicety. Until the track survived a reboot, *"is a track loaded"*
and *"is the RIGHT track loaded"* were the same question. Now a bar can come up holding
last night's line, pass a presence check, and then refuse the cue. The box checks the clip.

**Program storage is at 89%** — LittleFS and Preferences cost ~50KB. Worth knowing before
the next library goes in.

**Not built: a lamp.** A chosen colour with solid / blinking / breathing motion, for a prop
that wants a plain coloured light rather than speech. Designed, not started: a lamp is an
ambience whose palette ramps from black to the chosen colour, so breathing and solid fall
out of the existing layers and only blink is new. It must not modulate a *spatial* scale —
see the note below on why `speechSpread` defaults to 0.

## The bug that made the effect unwatchable — 2026-09-11

**A noise coordinate must be an ACCUMULATED PHASE, never `time * rate`.** The
renderer read:

    uint16_t tz = (uint16_t)(tSec * P.timeScale * (1.0f + 0.6f * env));

That multiplies *absolute* time by a rate speech is modulating. The instant `env`
moves, the product jumps — thirty seconds in, env going 0.4 to 0.9 shifts `tz` by
over a **thousand** noise units. The pattern does not speed up, it **teleports**.
Against a real speech envelope moving several times a second, the whole strip
scrubs back and forth, and what you see is rapid blinking. Fixed by integrating
the rate: `noisePhase += dt * timeScale`.

**Why it survived every earlier judgement of this renderer**, which is the part
worth remembering:

- a **held** `env` renders perfectly — constant rate, so no jumps at all
- `speak` was only ever watched for a few seconds
- the bug needs a **fast-moving** envelope to show itself, and only a real track
  provides one

So every tool used to tune the fire was blind to it by construction. It took an
afternoon of a real greeting on the prop, and the diagnosis only came from
bisecting the *renderer* rather than the track: held env = calm, any playing
track = blinking, frozen noise field (`timeScale 0`) = calm again. **Three
commands, no walk-past.** Reach for that bisection first next time.

**`speechSpread` now defaults to 0** for a related but unfixable reason: it
scales a *spatial* coordinate, so modulating it rescales the pattern along the
strip — a zoom — and a zoom driven by speech is the same visual fault. There is
no rate to integrate on a spatial axis. The parameter is kept because a slow
envelope can use it safely.

**The lesson for the box, too:** roughly two hours went into adjusting the
*track* — word boundaries, phrase merging, gating, smoothing, a floor — for a
fault that was entirely in the renderer. Several of those changes are real
improvements and are keeping their place, but none of them could ever have fixed
this. **When the shape of a complaint does not change as you change one side of
a link, the fault is on the other side.**

## The trap that distorted everything before it

**FastLED's `inoise8` does not use its full range** — it clusters around 128 and rarely leaves
roughly 50..205. Treating the raw byte as 0..1 gave a breath swinging 0.81..0.94 (a wobble,
not a breath) and noise running 0.24..0.75 that never reached the top of the palette. The fire
rendered in about a third of its intended range for the whole first day, which is also why the
palette read as wrong in both directions. `noiseNorm()` stretches it back.

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
