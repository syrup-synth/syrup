# How Syrup works

Syrup is one C program (`source/main.c`, on libctru, citro2d and citro3d) built around fixed hardware pools, separate threads for separate jobs, and one shared clock. The same file also builds for the host tests and for the web (Emscripten).

## 1. Engine and hardware pools

- **Fixed allocations.** The engine runs on static pools, with no memory allocation in the audio path: 24 DSP channels (`NCH`), 32 CSND channels (`NCS`) and 48 voice structures (`NVS`).
- **Wavetable pipeline.** A single-cycle waveform (`CYC` = 2048 samples) is morphed between frames, warped, driven, then FFT'd into 7 band-limited copies (`NLEV`, 2048 down to 32 samples) so high notes don't alias. 12 banks (`MBANK`) let a new note get fresh copies (`mipsFork`) while older notes keep ringing on theirs, so the waveform never jumps or clicks under them.
- **Threads and cores.**
  - Core 0, main loop: input, drawing, parameter updates, SD file queue, `voicesUpdate()`.
  - Core 1, `workerMain`: wavetable rebuilds (`doRebuild`), and on older consoles the master FX frames (`fxService`).
  - Core 2 on New 3DS / New 2DS, `fxMain`: the master FX chain, one 160-sample DSP frame (about 4.9 ms) behind.
- **Audio tick** (`audioTick`): every 160 samples at 32728 Hz (`DSPRATE`). It renders the software layers (`softRender`), advances the one shared clock (sequencer, arp, chords, bass, drums, gate, MIDI clock out), runs the volume ramps (`mixRampTick`, `csRampTick`) and drains incoming MIDI (`tqDrain`, `mqDrain`) so it lands on time. After a stall the clock catches up silently instead of bursting out the missed notes.

## 2. Synthesis, modulation and effects

- **Oscillators.** Two wavetable oscillators with 20 warp modes (including FM / AM / RM from the other oscillator) and up to 3-voice unison, a sub with 6 shapes, 6 noise types (white, pink, crackle, brown, blue, corrupt), and a square layer from the second sound chip. SOFT LAYERS mixes unison voices in software onto one stereo DSP channel to save hardware channels.
- **Filters.** A per-voice DSP biquad (with FILTER SCREAM's self-oscillation kept inside the unit circle so it can't lock the chip up), plus a CPU OUT FILTER on the mix with 13 types: ladder 24 / 12, acid 18, dirty, creamy 24, SVF low / high / band / notch, comb + / -, phaser and flanger.
- **Modulation.** 4 envelopes (AHDSR), 2 LFOs with classic shapes, 2 drawn 64-step LFOs (LFO 3 / 4), 4 bar-synced motion lanes, a Lorenz chaos source, 4 macros, and an 8-slot mod matrix with 33 sources: tilt, gyro spin and twist, shake, mic breath, voice brightness, camera light / colour / motion, pressure, MPE slide, the circle pads and more. Synced LFOs take their phase from the transport.
- **Master chain** (`auxMaster`): a reorderable chain of 5 blocks (lo-fi, mod FX, EQ, SPACE delay / reverb with shimmer and freeze, 3-band OTT), then speaker EQ, sidechain pump, soft limiter and the surround orbit.

## 3. Sequencing, drums, tape and connectivity

- **One transport, two sequencers.** The piano roll (64-step patterns with per-step locks, ratchets and trig conditions, 16 patterns, song mode) and the drum sequencer (10 rows, each with its own length, rate and Euclid fill, per-drum sound shaping, WAV kits from `/3ds/syrup/kits`). The chord pads (strum strip, morphing, FLAVOR moves (hold a pad, the circle pad adds a 7th, 9th, sus4, inversion or flips it minor), progressions, their own arp) and bass lines (11 patterns plus HOLD and a custom line) play on the same clock. Swing follows the clock's own step count, so odd-length patterns stay together.
- **Tape and grains.** Four tracks recorded from the output as IMA-ADPCM (`TAPE_MAX`, 2 minutes a track), which the sound chip decodes itself, with speeds from 1/4 to X4 and WAV export. GRAINS plays the tape, OSC A's table or the mic as grains.
- **Safety.** A crash guard (`running.txt` turns on safe mode at the next start), background SD saving (`saverMain`), and the DSP firmware copied automatically from the console's own sound app (`dspAutoDump`).
- **Connectivity.**
  - Local wireless and IR link between consoles.
  - A Wi-Fi server on port 8080: the phone remote, file upload, screen mirror / stream, and `/unit` (a full second Syrup in a browser on the 3DS's clock).
  - RTP-MIDI on 5004 / 5005 (MIDI in and out, with clock out stamped to the audio tick).
  - Plain MIDI over UDP on 5008.
  - MIDI clock in phase-locks the transport.
