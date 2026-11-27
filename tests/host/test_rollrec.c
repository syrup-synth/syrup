// REC in the piano roll: the roll's own keys (the strip down its left side) write notes while REC is on, like the
// keyboard; with REC off they only play.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static int count(void) { int n = 0; for (int i = 0; i < NSTEP; i++) n += gSeq[i].n; return n; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) { seqClear(); gSeqLen = 16; gGridOn = 1; gRollChord = 0; gVoiceMode = 0; int y = ROLL_TOP + 3 * ROLL_RH + 4; gridTouch(5, y, 1, 0); gridTouch(5, y, 0, 1);
		if (count()) fail("REC off: the roll's keys shouldn't write"); gLiveRec = 1; transportToggle(); }
	if (fr == 6) { int y = ROLL_TOP + 3 * ROLL_RH + 4; gridTouch(5, y, 1, 0); }
	if (fr == 12) { int y = ROLL_TOP + 3 * ROLL_RH + 4; gridTouch(5, y, 0, 1); }
	if (fr == 14) { int n = count(); printf("rollrec: %d note(s) written\n", n); if (n < 1) fail("REC: the roll's keys should write a note"); transportToggle(); gLiveRec = 0; done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 16; app_main(); if (!done) fail("didn't run"); printf("rollrec: the roll's keys record\n"); return gFails ? 1 : 0; }
