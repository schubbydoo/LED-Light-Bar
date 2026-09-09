# Fire Effect Design — "the fire speaks"

Date: 2026-09-04 · Hardware: ESP32-C3 pair, SK6812/WS2812 strip, ESP-NOW from the Pi 4

---

## 1. The concept, restated

Two states, one continuous simulation.

**Steady state — firelight in a cave.** Not a flame you look at, but what the flame *does to a
wall*: a warm, breathing, slowly-wandering wash of orange and deep red, with the occasional
crackle flaring brighter for a moment before falling back. It should look like something is
burning just out of frame.

**Triggered state — the fire speaks.** Motion detector hits the Pi, the greeting plays, and the
same fire now **surges in time with the speech**. Syllables push energy into the flame; pauses
let it settle back to embers. The fire is the mouth.

**The critical constraint, in your words:** the speech modulation *overlays the running fire
simulation*. It never replaces it. The fire simulation runs continuously from power-on to
power-off — during the greeting it simply gets an extra energy input. If you ever find yourself
switching to a separate "talking mode" renderer, the illusion dies: it stops being a fire that
speaks and becomes a lamp on a dimmer.

Decisions locked in: **strip hidden, washing a surface** · **pre-recorded greeting clips**.

---

## 2. What "hidden, washing a surface" changes

You made the right call, and it changes the effect design more than it might seem.

You're rendering **reflected** light, not the flame itself. That means:

- **Lower contrast.** A real flame has ferocious dynamic range; a wall lit by one does not.
  Compress the range — the darkest ember state should still be clearly visible, and the brightest
  flare shouldn't blow out.
- **Spatial correlation matters far more than sparkle.** On a wall you see *broad soft zones*
  brightening and dimming, not individual points. Neighboring pixels must move together.
- **No pixel is ever seen directly**, which forgives a lot — you don't need dense LEDs, you need
  smooth gradients. 30 LEDs washing a surface looks better than 60 seen through a diffuser.
- **The surface is part of the instrument.** Rough, matte, warm-toned surfaces (textured rock
  paint, burlap, crumpled kraft paper) scatter beautifully. Smooth white gloss will look flat and
  give you a visible hot spot. Worth experimenting with before tuning a single line of code.

Aim the strip along the base of the surface firing upward, or from behind an obstruction, so the
light rakes across texture. Raking light is what makes it read as a cave.

---

## 3. The fire model — four layers stacked

Run at **60–100 FPS**. Everything below builds a single `heat[]` array in the range 0.0–1.0,
which then goes through a palette and gamma. Nothing writes color directly.

**Layer 0 — ember floor.** A constant minimum. The fire never goes out. `heat` is clamped to
never fall below ~0.12. Without this you get dead black gaps that read as a fault, not a fire.

**Layer 1 — global breath.** One low-frequency value, roughly 0.5–3 Hz, multiplying the whole
strip. This is the **dominant cue** and the thing most fire effects miss. Real firelight brightens
and dims *as a whole* as the flame draws air. Get this right and half the work is done.

**Layer 2 — spatial noise field.** 1D Perlin/simplex noise (`inoise8` in FastLED) sampled as
`noise(i * SPACE_SCALE, t * TIME_SCALE)`. Two scales to tune:
- `SPACE_SCALE` sets how wide the bright zones are. Start with features spanning ~1/3 of the
  strip. Too fine and it turns to sparkle.
- `TIME_SCALE` sets how fast they drift. Slow — the zone should take a second or two to migrate.

**This is the layer people get wrong.** Independent per-pixel randomness produces glitter, which
your eye instantly reads as "LEDs." Correlated noise produces flame.

**Layer 3 — flares and crackles.** Discrete events, Poisson-distributed, mean interval ~2–6 s. A
flare is a Gaussian bump at a random position with **fast attack (~40 ms) and slow release
(~400–800 ms)**, pushing that region hotter. These are the pops and settles that make it feel
alive rather than looped.

### Color: intensity drives hue, never randomness

Map `heat` through a fixed heat ramp. **Never randomize hue** — that's the other giveaway.

| heat | color | reads as |
|---|---|---|
| 0.00 | (40, 0, 0) | dying ember |
| 0.25 | (120, 10, 0) | deep red |
| 0.50 | (200, 50, 0) | orange |
| 0.75 | (255, 110, 10) | bright flame |
| 1.00 | (255, 190, 80) | white-hot tip |

Note the top is **warm**, not white. Pure `(255,255,255)` looks like a camera flash.

**If you use SK6812 RGBW:** hold the W channel at zero for most of the range and bring it in only
above ~0.85 heat. That gives genuine white-hot peaks without desaturating the whole effect the
way pushing R+G+B does.

**Gamma-correct everything.** Apply γ≈2.2 as the last step before writing pixels. Linear PWM
makes smooth fades look stepped and makes the low end vanish. This single line is the difference
between "breathing" and "flickering."

---

## 4. The speech layer — modulation, not replacement

The speech envelope is **an extra energy input to the same `heat[]` field.** Structurally:

```
heat[i] = clamp(
    EMBER_FLOOR,
    ( noise_field[i] + flares[i] ) * breath * (1.0 + SPEECH_GAIN * env),
    1.0 )
```

That `(1.0 + SPEECH_GAIN * env)` is the entire speech feature. When `env` is 0 the expression
collapses to the idle fire — which is exactly what you want, and means there is **no mode switch
and no crossfade to get wrong.** Idle is just speech with a zero envelope.

Two envelope channels do the work:

**`env` — the amplitude envelope (~50 Hz).** Drives the flame's *energy*:
- **Heat gain** — louder speech, hotter fire (the term above)
- **Spatial spread** — loud passages widen the bright zone outward from the center; quiet ones let
  it contract. Modulate `SPACE_SCALE` slightly, or add a center-weighted boost scaled by `env`.
- **Flicker rate** — nudge `TIME_SCALE` up with `env` so the fire agitates when it's talking.

**`onset` — a transient/attack channel.** Drives *crackles*. When onset crosses a threshold, spawn
a flare (Layer 3) with amplitude proportional to the onset strength. Plosives — p, t, k — produce
beautiful sharp pops this way. This is what makes consonants visible.

### Asymmetric smoothing is essential

Raw speech RMS is far too twitchy to look like fire. Smooth it with **fast attack, slow release**:

```
if (raw > env) env += (raw - env) * ATTACK;    // ATTACK ≈ 0.5   (~10–20 ms)
else           env += (raw - env) * RELEASE;   // RELEASE ≈ 0.08 (~80–150 ms)
```

Fast attack keeps syllable onsets crisp; slow release gives the flame thermal inertia — real fire
doesn't stop instantly. Tune `RELEASE` first; it's the parameter that most changes the character.

### One craft note on "flash red"

You described flashing **red** on the words. Worth trying both mappings, because they read
differently and it's a one-line change:

- **Red flash on loud** — brightness up, hue held red. Reads urgent, a bit alarm-like. Dramatic.
- **Heat surge on loud** — brightness up *and hue climbing toward yellow*, settling back to deep
  red in the gaps. Counterintuitive, but this is what an actual fire does when you feed it, and I
  think it will read far more as *a living fire speaking* than as a light flashing.

The palette in §3 already does the second one for free — loud pushes `heat` up, and `heat`
carries hue with it. To get the first, just clamp the palette lookup to the red end during
speech. **Build the second, try the first, keep whichever looks right on the actual wall.**

### Use your word timestamps for structure

You mentioned the greetings carry timestamps. The amplitude envelope handles syllable-level
motion, but timestamps give you **phrase structure**, which is a different and complementary
thing:
- Between phrases, let the fire *settle noticeably* — a beat of calm before the next line makes
  the next surge land harder.
- On emphasized words, permit larger flares (raise the flare cap for that span).
- At the very end of the greeting, a slow decay back to idle over ~2 s rather than an abrupt stop.

---

## 5. Envelope extraction pipeline (pre-recorded clips)

Because the clips are fixed, **all of this happens once, offline, on the Pi.** Nothing runs at
show time.

Per clip:

1. Load the WAV, mix to mono.
2. **Band-limit to roughly 300–3400 Hz** before measuring. Speech energy lives there; this stops
   room rumble and sibilance from driving the flame.
3. **RMS in 20 ms frames** (50 Hz), which matches human syllable rate nicely.
4. **Normalize per clip** — TTS output level varies between renders, and you want every greeting
   to drive the fire equally hard.
5. **Compress** — take something like `env^0.6`, so quiet speech still moves the fire instead of
   only the loud parts registering.
6. **Onset channel** — frame-to-frame positive difference in energy (spectral flux works better if
   you want to go further), half-wave rectified.
7. **Quantize both to uint8** and write as a 2-byte-per-frame binary.

**Size:** 2 bytes × 50 Hz = **100 bytes per second of audio.** A 15-second greeting is 1.5 kB. The
C3 has 4 MB of flash, so you can store **hundreds of clips** in LittleFS on the box and never
transmit an envelope at show time at all.

That's the nicest property of going with pre-recorded clips: the runtime cue is just
`PLAY clip_id`, and packet loss during the greeting is impossible because there are no packets
during the greeting.

---

## 6. Cue and sync

**At provisioning time:** copy the envelope `.bin` files onto the box (LittleFS, over USB or an
OTA endpoint). Same clip IDs as the Pi's audio files.

**At show time:**

```
Pi:  motion detected
Pi:  ESP-NOW ─> { cmd: PLAY, clipId: 7, leadMs: -40 }
Pi:  start audio playback
Box: note millis() as t0, step the envelope at 50 Hz from frame 0
```

**Latency budget:** ESP-NOW is 1–3 ms — irrelevant. Your real variable is how long the Pi takes
between sending the packet and the first sample hitting the speaker, which depends on your audio
stack and can be tens of milliseconds. So:

**Make the lead/lag a tunable field in the cue packet** (`leadMs` above). Start around **−40 ms**
(light slightly *ahead* of sound) and adjust by eye. Light arriving a touch early reads as
natural; light arriving late reads as broken. Being able to nudge this from the Pi without
reflashing the box will save you a lot of grief.

**Clock drift** is a non-issue: the C3's crystal is ~±20 ppm, which is under a millisecond of
error across a 30-second clip. No resync needed. If you later run multi-minute pieces, add a sync
packet every few seconds carrying the current frame index and let the box ease toward it.

**Failsafe:** if the box is mid-greeting and hears nothing further, it just finishes the envelope
and decays to idle. Nothing to hang.

---

## 7. State machine

There isn't really one, and that's the point.

```
  ┌──────────────────────────────────────────┐
  │  fire simulation — always running        │
  │  env = 0 by default                      │
  └──────────────────────────────────────────┘
        ▲                          │
        │  PLAY cue                │ envelope exhausted
        │                          ▼
     env = envelope[frame]     env ramps to 0 over ~2 s
```

One renderer, one code path, one thing to debug. The only "state" is whether an envelope is
currently being stepped.

---

## 8. The knobs you'll actually turn

| Parameter | Start at | What it changes |
|---|---|---|
| `EMBER_FLOOR` | 0.12 | How dark the quiet moments get |
| `BREATH_HZ` | 0.5–3 | The whole-wall breathing rate — **tune this first** |
| `BREATH_DEPTH` | 0.25 | How much the breath swings brightness |
| `SPACE_SCALE` | ~3 features across the strip | Width of the wandering bright zones |
| `TIME_SCALE` | slow | How fast zones drift |
| `FLARE_MEAN_S` | 3.5 | Average seconds between crackles |
| `FLARE_RELEASE` | 400–800 ms | How long a crackle takes to fade |
| `SPEECH_GAIN` | 1.2 | How hard speech drives the fire |
| `SPEECH_SPREAD` | 0.4 | How much loud speech widens the flame |
| `ATTACK` | 0.5 | Syllable crispness |
| `RELEASE` | 0.08 | **Thermal inertia — most character-defining** |
| `leadMs` | −40 | Light-vs-audio sync trim |
| `GAMMA` | 2.2 | Perceptual smoothness |

---

## 9. Failure modes to design away from

Every one of these is a way a fire effect announces itself as an LED strip:

- **Per-pixel independent randomness** → glitter, not flame. Use correlated noise.
- **VU-meter mapping** — brightness directly proportional to volume, nothing else moving. Reads as
  a light on a dimmer. The fire must keep doing its own thing underneath.
- **Randomized hue.** Hue must be a function of heat.
- **Reaching full black** in the gaps. Keep the ember floor.
- **Pure white peaks.** Cap at warm amber.
- **No gamma correction** — the single most common cause of "why does it look steppy."
- **Noise that moves too fast.** When in doubt, slow everything down; real fire is lazier than
  you think.
- **A hard cut between idle and speaking.** The formula in §4 makes this structurally impossible,
  which is why it's written that way.

---

## 10. How to judge it

- **Test in real darkness, at the real viewing distance, on the real surface.** A fire effect
  evaluated on a bench under room lights will mislead you completely.
- **Film it on a phone and watch the playback.** Video flattens the dynamic range in roughly the
  way memory does, and problems jump out that your eye forgives live.
- **Put a real fire video on a laptop next to it** and glance between them. You'll immediately see
  which of the four layers is wrong.
- **For the speech test, close your eyes and listen, then open them.** If the motion feels like it
  belongs to the voice, `RELEASE` and `leadMs` are right.

---

## 11. Open questions

- **How many LEDs, and how long a run?** For a wash you want enough length to get a real spatial
  gradient. 30 works; 45–60 across a wider surface gives the bright zone somewhere to travel.
- **What's the surface?** Worth prototyping with a few materials before tuning parameters — it
  affects the effect more than most code changes will.
- **One zone or two?** Splitting into a "core" and a "spill" that respond with slightly different
  timing adds a lot of depth for modest extra work. Worth considering if the box is a centerpiece.
- **Audio — resolved.** The greeting plays from SFX_box through speakers behind and to the sides
  of the tiki. Sound and light arrive from roughly the same direction, so visual capture binds the
  voice to the mouth. Keep speakers out of the audience's approach path — walking right past one
  overrides the effect.
