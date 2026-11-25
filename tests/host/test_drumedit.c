// DRUM EDIT: the EDIT button opens the editors with one tap (a step: VEL / TUNE / RATCHET / IF / NUDGE as rows of bars;
// a name: its SOUND, with SOLO). A step's TUNE plays it lower; NUDGE moves it; SOLO leaves only that row
#include "syrup_host.c"
#include "harness.h"
static int done = 0, h0[10];
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 5) gPageWant = 11;
	if (fr == 10) { myDrumLoad(); memset(gMyDrum, 0, sizeof(gMyDrum)); memset(gDrumX, 0, sizeof(gDrumX)); gKickPat = MINE_IDX[0]; gSnarePat = MINE_IDX[1]; gHatPat = MINE_IDX[2];
		for (int i = 0; i < 16; i += 4) gMyDrum[0][i] = 230; for (int i = 2; i < 16; i += 4) gMyDrum[2][i] = 200; gDrums = 1; drumsChanged(); }
	if (fr == 14 || fr == 15) { TOUCH = 1; TX = 290; TY = DG_Y + 5; }   // EDIT
	if (fr == 18) { if (!gDgEditMode) fail("EDIT should turn on"); }
	if (fr == 20 || fr == 21) { float cw = (320 - DG_LX - 2) / 16.0f; TOUCH = 1; TX = DG_LX + (int)(4 * cw) + 3; TY = DG_TOP + 0 * DG_RH + 4; }   // KICK step 5
	if (fr == 24) { if (gDgEd != 1 || gDgEdI != 4) fail("EDIT: a tap on a step should open its editor (%d %d)", gDgEd, gDgEdI); shotBot("shots/drum_step.ppm"); }
	if (fr >= 26 && fr < 30) { TOUCH = 1; TX = 94; TY = DSE_B + 2; }   // TUNE: all the way down
	if (fr == 32) { if (gDrTn[0][4] > -20) fail("TUNE: the step should go down (%d)", gDrTn[0][4]); }
	if (fr == 34 || fr == 35) { TOUCH = 1; TX = 132 + 120; TY = DE_Y + 124; }   // NUDGE: later
	if (fr == 38) { if (!gMyDrum[0][5] || gMyDrum[0][4] || gDrTn[0][5] > -20 || gDgEdI != 5) fail("NUDGE: the hit (and its TUNE) should move to step 6 (%d %d %d)", gMyDrum[0][5], gMyDrum[0][4], gDrTn[0][5]); shotBot("shots/drum_step2.ppm"); }
	if (fr == 40 || fr == 41) { TOUCH = 1; TX = 280; TY = DE_Y + 6; }   // DONE
	if (fr == 44 || fr == 45) { TOUCH = 1; TX = 10; TY = DG_TOP + 2 * DG_RH + 4; }   // HAT's name: its SOUND
	if (fr == 48) { if (gDgEd != 2 || gDgEdR != 2) fail("EDIT: a name should open its SOUND (%d %d)", gDgEd, gDgEdR); }
	if (fr == 50 || fr == 51) { TOUCH = 1; TX = 160; TY = DE_Y + 6; }   // SOLO
	if (fr == 54) { if (gDrSolo != 2) fail("SOLO: the HAT row (%d)", gDrSolo); shotBot("shots/drum_sound.ppm"); for (int d = 0; d < 10; d++) h0[d] = gDrumHitN[d]; transportToggle(); }
	if (fr == 56) { TOUCH = 1; TX = 90; TY = DE_Y + 22 + 8 * DV_RH + 8; }   // RATE: a list
	if (fr == 58) { if (!gLs || gLsN != NDRATE) fail("RATE: a tap should open its list (%d %d)", gLs, gLsN); shotBot("shots/list_rate.ppm");
		int c = lsCols(), w = 312 / c; TOUCH = 1; TX = 4 + 2 * w + w / 2; TY = LS_Y + 8; }   // X2
	if (fr == 60) { if (gLs || gDrRate[2] != 2) fail("RATE: picking X2 should set it (%d %d)", gLs, gDrRate[2]); }
	if (fr == 174) { int k = gDrumHitN[0] - h0[0], h = gDrumHitN[2] - h0[2]; if (k || h < 3) fail("SOLO: only the hats should play (kicks %d, hats %d)", k, h); gDrSolo = -1; transportToggle(); done = 1; }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 178; app_main(); if (!done) fail("didn't run"); printf("drumedit: EDIT, TUNE, NUDGE, SOLO, LIST\n"); return gFails ? 1 : 0; }
