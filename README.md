# SYRUP

![SYRUP](art/syrup_logo.png)

**A wavetable synth for the Nintendo 2DS and 3DS.** Serum-style oscillators played by the console's DSP chip, a filter and envelope section, a master effects chain, a step sequencer with a piano roll, drums, a four-track tape recorder and a granular engine. It all runs on the handheld, with nothing to plug in.

> **Beta.** Syrup 1.0 is in beta. Things may still break; see [Reporting bugs](#reporting-bugs).

## Install

You need a 2DS / 3DS with custom firmware (Luma3DS).

**Sound needs a one-time setup.** The console's sound chip runs Nintendo firmware that homebrew can't include, so you copy it to the SD card once:

1. Press **L + Down + Select** to open the Rosalina menu (this works anywhere, even inside Syrup).
2. Choose **Miscellaneous options > Dump DSP firmware**. It saves `dspfirm.cdc` to `/3ds/` on the SD card.
3. Close the menu with B, quit Syrup and open it again.

No Luma3DS menu? Run the DSP1 app once instead. If the file is missing, Syrup shows these steps on screen.

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
- **Tape:** four tracks that capture everything you hear, in time with the sequencer. Each track can play back at a bar-friendly speed (1/4 to X4).
- **TAPE:** four tracks that bounce the sequencer in sync, with mute, level and WAV export to the SD card.
- **Grains:** play a loop, a wavetable or the mic as grains. The HARDWARE page has a waveform editor for it.
- **Second sound chip:** chip voices, chip echo and CHIP > FX.
- **Presets:** about 200 factory patches in styles, plus 8 user slots.
- **Extras:** Wi-Fi and IR MIDI (MPE, aftertouch, MIDI LEARN, LFOs out as CCs), a phone remote page, chord PADS with progressions and a strum strip, a trance gate, a visualizer, and themes.

The full list of every page, parameter and control is in [docs/REFERENCE.md](docs/REFERENCE.md).

## Controls

| Input | Action |
|---|---|
| X / Y | Next / previous page |
| L / R | Octave down / up |
| D-pad up / down | Pick a parameter |
| D-pad left / right | Change it (hold to repeat) |
| Circle pad | Default: edits the selected value. HARDWARE > CIRCLE PAD switches it to pitch bend + mod, page values or macros |
| SELECT (tap) | Play / stop the sequencer and drums |
| SELECT + circle pad | Quick select: up = any page, left / right = the page's main lists, down = REC / PLAY / TAPE |
| SELECT + L / R | Undo / redo |
| SELECT + D-pad down | Panic |
| START | Menu: SAVE PATCH, BROWSER, PANIC, HELP, THEME, MIDI LEARN, QUIT |
| A / B | Chord mode / latch |
| Touch keyboard | Play (lower on a key = louder, slide to glide); OCT / CHORD / LATCH to its left |
| XY pad | Wavetable position and cutoff (the corner button changes what it controls) |

## Files on the SD card

Everything lives in `sdmc:/3ds/syrup/`: user patches, sequences, settings, your wavetables (`.wav`), and tape exports in `tape/`.

## Reporting bugs

Open an [issue](../../issues) with what you did, what happened, and which build you're on. The version is on the splash screen and the HARDWARE page. If Syrup crashed, a photo of the crash screen helps a lot.

## Building

Needs devkitPro (devkitARM, libctru, citro2d / citro3d, 3ds-curl). `make` builds `syrup.3dsx`; the GitHub Actions workflow also builds the `.cia`. Host tests (no console needed) are run with `python3 tests/host/run_tests.py`.
