# SYRUP

![SYRUP](art/syrup_logo.png)

**A wavetable synth and groovebox for the 3DS and 2DS.** Two wavetable oscillators played by the console's DSP chip, filters, modulation, master effects, a piano-roll sequencer, a drum machine and a four-track tape, all running on the handheld. There's also a web version that runs in any browser.

> **Open beta.** Things may still break; see [Reporting bugs](#reporting-bugs).

**Play it in your browser:** https://syrup-synth.github.io/syrup/

## Install on a 3DS / 2DS

You need a 3DS or 2DS with custom firmware (Luma3DS).

- **QR code:** FBI > Remote Install > Scan QR Code, and scan [`install-qr.png`](install-qr.png). It installs the newest `syrup.cia`.
- **Manual:** download `syrup.cia` (install with FBI) or `syrup.3dsx` (copy to `/3ds/` on the SD card, open from the Homebrew Launcher) from [Releases](../../releases).
- **Updates:** HARDWARE page > UPDATE SYRUP downloads and installs the newest build over Wi-Fi.

**Sound needs a one-time setup.** The console's sound chip needs its own firmware on the SD card, which homebrew can't include. On first start Syrup offers to copy it from the console's built-in sound app (press A). Or do it yourself:

1. Press **L + Down + Select** to open the Rosalina menu (this works anywhere, even inside Syrup).
2. Choose **Miscellaneous options > Dump DSP firmware**. It saves `dspfirm.cdc` to `/3ds/` on the SD card.
3. Close the menu with B, quit Syrup and open it again.

## What's in it

- **Sound:** two wavetable oscillators plus sub and noise, warp modes (sync, bend, PWM, FM / AM / RM), unison, built-in tables and your own `.wav` wavetables. Two filters, LFOs with classic shapes or your own hand-drawn ones, four macros and a mod matrix. 236 factory patches.
- **Effects:** a reorderable master chain with chorus, phaser, delays (tape, digital, ping-pong, dub), reverbs (hall, room, plate, spring), shimmer, OTT, pump and lo-fi. 15 drive types.
- **Sequencer:** piano roll with 64-step patterns, 16 patterns and song mode, per-step locks (gate, cutoff, slide, nudge, probability), live recording with undo, an arpeggiator and a 303-style RIFF generator.
- **Chords:** chord pads with progressions, bass lines and an arp of their own.
- **Drums:** a 10-row drum sequencer with ratchets, velocity and chance / fill conditions, a sound editor for every drum, 30 preset beats, and synth, chip, wavetable or sample kits.
- **Tape:** four tracks that bounce the sequence in time, with overdub, mute, level and WAV export.
- **Hardware:** the console's second sound chip for chip voices, echo and grains. Tilt, twist, shake, breath (mic) and camera as mod sources. Mic-based velocity for drums. Camera, mic, tape or a web page turned into wavetables.
- **Connect:** Wi-Fi MIDI in and out, Network MIDI (Mac, iOS, DAWs), MIDI clock in and out, IR MIDI, MIDI LEARN, MPE and aftertouch.
- **Phone:** START > LINK PHONE shows a QR code. REMOTE turns the phone into keys, chord pads, drum pads, an XY pad and knobs. 2ND UNIT opens a full second Syrup on the phone, with its own sounds and patterns, on the 3DS's tempo and play / stop.
- **Streaming:** a browser view of Syrup for OBS or other streaming software.

Every page, parameter and control is in [docs/REFERENCE.md](docs/REFERENCE.md). What's planned is in [ROADMAP.md](ROADMAP.md).

## Controls

| Input | Action |
|---|---|
| X / Y | Next / previous page |
| L / R | Octave down / up |
| D-pad up / down | Pick a parameter |
| D-pad left / right | Change it (hold to repeat) |
| Circle pad | Edits the selected value (HARDWARE > CIRCLE PAD switches it to pitch bend + mod, page values or macros) |
| SELECT (tap) | Play / stop (sequencer and drums together) |
| SELECT + B | Record |
| SELECT + circle pad | Quick select: up = any page, left / right = the page's main lists, down = REC / PLAY / TAPE |
| SELECT + L / R | Undo / redo |
| SELECT + D-pad down | Panic |
| START | Menu: sessions, patches, browser, panic, help, theme, MIDI learn, link phone, quit |
| A / B | Chord mode / latch |
| Touch keyboard | Play (lower on a key = louder, slide to glide) |
| XY pad | Wavetable position and cutoff (the corner button changes what it controls) |

The web version takes a computer keyboard, a game controller or a MIDI keyboard.

## Files on the SD card

Everything lives in `sdmc:/3ds/syrup/`: sessions, user patches, settings, your wavetables (`.wav`), sample kits in `kits/`, and tape exports in `tape/`.

## Reporting bugs

Open an [issue](../../issues) with what you did, what happened, and which build you're on. The version is on the splash screen and the HARDWARE page. If Syrup crashed, a photo of the crash screen helps a lot.

## Building

Needs devkitPro (devkitARM, libctru, citro2d / citro3d, 3ds-curl). `make` builds `syrup.3dsx`; the GitHub Actions workflow also builds the `.cia`. The web version builds with Emscripten (`bash web/build.sh`). Host tests (no console needed): `python3 tests/host/run_tests.py`.
