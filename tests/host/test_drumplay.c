// ONE TRANSPORT on the drum pages: the drum grid's PLAY plays the drums (DRUMS page, and SEQ > DRUM); a drum's name
// tapped while recording records that drum
#include "syrup_host.c"
#include "harness.h"
static int h0, done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; CX = 0; CY = 0;
	if (fr >= 10 && fr < 18) { HD = KEY_SELECT; CX = -150; }   // the quick ring: DRUMS
	if (fr == 24) { if (gCurPage != 11) fail("quick ring should reach DRUMS (%d)", gCurPage); gDrums = 0; drumsChanged(); myDrumLoad(); memset(gMyDrum, 0, sizeof(gMyDrum)); memset(gDrumX, 0, sizeof(gDrumX));
		gKickPat = MINE_IDX[0]; gSnarePat = MINE_IDX[1]; gHatPat = MINE_IDX[2]; for (int i = 0; i < 16; i += 4) gMyDrum[0][i] = 230; gSeqMode = 0; seqStop(); }
	if (fr == 30 || fr == 31) { TOUCH = 1; TX = 20; TY = DG_Y + 5; }   // the grid's PLAY
	if (fr == 34) { if (gSeqMode != 1) fail("drum grid PLAY should start the transport"); h0 = gDrumHitN[DG_DRUM[0]]; }
	if (fr == 154) { int n = gDrumHitN[DG_DRUM[0]] - h0; if (n < 3) fail("PLAY on the DRUMS page should play the drums (%d kicks in 2 s)", n); }
	// a drum's name while recording (MIC TAP armed): records that drum
	if (fr == 160) { gTapRow = 0; memset(gMyDrum[1], 0, DSTEP); }
	if (fr == 170 || fr == 171) { TOUCH = 1; TX = 10; TY = DG_TOP + 1 * DG_RH + 4; }   // SNARE's name
	if (fr == 176) { int n = 0; for (int i = 0; i < DSTEP; i++) n += gMyDrum[1][i] != 0; if (n != 1) fail("a drum's name tapped while recording should record one hit (%d)", n); gTapRow = -1; }
	// SEQ's DRUM button: the DRUMS page (its settings on top)
	if (fr == 180 || fr == 181) { TOUCH = 1; TX = 5 * 320 / 8 + 20; TY = 8; }   // SEQ chip
	if (fr == 186 || fr == 187) { TOUCH = 1; TX = 4 * 320 / GRID_BTNS + 20; TY = XY_Y + 5; }   // DRUM
	if (fr == 192) { if (gCurPage != 11) fail("SEQ > DRUM should bring up the DRUMS page (%d)", gCurPage); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 196; app_main(); if (!done) fail("didn't run"); printf("drumplay: the drum grid's PLAY, names as pads while recording\n"); return gFails ? 1 : 0; }
