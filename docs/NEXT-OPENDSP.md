# Next version: OpenDSP

Parked for the release after v1.0. Nothing here is in v1.0.

[OpenDSP](https://github.com/Cellenseres/OpenDSP) is a from-scratch, BSD-2 replacement for
the DSP firmware that ndsp needs. Two reasons to use it:

1. Players no longer need to dump `dspfirm.cdc`; Syrup can ship its own firmware.
2. Reverb and delay can run on the DSP itself, freeing ARM11 time on the Old 2DS.

## Where Syrup is today

- `ndspInit()` at startup (`source/main.c`, audio init), CSND for sub + effect taps.
- Aux bus 0 = reverb, bus 1 = ping-pong delay, but the effects run **on the ARM**:
  `busInit()` installs `ndspAuxSetCallback(0, aux0)` / `(1, auxDelay)`, and the comb/allpass
  work runs on the audio thread or the core-2 worker.
- Six reverb types: HALL, ROOM, PLATE, SPRING, GATED, SWELL.

## Phase 1: swap the init, keep everything working

```c
#include <opendsp.h>
#include <opendsp/effects.h>

static bool gOpenDSP; static u32 gDspCaps;

gOpenDSP = R_SUCCEEDED(opendspInit());
if (gOpenDSP) opendspGetCapabilities(&gDspCaps);
gAudioOK = gOpenDSP || R_SUCCEEDED(ndspInit());   // ndsp needs the user's dspfirm.cdc dump
// exit: gOpenDSP ? opendspExit() : ndspExit();
```

Build: `make -C library install` in the OpenDSP checkout, link `-lopendsp`. The library
embeds a prebuilt firmware, so Phase 1 needs no DSP toolchain.

Test first: install OpenDSP's Sound Lab demo on the Old 2DS. OpenDSP has only been tested
on a New 3DS.

## Phase 2: move effects onto the DSP

API: `opendspReverbSet(bus, &cfg)` and `opendspDelaySet(bus, &cfg)`; NULL turns one off.
Sends stay the same (`ndspChnSetMix` gains 4-5 to bus 0, 8-9 to bus 1). Effect output is
fully wet; `ndspAuxSetVolume()` sets the return.

- Delay: 5-975 ms (4.9 ms steps), feedback 0-0.95, damping, level. Syrup's `DLMAX` is ~0.8 s,
  so the range fits. Check that the ping-pong feel survives; OpenDSP's delay may not be ping-pong.
- Reverb: room size, decay 0.1-10 s, damping, diffusion, width, level. About 13% DSP time
  per bus while a tail plays.
- When using DSP effects, do **not** also install the ARM aux callbacks on that bus.

Tradeoff: OpenDSP has one reverb algorithm. HALL / ROOM / PLATE map onto its parameters;
SPRING, GATED and SWELL are Syrup's own and do not. Options:
- keep those three on the ARM path (current code), DSP for the rest, or
- port them in Phase 3.

Fallback: if `opendspInit()` failed or the caps lack `OPENDSP_CAP_REVERB` /
`OPENDSP_CAP_DELAY`, use the current ARM bus effects unchanged.

## Phase 3: Syrup firmware fork (later)

- Fork OpenDSP's `firmware/`, build with its pinned teak-llvm (`toolchain/build_teak_llvm.sh`,
  Linux/CI, not the Mac).
- New commands go through the existing extension mailbox (`docs/EXTENSIONS.md`, DSP data
  `0x0D00`). Commands 0-3 and 0x20-0x23 are taken; pick a new range, keep protocol version
  compatibility.
- Candidates: SPRING / GATED / SWELL reverbs, wavetable voices on the DSP (send note and
  knob values, not audio; 160-sample frames at 32728 Hz).
- DSP code is fixed-point (16-bit data, wide accumulators); hot loops in assembly if
  teak-llvm's output is too slow.
- Test in Azahar with **LLE** audio (HLE never runs the firmware), and with teakra in CI.
- Ship the built firmware with `opendspInitFromMemory()`; keep it free of any console firmware code.

## Order

1. Sound Lab on the Old 2DS.
2. Phase 1 behind the fallback.
3. Phase 2 for HALL / ROOM / PLATE + delay; measure ARM time saved.
4. Phase 3 only if Phase 2 pays off.
