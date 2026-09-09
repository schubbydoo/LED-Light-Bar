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

1. A `speech_envelope` sidecar kind in `media.py`, alongside `normalized_pcm`
2. A `PIXEL` channel kind in the hardware registry — remote, so it carries an address rather
   than a pin
3. A serial ⇄ ESP-NOW bridge on a USB-attached XIAO ESP32C3 (the Pi 4 cannot transmit ESP-NOW
   on its own radio)
4. Cue emission bound to `player.play()`'s actual start, plus periodic `time-pos` updates and
   the existing `end-file`/`eof` completion callback
