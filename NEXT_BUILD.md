# Next build (from Matt, after the video)
**TOP: SCREAM filter hard-freezes the sound engine — panic and closing the app don't help, only power off**
- [done] Piano roll should play notes on the keys
- [done] Two R presses clear the sequencer
- [done] Rearrange menu items by need (e.g. BPM at the top of the sequencer menu)
- [done] Drag notes in the sequencer
- [done] Turning drums on / changing drum kits makes the audio skip
- [done] XY + circle pad + drums all at once slows things down
- [done] SELECT stops the sequence but not the drums
- [done] Mono instrument in CHORD mode plays the wrong note and doesn't light the key played
- [done] CHIP DRUMMER is too loud
- [done] DRUMS page: a proper drum step sequencer on the bottom screen instead of the XY pad
- [done] Hold SELECT = panic
- [done] XY menu: every page one tap away
- [done] Idea: an LFO on the grain position (graintable LFO)
- [done] Question: should master FX MODE reach the drum channels
- [done] Mono output option
- [done] GRAINSCAN oscillator: an osc that plays grains scanning through the wavetable/sample, pitched to the key
- [done] MIDI: CC map (learn / more params) and a full MIDI bug test in the simulator
- [done] OCT-/OCT+ buttons should flash/show they've been hit
- LFOs sending MIDI CC out (next)
- [done] G-FUNK SINE and EP SATURDAY click when changing notes
- [done] Browsing presets: the interface hiccups and freezes
- [done] Changing wavetable while the sequencer plays causes pauses
- [done] Map L and R back to OCT-/OCT+
- [done] On the SEQ tab you can't open the PADS tab
- [done] SELECT+L = undo, SELECT+R = redo
- [done] Piano roll: shorten to one octave
- [done] ACID line generator: rename it; generate only within the current octave
- [done] Hold SELECT + circle pad = quick-select menu: each direction opens a section (8 directions: PATCH OSC SHAPE FX SEQ VIS HW DRUMS)
- [done] SELECT + D-pad Down = panic (instead of hold)
- [done] Quick-select: bottom direction of the circle pad = transport controls (play/stop, rec, tape)
- [done] Rename ACID to RIFF
- [done] Check drums recording to tape (works via CHIP > FX; make it obvious / automatic when taping)
- [done] Tape REC switches CHIP > FX on by itself (so drums record)
- [done] Chord progression pads
- [done] TRANCE GATE: tempo-synced rhythmic chopping with a step pattern
- [done] Still too easy to drop the filter (XY) when reaching for SCALE / LATCH etc.
- [done] Quick menu: patch changing in it
- [done] In HELP, L/R flip between help slides
- [done] Layout: CHORD, LATCH and OCT-/OCT+ go to the left of the keyboard
- [done] CHECK FIRST: SUB osc may not follow the amp envelope (hung note); Matt thinks it may have been the wavetable he was using
- [done] Drums can't keep time on the MOTION HAZE patch (CPU load; check with the XY+stick+drums slowdown)

- Replace the voxel visuals with Scream Tracker style visuals (noted 2026-11-22, not started)
- Web version: an animated ANSI-art finger pointing at SOUND ON until it is tapped (noted 2026-11-22, not started)
- Web version: the top screen preset browser takes touch too (tap a style / a patch in its lists) (noted 2026-11-22, not started)
- Web version: a BOTTOM SCREEN mode connected to a real 3DS over Wi-Fi, for the fiddly bits (edit on the phone, the 3DS plays) (noted 2026-11-22, not started)
- A held / latched chord keeps sounding while you change patches (it is cut now) (noted 2026-11-22, not started)
- Web version: SELECT and START move up between L and R on the on-screen controls (noted 2026-11-22, not started)
- Web version: the D-pad and the circle pad on screen at the same time (now it is one or the other) - work out a layout that fits both (noted 2026-11-22, not started)
- Web version: proper wide-screen layout (phone sideways, tablets, computers): screens and controls side by side, not one tall column (noted 2026-11-22, not started)
- PADS chord progressions: a bass note under each chord, and more progressions to choose from (noted 2026-11-22, not started)
- Circle pad drives the menus (START menu, theme picker, patch browser lists): up / down to move, A to pick (noted 2026-11-22, not started; check with Matt which menu he meant)
- Record the chord progression pads into the sequencer (live REC takes the pad chords as steps) (noted 2026-11-22, not started)
- NEW 3DS control mode (the point: a SECOND THUMB STICK): use the C-stick and ZL / ZR (New 3DS / New 2DS XL), and the same in the browser (gamepad right stick = C-stick, triggers = ZL / ZR, plus on-screen versions) - decide what they do (noted 2026-11-22, not started)
  - second stick: its own X / Y as mod sources (STICK 2 X / Y in the matrix), and by default a live XY on the page you are on; gamepad right stick and an on-screen second stick in the browser
  - it is an OPTION (HARDWARE page: NEW 3DS CONTROLS on / off); on by itself when Syrup finds a New 3DS / New 2DS XL, off on older models; the browser offers it as well

## From Matt, Nov 22 evening
- Chords still can't be recorded into the sequencer (PADS/chord mode: recording should write the whole chord to the step, all notes).
- Chords still can't be recorded to TAPE (tape should capture chord playing like anything else coming out of the synth).
- "The gold visualizer stops nearly everything." Most likely PLASMA (its colours come from the theme, which comes out gold on BBS). It draws with raw ProcTex + texenv after C2D_Flush, at VIS_SS supersampling. Check: frame time, whether the citro2d state is restored afterwards (other drawing dying), and audio dropouts. Fix or cut it; Matt confirmed: sound AND controls freeze, everything locks up. Likely a GPU stall/hang (ProcTex path) blocking the main loop; audio dies with it. Top priority.
- More chord types (e.g. sus2/sus4, add9, 6, 9, 11, 13, dim7, m7b5, aug, 7#9, quartal) and more progressions in PADS (ties in with the bass-notes/progressions item above).
- Custom FM-1 firmware made for Syrup (based on fm1-ctl / CTL-1): knobs and screen labelled with Syrup's params (cutoff, WT POS, etc.), pads for patches/transport, sends the CCs Syrup maps by default. Works with the web version over USB MIDI (Chrome); for the 3DS it needs the Wi-Fi MIDI bridge (no USB host on 3DS).

## From Matt, Nov 22 night (after "Ok build")
- Themes: more readable; each theme forced to its own palette (no red in GB). Bug: IMPULSE theme START menu shows only THEME (other rows invisible).
- Conditionals "make the device loop" (asked Matt: freeze vs pattern stuck)
- L and R should change the bottom screen (tabs)
- CHIP arp presets
- Web: side-by-side screens in landscape when a controller is plugged in
- XY follow mode (asked Matt what he means)
- Bigger buttons on the effect type
- Longer sequences (more than 16 steps): needs a new pattern format (32/64 steps) — not started
- Accent system in drum sequencer — DONE (ACC button)
- Joystick too sensitive in roll/sequencer — DONE (calmer)
- A / B edit notes in sequencers — DONE (B deletes, hold B = sound)
- 8BitDo X/Y reversed — web ABXY: POS / LETTERS toggle (check with Matt)

## After the open beta (from Matt, Nov 25)
- Monochrome color schemes (one-colour themes)
- Demo example session (a ready-made song to open and pull apart)
- Deferred from the v2 spec: tracker-style VU mixer view, performance mode with punch-ins, project stems
