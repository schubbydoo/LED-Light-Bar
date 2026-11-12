# Speech-driven jaw — plan

Date: 2026-11-12 · **Status: PLAN. Nothing here is built or measured.**

A second renderer for the same link: a servo jaw on an animatronic, driven by the same speech
track the light bar uses. This document is the plan and the reasons for it. Where it says
*unknown*, that is a thing to measure, not to assume.

---

## 1. The idea

The SFX Box owns the speech; the prop owns the rendering. The box already sends a named
behaviour, a pre-computed track, a cue at actual playback start, and a position update every
couple of seconds. It never sends colours — and it will never send angles. The jaw receives the
same two scalars the bar does (`excitation`, `onset`) and turns them into a jaw angle the way the
bar turns them into heat.

`sfx-box/spec-addendum-5.7A-speech-driven-channels.md` already names this: `speech_envelope`
× `SERVO` is "proportional jaw opening — better than binary, free". This plan builds it as a
**remote** renderer, the second one after the light bar.

**The structural rule carries over unchanged:** at zero envelope the jaw is at its rest angle,
and speech modulates that. There is no "talking mode", no crossfade, no second renderer to blend
to. Idle is speech with a zero envelope.

---

## 2. One repo, one new sketch: `firmware/jaw/`

The bar and the jaw are two ends of one protocol and have to version together — the same reason
the bridge lives here. A separate repo would let the two drift apart, and drift shows up as a
prop that half-works rather than as a build error.

**How the shared code is handled — fork first, share later.** `bar.ino` is ~1,900 lines and the
radio, track, cue and persistence code inside it is exactly what the jaw needs. Two ways to reuse
it, and the order matters:

1. **Now:** copy that code into `jaw.ino`, with a comment at each copied block saying where it
   came from. `bar.ino` is **not touched** — it is the approved effect, and nothing about a jaw
   justifies risking it.
2. **Later, only once the jaw runs:** move the shared parts into `firmware/common/` and have both
   sketches use them. Then verify the bar behaves identically (`st`, a greeting, the measured
   sync) before accepting the change.

Sharing first would mean editing a working, approved sketch while the jaw design is still
changing. Copying first costs a short period of duplicated code, which is the cheaper risk.

---

## 3. What is reused, and what is new

| | |
|---|---|
| **Reused as-is** | ESP-NOW link, the bridge, the `trk` / `cue` / `pos` messages, the 50 Hz `level, onset` track format, one-clock rule (position derived from the box's cue), track in LittleFS and params in NVS, live tuning through the bridge, `flash.sh` / `monitor.sh` / `send.sh`, the strapping-pin rule, the USB power meter |
| **Reused with changes** | the bridge (one hard-coded peer today — see §4), the command parser (new parameters), the rest-state rule |
| **New** | the servo output; shaping of level → angle (deadband, smoothing, slew limit, lookahead); calibration tools; stall / brownout protection; servo detach when idle |
| **Not needed** | FastLED, the level shifter (probably — see §6), colour, palettes, the power-cap model |

Program storage on the bar is at 89 %, mostly libraries the jaw does not need, so the jaw starts
with plenty of room.

### How the jaw renders

`angle = closed + (open − closed) × shape(level)`, where `shape` is the part to tune by eye:

- **gain and deadband** — quiet murmur should not move the jaw at all
- **attack / release** — how fast it opens and how fast it closes
- **slew limit** — the most the servo is *asked* to move per frame; a servo cannot follow a raw
  50 Hz envelope and will chatter and wear if asked to
- **`open_angle`, `closed_angle`** — set by measurement on the real mechanism, never guessed
- **lookahead** — see below

**Lookahead is the one thing the jaw can do that the bar cannot.** The track is resident on the
device, so the jaw can read a few frames *ahead* and start moving the servo early by its
mechanical latency. The existing per-channel `lead_ms` trim is the same idea; for a servo it is
likely to matter far more than for light, because a servo lags the command by tens of
milliseconds. Light slightly early reads as natural; a jaw that moves *after* the sound reads as
broken.

**Words versus loudness.** Zoltar's jaw was driven from word starts, and a jaw genuinely does
want something closer to word and syllable structure than the fire does. The fire is driven by a
*gated envelope* — loudness, forced to zero outside a phrase. Start with that same track. If the
jaw reads as mushy, shape a second track on the box side for it; do not teach the jaw about
speech. The prop stays ignorant of speech, same as the bar.

---

## 4. The SFX Box side

**The box's code is the same for both props, with three small differences.** The messages on the
wire are identical. What changes is addressing and bookkeeping:

1. **Channel registry:** a remote channel gains an *address* (a MAC) and a *renderer kind*
   (`PIXEL` or `SERVO`). The addendum already says a remote channel "consumes no GPIO — the
   registry gains an address instead of a pin". The jaw is a second row in that table.
2. **A `play` binding per prop:** `sync: {track: envelope, channel: fire}` and
   `sync: {track: envelope, channel: jaw}` — vocabulary the addendum already defines, not a new
   verb.
3. **`lead_ms` per channel**, not global. A servo's latency is not a light's.

**The bridge changes too, and it is in this repo.** It forwarded every line to one hard-coded
peer (`PEER_BAR`). For two props it needs a small peer table and a line prefix — `@jaw cue ...`
— with the **default staying the bar, so nothing that works today changes.**
*Status 2026-11-12: written and tested on a PC against a fake radio; **not yet flashed to a real
bridge**. See "More than one prop on the bridge" in `firmware/README.md`.*

### Two props talking at once

Both props will often speak different lines at the same time. That works with the design as it
stands, because each device holds its own track and takes its own cue — but three things have to
be true, and two are untested:

- **The box needs two independent playback handles**, each with its own position feed. Whether
  the box's current playback supports two simultaneous voices with separate `time-pos` is a
  question for the SFX Box repo — nothing in this one can answer it.
- **Tracks are pushed before the show, never during it.** Unchanged, and now more important:
  a track push to one prop must not collide with a live cue to the other.
- **The bridge currently waits 60 ms after every line it forwards.** That was a bring-up
  convenience so the printed reply matched the line sent. With two props it would delay one
  prop's cue behind the other's `pos` update by tens of milliseconds, in a system whose sync is
  measured at ±55 ms. It needs to become non-blocking before two props share it. This is the
  most likely place for this plan to bite.

This is also the first test of "multiple renderers on one `play`" listed as an open question in
the addendum — a jaw and a fire on the same greeting — and nothing has tried it yet.

---

## 5. Hardware

**A third XIAO is required.** Both existing boards are in use: one in the bar, one on the Pi.

| Item | Status |
|---|---|
| XIAO ESP32C3 ×1 (buy 2 for a spare) | **Buy**, ~$6 each. Ships with its u.FL antenna |
| S51 servo | On hand. **Stall current and voltage range unknown — to be measured** (§7) |
| Bulk capacitor, 470–1000 µF 16 V, at the servo | Probably in the bins (same part as the bar's) |
| Schottky diode + small cap for the XIAO's own supply | ~$1 |
| 10 kΩ pull-down for the servo signal line | Bins |
| 3-pin servo header, 22 AWG wire, perfboard | Bins / ~$1 |
| USB power meter | On hand |
| SN74AHCT125N level shifter | **Do not buy yet.** A 3.3 V signal usually drives a servo; test before adding a part |
| Power for the jaw | **Open** — see below |
| Jaw linkage (horn, pushrod, hinge) | Depends on the animatronic |

**Power.** The two props will run the same night, so they cannot share the TalentCell: the bar
alone measures ~1.0–1.2 A of its 2 A ceiling, and a servo's stall current could use the rest and
more. The jaw gets **its own 5 V source**. Which one depends on the S51 measurement, and on
whether a power bank's 2 A ceiling is enough or a small regulated supply is wanted.

**Wiring principles, from the problems the bar already taught:**

- **Servo power goes straight to the supply, not through the XIAO.** The XIAO sits behind a
  Schottky diode and its own small capacitor, so a servo current dip cannot reset it.
- **Signal on D10**, same pin as the bar, which has no strapping role. Never D0, D8 or D9.
- **Pull-down on the signal line**, so the servo does not twitch while the XIAO boots with the
  pin floating.
- **Servo detaches when idle**, which stops buzzing and saves current.
- **A brownout shows as odd behaviour, not as an error** — resets, twitching, a servo that
  "hunts". Suspect power before suspecting code, the same lesson as the strip.

---

## 6. Measuring the servo (not in the Zoltar repo, so we find out)

Do this on the bench with **no linkage** first, then once with the real mechanism. A USB power
meter in line between the supply and the servo, read over time (an average, not a spot reading —
the bar's CLAUDE.md note about a misleading mid-breath reading applies here).

| Measure | How |
|---|---|
| Idle (holding, no load) | servo powered, commanded to a fixed angle |
| Moving, no load | slow sweep and fast sweep |
| Moving, on the real linkage | the actual jaw motion |
| **Stalled** | push to the mechanical stop **for one second at most**, read the peak |
| Voltage range | run at 5 V only. Do not raise past 6 V on a guess |

**A USB meter averages. It will miss the very short inrush spike at start-up**, so it
understates the true peak. Design for the spike anyway with the bulk capacitor and the XIAO's
isolation, and watch for resets rather than trusting a number alone.

Software also needs a **hard angle limit** (`closed_angle` to `open_angle`, set from
measurement) so a typo or a bad track can never drive the jaw past its stop. A servo stalled
against a stop will draw its stall current until it is told to move or powered off.

---

## 7. Phases

Each phase has a **done-when**. Do not start the next until the current one is true.

| # | Phase | Done when |
|---|---|---|
| 1 | **Bench power** — XIAO + servo + its supply, no linkage | the §6 table is filled in, and the XIAO does not reset when the servo is stalled briefly |
| 2 | **`firmware/jawcheck/`** — like `pixelcheck`: sweep, hold an angle, find the limits on the real mechanism | `open_angle` and `closed_angle` are written down from measurement |
| 3 | **`firmware/jaw/` renderer** with a synthetic `speak`, shaping parameters live-tunable | the jaw looks like talking on a synthetic envelope |
| 4 | **Link** — bridge peer table and `@jaw` addressing, non-blocking forwarding; jaw takes `trk` / `cue` / `pos` | a track pushed over the air survives a power cycle, `st` reports the clip, and a cue starts it |
| 5 | **Real greeting** — through the box, with the bar running too | sync measured the way the bar's was; `lead_ms` tuned; both props speaking different lines at once |
| 6 | **Extract shared code** to `firmware/common/` | the bar passes the same checks it passed before the change |

**Test with a real track early.** Every tool used to tune the fire was blind to its worst bug,
because a synthetic or held envelope never moves fast enough to show it. Phase 3 will look right
and phase 5 may not. If the jaw misbehaves with a real track but not a synthetic one, **bisect
the renderer rather than the track**: hold the envelope, then play a track, then freeze the
smoothing. When a complaint does not change as one side of a link changes, the fault is on the
other side.

---

## 8. How the code gets developed (for the owner who is not a developer)

These are working rules for whoever — human or Claude — touches this, written down so they do not
depend on anyone's memory.

- **One thing at a time, and prove it.** Each phase above changes one thing. If two things change
  and the result is wrong, there is nothing to learn from it.
- **The bar is protected.** `bar.ino` is not edited until the jaw works (phase 6), and then only
  with the before/after check above. A working approved effect is worth more than tidy code.
- **Measurements go in the docs, with a date.** Numbers that were measured are marked as
  measured; numbers that were calculated are marked as calculated. This repo's earlier mistakes
  came from a calculation presented as a fact.
- **Small commits with plain messages**, one per meaningful step, so any step can be undone
  without losing the others.
- **Anything outward-facing waits for a person.** Claude can write, build and flash. Deciding
  whether the jaw *looks right* needs a person looking at it, and nothing here treats a passing
  build as a passing prop.
- **Docs are updated in the same change as the code.** If a doc and the code disagree, one of
  them is wrong and the next person will not know which.
- **A bad day's lesson goes in `CLAUDE.md`.** Anything that cost an afternoon is cheaper to
  write down than to rediscover.

---

## 9. Open items

1. S51 stall current and voltage range — measure (§6).
2. Which 5 V source powers the jaw.
3. Does the SFX Box play two clips at once, each with its own position feed? (SFX Box repo.)
4. Does the jaw want a word-structured track rather than the gated envelope? Decide by looking,
   at phase 5.
5. What the linkage is, and what its real travel and load are. Depends on the animatronic.
6. Naming: this repo is "LED Light Bar". A second prop type is the point at which that may stop
   being the right name. Not urgent.
