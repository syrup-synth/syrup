# SYRUP

A Serum-style wavetable synth for Nintendo 2DS / 3DS (homebrew).

The 3DS's DSP chip plays the oscillators: each voice layer is a DSP channel looping a single-cycle waveform at the note's pitch, through that channel's own biquad filter. The main processor only rebuilds a cycle when something changes or is modulated:

1. Morph between two wavetable frames at **WT POS**.
2. Apply the **warp**, including FM/AM/RM from the other oscillator, baked into the cycle.
3. Apply **drive**.
4. FFT it into 7 band-limited copies (2048 down to 32 samples), so high notes don't alias.

Waveform rebuilds run on the 3DS's second CPU core (a worker thread on core 1, with the app given up to 80% of that core), so wavetable sweeps and modulation don't load the main core.

## Scales, arpeggiator, sequencer (page ARP+SEQ)

- **ROOT / SCALE:** 14 scales (major, minor, modes, pentatonics, blues, whole tone, Japanese, Arabic). With a scale on, the keyboard becomes 14 pads, one per scale note, root pads highlighted, and chords stack inside the scale.
- **ARP:** up, down, up+down, random, as played, chord repeat; rate 1/4 to 1/32 incl. triplets; 1-4 octaves; gate. Works with latch.
- **SEQ:** 16 steps. REC: each key (or chord) you play writes the next step; ADD REST inserts a rest; the pattern length follows what you record. PLAY: keys transpose the pattern live. SEQ GATE, SWING and BPM. With ARP on, each step is arpeggiated.
- **OCT- / OCT+** tabs on the touch screen shift the keyboard (also L / R).

## Presets

Page 1 is a preset browser organised by style. Use the D-pad to pick a style and a patch; the patch loads immediately, so you can play keys to audition it.

| Style | Patches |
|---|---|
| BASS | Sub Wobble, Reese, FM Growl, 808 Sub, Acid, Wobble 1/8, Digi Bass, Sine Bass, Pluck Bass, Hoover |
| LEAD | Supersaw, Sync Scream, Vowel Lead, Chip Square, Digi Pipe, Brass Lead, Whistle, Glide Square, Fuzz Lead, Fifths |
| PAD | Syrup Pad, Glass, Choir, Warm Strings, Dark Drone, Vapor Pad, Sweep Pad, Formant Pad, Shimmer Pad, PWM Pad |
| PLUCK | Syrup Pluck, Marimba, Kalimba, Harp, Dub Stab, Bell, Steel Drum, Synth Pluck, Muted, Pizz |
| KEYS | E Piano, Organ, Clav, Acordeon, Vinyl Keys, Dark Rhodes, Wurli, Church Organ, Harpsichord, Lofi Piano |
| CHORDS | House Organ, Neo Soul, Power, Sus Shimmer, Dub Chords, Min7 Pad, Maj7 Keys, Power Pluck, Sus Organ, Techno Stab |
| RETRO | 8-Bit Lead, 8-Bit Bass, VGM Brass, Arcade, 80s Poly, Synthwave, Chip Bell, GB Wave |
| AMBIENT | Ocean, Aurora, Dust, Slow Bloom, Dream Keys, Deep Space, Rain, Lullaby |
| FX | Riser, Laser, Wind, Radio, Robot, Siren, Bubbles, Noise Sweep, Alarm, Space Drone |
| USER | 8 save slots |

To save, pick a SAVE SLOT and press right on SAVE NOW. User patches are stored in `sdmc:/3ds/syrup/user_presets.bin`. INIT PATCH resets to a single saw.

## Engine (laid out like Serum)

| Section | What's there |
|---|---|
| OSC A / OSC B | Table, WT POS, warp + amount, level, octave / semi / fine, unison 1-3 with detune, blend, width |
| Warps | Sync, Windowed Sync, Bend+, Bend-, Bend+/-, PWM, Asym+, Asym-, Asym+/-, Flip, Mirror, Remap 1-4, Quantize, FM / AM / RM from the other oscillator |
| SUB | Sine, triangle, saw, square; 0 / -1 / -2 octaves |
| NOISE | White, pink, vinyl crackle |
| FILTER | Low pass, high pass, band pass, notch, peak; cutoff, resonance, keytrack |
| ENV 1 | Amp envelope, AHDSR |
| ENV 2 | Mod envelope, ADSR |
| LFO 1-2 | Sine, triangle, saw, square, S&H; free or retrigger |
| MOD MATRIX | 4 slots. Sources: LFO 1, LFO 2, ENV 2, velocity, circle pad, XY pad. Destinations: A/B WT POS, A/B warp, cutoff, resonance, pitch, A/B level, pan |
| VOICING | Poly, mono, legato; glide; bend range; chord mode |
| FX | Chorus, echo (ping-pong), space (early reflections), reverb (size, damp), FX mix, drive (tube, hard, fold, rectify), master |

## Controls

| Input | Action |
|---|---|
| X / Y | Next / previous page (OSC A, OSC B, SUB+NOISE, FILT+ENV, LFO+MOD, VOICE+FX) |
| D-pad up / down | Pick a parameter |
| D-pad left / right | Change it (hold to repeat) |
| Bottom tabs | HELP, < PAGE, PAGE >, LATCH |
| XY pad | X = OSC A WT POS, Y = filter cutoff |
| Keyboard | Play; lower on a key = louder; slide to glide |
| A | Chord mode |
| B | Latch |
| L / R | Octave |
| Circle pad | Left/right pitch bend, up/down = STICK mod source |
| SELECT | Help |
| START | Quit |

## Wavetables

Built in: Basic Shapes, PWM, Harmonix, Vowels, Sync Saw, FM Growl, Digital.

Your own: copy Serum-format wavetables (`.wav`, 2048-sample frames, 16/24/32-bit) into `sdmc:/3ds/syrup/`. They appear after the built-in tables.

## Install

FBI > Remote Install > Scan QR Code, and scan `install-qr.png`. Sound needs the DSP firmware dump (run DSP1 once).

## Differences from Serum

- WT POS and warp modulation are shared by all notes playing (one cycle per oscillator). Filter, pitch, level and pan modulation are per note.
- FM / AM / RM from the other oscillator use whole-octave ratios.
- Two sound units are used together: the DSP (24 filtered channels) plays OSC A / OSC B plus one shared noise channel; CSND (the 3DS's second sound unit, ~24 channels, no filter) plays the sub and all effect taps. CSND layers are "filtered" by picking a darker band-limited copy of the waveform to match the cutoff.
- With full unison on both oscillators a note uses 6 DSP channels, so 4 such notes fit; with 3-voice unison on A alone, 8.
- Effects are built from extra voices (homebrew can't route the DSP's effect buses back for processing). REVERB is 8 diffuse taps per note: spread, slightly detuned, each darker and with a longer release. Drive is applied to the waveform.
- Envelopes and LFOs run at about 60 Hz.
