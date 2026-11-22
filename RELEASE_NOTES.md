**Syrup 1.0 beta**: a wavetable synth for Nintendo 2DS / 3DS.

This is a beta: it's feature-complete, but please report anything that breaks (open an issue, and say which build you're on, shown on the splash screen and the HARDWARE page).

**Install:** FBI > Remote Install > Scan QR Code with `install-qr.png`, or install `syrup.cia` with FBI, or copy `syrup.3dsx` to `/3ds/`. Sound needs a one-time setup: press L + Down + Select (Rosalina menu) > Miscellaneous options > Dump DSP firmware, then reopen Syrup.

### New since the first beta build
- **QUICK SELECT:** hold SELECT and push the circle pad. UP: every page. LEFT / RIGHT: the page's two main lists (style + patch, table, filter type, kit ...). DOWN: REC / PLAY / TAPE REC. Lean along a bar, let go of SELECT.
- **SELECT:** tap = play / stop (sequencer and drums); SELECT+L / SELECT+R = undo / redo (12 steps: notes, RIFF, CLEAR, knob edits); SELECT+DOWN = panic. L / R change the octave.
- **Layout:** OCT+ / OCT- / CHORD / LATCH left of the keys; SCALE / PLAY / PANIC above them, with a gap so reaching for them doesn't drag the XY pad.
- **Piano roll:** one octave with taller rows, the side keys light as notes play, drag notes to move them, a velocity lane underneath. ACID is now RIFF and stays in your octave. CLEAR takes two presses.
- **DRUMS page:** a KICK / SNARE / HAT step grid where the XY pad was.
- **TRANCE GATE:** chops the sound to the beat (VOICE+FX: GATE, GATE RATE, GATE DEPTH).
- **PADS:** a PROGRESSION row (I V vi IV and more): tap the chords in order, or AUTO plays one a bar.
- **Grains:** GRAIN POS in the mod matrix (an LFO sweeps the read point); new GRAINSCAN patch.
- **MIDI:** MIDI LEARN in the START menu; IR MIDI notes work (they were dropped); OUTPUT MONO.
- **HELP:** four slides, L / R flip them.
- **Fixes:** FILTER SCREAM could lock up the sound chip until a power-off; drums skipped when turned on or when the kit changed; XY / circle pad / MOTION HAZE / wavetable changes held up the drums and sequencer; preset browsing hiccups; clicks on note changes (G-FUNK SINE, EP SATURDAY); mono patches in CHORD mode played the wrong note; CHIP KIT too loud; the PADS tab didn't open from the piano roll; the value pop-up named the previous table; tape REC now records the drums.

### New in this beta
- **TAPE:** bounce the sequencer to four tracks (overdub, mute, level, erase), then export WAVs to `sdmc:/3ds/syrup/tape/`.
- **Recording:** preroll and count-in click; live REC from the keys or MIDI, quantized to 16ths or 8ths (or nudged); TAP REC records knocks on the table through the mic.
- **ARP STRUM:** the CHORD arp rolls its notes like a strummed guitar (down, up, down+up, random). New patches: STRUM 8THS, HARP ROLL, RAKE 16THS.
- **Drums:** live kick / snare / hat tuning, per-row chance, mute, a punchier kick, and WAVE KIT (drums built from your wavetable).
- **Grain editor:** with GRAINS on, the HARDWARE page shows the source waveform. Drag to place the grain and set its size.
- **Sequencer:** piano roll with note lengths, ratchets, step locks, copy / paste, reverse / rotate / humanize, and a 64-slot song mode.
- **Controls:** SELECT plays / stops; START opens a quick menu (save, browser, panic, help, theme, quit); PANIC now stops everything, including the loop and tape.
- **Safer saves:** a power cut mid-save can't corrupt your patches or sequences.
- **Fixes:** loading some patches (KITCHEN SINK, chip echo patches) could kill a recorded loop; the circle pad could change looper settings; filter scream at high cutoff and resonance; patches no longer change your tempo.
