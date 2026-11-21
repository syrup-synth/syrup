**Syrup 1.0 beta**: a wavetable synth for 2DS / 3DS.

This is a beta: it's feature-complete, but please report anything that breaks (open an issue, and say which build you're on, shown on the splash screen and the HARDWARE page).

**Install:** FBI > Remote Install > Scan QR Code with `install-qr.png`, or install `syrup.cia` with FBI, or copy `syrup.3dsx` to `/3ds/`. Sound needs a one-time setup: press L + Down + Select (Rosalina menu) > Miscellaneous options > Dump DSP firmware, then reopen Syrup.

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
