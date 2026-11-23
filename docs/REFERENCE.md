# SYRUP: full reference

![SYRUP](../art/syrup_logo.png)

A Serum-style wavetable synth for Nintendo 2DS / 3DS (homebrew).

The 3DS's DSP chip plays the oscillators: each voice layer is a DSP channel looping a single-cycle waveform at the note's pitch, through that channel's own biquad filter. The main processor only rebuilds a cycle when something changes or is modulated:

1. Morph between two wavetable frames at **WT POS**.
2. Apply the **warp**, including FM/AM/RM from the other oscillator, baked into the cycle.
3. Apply **drive**.
4. FFT it into 7 band-limited copies (2048 down to 32 samples), so high notes don't alias.

Waveform rebuilds run on the 3DS's second CPU core (a worker thread on core 1, with the app given up to 80% of that core), so wavetable sweeps and modulation don't load the main core.

## Scales, arpeggiator, sequencer (page ARP+SEQ)

- **SEQ RATE**: what a step is: 1/16, 1/32 (twice as fast: 16 steps = half a bar) or 1/8. The drums, the gate and tape lengths in bars stay in real time.
- **REC QUANT**: 16TH, 8TH, 32ND (a note lands on the step or half a step after it, as NUDGE +50) or OFF (your timing, as nudge).
- **CLICK** (the metronome): OFF, COUNT-IN, RECORDING (the count-in, then while the tape or live REC records) or ALWAYS (whenever the sequence or drums run). CLICK LEVEL sets its volume. It plays straight to the speakers from the second chip, so it never ends up on the tape, even with CHIP > FX on.

- **ROOT / SCALE:** 14 scales (major, minor, modes, pentatonics, blues, whole tone, Japanese, Arabic). With a scale on, the keyboard becomes 14 pads, one per scale note, root pads highlighted, and chords stack inside the scale.
- **ARP:** up, down, up+down, random, as played, chord repeat; rate 1/4 to 1/32 incl. triplets; 1-4 octaves; gate. Works with latch.
- **SEQ:** 16 steps. REC: each key (or chord) you play writes the next step; ADD REST inserts a rest; the pattern length follows what you record. PLAY: keys transpose the pattern live. SEQ GATE, SWING and BPM. With ARP on, each step is arpeggiated.
- **OCT+ / OCT-** (the column left of the keys) shift the keyboard an octave. **L / R** step the bottom screen's tabs (OSC, FILT, MOD, VOICE, FX, SEQ, PADS). **X / Y** step through the pages.

## Hardware effects and sensors

- **DSP effect buses:** Syrup ships its own copy of libctru's sound driver (`source/ndsp/`, zlib licence) with the aux-bus callback implemented. Every DSP channel sends to two aux buses; REVERB runs a stereo Freeverb-style reverb on bus 1 and ECHO a darkening ping-pong delay (ECHO TIME, ECHO FDBK) on bus 2, processed in the audio thread every 160 samples. If the DSP never delivers aux audio, Syrup falls back to the old per-note tap effects automatically (status line on VOICE+FX: FX BUS OK / ON / FALLBK).
- **Second filter stage:** FILTER 2 uses each DSP channel's one-pole filter: +6DB SLOPE (18 dB low pass) or LOW CUT (30 Hz to 1 kHz).
- **OUTPUT:** STEREO or the DSP's virtual SURROUND (with depth). Soft clipping is on.
- **Tilt and breath:** mod sources TILT L/R, TILT F/B (accelerometer, centred on how you hold it at start; TILT CENTER re-centres) and BREATH (microphone level). Patches: TILT WOBBLE, BREATH LEAD, TILT PAD.
- **Clock:** the arpeggiator and sequencer run in the audio thread on every DSP frame (~4.9 ms) instead of once per screen refresh.

## Look

The original amber look with a tab bar across the top (PATCH, OSC, SHAPE, FX, PLAY, VIS) and sub-pages under it, bracket frames on the graphics, and a footer that explains the selected control. FILTER+ENV shows both envelopes as real A-H-D-S-R shapes (stage of the selected control highlighted, a dot riding the shape for the newest note). VIS page: THEME (ORIGINAL, STICKER, or EGA PIP GREEN / DOS BLUE / AMBER EGA / MAGENTA) and SCANLINES.

## LFOs

Two LFOs with 13 shapes: sine, triangle, saw, square, S&H, steps, bounce, exp rise/fall, 25% pulse, and wavetable shapes: WT OSC A / WT OSC B (the oscillator's live, warped cycle) or WAVETABLE (any frame of a built-in table, picked with LFO TABLE and LFO WT POS). LFO SYNC locks the rate to the tempo (4 bars to 1/16T). The LFO+MOD page draws both LFOs with a moving playhead. Patches: VOWEL TALK (FX), TABLE DRIFT (PAD).

## MASTER FX (FX tab)

With MASTER FX on, every DSP voice goes through the effect bus to a chain that runs in the audio thread: LOFI (bits, rate) -> MOD FX (chorus, flanger, phaser, comb, formant; FX COLOR is a mod destination) -> 3-band EQ -> tape delay (tempo sync, wow/flutter, saturation) -> reverb (shimmer, freeze) -> OTT multiband compression -> sidechain PUMP (locked to the beat / kick) -> limiter. DELAY MIX / REVERB MIX add to the FX ECHO / REVERB amounts, so older patches sound the same. The page shows the chain and its CPU use. The sub, drums and tape (second sound chip) stay dry. Every factory patch is tuned for the chain (tempo-synced tape delays on leads, plucks and arps; chorus and shimmer on pads; OTT on basses; pump on house chords; lo-fi on retro sounds). Showcase patches: OTT GROWL, FLANGE LEAD, SHIMMER HALL, TAPE KEYS, PUMP CHORDS, TALK BOX, COMB DRONE.

**Patches (Nov 2026):** new DARK 808 and 303 SCREAM (BASS), CINE SWELL (PAD), TAPE RHODES (KEYS), NEON PWM (RAVE). BIG UNISON, STRINGS 77, SHOEGAZE WALL and the RAVE supersaw use fewer unison voices per note (5 / 4 / 4 / 4), so chords don't steal notes from the 24 sound channels. REESE CRATE, MOTION HAZE, TIDAL and the supersaw gained motion lanes; these and the new ones open with a fitting XY PAD mode.

**Cinematic set:** BREATH PAD (SPACE: wavetable and out filter breathing over 8 bars, out of step), TENSION DRONE (TEXTURE: low sine + pink noise, two slow LFOs at unrelated rates for a drift that doesn't repeat, tape delay, swell reverb), SOFT PLUCK (PLUCK).

**SPEAKER EQ** (HARDWARE): a gentle high-pass (~120 Hz) and low-pass (~12 kHz) at the end of the MASTER FX chain for the console speaker. OFF / ON / AUTO; AUTO (the default) turns it on only when headphones are out. It works when MASTER FX is on and is saved with the console settings, not the patch.

**XY PAD PAGE mode** (the new default): the pad follows the page you're on. OSC A / OSC B: X = WT POS, Y = WARP AMT. SUB+NOISE: SUB LEVEL / NOISE LEVEL. VOICE+FX: DRIVE / FX MIX. DRUMS: DRUM LEVEL / SWING. Other pages: WT POS + CUTOFF. Axis labels show what it's moving.

**KARPLUS wavetable** (PLUCK): each frame is a plucked string after more trips round its delay line, bright at frame 0 and dark at the end. KS GUITAR, KS BASS and KS HARP sweep it with ENV2, harder hits stay brighter. The meters on unselected rows are dimmed so the selected one stands out.

**DRY check** (FX ORDER view, next to RESET): switches every effect off for a listen: the master chain, pump, delay/reverb sends and echo taps. The OUT FILTER and limiter stay, since they are part of the sound. If the patch falls apart dry, the fix is in the oscillator, octave or envelope. The header says DRY while it is on; it is not saved.

**INIT PATCH** comes with the macros wired (they do nothing at 0): M1 opens the filter, M2 moves A's wavetable, M3 adds resonance, M4 is the FX MIX (at 0 the patch is dry; raise it and the reverb comes in); the XY pad starts on MACRO 1+2. FX MIX is also a destination in the mod matrix now, for any patch.

**FX CHAIN:** tap a block on the bottom screen and its settings come up on the top screen; MOD FX, SPACE (delay and reverb types) and LOFI show their types under the chain, tap one to change the effect. Selecting a setting on the top screen highlights its block. The bottom screen of the MASTER FX page (or the CHAIN chip) shows the chain as five blocks: LOFI, MOD FX, EQ, SPACE (delay and reverb, side by side) and OTT. Drag a block left or right and the chain changes while you hear it (lofi after the reverb crushes the tails; OTT first, before the EQ, pumps the dry sound only). A lit block is doing something. PUMP and the limiter always stay last. RESET puts back the default order. The order is saved with the patch (patch format v13; older user patches load with the default order).

**Shape editors:** FILT+ENV's bottom screen has touch editors for ENV 1, ENV 2 (drag the handles: attack, hold, decay, sustain, release) and FILTER (drag the dot: left/right = cutoff, up/down = resonance; LP...PEAK buttons and F2). LFO+MOD has MATRIX, LFO 1+2 (tap the shape or sync box for a list, drag the wave for the rate), LFO 3, LFO 4 and MOTION tabs.

## DRUMS (PLAY tab)

A synthesised kit (808, 909, LOFI, CHIP) on three channels of the second sound chip with KICK / SNARE / HAT pattern presets, DRUM LEVEL, BPM and SWING. It follows the sequencer when that's playing and drives the PUMP.

**The DRUMS grid** (the DRUMS page's bottom screen; SELECT + circle pad left gets there): a row for each of the 10 drums, 16 steps.
- Tap a step: on / off; drag along a row to paint. Tap a drum's name to hear it and pick the row.
- **Hold a step**: VELOCITY, RATCHET (1-4 hits in the step) and a condition: 90 / 75 / 50 / 25 / 10 %, 1:2 ... 4:4 (plays on loop A of every B of the row), FILL / !FILL (while FILL is held), PRE / !PRE (the last condition on this row), NEI / !NEI (the row above), 1ST / !1ST (the first loop after start). Steps show a dot per extra ratchet and a corner when they have a condition.
- **Hold a drum's name**: its SOUND (TUNE, PITCH ENV and its decay, DECAY up to twice as long, TONE darker / thinner, DRIVE, LEVEL, PAN) and the row's RATE (X1, 1/2, X2, 3/4, 3/2, 1/4, 1/8) and LENGTH (1-16): every row can run its own polymeter. The kit rebuilds when you let go and plays the drum.
- **Header**: DR ON / OFF, KIT, BEAT (the next of 30 whole-kit rhythms each tap: HOUSE, BOOM BAP, TRAP, DNB, BREAK, DISCO, FUNK, HALFTIME, DEMBOW, AFROBEAT, 2-STEP, ELECTRO, ROCK, SHUFFLE, CUMBIA, BOSSA, SAMBA, RUMBA, BEGUINE, CHA-CHA, TANGO, MAMBO, MARCH, SWING, SLOW ROCK in 12/8, WALTZ in 3/4, REGGAE, FOXTROT, TECHNO, GO-GO), CLR (then a name clears that row; CLR again clears all), MIC (knocks near the mic play and record onto the picked row), FILL (hold).
- KICK / SNARE / HAT patterns are pastes: picking one writes it into the row, which then edits like your own. EUCLID rows stay live.
- **Circle pad**: moves a cursor on the grid (the first push shows it); A = hit, hold A = the step editor, B = take the hit away, hold B = the row's SOUND (with no cursor up, A or B brings it up). In the editors up / down picks a setting, left / right changes it, B closes. The piano roll works the same way (A = note, hold A = its step editor, B = take the note away).
- Everything is saved on the SD card and SELECT + L undoes it.

## Macros and mod matrix

Four MACRO knobs (mod sources MACRO 1-4); XY PAD can play MACRO 1+2 or 3+4 instead of WT POS + CUTOFF. Eight mod slots, each with a CURVE (linear, exp, log, S).

## HARDWARE page (HW tab)

- **TAPE** (SEQ page, or the quick ring): four tracks recorded from Syrup's output, starting with the sequence so every take lines up. The tracks already on tape start with the sequence too. TRACK SPEED on each track: 1/4 1/3 1/2 2/3 3/4 1X 4/3 3/2 X2 X3 X4 (faster ones repeat, so they stay on the bar). TAPE ALIGN (ARP+SEQ page) trims new takes by a few ms if they sit early or late. Drums, kits and the sub are recorded through CHIP > FX, which the tape turns on by itself.
- **TAPE > TABLE / MIC > TABLE / CAM > TABLE:** turn the last tape track, one second of mic (sing or play into it), or a camera photo (each image row becomes a frame) into a 64-frame wavetable. It's saved as a Serum-style .wav in /3ds/syrup/ and loaded into OSC A. Pitched audio is cut into clean single cycles; anything else becomes even slices.
- **UPDATE SYRUP:** checks the latest build over Wi-Fi and installs it (CIA through the system installer, or replaces /3ds/syrup.3dsx when run from the Homebrew Launcher). Close and reopen Syrup afterwards.
- **GET WAVETABLES:** downloads the Syrup Pack (35 original wavetables in `wavetables/` of this repo) over Wi-Fi into /3ds/syrup/, skipping ones you already have. Any Serum-format .wav you copy there works too.
- **CAMERA:** CAM LIGHT and CAM COLOR mod sources (thumbnail on the page).
- **Motion:** TWIST (gyroscope) and SHAKE mod sources join TILT and BREATH.
- **LINK:** sync tempo and sequencer steps with another console running Syrup, over local wireless (one HOSTs, the other JOINs) or infrared (IR SEND / IR RECEIVE, consoles pointed at each other).
- **BEAT LIGHT:** both screens' backlights flash on each arp / sequencer step.
- OUTPUT AUTO (VOICE+FX) switches to surround when headphones are plugged in; New 3DS / New 2DS run at 804 MHz automatically.
- Visualizer: TRAILS (the GPU keeps and fades the last frame) and PARTICLES (sparks on notes and beats).

## Visualizer page

The last page (VISUALIZER) fills the top screen with GPU visuals while you play on the bottom screen: WAVETABLE (big 3D table), SCOPES, ORBIT (OSC A against OSC B, spirograph trails) and SPECTRUM (harmonic bars), VOXELS (a scrolling 3D block landscape of the notes you play), VOXEL TABLE (the wavetable as blocks, playing frame lit), PLASMA (the GPU's procedural-texture unit draws a flowing plasma by itself; the CPU only cycles its colour table, faster and brighter as the sound gets louder, with the oscillator's live cycle glowing on top and sparks on loud hits), or AUTO to cycle every 10 s. Arp/sequencer steps pulse the background. D-pad picks VIS MODE / VIS OSC / OVERLAY.

## Graphics chip visuals

On the OSC and sound pages the left panel of the top screen is drawn by the 3DS's GPU (PICA200, via citro2d): a slowly swaying 3D wavetable with the live (warped) cycle glowing and leaving trails, and glowing scopes with phosphor trails on the other pages. It's rendered at 2x and scaled down by the display-transfer hardware for smooth lines; colour follows the filter and envelope. Turn it off with GPU VISUALS on the VOICE+FX page.

## Presets

Page 1 is a preset browser organised by style. D-pad left / right changes style, up / down moves through the patches (hold to scroll fast); each patch loads as you land on it, so you can play keys to audition it. The right side shows what's loaded and whether you've edited it.

The factory library: 154 patches in 14 styles, with no duplicates, and levels matched so switching patches doesn't jump in volume. Every patch plays from the keys, buttons and circle pad alone. Mic, tilt and camera are optional extras, never required.

| Style | Patches |
|---|---|
| BASS | Sub Floor, 303 on 3rd Ave, Reese Crate, Wobble Tide, FM Knock, Diode Buzz, SP-12 Drop, Cumbia Bajo, Rolling Notch, Stick Growl, Upright Thumb, Chip Bass |
| LEAD | Gulf Sync, Talk Box, Wide Unison, Whistle, Fold Scream, Ladder Tape, Chip Hero, G-Funk Sine, Ringer, Acid Scream, Pad Bender, Flute, Stick Theremin, XY Sculpt |
| PAD | Warm Felt, Glass Hour, Vowel Dusk, Comb Weather, Cloud Bank, Strings 77, Drawn Swell, Motion Haze, Swell Backward, Deep Sonar, Grain Choir, Gate 85, Spin Pad (rotate the console to swing it), XY Morph (the XY pad plays MACRO 1 + 2) |
| KEYS | EP Saturday, Drawbar, Lofi Upright, Clav Funk, Wurli Bark, Vibes, Toy Keys, Harpsi, Rotary, Cumbia Organo, Reel Piano (high notes darker), Glass Top |
| PLUCK | Nylon, Marimba Wood, Kalimba, Digi Harp, Stab 1999, Koto Spring, Chip Plink, Grain Pluck, Steel Pan, Rain on Tin |
| CHORD | Organ Stab, Neo Soul 9, Dub Chord, Techno Stab, Strum Guitar, Omni Harp, Pump Chords, Gospel Organ, Shoegaze Wall |
| SEQUENCE | Berlin 16, Acid Circle, Trance Gate, Ping Arp, Chaos Arp, Motion Bells, Octave Bounce, Drawn Steps, Grain Run, Polyrhythm |
| TEXTURE | Tide Pool, Frozen Table, Net Static, Tape Room, Brown Sea, Geiger, Drone 60, Shimmer Cloud, Sonar Ping, Storm Front, Freeze Glass (reverb freeze), Jet Comb, Wire Drone |
| CHIP | GB Wave, NES Pulse, C64 PWM, Arcade Arp, Power Up, Laser, 8-Bit Bass, Bit Organ, Square Choir, 4-Bit GB, Chip Sweep, Filter Drums, OTT Chip Bass (these three run CHIP > FX: the chip square and drums through a real filter and the master effects) |
| SPACE | Orbit Pad, Helicopter, Circle Arp, Around You, 3D Bells, Spiral Pluck, Passing Car, Steer Orbit (these switch OUTPUT to SURROUND when it's on STEREO; headphones work best) |
| CHIPSET | PSG Stack, Echo Canyon, Crush Bell, 8-Bit Choir, Capture Dub, Two Chips, Quant Grit, GB Orbit, Double Echo, Sub Chip, Scream Lead, Raw Sampler, Hot Organ, Glitch Storm, Pitch Canyon (the 2DS's second sound chip: CHIP SQUARE, CHIP ECHO through its capture units, bit crush, 3D orbit) |
| MOTION | Busy Boy, Tidal Table, Step Sculpt, Glass Breath, Chaos Garden, Acid Morph, Vowel Walk, Liquid Sync, Drawn Dance, Grain Tide, Talk Box, Gyro Vibe, Hand Wave, Shake Grit (shake for grit, turn for brightness), Sing It (the table follows your voice) (motion lanes, drawn LFOs, chaos and grains; Busy Boy uses nearly everything at once; Talk Box needs the mic on, Hand Wave the camera) |
| QUIRKS | Glass Pluck, Scream Solo, Acid Scream, Grit Keys, Corrupt Perc, Sub Scream, Drive Mono, Tape Dub, Vowel Pad, Turntable (record stop, 12-semitone bend), Scream Dub (one patch per hardware trick, in listening-test order; Glass Pluck is the clean reference) |
| RAVE | Rave Strings, Dream Pad, Hoover Pad, Supersaw Pump, Euro Choir, Flange Pad, House Piano, Organ Bass, Rave Stab, Deep Chord, Euro Bass, Euro Lead, Hoover (euro house and rave; the stabs play minor 7 chords) |
| DRUMS | 808 Kit, 909 Kit, LoFi Kit, Chip Kit, Perc Kit (shaker, maracas, cabasa, tambourine, guiro, claves, two congas, bongo, woodblock; shake the console and it plays the shaker): the keyboard becomes 10 drum pads (kick, snare, clap, rim, closed and open hat, two toms, cowbell, crash); lower on a pad hits harder. MIDI notes follow General MIDI (36 kick, 38 snare, 42 hat, 46 open hat, 49 crash ...), so a DAW, the FM-1 or Drambo plays the right drums; the arpeggiator and sequencer play them too. Any patch becomes a kit with voice MODE 808 KIT ... CHIP KIT. Drums play on the second sound chip, so they skip MASTER FX. |
| GROOVE | Boom Bap Bass, Trap SP-12, Euclid Chip, Beatbox Tap, Loop Partner, Tape Loop, Variecho, Slowdown: patches that bring a beat (drums, kit, patterns, euclid rows, swing, tempo), a loop setup (bars, speed, wow) or tape movement with them. Beatbox Tap arms MIC TAP so you knock the beat in; Slowdown runs the loop only while you hold notes |
| USER | 8 save slots, plus INIT PATCH at the end of the list |

Patches (format v12) also carry the drums (on/off, kit, patterns, euclid rows, level, swing), but only when the patch has a beat. A beat that came with a patch stops when you move to a patch without one; a beat you started or changed yourself keeps running whatever patch you pick. CHIP > FX, ECHO WOW and the XY PAD mode follow every patch. Older user patches load with these at their defaults.

To save, press A twice on the PATCH page: the first press shows which USER slot it will use (the first empty one, or the slot you're on in USER), the second opens the 3DS keyboard to name the patch (up to 13 characters). Pressing A twice on a saved USER slot saves over it and lets you rename it. User patches are stored in `sdmc:/3ds/syrup/user_presets.bin`. INIT PATCH (the last entry in USER) resets to a single saw.

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
| LFO 1-4 | LFO 1-2: 13 shapes including wavetable shapes, free or retrigger, tempo sync. LFO 3-4: drawn shapes |
| MOD MATRIX | 8 slots with response curves. 27 sources (LFOs, envelope, velocity, note, random, circle pad, XY pad, macros, chaos, tilt, gyro, shake, mic, camera), 13 destinations (A/B WT POS, A/B warp, cutoff, resonance, pitch, A/B level, pan, FX colour, volume, out cutoff) |
| VOICING | Poly, mono, legato; glide; bend range; chord mode |
| FX | Chorus, echo (ping-pong), space (early reflections), reverb (size, damp), FX mix, drive (tube, hard, fold, rectify), master |

## Controls

| Input | Action |
|---|---|
| Start | A splash screen shows the logo while Syrup loads (tap or press any button to skip; it moves on by itself after 4 s). Syrup opens on the page you were on last time (first run: the browser). CIRCLE PAD and SPEAKER EQ are kept with it in settings.bin |
| L / R | Octave down / up (on the piano roll: scroll an octave) |
| SELECT + circle pad | QUICK SELECT, drawn over the page: push UP for the row of every page, LEFT / RIGHT for the page's two main lists (style + patch, the table, filter types, the kit, the arp ...), DOWN for REC / PLAY-STOP / TAPE REC. Lean along a bar to slide, let go of SELECT to take what's lit |
| SELECT + L / R | Undo / redo: pattern edits (notes, RIFF, CLEAR, the PAT tools) and patch edits, 12 steps |
| SELECT + D-pad down | PANIC |
| X / Y | Next / previous page, all of them (OSC A, OSC B, SUB+NOISE, FILT+ENV, LFO+MOD, VOICE+FX) |
| Bottom top row | Jump chips: OSC, FILT, MOD, VOICE, FX, SEQ, PADS. One tap goes to that part of the synth; tapping the lit chip again steps inside it (OSC A, OSC B, SUB+NOISE; ENV 1 / ENV 2 / FILTER; the MOD views). PADS lays the chord pads over any page |
| PERF row + key column | Above the keys: SCALE (scale on/off, back to the last one), PLAY/STOP (sequencer), PANIC. Left of the keys: OCT+, OCT-, CHRD (chord on/off, last chord), LTCH (latch). A touch that starts on the XY pad's bottom edge never moves the pad |
| XY pad mode button | Top right of the XY pad steps through WT + CUTOFF, MACRO 1+2, MACRO 3+4, CUTOFF + RES, WT A + WT B, FX MIX + COLOR and PERF (four sliders: CUTOFF, RES, WT POS, FX MIX over a smaller pad that plays macros 1 + 2). When modulation is moving an axis, a ghost square shows where the sound really is, with a line back to your finger |
| Circle pad | HARDWARE > CIRCLE PAD: DATA (default): left / right edits the value selected on the top screen (push further for faster), up / down stays the STICK mod source. BEND + MOD: pitch bend and STICK. PAGE: pushes two values on the page you're on. MACROS: M1 / M2 |
| Keyboard touch | Black keys answer 3 px past their drawn edges (the resistive screen and stylus wander); a key you hold keeps the note until the stylus is 3 px into the next key, so a slip doesn't retrigger |
| Matrix slot | Tap a box to pick, hold it to clear the slot |
| VISUALIZER / HARDWARE pages | Bottom screen: a tile per visualizer mode (plus OVERLAY); live sensor meters and the camera picture |
| D-pad up / down | Pick a parameter |
| D-pad left / right | Change it (hold to repeat) |
| Bottom tabs | HELP, < PAGE, PAGE >, LATCH |
| XY pad | X = OSC A WT POS, Y = filter cutoff |
| Keyboard | Play; lower on a key = louder; slide to glide |
| A | Chord mode |
| B | Latch |
| L / R | Octave down / up |
| Circle pad | Left/right pitch bend, up/down = STICK mod source |
| SELECT | Tap: PLAY / STOP the sequencer and the drums (on release). Held: QUICK SELECT, undo / redo, panic (above) |
| START | Quick menu over the top screen: SAVE PATCH, BROWSER, PANIC, HELP, THEME, QUIT (D-pad + A; B or START closes it). The touch screen keeps playing while it's open. |

## Wavetables

Built in: Basic Shapes, PWM, Harmonix, Vowels, Sync Saw, FM Growl, Digital.

Your own: copy Serum-format wavetables (`.wav`, 2048-sample frames, 16/24/32-bit) into `sdmc:/3ds/syrup/`. They appear after the built-in tables.

## Install

FBI > Remote Install > Scan QR Code, and scan [`install-qr.png`](../install-qr.png). Sound needs the DSP firmware dump (run DSP1 once).

## Differences from Serum

- WT POS and warp modulation are shared by all notes playing (one cycle per oscillator). Filter, pitch, level and pan modulation are per note.
- FM / AM / RM from the other oscillator use whole-octave ratios.
- Two sound units are used together: the DSP (24 filtered channels) plays OSC A / OSC B plus one shared noise channel; CSND (the 3DS's second sound unit, ~24 channels, no filter) plays the sub and all effect taps. CSND layers are "filtered" by picking a darker band-limited copy of the waveform to match the cutoff.
- With full unison on both oscillators a note uses 6 DSP channels, so 4 such notes fit; with 3-voice unison on A alone, 8.
- Effects run on the DSP's aux buses: Syrup's own copy of the sound driver routes every DSP channel to two buses that the ARM11 processes (MASTER FX chain, reverb, echo). The sub, drums, chip layers and tape on the second sound chip stay dry. Drive is applied to the waveform.
- With MASTER FX on core 2, the DSP mix comes out one frame (~4.9 ms) after the second sound chip's parts.
- Envelopes and LFOs update once per screen frame (about 60 Hz); the arpeggiator, sequencer and drums run in the audio thread (~4.9 ms).

## Screens

- **Values:** the selected parameter's value is drawn bold. While you change a value it also appears large in the middle of the top screen for a moment, with a bar showing where it sits in its range; values that go both ways (pan, warp, macros, bend) grow from a centre line so the sign shows at a glance.
- **Filter response (FILT+ENV page):** the voice filter and FILTER 2 drawn as a curve from 30 Hz to 15 kHz, with the harmonics of OSC A at the note you're playing: dim before the filter, bright after it. The dotted line is the cutoff.
- **Keyboard:** a key lights brighter the harder you play it, keys you let go of glow for a moment (slides leave a trail), the held chord's name shows in chord mode, and LATCH frames the keyboard.
- **Sequencer strip:** while the sequencer plays, a thin bar over the keyboard shows every step on every page: notes lit, accents brighter, the playing step brightest, swing as uneven widths.

## CHIP > FX and tape speed (HARDWARE page)

- **CHIP > FX:** the second sound chip's parts (sub, CHIP SQUARE, the drums and kits) stop going straight to the speakers. Its two capture units record them instead, and DSP channels 19 and 20 play that recording through a low-pass (CHIP CUTOFF, CHIP RES) and the MASTER FX chain, so the drums get reverb, delay, OTT and pump. Both chips run off the same clock, so it never drifts; it adds about 10 ms. It uses both capture units, so CHIP ECHO is off while it's on (use the master delay instead). The tape stays dry.
- **ECHO WOW:** the chip echo's replay speed drifts, so each repeat bends like a tape echo.

## Chip-level extras (no CPU in the audio path)

- **CHIP SQUARE** (SUB+NOISE page): a layer from the second sound chip's own square-wave generators, with CHIP WIDTH (pulse width) and CHIP OCTAVE.
- **CHIP kit** (DRUMS): snare and hat are sampled LFSR noise (15-bit and 7-bit), played from the second sound chip.
- **CHIP ECHO / ECHO TIME / ECHO REPEATS** (DRUMS): a ping-pong echo made by the chip's two capture units recording its own output. It echoes the sub, drums and chip layer.
- **Tape**: stored as IMA-ADPCM, which the chip decodes itself: up to 2 minutes a track.
- **GRAINS** (HARDWARE): granular playback of the loop. The DSP plays overlapping slices back to back. Set SIZE, SCAN (50% = frozen), SPRAY, PITCH and LEVEL.
- **3D ORBIT / ORBIT RATE** (MASTER FX): with OUTPUT set to SURROUND, the sound circles around you.
- **New visualizer modes**: TUNNEL, KALEIDO and RIPPLES.

## Step grid and RIFF lines (PLAY page)

On the PLAY page the bottom screen's XY pad becomes a 16-step grid. Each of the 8 rows is a note of the current scale; with SCALE off the rows are a major scale.

- **Placing notes:** tap a cell to place a note, and you'll hear it. Tap it again to cycle through accent (dark dot), slide (tail into the next step), accent + slide, then off. Up to 4 notes per step.
- **Piano roll:** the SEQ page's bottom screen is a piano roll: pitch up the side with a keyboard, the 16 steps across. Tap an empty cell for a note; drag from it for a longer note; tap a note to remove it; hold a cell for the step editor. One octave shows at a time; drag the keys up or down to scroll, L / R or OCT- / OCT+ jump an octave, and the side keys light while their notes play. Drag a note's head (or up / down) to move it; drag its tail to stretch it. The LANE under the notes edits every step: VEL, GATE, PROB as bars (drag up / down, or across the steps), ACC and SLD as switches; tap the lane's name to switch. How much of a note's bar is lit shows its velocity; dots over a step = ratchet. A note with step locks shows them as letters inside it, tracker-style: C cutoff, R res, P pitch, G gate, N nudge, % chance (as many as fit; a one-step note shows the first). The keyboard and play row are hidden on this page (PLAY / STOP is in the roll's button row).
- **Pages (long patterns):** a pattern runs up to 64 steps, 4 pages of 16. The strip under the piano roll (and under the drum grid) shows the pages: tap one to see it, tap one past the end to make the pattern that long (drum rows repeat what they play into it), tap the page you're on to turn FOLLOW on / off (the view turns with the playhead), COPY> puts this page on the next. <LEN / LEN> go to 64. Files from before pages load into page 1.
- **Drum tables:** the built-in table **DRUM HITS** holds 8 drums (kick, snare, tom, clap, hat, rim, cowbell, zap), 8 frames each from the hit to its tail; **KIT > TABLE** (HARDWARE page) makes the same from the drum kit you have, sound edits and all, and puts it on OSC A. Sweep WT POS through one drum with a fast envelope and it plays the hit, at any pitch: the **TABLE KICK / SNARE / TOMS / METAL / ZAP** presets (SEQUENCE) are set up that way.
- **Step editor:** VEL, GATE, PROB, CUTOFF / RES locks, PITCH, NUDGE, ACCENT, SLIDE, RATCHET (2-4 hits in the step), LENGTH (of the note you held), and for that pitch ROW LEN / EUCL / ROT (its own rhythm, up to 8 pitches a pattern). <ST / ST> walk the steps; RND loosens the step (velocity, timing, a little chance to skip). Two tabs of full-width sliders: NOTE (VEL, GATE, PROB, NUDGE, ACCENT, SLIDE, RATCHET, LENGTH) and LOCKS + ROW (CUTOFF, RES, PITCH, ROW LEN / EUCL / ROT). A slider keeps the row you pressed on, so slipping up or down while dragging doesn't change another setting. Tap a name to select it without moving it; ACCENT and SLIDE flip with a tap. The circle pad nudges the selected setting one notch at a time (faster the further you push), whatever CIRCLE PAD is set to.
- **UNDO** (in PAT, or SELECT + L anywhere): takes back RIFF, CLEAR, REV, ROT, HUM, a PASTE, note edits; SELECT + R redoes. CLEAR in the menu takes two presses of right.
- **REV / ROT / HUM** (in PAT): REV plays the pattern back to front (a long note ends where it used to start); ROT moves everything one step later, the last step wrapping to the first; HUM nudges each note's velocity (±20%) and timing (±10% of a step).
- **COPY / PASTE** (step editor, under the sliders): copies a whole step, notes, lengths and locks, onto another step.
- **TAPE** (PAT > TAPE >>>): bounce the sequence to 4 tracks. Pick the length (4 to 64 bars, or SONG: the song chain), ARM a track, press REC: the sequence (or song) starts from the top and Syrup's output is recorded to that track; the tracks already on tape play along on the second sound chip, so change the patch and bounce the next part over them. The next empty track arms itself. Each track has MUTE, a level slider and X (tap twice to erase). PLAY plays the tape; WAV saves track1-4.wav and mix.wav to /3ds/syrup/tape on the SD card. The first track sets the length (up to 2 minutes); erase every track to change it. The recorder hears the DSP; the drums play on the second chip, so tape REC turns CHIP > FX on by itself when they're running. The tape is in memory: it's gone when you quit unless you save the WAVs. PREROLL (OFF / 1 BAR / 2 BARS) counts in before REC; CLICK: OFF, COUNT-IN, or ALWAYS (keeps clicking while recording). The click plays on the second chip, so it never lands on tape. Both are on the tape view and the ARP+SEQ page.
- **Live REC** (REC on the roll's button row): counts in (PREROLL), then the pattern plays from the top and whatever you play on the keys (any page with keys) or over MIDI is written to the nearest 16th, as long as you hold it; it keeps overdubbing as the pattern loops. REC again stops; UNDO in PAT takes the whole take back. The keys play instead of transposing while it records. REC QUANT (ARP+SEQ page): 16TH, 8TH, or OFF (your exact timing, kept as the step's NUDGE).
- **WAVE kit** (DRUMS > KIT, or the WAVE KIT patch / voice MODE): every drum is made from OSC A's wavetable at its WT POS. The kick and toms sweep down from the table's bright end with a falling pitch; the snare is a table tone plus "table noise" (the table read at random phases); hats and crash are that noise ring-modulated by the table up high; rim is a short table ping, clap bursts of table noise, bell two table voices ring-modulated. Change OSC A's table or move WT POS and the kit rebuilds a moment later, so the drums follow the sound.
- **Drum shaping** (DRUMS page): KICK / SNARE / HAT TUNE move each row's pitch ±12 semitones live, applied to every hit (SNARE TUNE also moves clap and rim, HAT TUNE the open hat and crash). KICK / SNARE / HAT CHANCE: how often each pattern hit of that row plays (lower = looser, ghostlier grooves). MUTE silences rows (any combination) without touching the patterns; the kit pads still play. The kicks also have a short 1.2 kHz beater click now, so they read on the 2DS speaker.
- **TAP REC** (ARP+SEQ page, works during live REC): knock on the table near the 2DS and the mic hears it; harder = louder, onto the grid by REC QUANT. NOTES writes the last pitch you played or put on the roll (you hear it at once); VELOCITY only re-sets the velocity of what's already on that step (a hard knock also sets ACCENT), so you can tap the dynamics into a pattern. Off while the drums' MIC TAP is on.
- **Patterns and song (PAT):** 16 patterns, A to P. Tap one to switch; while playing, it waits for the end of the current pattern. COPY> copies the pattern into the next one and moves there (for variations), CLEAR empties it. +ADD puts the current pattern at the end of the song chain, <DEL removes the last one; SONG plays the chain (tap a slot to jump to it). Patterns and song are saved on the SD card when you leave the page and when you quit (START > QUIT).
- **RIFF:** writes a random 303-style line inside the octave on screen. It favours the root, adds rests, accents and slides. Tap it again for a new one.
- **While it plays:** the keys transpose the pattern.

## Reverb and delay types (MASTER FX page)

- **REVERB TYPE:** HALL, ROOM, PLATE, SPRING (a dispersive "boing" and the narrow tone of a spring tank), GATED (the 80s snare reverb: a big tail that cuts off) and SWELL (ducked while you play, then blooms up afterwards, like a reversed reverb).
- **DELAY TYPE:** TAPE (ping-pong with wow and saturation), DIGITAL (clean stereo), PING-PONG (clean), DUB (dark, saturated, long), LO-FI (each repeat crushed further) and REVERSE (repeats play backwards).

Both are saved with patches, and factory patches pick types to suit them.

## Wi-Fi MIDI (HARDWARE page: WI-FI MIDI ON)

When it starts, Syrup shows an address such as `http://192.168.1.23:8080`. Your phone, tablet or computer must be on the same Wi-Fi network.

- **Web remote:** open that address in any browser. You get a two-octave multitouch keyboard, an XY pad (WT position and cutoff), resonance and macro sliders, patch back/forward, sequencer PLAY/STOP and ACID.
- **Web MIDI:** the remote page also forwards a USB MIDI keyboard plugged into the computer. Browsers only allow Web MIDI on secure pages. In desktop Chrome, add the Syrup address under `chrome://flags/#unsafely-treat-insecure-origin-as-secure`. iPhone Safari has no Web MIDI.
- **Network MIDI (AppleMIDI / RTP-MIDI):** connect from macOS Audio MIDI Setup (Network), from rtpMIDI on Windows, or from iOS apps that support network MIDI sessions. Use the 2DS's IP address on port 5004.
- **MIDI handled:** notes (and the arpeggiator), pitch bend, program change, mod wheel, start/stop and clock (tempo follows).
- **CC map:** 74 cutoff, 71 resonance, 70 WT position, 1 and 16-19 macros, 102/103 previous/next patch, 104 RIFF line, 120/123 all notes off.
- **MIDI LEARN** (START menu): pick a control on a page with the D-pad, choose MIDI LEARN, turn a knob. Up to 16 bindings, kept on the SD card; a learned CC takes over the fixed map.
- **Aftertouch / MPE:** channel pressure and poly aftertouch drive the PRESSURE mod source. With MPE on (VOICE+FX), notes on channels 2-16 each get their own bend (+-48 semitones), pressure and slide (CC 74, the MPE SLIDE source).
- **LFO1 / LFO2 CC OUT** (LFO+MOD page): send the LFO as that CC (channel 1) back over the Network MIDI session. 0 = off.
- **IR MIDI** reads byte by byte now: notes, running status, clock in the middle of a message.

## Video wavetables (HARDWARE page)

- **CAM WAVE LIVE:** OSC A's wavetable becomes the live camera picture, updated about 8 times a second, with each image row as one frame.
- **CAM WAVE RECORD:** records about 8 seconds of camera, giving 64 moments with one waveform each. It's saved as `VID n.wav` and loaded into OSC A.
- **WT SCAN:** sweeps OSC A's WT position automatically (1X = the whole table in 8 seconds). It works on any table, and plays a recorded video table back.

## IR MIDI adapter (RP2040)

Any RP2040 board (Raspberry Pi Pico, RP2040-Zero and so on) becomes a USB MIDI device called "Syrup IR MIDI". It flashes whatever it receives to the 2DS's infrared port. Plug it into a computer, a DAW, or a phone running a MIDI app; the USB-C boards work directly.

- **Firmware:** the adapter firmware is a separate project, not part of this repo.
- **Wiring the IR LED:** GP15 → 1 kΩ → base of an NPN transistor (2N2222 / 2N3904), with the emitter to GND. Run VBUS (5 V) → 47 Ω → IR LED → collector. Use an 850 nm LED (IrDA wavelength); a 940 nm LED also works at short range.
- **Optional 5-pin DIN MIDI in:** the standard 6N138 opto circuit into GP5.
- **In Syrup:** go to HARDWARE → LINK → IR MIDI IN and point the LED at the IR window on the 2DS's back edge, from a few centimetres to about 20 cm.
- **How it works:** the signal is IrDA SIR at 115200 baud (a 3/16-bit pulse for each 0 bit), the same format the 2DS's IR chip reads.

## New styles and theme

- The WAVES, 3D, BREATH and GRAIN styles have been folded into the rebuilt library above. 3D patches are now SPACE, and the breath patches play from the keys, with breath as an optional extra.
- **IMPULSE theme:** Impulse Tracker look, with black fields, grey panels and tabs, and a blue selection bar.

## Sequencer button, grain oscillator, GRAIN patches, NET > TABLE, mic-tap drums

- **SEQ button** (bottom bar, far right): jumps to the step grid, and tapping it again goes back. The top tab is now called SEQ.
- **GRAINS** (HARDWARE page):
  - **LOOP** plays the grain source by itself; **KEYS** makes it an oscillator, with pitch from the keys and loudness from the voice envelope.
  - **GRAIN SOURCE** is LOOP (your loop recording), TABLE (OSC A's wavetable, where WT POS and its LFOs move through it) or MIC (the last second the mic heard).
  - Grain settings are saved with patches.
- **NET > TABLE** (HARDWARE page): fetches a random web page and turns its raw bytes into a 64-frame wavetable (data-bending).
- **MIC TAP** (DRUMS page):
  - Pick TAP KICK, TAP SNARE or TAP HAT, then tap the 2DS near the mic. Each hit plays the drum and is written into the MINE pattern at the nearest 16th note, as loud as you hit it.
  - The row's current pattern is copied into MINE first, so you can overdub. CLEAR MINE empties it.
  - MINE is saved to the SD card.
- **Key velocity:** press lower on a key to play louder, near the top to play softer.

## Shrub-inspired additions, stage 1

These follow the feature list and guide of Xfer's SHRUB firmware. SHRUB is closed source, so no code or content was used.

- **Scales:** 51, including modes, bebop, diminished, Japanese, Indonesian, Hungarian and raga scales.
- **Sub shapes:** 6 (adds PULSE 25 and ROUNDED). **Noise colours:** 5 (adds BROWN and BLUE).
- **Mod sources:** CHAOS X and CHAOS Y (a Lorenz attractor, with CHAOS RATE on the MOD page), RANDOM (a new value every note) and NOTE (keytrack).
- **DRIVE:** 15 waveshapers on the oscillator cycle: TANH, HARD CLIP, SINE FOLD, RECTIFY, TUBE, DIODE, ASYM, LIN FOLD, ZERO SQUARE, HALF RECT, BITCRUSH, CUBIC, X-SHAPE and OVERDRIVE. They cost no CPU while playing.
- **MOD FX:** adds HYPER (five drifting taps per side) and DIMENSION (crossed short delays for width).
- **ADD EDITOR** (HARDWARE page): draw 24 harmonics on the bottom screen and OSC A plays the result live. WT POS sweeps from the fundamental to the full spectrum. Turning it off saves the table as `ADD n`.
- **RULE > TABLE:** a cellular-automaton (Wolfram rule) wavetable, with one generation per frame.
- **Value pop-ups:** stepping a named value shows it large on screen for a moment.
- **Edit guard:** after you edit a patch, the first press to change patch or style only warns you; the second one goes.

## OUT FILTER (MASTER FX page)

One stereo CPU filter on the summed voices (paraphonic, like SHRUB's), after the per-note DSP filters.

- **Types:** LADDER 24, LADDER 12, ACID 18, DIRTY, CREAMY 24, SVF LP/HP/BP/NOTCH, COMB +/-, PHASER and FLANGER. The ladders and acid have saturated resonance feedback.
- **Controls:** OUT CUTOFF, OUT RES, OUT DRIVE, and OUT ENV2 (the newest note's ENV 2 sweeps it).
- **Mod destination:** OUT CUTOFF can also be modulated from the matrix.
- **Steeper slopes:** stack it on the per-note filter. FILTER 2 → +6DB SLOPE gives 18 dB per note, plus 24 dB from a ladder here.

## PADS (bottom bar, far left)

A touch chord instrument that replaces the keys and XY pad. There is one pad per chord of the key (I to vii, with the chord name shown), following the current scale; with no scale or a scale that doesn't have seven notes, it uses major.

- **Where you touch a pad:** low = triad, middle = 7th, top = 9th, left edge = first inversion, right edge = sus4.
- **STRUM strip:** slide along the bottom strip to strum the last chord across three octaves, autoharp-style. Lower on the strip strums louder.
- **With other features:** works with LATCH, the arpeggiator and every patch. Help is in the START menu.

## MOD page bottom screen: MATRIX and drawn LFOs

- **MATRIX** shows all 8 mod slots as two columns joined by a line, one colour per slot:
  - Tap the left box to pick the source (OFF, LFO 1-4, ENV 2, VELOCITY, NOTE, RANDOM, STICK, XY Y, MACRO 1-4, CHAOS X/Y and every sensor). OFF clears the slot.
  - Tap the right box to pick the destination.
  - Drag along the line to set the amount: centre is 0, right is +100, left is -100. The bar under the line shows it.
  - The white dot rides the source's live value, so you can see what's moving.
  - It's the same 8 slots as the MOD page list on the top screen.
- **DRAW LFO 3 / DRAW LFO 4:** paint a 64-point shape with the stylus, or start from SINE, RAMP, STEPS, RANDOM or FLAT. The vertical line is the playhead.
  - Rate and tempo sync are on the MOD page (LFO3 RATE / SYNC, LFO4 RATE / SYNC).
  - LFO 3 and LFO 4 are mod sources, and their shapes save with patches (format v9).

## MOTION (MOD page, bottom screen, fourth tab)

Four lanes of movement that loop over 1, 2, 4 or 8 bars in time with BPM (tap LOOP to change it).

- **Draw a lane** with the stylus. Drawing switches the lane ON, and it then sets its target as it plays.
- **REC:** arms the lane. Recording starts at the next loop and takes the circle pad's height for one loop, then the lane switches on.
- **Target:** tap the lane name to change it. Targets are A/B WT POS, CUTOFF, RES, OUT CUT, OUT RES, A/B WARP, FX COLOR, DLY MIX, REV MIX, GRAIN SCAN and MACRO 1-4.
- **Saving:** lanes save with patches (format v10).
- **While drawing:** the keys are hidden on the MOD page, so LATCH a chord or play from Wi-Fi/IR MIDI.

## SYRUP LINK: the FM-1 as a Wi-Fi bridge

SYRUP LINK is separate firmware for the M-VAVE FM-1, built on Charles Vestal's fm1-base. It turns the FM-1 into a Wi-Fi MIDI bridge and controller for Syrup.

- **Phone or computer to 2DS:** plug the FM-1 into an iPhone, iPad or computer over USB. Everything Drambo (or any app) sends to "SYRUP LINK" goes over Wi-Fi to Syrup: notes, CCs, clock, start/stop. TRS MIDI in is passed on too.
- **No address to type:** with WI-FI MIDI on, Syrup announces itself on the network (UDP 5009) and listens for plain MIDI on UDP 5008. The FM-1 finds it by itself. EDIT sets an address by hand if needed.
- **The FM-1 plays Syrup:** the keys play notes (OCT-/OCT+ move them), K1-K4 send cutoff, resonance, WT position and macro 1, PRESET steps patches, PLAY starts/stops the sequencer, ARP makes a new acid line, FX is all notes off.
- **Wi-Fi setup on the FM-1:** SEL lists networks, SAVE joins; a new network asks for its password on the PASSWORD page (SELECT picks characters, SAVE adds, FX deletes, PLAY joins).
- **Timing buffer:** every message is stamped on the FM-1 when it arrives, and Syrup plays it at that time plus **NET BUFFER** (HARDWARE page, default 20 ms), from the audio clock rather than the screen loop. The Wi-Fi's wobble becomes one steady delay; if your app can send MIDI early to a port, set it that much early for SYRUP LINK. Raise NET BUFFER if notes still wobble, lower it for less delay.
- **Install:** the LINK firmware is a separate project, not part of this repo.

## Chip quirks and FX on core 2

On the MASTER page (saved with patches):
- **DSP GRIT:** the DSP's interpolation per voice: OFF (smooth), LINEAR (duller), RAW (none: aliased, ringing crunch on high notes).
- **FILTER SCREAM:** the low pass's coefficients written straight into the DSP's biquad, pushed past stable, so it rings by itself and saturates.
- **MIX OVERDRIVE:** the DSP channel volumes pushed past full and clipped at the 16-bit ceiling, then brought back down.
- **FX ON CORE 2:** the master effects chain runs on the second CPU core, one DSP frame (~5 ms) behind. If core 2 ever misses a frame it falls back to core 1 (HARDWARE page shows which).

Elsewhere: **NOISE TYPE CORRUPT** (junk fed to the DSP's ADPCM decoder), **ECHO PITCH** on the DRUMS page (the chip echo replayed faster or slower than it records, so repeats climb or fall), and **CHIP OCTAVE** up to +5 (the squares run past the output rate and alias back as whines).

## New sensor sources

SPIN F/B and SPIN L/R (gyro: how fast you tip or roll it, not where it sits), VOICE BRIGHT (talk into the mic: "ss" is bright, "oo" is dark), CAM MOTION (how much the camera picture changes). They're in the mod matrix.

## Tests

`tests/host` runs Syrup's real code on a normal computer, with the 2DS hardware faked (`fake3ds.h`: the DSP's channels, its fixed-point filter and aux buses, the second sound chip, buttons, touch screen and a simulated clock). GitHub runs it on every push that touches `source/` or `tests/` (workflow "Syrup tests"); it takes under a minute.

- **Every factory patch** plays a held C4: fails if it's silent, clips, carries runaway DC, or is more than 3 dB away from `tests/baseline/levels.txt`.
- **Buzz:** a sine through MASTER FX, with the chain on core 1 and on core 2, must have no 204.5 Hz frame-rate tone; core 2 must not fall back.
- **LINK timing:** jittery FM-1 LINK packets must come out evenly spaced and never due before they arrive; the tick clock must convert exactly.
- **Controls:** the preset browser, saving and naming a patch, and the MATRIX (pick source, pick destination, drag amount).
- Both screens of every page are saved as PNGs, and failing patches as WAVs, attached to the run.

Run it locally with `python3 tests/host/run_tests.py` (needs gcc, libcurl headers, Python; Pillow for PNGs). After an intended sound change, `python3 tests/host/run_tests.py --update` accepts the new levels; commit `tests/baseline/levels.txt` with it.

It can't check what only the hardware does: real chip quirks, CPU load on a 2DS, Wi-Fi, IR, camera and the system keyboard.

## New in the later beta builds

- **DRUMS page:** a KICK / SNARE / HAT step grid where the XY pad was. Tap or drag across steps (the row becomes MINE, starting from the pattern it was playing); tap a row's name for its next pattern; DRUMS ON/OFF and KIT along the top.
- **One kit:** kit patches (MODE 808 KIT .. WAVE KIT) and the drum machine share one kit: change either and the other follows.
- **TRANCE GATE** (VOICE+FX): GATE picks a 16-step pattern (16THS, 8THS, OFFBEAT, TRANCE 1 / 2, HALF, STUTTER), GATE RATE 1/16, 1/8 or 1/32, GATE DEPTH how far it closes. Locks to the sequencer's steps; saved in the patch.
- **PADS > PROGRESSION:** pick a progression (I V vi IV, vi IV I V, I vi IV V, ii V I, I IV V IV, i VI III VII, i iv v i), tap its chords in order, or AUTO plays one a bar with the clock.
- **GRAIN POS** is a matrix destination: an LFO (or anything) sweeps where the grains read. The GRAINSCAN patch (SPACE) shows it off.
- **OUTPUT: MONO** (VOICE+FX) for one speaker or a mono cable.
- **HELP** has four slides; L / R flip them.
- **FILTER SCREAM** stays just inside the filter's stable limit (it could lock up the sound chip), and a watchdog turns it off if the chip ever stops answering.
