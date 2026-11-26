// MUTATE: MUT on the drum grid changes the beat a little each tap (slides, drop-outs, ratchets, velocities, new hits
// next to old ones); the kick's downbeats stay; empty rows stay empty; SELECT + L takes it back; the button is drawn.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static u8 snapK[DSTEP], snapS[DSTEP], snapH[DSTEP];
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		gDrums = 1; drumsChanged();
		for (int r = 0; r < DG_ROWS; r++) drumRowClear(r);
		for (int i = 0; i < 16; i++) { drumCellSet(0, i, (i & 3) == 0); drumCellSet(1, i, i == 4 || i == 12); drumCellSet(2, i, (i & 1) == 0); }
		memcpy(snapK, gMyDrum[0], DSTEP); memcpy(snapS, gMyDrum[1], DSTEP); memcpy(snapH, gMyDrum[2], DSTEP);
		srand(7); gDrMutAmt = 0.6f; int total = 0;
		for (int k = 0; k < 4; k++) total += drumMutate(gDrMutAmt);
		printf("mutate: %d changes over 4 taps\n", total);
		if (total < 4) fail("MUTATE should change the beat (%d)", total);
		for (int i = 0; i < 16; i += 4) if (!gMyDrum[0][i]) fail("MUTATE: the kick's downbeat %d should stay", i);
		for (int r = 3; r < DG_ROWS; r++) for (int i = 0; i < 16; i++) if (drumCellOn(r, i)) fail("MUTATE: empty row %d should stay empty", r);
		int same = !memcmp(snapS, gMyDrum[1], DSTEP) && !memcmp(snapH, gMyDrum[2], DSTEP); if (same) fail("MUTATE: snare / hats unchanged");
		int sh = 0; for (int i = 0; i < 16; i++) sh += gMyDrum[1][i] != 0; if (!sh) fail("MUTATE: a row shouldn't lose its last hit");
		for (int k = 0; k < 4; k++) histStep(1);
		if (memcmp(snapK, gMyDrum[0], DSTEP) || memcmp(snapS, gMyDrum[1], DSTEP) || memcmp(snapH, gMyDrum[2], DSTEP)) fail("MUTATE: undo should bring the beat back");
		gDrumOnB = 1; gPageWant = 11;
	}
	if (fr == 6) { int before = 0; for (int i = 0; i < 16; i++) before += gMyDrum[2][i]; drumGridTouch(258, DG_Y + 5, 1); int after = 0; for (int i = 0; i < 16; i++) after += gMyDrum[2][i]; (void)before; (void)after; if (gMutFlash <= 0) fail("MUT: the button should be at x 242-275"); }
	if (fr == 8) { shotBot("shots/drum_mutate.ppm"); done = 1; }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 10; app_main(); if (!done) fail("didn't run"); printf("mutate: drum MUTATE\n"); return gFails ? 1 : 0; }
