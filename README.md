# SYRUP

A Serum-style wavetable synth for Nintendo 2DS / 3DS (homebrew). Proof of concept.

The 3DS's DSP chip plays the oscillators: each voice is a DSP channel looping a single-cycle waveform at the note's pitch, with the channel's own biquad low-pass as the filter. The main processor only rebuilds that waveform when something changes, about 60 times a second:

1. Morph between two wavetable frames at the current **position**.
2. Apply the **warp** (Sync, Bend+, Bend-, PWM, Mirror, FM, Quantize).
3. FFT it and write 7 band-limited copies (2048 down to 32 samples), so high notes don't alias.

8 voices × up to 3 unison = all 24 DSP channels.

## Controls

| Input | Action |
|---|---|
| Bottom screen, tabs | HELP (on-screen help), STEREO (Narrow / Wide / Spread), FX (Off / Echo / Space), echo TIME (250 / 375 / 500 ms) |
| Bottom screen, middle | XY pad: X = wavetable position, Y = filter cutoff |
| Bottom screen, bottom half | 2-octave keyboard (slide to glide between notes) |
| D-pad left / right | Wavetable |
| D-pad up / down (hold) | Warp amount |
| X | Warp mode |
| A | Unison 1 / 2 / 3 |
| Y | Envelope: Pluck, Keys, Pad, Lead |
| B | LFO on position |
| L / R | Octave |
| SELECT | Chord: Off, Min7, Maj7, Power, Sus |
| Circle pad | Left/right pitch bend, up vibrato, down filter resonance |
| START | Quit |

## Wavetables

Built in: Basic Shapes, PWM, Harmonix, Vowels, Sync Saw, FM Growl, Digital.

Your own: copy Serum-format wavetables (`.wav`, 2048-sample frames, 16/24/32-bit) into `sdmc:/3ds/syrup/`. They appear after the built-in tables.

## Install

FBI > Remote Install > Scan QR Code, and scan `install-qr.png`. Sound needs the DSP firmware dump (run DSP1 once).

## Proof-of-concept limits

- The waveform is rewritten while the DSP is playing it, so fast position sweeps can add slight grit.
- Envelopes and LFO run at ~60 Hz, so attacks shorter than one frame are rounded off.
- Effects are made from extra DSP voices (delayed, panned, darker copies of each note), because the DSP's effect buses can't be routed back for processing from homebrew yet. ECHO is a 3-repeat ping-pong, SPACE is 6 dense early reflections.
