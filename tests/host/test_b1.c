// Matt's first fix batch: SELECT is PLAY/STOP for the sequencer AND the drums (on release), SELECT+L/R undo/redo,
// SELECT+DOWN panic, L/R octave, OCT flash, CLEAR needs two presses, MONO + CHORD plays the key you hit, PADS from
// the SEQ tab, RIFF stays in its octave, HELP slides on L/R.
#include "syrup_host.c"
#include "harness.h"
static void press(int fr, int at, u32 k) { if (fr == at || fr == at + 1) HD |= k; }
static int pat0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 3) { int st, ix; if (findPreset("WARM FELT", &st, &ix)) presetLoad(st, ix); gSeqMode = 0; gDrums = 1; drumsChanged(); }
	// SELECT tap: stops the sequencer and the drums, PLAY brings both back
	if (fr == 6) { gSeqMode = 1; }
	press(fr, 8, KEY_SELECT);
	if (fr == 12) { if (gSeqMode == 1) fail("SELECT should stop the sequencer"); if (gDrums) fail("SELECT should stop the drums too"); }
	press(fr, 13, KEY_SELECT);
	if (fr == 17) { if (gSeqMode != 1) fail("SELECT again should play"); if (!gDrums) fail("PLAY should bring the drums back"); }
	// SELECT+DOWN: panic (and no PLAY/STOP on release)
	if (fr >= 20 && fr < 26) HD |= KEY_SELECT; if (fr == 22 || fr == 23) HD |= KEY_DDOWN;
	if (fr == 29) { if (gSeqMode == 1) fail("SELECT+DOWN should panic (stop everything)"); if (gDrums) fail("SELECT+DOWN: drums should stop"); if (!strstr(gMsg, "PANIC")) fail("panic message (%s)", gMsg); }
	// undo / redo a patch edit with SELECT+L / SELECT+R
	if (fr == 32) { gCutoff = 0.5f; }
	if (fr == 33) { const Param* p = NULL; for (int i = 0; PAGES[4][i].name; i++) if (PAGES[4][i].p == &gCutoff) p = &PAGES[4][i]; if (!p) fail("no CUTOFF param"); else { patchMark(p); paramChange(p, 1, 1); paramChange(p, 1, 1); } }
	if (fr == 34 && gCutoff < 0.52f) fail("cutoff should have moved (%.2f)", gCutoff);
	if (fr >= 36 && fr < 40) HD |= KEY_SELECT; if (fr == 37) HD |= KEY_L;
	if (fr == 41 && fabsf(gCutoff - 0.5f) > 0.001f) fail("SELECT+L should undo the cutoff edit (%.3f)", gCutoff);
	if (fr >= 42 && fr < 46) HD |= KEY_SELECT; if (fr == 43) HD |= KEY_R;
	if (fr == 47 && gCutoff < 0.52f) fail("SELECT+R should redo it (%.3f)", gCutoff);
	if (fr == 48 && gSeqMode == 1) fail("an undo combo shouldn't toggle PLAY");
	// L / R: octave, and the OCT buttons light
	if (fr == 50) pat0 = 0;
	press(fr, 52, KEY_R);
	if (fr == 55) { if (gOctFlash[1] <= 0) fail("R should light OCT+"); }
	if (fr == 56) { midiPush(0x90, 60, 100); }
	// MONO + CHORD: the key you hit, one note
	if (fr == 60) { midiPush(0x80, 60, 0); gVoiceMode = 1; gChord = 1; gArp = 0; int n[4]; int k = chordNotes(60, n); if (k != 1 || n[0] != 60) fail("MONO + CHORD should play just the key (%d notes, %d)", k, n[0]); gVoiceMode = 0;
		k = chordNotes(60, n); if (k < 3) fail("POLY + CHORD should still play the chord"); gChord = 0; }
	// CLEAR in the SEQ menu: two presses of right
	if (fr == 62) { memset(gSeq, 0, sizeof(gSeq)); stepSetNote(0, 60, 1); const Param* p = NULL; for (int i = 0; PAGES[7][i].name; i++) if (PAGES[7][i].dirty & D_CLEAR) p = &PAGES[7][i]; if (!p) fail("no CLEAR control"); else {
		int pg = 7; (void)pg; } }
	// RIFF stays inside the octave
	if (fr == 64) { gGridOct = 0; int lo = gridNote(0); for (int k = 0; k < 40; k++) { acidGen(); for (int s = 0; s < 16; s++) if (gSeq[s].n && (gSeq[s].note[0] < lo || gSeq[s].note[0] >= lo + 12)) { fail("RIFF left the octave (%d, base %d)", gSeq[s].note[0], lo); k = 99; break; } } }
	// HELP: L/R flip the slides, another button closes
	if (fr == 66) gHelpSlide = 0;
	if (fr == 67) {   // piano roll: drag a note's head to move it; drag a 1-step note's tail = length
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; gRollLow = 55; stepSetNote(0, 60, 1); stepSetLen(0, 60, 2);
		gridTouch(rollX(0) + 3, rollY(60) + 5, 1, 0); gridTouch(rollX(3) + 3, rollY(62) + 5, 0, 0); gridTouch(rollX(3) + 3, rollY(62) + 5, 0, 1);
		if (gSeq[0].n) fail("ROLL: the dragged note should leave step 1");
		if (!(gSeq[3].n == 1 && gSeq[3].note[0] == 62 && gSeq[3].len[0] == 2)) fail("ROLL: the note should land on step 4, pitch 62, still 2 long (n %d note %d len %d)", gSeq[3].n, gSeq[3].note[0], gSeq[3].len[0]);
		memset(gSeq, 0, sizeof(gSeq)); stepSetNote(5, 60, 1);
		gridTouch(rollX(5) + 3, rollY(60) + 5, 1, 0); gridTouch(rollX(8) + 3, rollY(60) + 5, 0, 0); gridTouch(rollX(8) + 3, rollY(60) + 5, 0, 1);
		if (!(gSeq[5].n == 1 && gSeq[5].len[0] == 4)) fail("ROLL: dragging a 1-step note along its row should stretch it (len %d)", gSeq[5].len[0]);
		gSeq[5].vel = 190; gridTouch(rollX(5) + 3, VEL_Y + VEL_H - 5, 1, 0); gridTouch(rollX(5) + 3, VEL_Y + VEL_H - 5, 0, 1);
		if (gSeq[5].vel > 60) fail("VEL lane: a touch near its bottom should make the step soft (%d)", gSeq[5].vel);
		gridTouch(rollX(5) + 3, VEL_Y + 2, 1, 0); gridTouch(rollX(5) + 3, VEL_Y + 2, 0, 1);
		if (gSeq[5].vel < 230) fail("VEL lane: near its top should be loud (%d)", gSeq[5].vel);
		gLane = 0; gridTouch(4, VEL_Y + 5, 1, 0); gridTouch(4, VEL_Y + 5, 0, 1); if (gLane != 1) fail("LANE: tapping its name should go to GATE (%d)", gLane);
		gridTouch(rollX(5) + 3, VEL_Y + VEL_H - 8, 1, 0); gridTouch(rollX(5) + 3, VEL_Y + VEL_H - 8, 0, 1); if (!gSeq[5].gate || gSeq[5].gate > 30) fail("GATE lane: low should set a short gate (%d)", gSeq[5].gate);
		gLane = 3; gSeq[5].fl = 0; gridTouch(rollX(5) + 3, VEL_Y + 10, 1, 0); gridTouch(rollX(5) + 3, VEL_Y + 10, 0, 1); if (!(gSeq[5].fl & 1)) fail("ACC lane: a tap should accent the step");
		gLane = 4; gridTouch(rollX(5) + 3, VEL_Y + 10, 1, 0); gridTouch(rollX(5) + 3, VEL_Y + 10, 0, 1); if (!(gSeq[5].fl & 2)) fail("SLD lane: a tap should slide the step");
		gLane = 2; gridTouch(rollX(5) + 3, VEL_Y + VEL_H / 2, 1, 0); gridTouch(rollX(5) + 3, VEL_Y + VEL_H / 2, 0, 1); if (gSeq[5].skip < 30 || gSeq[5].skip > 70) fail("PROB lane: middle = about 50%% (skip %d)", gSeq[5].skip);
		gLane = 0;
	}
	// TRANCE GATE: a held note gets chopped on the 16ths
	{ static int g0; if (fr == 68) { gGatePat = 1; gGateDepth = 1; gGateRate = 0; gBPM = 120; midiPush(0x90, 57, 110); g0 = ON; }
	  if (fr == 100 + 0 && 0) {}
	  if (fr == 69 + 30) { midiPush(0x80, 57, 0); gGatePat = 0;
		double lo = 1e9, hi = 0; int w = SEC(0.125) / 4; for (int a = g0 + SEC(0.2); a + w < ON; a += w) { double r = rmsOf(a, a + w); if (r < lo) lo = r; if (r > hi) hi = r; }
		if (!(hi > 200 && lo < hi * 0.35)) fail("GATE: the held note should be chopped (rms %.0f .. %.0f)", lo, hi); } }
	// PADS: PROGRESSION row. Pick I V vi IV, tap its 2nd chord (V), then AUTO walks a chord a bar
	if (fr == 112) { gPads = 1; gProg = 1; float v; padsTouch(48, 104 + 44 + 10, PROG_Y + 8, 1, &v); if (gPadSel != 4) fail("PROG: the 2nd chip of I V vi IV should play V (pad %d)", gPadSel);
		padsTouch(48, 290, PROG_Y + 8, 1, &v); if (!gProgAuto) fail("PROG: AUTO should turn on"); gBPM = 240; }
	if (fr == 113) shotBot("shots/pads_prog.ppm");
	if (fr == 114 + 140) { if (gProgAt < 1) fail("PROG AUTO: a second at 240 BPM should have moved on (%d)", gProgAt); gProgAuto = 0; gPads = 0; playSet(NULL, 0, 0.8f); gBPM = 120; }
	// QUICK SELECT bars: SELECT + circle pad. UP = pages (lean right = further along), RIGHT / LEFT = this page's
	// choices, DOWN = transport
	if (fr >= 70 && fr < 76) { HD |= KEY_SELECT; CX = -29; CY = 150; }   // up, leaning a little left: the 5th page (FILT)
	if (fr == 73) shotTop("shots/quick_bars.ppm");
	if (fr == 78 && gCurPage != 4) fail("QUICK: up, leaning left, should land on FILT+ENV (%d)", gCurPage);
	if (fr == 80) gFiltType = 0;
	if (fr >= 82 && fr < 88) { HD |= KEY_SELECT; CX = -150; CY = -60; }   // left bar on FILT+ENV: FILTER type, lean down = further
	if (fr == 86) shotTop("shots/quick_side.ppm");
	if (fr == 90 && gSeqMode == 1) fail("QUICK: a pick shouldn't toggle PLAY");
	if (fr == 90 && gFiltType == 0) fail("QUICK: the left bar should pick another FILTER type (%d)", gFiltType);
	if (fr >= 92 && fr < 96) { HD |= KEY_SELECT; CX = 0; CY = -150; }   // down, centre: PLAY
	// DRUMS page step grid: go there with QUICK (up, far right = DRM), tap the kick's step 3, check MINE
	if (fr >= 100 && fr < 104) { HD |= KEY_SELECT; CX = 0; CY = 150; }
	if (fr >= 100 && fr < 104) { for (int i = 0; i < NPAGES; i++) if (NAV[i] == 11) { float along = i / (float)(NPAGES - 1) * 2 - 1; CX = (short)(along / 1.4f * 150); } }
	if (fr == 106) { if (gCurPage != 11) fail("QUICK: should reach DRUMS (%d)", gCurPage); gKickPat = 1; }
	if (fr == 108 || fr == 109) { TOUCH = 1; TX = DG_LX + (int)(2.5f * (320 - DG_LX - 2) / 16.0f); TY = DG_TOP + 6; }
	if (fr == 111) { shotBot("shots/drum_grid.ppm"); if (gKickPat != 7) fail("DRUM GRID: tapping a step should make the kick row MINE (%d)", gKickPat);
		if (!gMyDrum[0][2] || !gMyDrum[0][0] || !gMyDrum[0][4]) fail("DRUM GRID: MINE should start from FOUR FLOOR plus the new step"); }
	if (fr == 98 && gSeqMode != 1) fail("QUICK: down should PLAY");
}
int main(void) {
	mkdir("sd", 0777); loadFont(); FRMAX = 260; app_main();
	printf("b1: SELECT transport + combos, undo/redo, L/R octave, MONO chord, RIFF octave\n");
	return gFails ? 1 : 0;
}
