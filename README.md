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

With MASTER FX on, every DSP voice goes through the effect bus to a chain that runs in the audio thread: LOFI (bits, rate) -> MOD FX (chorus, flanger, phaser, comb, formant; FX COLOR is a mod destination) -> 3-band EQ -> tape delay (tempo sync, wow/flutter, saturation) -> reverb (shimmer, freeze) -> OTT multiband compression -> sidechain PUMP (locked to the beat / kick) -> limiter. DELAY MIX / REVERB MIX add to the FX ECHO / REVERB amounts, so older patches sound the same. The page shows the chain and its CPU use. The sub, drums and looper (second sound chip) stay dry. Every factory patch is tuned for the chain (tempo-synced tape delays on leads, plucks and arps; chorus and shimmer on pads; OTT on basses; pump on house chords; lo-fi on retro sounds). Showcase patches: OTT GROWL, FLANGE LEAD, SHIMMER HALL, TAPE KEYS, PUMP CHORDS, TALK BOX, COMB DRONE.

## DRUMS (PLAY tab)

A synthesised kit (808, 909, LOFI, CHIP) on three channels of the second sound chip with KICK / SNARE / HAT pattern presets, DRUM LEVEL, BPM and SWING. It follows the sequencer when that's playing and drives the PUMP.

## Macros and mod matrix

Four MACRO knobs (mod sources MACRO 1-4); XY PAD can play MACRO 1+2 or 3+4 instead of WT POS + CUTOFF. Eight mod slots, each with a CURVE (linear, exp, log, S).

## HARDWARE page (HW tab)

- **LOOPER:** REC records 1, 2 or 4 bars of Syrup's output (the DSP's own output tap), then loops it on two channels of the second sound chip while you play on top. LOOP LEVEL sets its volume.
- **LOOP > TABLE / MIC > TABLE / CAM > TABLE:** turn the loop, one second of mic (sing or play into it), or a camera photo (each image row becomes a frame) into a 64-frame wavetable. It's saved as a Serum-style .wav in /3ds/syrup/ and loaded into OSC A. Pitched audio is cut into clean single cycles; anything else becomes even slices.
- **UPDATE SYRUP:** checks the latest build over Wi-Fi and installs it (CIA through the system installer, or replaces /3ds/syrup.3dsx when run from the Homebrew Launcher). Close and reopen Syrup afterwards.
- **GET WAVETABLES:** downloads the Syrup Pack (35 original wavetables in `wavetables/` of this repo) over Wi-Fi into /3ds/syrup/, skipping ones you already have. Any Serum-format .wav you copy there works too.
- **CAMERA:** CAM LIGHT and CAM COLOR mod sources (thumbnail on the page).
- **Motion:** TWIST (gyroscope) and SHAKE mod sources join TILT and BREATH.
- **LINK:** sync tempo and sequencer steps with another console running Syrup, over local wireless (one HOSTs, the other JOINs) or infrared (IR SEND / IR RECEIVE, consoles pointed at each other).
- **BEAT LIGHT:** both screens' backlights flash on each arp / sequencer step.
- OUTPUT AUTO (VOICE+FX) switches to surround when headphones are plugged in; New 3DS / New 2DS run at 804 MHz automatically.
- Visualizer: TRAILS (the GPU keeps and fades the last frame) and PARTICLES (sparks on notes and beats).

## Visualizer page

The last page (VISUALIZER) fills the top screen with GPU visuals while you play on the bottom screen: WAVETABLE (big 3D table), SCOPES, ORBIT (OSC A against OSC B, spirograph trails) and SPECTRUM (harmonic bars), VOXELS (a scrolling 3D block landscape of the notes you play), VOXEL TABLE (the wavetable as blocks, playing frame lit), or AUTO to cycle every 10 s. Arp/sequencer steps pulse the background. D-pad picks VIS MODE / VIS OSC / OVERLAY.

## Graphics chip visuals

On the OSC and sound pages the left panel of the top screen is drawn by the 3DS's GPU (PICA200, via citro2d): a slowly swaying 3D wavetable with the live (warped) cycle glowing and leaving trails, and glowing scopes with phosphor trails on the other pages. It's rendered at 2x and scaled down by the display-transfer hardware for smooth lines; colour follows the filter and envelope. Turn it off with GPU VISUALS on the VOICE+FX page.

## Presets

Page 1 is a preset browser organised by style. Use the D-pad to pick a style and a patch; the patch loads immediately, so you can play keys to audition it.

| Style | Patches |
|---|---|
| BASS | Sub Wobble, Reese, FM Growl, 808 Sub, Acid, Wobble 1/8, Digi Bass, Sine Bass, Pluck Bass, Hoover |
| LEAD | Supersaw, Sync Scream, Vowel Lead, Chip Square, Digi Pipe, Brass Lead, Whistle, Glide Square, Fuzz Lead, Fifths |
| PAD | Syrup Pad, Glass, Choir, Warm Strings, Dark Drone, Vapor Pad, Sweep Pad, Formant Pad, Shimmer Pad, PWM Pad |
| PLUCK | Syrup Pluck, Marimba, Kalimba, Harp, Dub Stab, Bell, Steel Drum, Synth Pluck, Muted, Pizz |
| KEYS | E Piano, Organ, Clav, Acordeon, Vinyl Keys, Dark Rhodes, Wurli, Church Organ, Harpsichord, Lofi Piano |
| ARP | Trance Gate, Berlin Seq, Crystal Arp, Glass Rain, Chip Arp, Synthwave Arp, Dub Echo Arp, Pad Ripple, Bass Octaves, Marimba Run (these set the arpeggiator, rate, octaves and tempo) |
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

## Chip-level extras (no CPU in the audio path)

- **CHIP SQUARE** (SUB+NOISE page): a layer from the second sound chip's own square-wave generators, with CHIP WIDTH (pulse width) and CHIP OCTAVE.
- **CHIP kit** (DRUMS): snare and hat come from the chip's hardware noise generators.
- **CHIP ECHO / ECHO TIME / ECHO REPEATS** (DRUMS): a ping-pong echo made by the chip's two capture units recording its own output. It echoes the sub, drums and chip layer.
- **Looper**: stored as IMA-ADPCM, which the chip decodes itself, so up to 16 bars (about 48 seconds).
- **GRAINS** (HARDWARE): granular playback of the loop. The DSP plays overlapping slices back to back. Set SIZE, SCAN (50% = frozen), SPRAY, PITCH and LEVEL.
- **3D ORBIT / ORBIT RATE** (MASTER FX): with OUTPUT set to SURROUND, the sound circles around you.
- **New visualizer modes**: TUNNEL, KALEIDO, SYNTHWAVE and RIPPLES.

## Step grid and ACID lines (PLAY page)

On the PLAY page the bottom screen's XY pad becomes a 16-step grid. Each of the 8 rows is a note of the current scale; with SCALE off the rows are a major scale.

- **Placing notes:** tap a cell to place a note, and you'll hear it. Tap it again to cycle through accent (dark dot), slide (tail into the next step), accent + slide, then off. Up to 4 notes per step.
- **Buttons:** PLAY/STOP, CLEAR, <LEN / LEN> (pattern length), ROW- / ROW+ (move the grid an octave), and ACID.
- **ACID:** writes a random 303-style bassline. It favours the root, adds octave jumps, rests, accents and slides, and sits in the bass octave. Tap it again for a new line. Try it with the BASS > ACID patch.
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
- **CC map:** 74 cutoff, 71 resonance, 70 WT position, 1 and 16-19 macros, 102/103 previous/next patch, 104 ACID line, 120/123 all notes off.

## Video wavetables (HARDWARE page)

- **CAM WAVE LIVE:** OSC A's wavetable becomes the live camera picture, updated about 8 times a second, with each image row as one frame.
- **CAM WAVE RECORD:** records about 8 seconds of camera, giving 64 moments with one waveform each. It's saved as `VID n.wav` and loaded into OSC A.
- **WT SCAN:** sweeps OSC A's WT position automatically (1X = the whole table in 8 seconds). It works on any table, and plays a recorded video table back.

## IR MIDI adapter (RP2040)

Any RP2040 board (Raspberry Pi Pico, RP2040-Zero and so on) becomes a USB MIDI device called "Syrup IR MIDI". It flashes whatever it receives to the 2DS's infrared port. Plug it into a computer, a DAW, or a phone running a MIDI app; the USB-C boards work directly.

- **Firmware:** `syrup_irmidi.uf2` from the latest release. Hold BOOTSEL while plugging the board in, then copy the file onto the RPI-RP2 drive.
- **Wiring the IR LED:** GP15 → 1 kΩ → base of an NPN transistor (2N2222 / 2N3904), with the emitter to GND. Run VBUS (5 V) → 47 Ω → IR LED → collector. Use an 850 nm LED (IrDA wavelength); a 940 nm LED also works at short range.
- **Optional 5-pin DIN MIDI in:** the standard 6N138 opto circuit into GP5.
- **In Syrup:** go to HARDWARE → LINK → IR MIDI IN and point the LED at the IR window on the 2DS's back edge, from a few centimetres to about 20 cm.
- **How it works:** the signal is IrDA SIR at 115200 baud (a 3/16-bit pulse for each 0 bit), the same format the 2DS's IR chip reads.

## New styles and theme

- **WAVES** (14 patches): built on the Syrup wavetable pack. Get the pack from HARDWARE → GET WAVETABLES first; until then a built-in table stands in, and Syrup tells you so.
- **3D** (10 patches): sounds that orbit around you. Loading one sets OUTPUT to SURROUND; headphones work best.
- **BREATH** (9 patches): blow into the mic while holding a key, or LATCH a note and play with breath alone. Breath sets the volume and opens the filter. The new mod destination VOLUME makes any source control loudness.
- **IMPULSE theme:** Impulse Tracker look, with black fields, grey panels and tabs, and a blue selection bar.

## Sequencer button, grain oscillator, GRAIN patches, NET > TABLE, mic-tap drums

- **SEQ button** (bottom bar, far right): jumps to the step grid, and tapping it again goes back. The top tab is now called SEQ.
- **GRAINS** (HARDWARE page):
  - **LOOP** plays the grain source by itself; **KEYS** makes it an oscillator, with pitch from the keys and loudness from the voice envelope.
  - **GRAIN SOURCE** is LOOP (your loop recording), TABLE (OSC A's wavetable, where WT POS and its LFOs move through it) or MIC (the last second the mic heard).
  - Grain settings are saved with patches.
- **GRAIN style** (12 patches): grain oscillator layered with OSC B, sub, noise and CHIP SQUARE.
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
- **With other features:** works with LATCH, the arpeggiator and every patch. Help moved to SELECT only.
