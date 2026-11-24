// PIANO ROLL MOVE: dragging a note past another note leaves that note where it was
#include "syrup_host.c"
#include "harness.h"
#define RX(st) (rollX(st) + 8)
#define RY(nt) (rollY(nt) + 5)
static int done = 0;
static void tap(int fr, int at, int x, int y) { if (fr == at) { TOUCH = 1; TX = x; TY = y; } }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	tap(fr, 5, 5 * 320 / 8 + 20, 8);   // SEQ chip
	if (fr == 9) { gScale = 0; gRollScale = 0; gGridOct = 0; gRollLow = 46; gRollChord = 0; memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; stepSetNote(1, 48, 1); stepSetLen(1, 48, 3); stepSetNote(5, 48, 1); }   // a 3-step note, and a note further along its row
	if (fr >= 12 && fr < 30) { TOUCH = 1; TY = RY(48); TX = RX(1) + (fr - 12) * (RX(8) - RX(1)) / 17; }   // drag the long note by its head along the row, over the other one, to step 9
	if (fr == 34) {
		int n = 0; for (int i = 0; i < NSTEP; i++) if (stepHasNote(i, 48)) n++;
		if (!stepHasNote(5, 48)) fail("ROLL MOVE: the note passed over should still be there");
		if (!stepHasNote(8, 48) || stepHasNote(1, 48) || n != 2) fail("ROLL MOVE: the moved note should start on step 9 (%d notes, 9:%d 2:%d)", n, stepHasNote(8, 48), stepHasNote(1, 48));
	}
	// ROLL CHORD + a drag from an empty cell: the whole chord, as long as the drag
	if (fr == 36) { memset(gSeq, 0, sizeof(gSeq)); gRollChord = 2; }   // MIN7? whatever chord 2 is: 3+ notes
	if (fr >= 38 && fr < 48) { TOUCH = 1; TY = RY(48); TX = RX(2) + (fr - 38) * (RX(5) - RX(2)) / 9; }
	if (fr == 52) {
		int n = gSeq[2].n; if (n < 3) fail("ROLL CHORD drag: the chord should be written (%d notes)", n);
		for (int k = 0; k < n; k++) if (noteLen(&gSeq[2], k) != 4) fail("ROLL CHORD drag: every chord note should be 4 steps (%d: %d)", gSeq[2].note[k], noteLen(&gSeq[2], k));
	}
	// the KEYS: held, the ROLL CHORD sounds until let go
	if (fr >= 56 && fr < 80) { TOUCH = 1; TX = 4; TY = RY(48); }
	if (fr == 75) { int n = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) n++; if (n < 3) fail("ROLL KEYS: a held key should hold its chord (%d voices after 0.3 s)", n); }
	if (fr == 86) { int n = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) n++; if (n) fail("ROLL KEYS: letting go should let the chord go (%d)", n); gRollChord = 0; done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 90; app_main(); if (!done) fail("didn't run"); printf("rollmove: a moved note leaves the ones it passes\n"); return gFails ? 1 : 0; }
