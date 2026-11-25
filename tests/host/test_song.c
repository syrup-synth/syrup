// SONG LOOP off (the default): the song plays through once, then the transport stops and rewinds; on: it wraps.
// PREROLL ON NOTE: live REC waits, stopped, for the first note - which starts the transport and is recorded
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 5) { gBPM = 240; patStore(); gSeqLen = 4; for (int p = 0; p < 2; p++) gPat[p].len = 4; gSong[0] = 0; gSong[1] = 1; gSongLen = 2; gSongOn = 1; gSongLoop = 0; transportToggle(); }
	// 2 patterns x 4 steps at 240 = 0.5 s
	if (fr == 60) { if (gSeqMode == 1) fail("SONG LOOP OFF: the transport should stop at the end (%d)", gSongPos); if (gSongPos != 0) fail("SONG: rewound to the start (%d)", gSongPos); gSongLoop = 1; transportToggle(); }
	if (fr == 130) { if (gSeqMode != 1) fail("SONG LOOP ON: it should keep going"); transportToggle(); gSongOn = 0; gSeqLen = 16; }
	if (fr == 140) { gPreroll = 3; memset(gSeq, 0, sizeof(gSeq)); liveRecToggle(); }
	if (fr == 150) { if (gSeqMode == 1 || gPreFor != 3) fail("ON NOTE: should wait, stopped (%d %d)", gSeqMode, gPreFor); int n = 64; recOnNote(); liveDown(&n, 1, 0.8f); }
	if (fr == 156) { if (gSeqMode != 1 || !gLiveRec) fail("ON NOTE: the first note should start the take"); if (!stepHasNote(0, 64 - gSeqTr) && !stepHasNote(1, 64 - gSeqTr)) fail("ON NOTE: the first note should land on step 1"); liveRecStop(); transportToggle(); gPreroll = 1; done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 160; app_main(); if (!done) fail("didn't run"); printf("song: LOOP off stops + rewinds, ON NOTE recording\n"); return gFails ? 1 : 0; }
