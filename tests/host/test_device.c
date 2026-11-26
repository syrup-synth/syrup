// CONTROLS: DEVICE. A / Y change the picked setting, X / B pick; the D-pad is the transport (> PLAY / STOP, < STOP,
// ^ REC, v TAP TEMPO); L / R step PATCH, OSC ... MIX, VIS, HW; the circle pad moves the page's picture (its two
// values), the C-stick turns the picked knob; the top's tabs sit over the bottom's. CLASSIC keeps the old buttons.
#include "syrup_host.c"
#include "harness.h"
static int done = 0; static float c0, r0, cut1;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; CX = CY = CSX = CSY = 0;
	if (fr == 2) { if (gCtl != 0) fail("DEVICE should be the default (%d)", gCtl); gN3 = 1; gPageWant = 4; gFiltType = 0; }
	if (fr == 6) HD = KEY_A;
	if (fr == 8) { if (gFiltType != 1) fail("A should turn the picked setting up (FILTER %d)", gFiltType); HD = KEY_Y; }
	if (fr == 10) { if (gFiltType != 0) fail("Y should turn it down (%d)", gFiltType); HD = KEY_B; }
	if (fr == 12) { c0 = gCutoff; HD = KEY_A; }
	if (fr == 14) { if (gCutoff <= c0) fail("B then A: the next setting (CUTOFF) should go up (%.3f -> %.3f)", c0, gCutoff); }
	if (fr == 16) HD = KEY_DRIGHT;
	if (fr == 18) { if (gSeqMode != 1) fail("D-pad > should start the transport"); HD = KEY_DLEFT; }
	if (fr == 20) { if (gSeqMode == 1) fail("D-pad < should stop it"); }
	if (fr == 22) { cut1 = gCutoff; }
	if (fr >= 23 && fr < 40) CSY = 150;   // C-stick up: the picked knob (CUTOFF) turns up
	if (fr == 41) { if (gCutoff <= cut1 + 0.01f) fail("C-stick should turn the picked knob (%.3f -> %.3f)", cut1, gCutoff); c0 = gCutoff; r0 = gRes; }
	if (fr == 41) { for (int m = 0; m < NMOD; m++) if (gMod[m].src == SRC_STICK) gMod[m].amt = 0; }   // (the INIT patch routes STICK: then the pad is bend + STICK, as before)
	if (fr >= 42 && fr < 60) CY = 150;   // the circle pad: this page's picture (cutoff, res)
	if (fr == 61) { if (fabsf(gRes - r0) < 0.01f) fail("circle pad should move the page's picture values (res %.3f -> %.3f)", r0, gRes); if (fabsf(gCutoff - c0) > 0.001f) fail("circle pad shouldn't be DATA in DEVICE"); }
	if (fr == 62) shotTop("shots/device_top.ppm");
	if (fr == 64) HD = KEY_L;   // FILT -> OSC
	if (fr == 66) { if (gCurPage != 1) fail("L from FILT should be OSC (%d)", gCurPage); HD = KEY_L; }
	if (fr == 68) { if (gCurPage != 0) fail("L from OSC should be PATCH (%d)", gCurPage); HD = KEY_L; }
	if (fr == 70) { if (gCurPage != 9) fail("L from PATCH should wrap to HW (%d)", gCurPage); HD = KEY_L; }
	if (fr == 72) { if (gCurPage != 8) fail("L from HW should be VIS (%d)", gCurPage); HD = KEY_R; }
	if (fr == 74) { HD = KEY_R; }
	if (fr == 76) { if (gCurPage != 0) fail("R from HW should be PATCH (%d)", gCurPage); HD = KEY_R; }
	if (fr == 78) { if (gCurPage != 1) fail("R from PATCH should be OSC (%d)", gCurPage); HD = KEY_DDOWN; }
	if (fr == 80) HD = KEY_DDOWN;   // TAP TEMPO
	if (fr == 82) { if (gBPM < 40 || gBPM > 240) fail("TAP TEMPO out of range (%d)", gBPM); shotTop("shots/device_osc.ppm"); done = 1; }
}
int main(void) { TEST_DEVICE = 1; mkdir("sd", 0777); mkdir("shots", 0777); remove("sd/settings.bin"); loadFont(); FRMAX = 84; app_main(); if (!done) fail("didn't run"); printf("device: the DEVICE controls\n"); return gFails ? 1 : 0; }
