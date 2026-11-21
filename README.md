# SYRUP

![SYRUP](art/syrup_logo.png)

**A wavetable synth for the Nintendo 2DS and 3DS.** Serum-style oscillators played by the console's DSP chip, a filter and envelope section, a master effects chain, a step sequencer with a piano roll, drums, a looper, a four-track tape recorder and a granular engine. It all runs on the handheld, with nothing to plug in.

> **Beta.** Syrup 1.0 is in beta. Things may still break; see [Reporting bugs](#reporting-bugs).

## Install

You need a 2DS / 3DS with custom firmware (Luma3DS) and the DSP firmware dumped once (run **DSP1**), or there's no sound.

- **QR code:** FBI > Remote Install > Scan QR Code, and scan [`install-qr.png`](install-qr.png). It installs the newest `syrup.cia`.
- **Manual:** download `syrup.cia` (install with FBI) or `syrup.3dsx` (copy to `/3ds/` on the SD card, open from the Homebrew Launcher) from [Releases](../../releases).
- **Updates:** HARDWARE page > UPDATE SYRUP downloads and installs the newest build over Wi-Fi.

## What's in it

- **Two wavetable oscillators** plus sub and noise. Warp modes (sync, bend, PWM, FM / AM / RM from the other oscillator), unison, built-in tables, and your own Serum `.wav` tables.
- **Filters:** per-voice filter with two envelopes, plus an OUT FILTER on the master (ladder, acid and more).
- **Modulation:** LFOs (including ones you draw), a mod matrix, four macros, motion lanes. The console's tilt, twist, shake, breath (mic) and camera can all be mod sources.
- **Master FX:** a reorderable chain with chorus, phaser, delay types, reverb types, shimmer, OTT, pump and lo-fi.
- **Sequencer:** 16 patterns of 16 steps with a piano roll, a 64-slot song mode, per-step locks, live and tap recording with preroll, and an arpeggiator (including CHORD with ARP STRUM).
- **Drums:** six synthesised kits, including WAVE KIT (built from your wavetable), with patterns, Euclid rows, tuning, chance and mute.
- **Looper:** captures everything you hear. Change its speed, add wow and tape stop.
- **TAPE:** four tracks that bounce the sequencer in sync, with mute, level and WAV export to the SD card.
- **Grains:** play a loop, a wavetable or the mic as grains. The HARDWARE page has a waveform editor for it.
- **Second sound chip:** chip voices, chip echo and CHIP > FX.
- **Presets:** about 200 factory patches in styles, plus 8 user slots.
- **Extras:** Wi-Fi MIDI (and a phone remote page), chord PADS with a strum strip, a visualizer, and themes.

The full list of every page, parameter and control is in [docs/REFERENCE.md](docs/REFERENCE.md).

## Controls

| Input | Action |
|---|---|
| L / R, X / Y | Change page |
| D-pad up / down | Pick a parameter |
| D-pad left / right | Change it (hold to repeat) |
| Circle pad | Default: edits the selected value. HARDWARE > CIRCLE PAD switches it to pitch bend + mod, page values or macros |
| Touch keyboard | Play (lower on a key = louder, slide to glide) |
| XY pad | Wavetable position and cutoff (the corner button changes what it controls) |
| SELECT | Play / stop the sequencer |
| START | Quick menu: SAVE PATCH, BROWSER, PANIC, HELP, THEME, QUIT |
| A / B | Chord mode / latch |

## Files on the SD card

Everything lives in `sdmc:/3ds/syrup/`: user patches, sequences, settings, your wavetables (`.wav`), and tape exports in `tape/`.

## Reporting bugs

Open an [issue](../../issues) with what you did, what happened, and which build you're on. The version is on the splash screen and the HARDWARE page. If Syrup crashed, a photo of the crash screen helps a lot.

## Building

Needs devkitPro (devkitARM, libctru, citro2d / citro3d, 3ds-curl). `make` builds `syrup.3dsx`; the GitHub Actions workflow also builds the `.cia`. Host tests (no console needed) are run with `python3 tests/host/run_tests.py`.
