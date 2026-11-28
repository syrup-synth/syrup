// CONTROLS: DEVICE. The top's left only shows, its right is the list you change: the D-pad works it (up / down pick,
// left / right change). X PLAY / STOP, Y REC, A CHORD, B LATCH. The circle pad works as in CLASSIC (the edit cursor on the grids), the C-stick turns the
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
	if (fr == 16) HD = KEY_X;
	if (fr == 18) { if (gSeqMode != 1) fail("X should start the transport"); HD = KEY_X; }
	if (fr == 20) { if (gSeqMode == 1) fail("X again should stop it"); i0 = gChord; HD = KEY_A; }
	if (fr == 22) { if (gChord == i0) fail("A should change the chord"); i0 = gLatchShow; HD = KEY_B; }
	if (fr == 24) { if (gLatchShow == i0) fail("B should toggle LATCH"); HD = KEY_B; cut1 = gCutoff; }
	if (fr >= 25 && fr < 40) CSY = 150;   // C-stick up: the picked knob (CUTOFF) turns up
	static float r0_;
	if (fr == 41) { if (gCutoff <= cut1 + 0.01f) fail("C-stick should turn the picked knob (%.3f -> %.3f)", cut1, gCutoff); if (gStickMode != SM_XY) fail("the circle pad should default to XY PAD (%d)", gStickMode);
		for (int m = 0; m < NMOD; m++) if (gMod[m].src == SRC_STICK) gMod[m].src = SRC_OFF; gXyMode = 3; gCutoff = 0.4f; gRes = 0.2f; c0 = gCutoff; r0_ = gRes; }
	if (fr >= 42 && fr < 52) { CX = 150; CY = 150; }   // XY PAD: pushes the dot (CUTOFF + RES)
	if (fr == 54) { if (gCutoff <= c0 + 0.01f || gRes <= r0_ + 0.01f) fail("XY PAD: the circle pad should push the XY pad (cut %.3f -> %.3f, res %.3f -> %.3f)", c0, gCutoff, r0_, gRes); c0 = gCutoff; }
	if (fr == 58) { if (fabsf(gCutoff - c0) > 0.002f) fail("XY PAD: let go, the dot stays (%.3f -> %.3f)", c0, gCutoff); gStickMode = SM_XYS; c0 = gCutoff; }
	if (fr >= 59 && fr < 63) CX = 150;   // XY SPRING: follows the pad, back when let go
	if (fr == 62) { if (gCutoff <= c0 + 0.05f) fail("XY SPRING: the dot should follow the pad (%.3f -> %.3f)", c0, gCutoff); }
	if (fr == 66) { if (fabsf(gCutoff - c0) > 0.002f) fail("XY SPRING: let go, the dot goes back (%.3f -> %.3f)", c0, gCutoff); gStickMode = SM_XY; }
	if (fr == 67) shotTop("shots/device_top.ppm");
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
	if (fr == 88) { if (gOsc[0].distAmt <= c0) fail("> should turn the picked slider up (%.2f)", gOsc[0].distAmt); shotTop("shots/device_osc.ppm"); HD = KEY_Y; }
	if (fr == 90) { if (!gLiveRec && gPreFor < 1) fail("Y should start recording (count-in or REC)"); HD = KEY_Y; }
	if (fr == 94) { gPageWant = 0; }
	if (fr == 98) { c0 = (float)gPreset; gBrFocus = 1; HD = KEY_DDOWN; }
	if (fr == 104) { if ((float)gPreset == c0) fail("D-pad v on PATCH should move through the list"); gBPM = 90; micTempoArm(); }
	if (fr >= 105 && gMicBuf) { s16* m_ = (s16*)gMicBuf; u32 N_ = gMicSize / 2; int k = fr == 112 || fr == 142 || fr == 172 || fr == 202; for (int i_ = 0; i_ < 300; i_++) m_[N_ - 1 - i_] = k ? 26000 : 0; }   // TAP TEMPO: four knocks 0.5 s apart
	if (fr == 210) { if (gBPM != 120) fail("TAP TEMPO by the mic: four knocks 0.5 s apart should be 120 BPM (%d)", gBPM); gPageWant = 7; }
	if (fr == 214) { gGridOn = 1; gPatView = 0; gPads = 0; seqClear(); gSeqLen = 16; }
	if (fr >= 216 && fr < 220) CX = 150;   // the piano roll: the circle pad is the edit cursor
	if (fr == 230) HD = KEY_A;
	if (fr == 234) { int any = 0; for (int i = 0; i < 16; i++) any |= gSeq[i].n > 0; if (!any) fail("A on the piano roll should place a note at the cursor"); i0 = gSeqMode; HD = KEY_B; }
	if (fr == 240) { int any = 0; for (int i = 0; i < 16; i++) any |= gSeq[i].n > 0; if (any) fail("B on the piano roll should take the note away"); if (gLatchShow) fail("B on the roll shouldn't latch"); gPageWant = 1; }
	if (fr == 242) { int st, ix; if (findPreset("STICK THEREMIN", &st, &ix)) presetLoad(st, ix); c0 = gCutoff; }
	if (fr >= 246 && fr < 256) CY = 150;   // a patch that routes STICK gets the circle pad as STICK (not the XY pad)
	if (fr == 254) { if (gStick < 0.3f) fail("STICK THEREMIN: the circle pad should be its STICK in DEVICE (%.2f)", gStick); transportToggle(); }
	static int sm0;
	if (fr == 256) { sm0 = gStickMode; HD = KEY_SELECT; }
	if (fr == 257) HD = KEY_SELECT | KEY_DUP;   // SELECT + UP while it plays: the circle pad's next mode
	if (fr == 258) HD = KEY_SELECT;
	if (fr == 260) { if (gStickMode != (sm0 + 1) % NSM) fail("SELECT + UP should step the circle pad mode (%d -> %d)", sm0, gStickMode); if (gSeqMode != 1) fail("SELECT + UP shouldn't stop playback"); { int st, ix; if (findPreset("SYRUP PAD", &st, &ix)) presetLoad(st, ix); } gXyMode = 8; }
	static float mc0;
	if (fr == 262) { if (!gMorphOk) fail("PATCH MORPH: corners should be captured"); mc0 = gCutoff; gMorphX = 1; gMorphY = 1; }
	if (fr == 264) { float want = gMorph[3].cutoff; if (fabsf(gCutoff - want) > 0.01f) fail("PATCH MORPH: top right = the fourth patch (cutoff %.3f, want %.3f)", gCutoff, want); if (gV[0].active == 0 && 0) ; gMorphX = 0; gMorphY = 0; }
	if (fr == 266) { if (fabsf(gCutoff - mc0) > 0.01f) fail("PATCH MORPH: bottom left = the patch again (%.3f, %.3f)", gCutoff, mc0); gXyMode = 0; done = 1; }
}
int main(void) { TEST_DEVICE = 1; mkdir("sd", 0777); mkdir("shots", 0777); remove("sd/settings.bin"); loadFont(); FRMAX = 270; app_main(); if (!done) fail("didn't run"); printf("device: the DEVICE controls\n"); return gFails ? 1 : 0; }
