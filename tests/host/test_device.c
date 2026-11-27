// CONTROLS: DEVICE. The top's left only shows, its right is the list you change: the D-pad works it (up / down pick,
// left / right change). A PLAY / STOP, B REC, X CHORD, Y LATCH. The circle pad is the XY pad, the C-stick turns the
// picked knob, TAP TEMPO hears knocks on the mic, L / R step PATCH, OSC ... MIX, VIS, HW. CLASSIC keeps the old buttons.
#include "syrup_host.c"
#include "harness.h"
static int done = 0; static float c0, r0, cut1; static int i0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; CX = CY = CSX = CSY = 0;
	if (fr == 2) { if (gCtl != 0) fail("DEVICE should be the default (%d)", gCtl); gN3 = 1; gPageWant = 4; gFiltType = 0; }
	if (fr == 6) HD = KEY_DRIGHT;
	if (fr == 8) { if (gFiltType != 1) fail("D-pad > should turn the picked setting up (FILTER %d)", gFiltType); HD = KEY_DLEFT; }
	if (fr == 10) { if (gFiltType != 0) fail("D-pad < should turn it down (%d)", gFiltType); HD = KEY_DDOWN; }
	if (fr == 12) { if (!gPnlHotP || strcmp(gPnlHotP->name, "FILTER")) fail("D-pad v with the sliders up should pick the tab's first control (%s)", gPnlHotP ? gPnlHotP->name : "none"); gPnlHotP = pnlFind("CUTOFF", 4); c0 = gCutoff; HD = KEY_DRIGHT; }
	if (fr == 14) { if (gCutoff <= c0) fail("D-pad > should turn the picked slider (CUTOFF) up (%.3f -> %.3f)", c0, gCutoff); }
	if (fr == 16) HD = KEY_A;
	if (fr == 18) { if (gSeqMode != 1) fail("A should start the transport"); HD = KEY_A; }
	if (fr == 20) { if (gSeqMode == 1) fail("A again should stop it"); i0 = gChord; HD = KEY_X; }
	if (fr == 22) { if (gChord == i0) fail("X should change the chord"); i0 = gLatchShow; HD = KEY_Y; }
	if (fr == 24) { if (gLatchShow == i0) fail("Y should toggle LATCH"); HD = KEY_Y; cut1 = gCutoff; }
	if (fr >= 25 && fr < 40) CSY = 150;   // C-stick up: the picked knob (CUTOFF) turns up
	if (fr == 41) { if (gCutoff <= cut1 + 0.01f) fail("C-stick should turn the picked knob (%.3f -> %.3f)", cut1, gCutoff); gXyMode = 3; gCutoff = 0.4f; gRes = 0.2f; c0 = gCutoff; r0 = gRes; }
	if (fr >= 42 && fr < 60) { CX = 150; CY = 150; }   // the circle pad: the XY pad (mode CUTOFF + RES)
	if (fr == 61) { if (gRes <= r0 + 0.01f || gCutoff <= c0 + 0.01f) fail("circle pad should push the XY pad (cut %.3f -> %.3f, res %.3f -> %.3f)", c0, gCutoff, r0, gRes); }
	if (fr == 63) c0 = gCutoff;
	if (fr == 67) { if (fabsf(gCutoff - c0) > 0.001f) fail("let go, the XY dot should stay (%.3f -> %.3f)", c0, gCutoff); shotTop("shots/device_top.ppm"); }
	if (fr == 68) HD = KEY_L;   // FILT -> OSC
	if (fr == 70) { if (gCurPage != 1) fail("L from FILT should be OSC (%d)", gCurPage); HD = KEY_L; }
	if (fr == 72) { if (gCurPage != 0) fail("L from OSC should be PATCH (%d)", gCurPage); HD = KEY_L; }
	if (fr == 74) { if (gCurPage != 9) fail("L from PATCH should wrap to HW (%d)", gCurPage); HD = KEY_L; }
	if (fr == 76) { if (gCurPage != 8) fail("L from HW should be VIS (%d)", gCurPage); HD = KEY_R; }
	if (fr == 78) { HD = KEY_R; }
	if (fr == 80) { if (gCurPage != 0) fail("R from HW should be PATCH (%d)", gCurPage); shotTop("shots/device_patch.ppm"); HD = KEY_R; }
	if (fr == 82) { if (gCurPage != 1) fail("R from PATCH should be OSC (%d)", gCurPage); gPnlHotP = NULL; HD = KEY_DDOWN; }
	if (fr == 84) { if (!gPnlHotP || strcmp(gPnlHotP->name, "ON")) fail("v on OSC A should pick its first control (%s)", gPnlHotP ? gPnlHotP->name : "none"); HD = KEY_DUP; }
	if (fr == 86) { if (!gPnlHotP || strcmp(gPnlHotP->name, "DIST A AMT")) fail("^ from the first should wrap to the last (%s)", gPnlHotP ? gPnlHotP->name : "none"); c0 = gOsc[0].distAmt; HD = KEY_DRIGHT; }
	if (fr == 88) { if (gOsc[0].distAmt <= c0) fail("> should turn the picked slider up (%.2f)", gOsc[0].distAmt); shotTop("shots/device_osc.ppm"); HD = KEY_B; }
	if (fr == 90) { if (!gLiveRec && gPreFor < 1) fail("B should start recording (count-in or REC)"); HD = KEY_B; }
	if (fr == 94) { gPageWant = 0; }
	if (fr == 98) { c0 = (float)gPreset; gBrFocus = 1; HD = KEY_DDOWN; }
	if (fr == 104) { if ((float)gPreset == c0) fail("D-pad v on PATCH should move through the list"); gBPM = 90; micTempoArm(); }
	if (fr >= 105 && gMicBuf) { s16* m_ = (s16*)gMicBuf; u32 N_ = gMicSize / 2; int k = fr == 112 || fr == 142 || fr == 172 || fr == 202; for (int i_ = 0; i_ < 300; i_++) m_[N_ - 1 - i_] = k ? 26000 : 0; }   // TAP TEMPO: four knocks 0.5 s apart
	if (fr == 210) { if (gBPM != 120) fail("TAP TEMPO by the mic: four knocks 0.5 s apart should be 120 BPM (%d)", gBPM); done = 1; }
}
int main(void) { TEST_DEVICE = 1; mkdir("sd", 0777); mkdir("shots", 0777); remove("sd/settings.bin"); loadFont(); FRMAX = 212; app_main(); if (!done) fail("didn't run"); printf("device: the DEVICE controls\n"); return gFails ? 1 : 0; }
