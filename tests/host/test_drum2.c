// DRUMS grid, round 2: RATCHETS, CONDITIONS (1:2, FILL, PRE), each row's RATE and LENGTH, the drum SOUND editor
// (DECAY / TUNE / PITCH ENV baked or live), HOLD to open the editors, BEAT presets with their own length and rate.
#include "syrup_host.c"
#include "harness.h"
static int base[NDRUM];
static void mark(void) { memcpy(base, gDrumHitN, sizeof(base)); }
static int hits(int d) { return gDrumHitN[d] - base[d]; }
static void only(int r, int i, int rat, int cond) {   // one hit on row r, nothing else on the grid
	myDrumLoad(); memset(gMyDrum, 0, sizeof(gMyDrum)); memset(gDrumX, 0, sizeof(gDrumX)); memset(gDrRat, 0, sizeof(gDrRat)); memset(gDrCond, 0, sizeof(gDrCond));
	for (int k = 0; k < 10; k++) { gDrLen[k] = 16; gDrRate[k] = 0; }
	gKickPat = 7; gSnarePat = 6; gHatPat = 6;
	if (r < 3) gMyDrum[r][i] = 230; else gDrumX[r - 3][i] = 230; gDrRat[r][i] = (u8)rat; gDrCond[r][i] = (u8)cond;
}
static int len0, len2, timer0, timerUp;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	// 120 BPM: a 16th = 7.5 frames, a bar = 120 frames
	if (fr == 3) { SPatch p; patchInit(&p); patchApply(&p); gBPM = 120; gSwing = 0; gSeqMode = 1; memset(gSeq, 0, sizeof(gSeq)); gDrums = 1; drumsChanged(); }   // the drums play with PLAY (one transport)
	if (fr == 20) { only(0, 0, 3, 0); gDrums = 0; drumsChanged(); gDrums = 1; drumsChanged(); seqStop(); mark(); }   // RATCHET 4 on the kick's first step
	if (fr == 20 + 240) { if (hits(0) != 8) fail("RATCHET 4: two bars should give 8 kicks (%d)", hits(0)); }
	if (fr == 262) { only(0, 0, 0, 6); gDrums = 0; drumsChanged(); gDrums = 1; drumsChanged(); seqStop(); mark(); }   // 1:2
	if (fr == 262 + 480) { if (hits(0) != 2) fail("1:2: four bars should give 2 kicks (%d)", hits(0)); }
	if (fr == 744) { only(4, 0, 0, 15); gDrums = 0; drumsChanged(); gDrums = 1; drumsChanged(); seqStop(); mark(); }   // FILL: none until held
	if (fr == 744 + 240) { if (hits(4)) fail("FILL: no claps without FILL (%d)", hits(4)); gDrFill = 1; mark(); }
	if (fr == 984 + 240) { if (hits(4) != 2) fail("FILL held: a clap a bar (%d)", hits(4)); gDrFill = 0; }
	if (fr == 1226) { only(5, 0, 0, 0); gDrLen[5] = 3; gDrums = 0; drumsChanged(); gDrums = 1; drumsChanged(); seqStop(); mark(); }   // LENGTH 3: every 3 16ths
	if (fr == 1226 + 118) { if (hits(5) != 6) fail("LENGTH 3: one bar should give 6 rims (%d)", hits(5)); }
	if (fr == 1346) { only(6, 0, 0, 0); gDrRate[6] = 1; gDrums = 0; drumsChanged(); gDrums = 1; drumsChanged(); seqStop(); mark(); }   // RATE 1/2: once every 2 bars
	if (fr == 1346 + 238) { if (hits(6) != 1) fail("RATE 1/2: two bars should give 1 tom (%d)", hits(6)); }
	if (fr == 1346 + 478) { if (hits(6) != 2) fail("RATE 1/2: four bars should give 2 toms (%d)", hits(6)); }
	// SOUND: DECAY 200 makes the kick about twice as long, 50 half; TUNE +12 doubles the playback rate
	if (fr == 1830) { len0 = gDrumLen[0]; gDV[0].decay = 200; gDrumWant = gKit + 100; }
	if (fr == 1834) { len2 = gDrumLen[0]; if (len2 < len0 * 1.8f) fail("DECAY 200: the kick should get about twice as long (%d -> %d)", len0, len2);
		gDV[0].decay = 50; gDrumWant = gKit + 100; }
	if (fr == 1838) { if (gDrumLen[0] > len0 * 0.6f) fail("DECAY 50: the kick should get about half as long (%d -> %d)", len0, gDrumLen[0]);
		gDV[0].decay = 100; gDV[0].penv = 12; gDrumWant = gKit + 100; }
	if (fr == 1842) { if (gDrumLen[0] >= len0) fail("P.ENV +12: the pitched-up start should shorten the kick (%d -> %d)", len0, gDrumLen[0]); gDV[0] = DV_DEF; gDrumWant = gKit + 100;
		drumHit(0, 1); timer0 = CS[gDrumLastCh].timer; gDV[0].tune = 12; drumHit(0, 1); timerUp = CS[gDrumLastCh].timer; gDV[0].tune = 0;
		if (abs((int)timerUp * 2 - timer0) > 4) fail("TUNE +12 should halve the timer (%d -> %d)", timer0, timerUp); }
	// HOLD a step: the step editor; HOLD a name: the SOUND editor
	if (fr >= 1844 && fr < 1848) { HD |= KEY_SELECT; CX = -150; CY = 0; }   // QUICK: left = DRUMS
	if (fr >= 1852 && fr < 1890) { TOUCH = 1; TX = DG_LX + 3; TY = DG_TOP + 5; }
	if (fr == 1891) { if (gDgEd != 1 || gDgEdR != 0 || gDgEdI != 0) fail("HOLD a step: its editor should open (%d %d %d)", gDgEd, gDgEdR, gDgEdI);
		fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320; drawDgEd(); shotBot("shots/drum_step_ed.ppm"); gDgEd = 0; }
	if (fr >= 1895 && fr < 1935) { TOUCH = 1; TX = 10; TY = DG_TOP + 5; }
	if (fr == 1936) { if (gDgEd != 2) fail("HOLD a name: the SOUND editor should open (%d)", gDgEd);
		fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320; drawDgEd(); shotBot("shots/drum_sound_ed.ppm"); gDgEd = 0; }
	// a quick tap on a lit step still turns it off
	if (fr == 1940) { only(1, 4, 0, 0); }
	if (fr == 1942 || fr == 1943) { TOUCH = 1; TX = DG_LX + (int)(4.5f * (320 - DG_LX - 2) / 16.0f); TY = DG_TOP + DG_RH + 5; }
	if (fr == 1946 && gMyDrum[1][4]) fail("a quick tap on a lit step should turn it off");
	// BEATS: WALTZ is 12 steps; SLOW ROCK 12 at 3/4; CUMBIA has its guacharaca
	if (fr == 1950) { beatPaste(25); if (gDrLen[0] != 12) fail("WALTZ: rows should be 12 steps (%d)", gDrLen[0]);
		beatPaste(24); if (gDrLen[2] != 12 || gDrRate[2] != 3) fail("SLOW ROCK: 12 steps at 3/4 (%d %d)", gDrLen[2], gDrRate[2]);
		beatPaste(14); if (gDrLen[0] != 16 || gDrRate[0] || !gMyDrum[2][2]) fail("CUMBIA: 16 steps, the guacharaca on the hat");
		histStep(1); if (gDrRate[2] != 3) fail("undo should bring back SLOW ROCK's rate"); }
	// CIRCLE PAD: a cursor on the grid (the first push only shows it), A = hit, hold A = the step editor, the pad edits it, B closes
	if (fr >= 1955) { CX = 0; CY = 0; }
	if (fr == 1956) { gCurT = 0; gDcR = 0; gDcI = 0; }
	if (fr >= 1960 && fr < 1964) CX = 150;
	if (fr >= 1968 && fr < 1970) CX = 150;
	if (fr == 1972) { if (gDcI != 1 || gDcR != 0) fail("PAD: one push right should move the cursor to step 2 (%d,%d)", gDcR, gDcI); gMyDrum[0][1] = 0; }
	if (fr == 1974 || fr == 1975) HD |= KEY_A;
	if (fr == 1978 && !gMyDrum[0][1]) fail("PAD: A should put a kick at the cursor");
	if (fr >= 1980 && fr < 2012) HD |= KEY_A;
	if (fr == 2013) { if (gDgEd != 1 || gDgEdI != 1) fail("PAD: holding A should open the step's editor (%d)", gDgEd); if (!gMyDrum[0][1]) fail("PAD: holding A shouldn't take the hit away"); }
	if (fr >= 2014 && fr < 2016) CY = -150;
	if (fr >= 2020 && fr < 2022) CX = 150;
	if (fr == 2025) { if (gDgSelF != 1) fail("PAD: down should pick RATCHET (%d)", gDgSelF); if (gDrRat[0][1] != 1) fail("PAD: right should add a ratchet (%d)", gDrRat[0][1]); }
	if (fr == 2026 || fr == 2027) HD |= KEY_B;
	if (fr == 2029 && gDgEd) fail("PAD: B should close the editor");
	// and on the piano roll
	if (fr >= 2032 && fr < 2036) { HD |= KEY_SELECT; CX = -138; CY = -57; }   // QUICK: SEQ (between DRUMS and TAPE)
	if (fr >= 2040 && fr < 2042) CX = 150;   // shows the cursor
	if (fr == 2038) gCurT = 0;
	if (fr >= 2038 && fr < 2040) {}
	if (fr == 2044) { gSeq[gRcS].n = 0; }
	if (fr == 2046 || fr == 2047) HD |= KEY_A;
	if (fr == 2050 && noteCover(gRcS, gRcN, NULL) < 0) fail("PAD: A should put a note at the piano roll cursor (step %d note %d)", gRcS, gRcN);
	if (fr >= 2052 && fr < 2084) HD |= KEY_A;
	if (fr == 2085 && gStepEd != gRcS) fail("PAD: holding A should open the note's step editor (%d)", gStepEd);
	if (fr >= 2086 && fr < 2088) CX = 150;
	if (fr == 2090) { int k = gSeSel; (void)k; }
	if (fr == 2092 || fr == 2093) HD |= KEY_B;
	if (fr == 2095 && gStepEd >= 0) fail("PAD: B should close the step editor");
	// SEQ's DRUM button: asks for the DRUMS page (its settings on top, the drum sequencer below)
	if (fr == 2097) { gPageWant = -1; gridButtons(4 * 320 / GRID_BTNS + 5); if (gPageWant != 11) fail("SEQ DRUM: the button should bring up the DRUMS page (%d)", gPageWant); }
	// VELOCITY: drag a lit step down = softer (the step stays on); on the piano roll a note dragged down too
	if (fr == 2105) { gDrumOnB = 0; only(1, 2, 0, 0); gMyDrum[1][2] = 230; gDgShift = 0; int cx = DG_LX + (int)(2.5f * (320 - DG_LX - 2) / 16.0f), cy = DG_TOP + DG_RH + 5;
		drumGridTouch(cx, cy, 1); drumGridTouch(cx, cy + 20, 0); drumGridTouch(cx, cy + 40, 0); drumGridRelease();
		if (!gMyDrum[1][2] || gMyDrum[1][2] > 160) fail("VELOCITY: dragging a hit down should soften it, not remove it (%d)", gMyDrum[1][2]);
		memset(gSeq, 0, sizeof gSeq); gVoiceMode = 0; gRollLow = 48; stepSetNote(3, 50, 1); gSeq[3].vel = 200; gPatView = 0; gStepEd = -1;
		int x = rollX(3) + 3, y = rollY(50) + 4; gridTouch(x, y, 1, 0); gridTouch(x, y + 15, 0, 0); gridTouch(x, y + 30, 0, 0); gridTouch(x, y + 30, 0, 1);
		if (gSeq[3].vel >= 200 || !stepHasNote(3, 50)) fail("VELOCITY: dragging a note down should soften it and keep its pitch (vel %d)", gSeq[3].vel); }
	if (fr == 2106) {   // IF on sequencer steps: 1:2 plays on loops 0, 2, ...; 2:2 on 1, 3; the step editor skips NEI
		int pre = 0, a = 0, c = 0; for (int cy = 0; cy < 4; cy++) { a += condTest(6, cy, &pre, 0); c += condTest(7, cy, &pre, 0); }
		if (a != 2 || c != 2 || !condTest(6, 0, &pre, 0) || condTest(6, 1, &pre, 0)) fail("IF: 1:2 / 2:2 should alternate loops");
		memset(gSeq, 0, sizeof gSeq); stepSetNote(0, 60, 1); gStepEd = 0; gStepEdRow = 60; seSet(14, 18); seSet(14, 19); if (gSeq[0].cond != 21) fail("IF: the step editor should step over NEI (%d)", gSeq[0].cond); gStepEd = -1; }
	if (fr == 2107) {   // THEME picker: every theme on one screen, a tap picks one
		gThemePick = 1; fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320; drawThemePick(); shotBot("shots/theme_pick.ppm");
		if (!themePickTouch(4 + 2 * 105 + 10, 22 + 34 * 0 + 10) || gTheme != 2 || gThemePick) fail("THEME: tapping DOS BLUE should pick it and close (%d)", gTheme);
		gTheme = 16; }
	if (fr == 2104) { fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320; splashDraw(); shotBot("shots/loading_tip.ppm"); }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 2110; app_main(); printf("drum2: ratchets, conditions, row rate / length, sound editor, holds, beats\n"); return gFails ? 1 : 0; }
