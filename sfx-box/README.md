# sfx-box/

**The SFX Box is a separate, private repository and is deliberately not vendored here.**

This folder holds only what the SFX Box project needs in order to understand the LED Light Bar:
the architectural proposal for where this capability belongs in its platform.

## Contents

- **`spec-addendum-5.7A-speech-driven-channels.md`** — a draft addendum to `sfx-box-spec.md`,
  written in that spec's voice. Move it into the SFX Box repo when adopted.

## What it argues

The SFX Box spec files speech-sync under §5.7 **motion**, because both existing instances were
motion — Ghost Host's gearmotor jaw and Zoltar's servo jaw, both driven from word timestamps.

The light bar is the third instance and it is not motion at all. That is the signal:

> Speech-sync is not a motion capability that happens to have two motion clients. It is a
> **rendering** capability whose first two clients happened to be motion.

The addendum separates two axes the spec conflated — the **track kind** (`word_timestamps` or
`speech_envelope`, both §5.8 sidecars) and the **renderer kind** (`MOTOR`, `SERVO`, `PIXEL`) —
and covers the one genuinely new problem, a renderer that is not on the same machine as the
audio.

It keeps §5.7's best decision untouched: **one clock, derived from the playback handle.**

## What the SFX Box side has to provide

1. ~~A speech-sync sidecar on the audio asset~~ — **built 2026-09-10.** See below.
2. A `speech_envelope` sidecar kind in `media.py`, alongside `normalized_pcm` and
   `word_timestamps` — the continuous track a pixel field wants
3. A `PIXEL` channel kind in the hardware registry — remote, so it carries an address rather
   than a pin
4. A serial ⇄ ESP-NOW bridge on a USB-attached XIAO ESP32C3 (the Pi 4 cannot transmit ESP-NOW
   on its own radio)
5. Cue emission bound to `player.play()`'s actual start, plus periodic `time-pos` updates and
   the existing `end-file`/`eof` completion callback

## What it already provides — word timestamps, 2026-09-10

The SFX Box now generates and serves the **discrete** half of the track model. This is not
the fire's primary driver — that is the envelope, item 2 above — but it is the half that
carries **phrase structure**, which the addendum argues the best result uses as well:

```
GET /media/timestamps/<asset_id>   →   the stored track, as JSON
```

```json
{ "version": 1, "language_code": "eng", "provider": "elevenlabs",
  "text": "The Key Master has been chosen.",
  "words": [ {"text": "The", "start": 0.119, "end": 0.239, "type": "word"},
              {"text": " ",   "start": 0.239, "end": 0.379, "type": "spacing"} ] }
```

Three entry types occur — `word`, `spacing`, and `audio_event` (a laugh, a door). All three
are real: the format was checked against Ghost Host's own stored tracks, not assumed.

Two properties that matter to a renderer at the far end of a radio link:

- **The shape does not depend on which provider answered.** The box normalises OpenAI's
  `{word, start, end}` into ElevenLabs' `{text, start, end, type}`, reconstructing the gaps,
  so no vendor name ever reaches a microcontroller's parser.
- **`version` is in the file.** A bar that cached a track needs to know when the shape moved
  under it, and a number in the file is cheaper than discovering it in the field.

The track is **resident before playback starts**, exactly as this project's design requires.
Fetching it is a provisioning-time action, not a show-time one.

## Where the XIAO firmware lives

**Both sketches live in this repo** — the bar's and the bridge's — even though the bridge
plugs into the SFX Box's USB. They are two ends of one ESP-NOW protocol and have to version
together; a copy in the box's repo would drift the moment the message set changed, and the
drift would show up as a prop that half-works. The SFX Box repo references this one and
vendors nothing. Decided 2026-09-10.
