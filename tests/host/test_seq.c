// Sequencer: tap / drag-draw / hold on the grid, the step editor (locks, euclid row), patterns, song mode,
// save + load. Saves shots of the grid, step editor and pattern view.
#include "syrup_host.c"
#include "harness.h"
static void tap(int fr, int at, int x, int y) { if (fr == at) { TOUCH = 1; TX = x; TY = y; } }
#define BTN_X(i) ((i) * 320 / GRID_BTNS + 20)
#define CELL_X(st) ((st) * 20 + 10)
#define CELL_Y(r) (GRID_TOP + (7 - (r)) * GRID_ROWH + 5)
static int songA = -1, songB = -1;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 3) { int st, ix; if (findPreset("REESE CRATE", &st, &ix)) presetLoad(st, ix); gBPM = 120; gScale = 0; gGridOct = 0; }
	tap(fr, 5, 290, 8);                               // SEQ chip
	if (fr == 8 && !gGridOn) fail("SEQ chip should open the grid");
	tap(fr, 10, BTN_X(1), XY_Y + 5);                  // PAT
	tap(fr, 13, 4 + 63 + 30, PV_Y + 33);              // CLEAR
	if (fr == 15) { for (int i = 0; i < NSTEP; i++) if (gSeq[i].n) { fail("CLEAR left notes"); break; } if (!gPatView) fail("PAT should open the pattern view"); }
	tap(fr, 17, BTN_X(1), XY_Y + 5);                  // back to the grid
	tap(fr, 20, CELL_X(0), CELL_Y(0));                // tap: place
	if (fr == 23 && !stepHasNote(0, gridNote(0))) fail("grid: a tap should place a note");
	if (fr >= 26 && fr < 33) { TOUCH = 1; TX = CELL_X(2) + (fr - 26) * 14; TY = CELL_Y(2); }   // drag: draw steps 2..6
	if (fr == 35) { for (int st = 2; st <= 6; st++) if (!stepHasNote(st, gridNote(2))) { fail("grid: dragging should draw step %d", st + 1); break; } shotBot("shots/seq_grid.ppm"); }
	if (fr >= 40 && fr < 80) { TOUCH = 1; TX = CELL_X(0); TY = CELL_Y(0); }   // hold: step editor
	if (fr == 80 && (gStepEd != 0 || gStepEdRow != 0)) fail("grid: holding a step should open its editor (%d)", gStepEd);
	if (fr == 80 && !stepHasNote(0, gridNote(0))) fail("grid: holding shouldn't toggle the note");
	if (fr >= 84 && fr < 87) { TOUCH = 1; TX = 313; TY = SE_ROW(3) + 4; }   // CUTOFF lock all the way up
	if (fr >= 90 && fr < 93) { TOUCH = 1; TX = 4 + 30; TY = SE_ROW(8) + 4; }   // ROW EUCL ~3
	if (fr == 95) {
		shotBot("shots/seq_step.ppm");
		if (gSeq[0].cut < 90) fail("step editor: CUTOFF lock didn't move (%d)", gSeq[0].cut);
		int hits = 0; for (int i = 0; i < 16; i++) hits += rowCellOn(0, i);
		if (gRowHits[0] < 2 || hits != gRowHits[0]) fail("step editor: euclid row should play %d hits, plays %d", gRowHits[0], hits);
		seqFireAt(0, 0); if (gLockCut < 0.5f) fail("step lock: firing the step should open the filter (%.2f)", gLockCut);
		playSet(NULL, 0, 0.8f); gLockCut = 0;
		gSeq[3].skip = 100; seqFireAt(3, 3); if (gHeldN) fail("PROB 0%%: the step shouldn't play"); gSeq[3].skip = 0;
	}
	tap(fr, 97, 280, SE_Y + 2);                        // DONE
	if (fr == 99 && gStepEd >= 0) fail("DONE should close the step editor");
	// patterns + song: copy A to B, clear B, put a note in it; song = B, A
	tap(fr, 101, BTN_X(1), XY_Y + 5);
	tap(fr, 104, 4 + 30, PV_Y + 33);                  // COPY>
	if (fr == 106) { if (gPatCur != 1) fail("COPY> should move to pattern B (%d)", gPatCur); else if (!stepHasNote(2, gridNote(2))) fail("COPY> should copy the notes"); }
	tap(fr, 108, 4 + 63 + 30, PV_Y + 33);             // CLEAR B
	tap(fr, 110, 4 + 3 * 63 + 30, PV_Y + 33);         // +ADD B
	tap(fr, 112, 4 + 5, PV_Y + 15);                   // pattern A
	if (fr == 114 && gPatCur != 0) fail("tapping A should switch to it (%d)", gPatCur);
	if (fr == 114 && !stepHasNote(2, gridNote(2))) fail("pattern A lost its notes after switching back");
	tap(fr, 116, 4 + 3 * 63 + 30, PV_Y + 33);         // +ADD A
	tap(fr, 118, 4 + 2 * 63 + 30, PV_Y + 33);         // SONG on
	if (fr == 120) { shotBot("shots/seq_patterns.ppm"); if (gSongLen != 2 || gSong[0] != 1 || gSong[1] != 0 || !gSongOn) fail("song chain should be B A (len %d)", gSongLen); gSeqMode = 1; }
	if (fr == 130) songA = gPatCur;                    // first pattern of the song
	if (fr == 130 + 150) songB = gPatCur;              // 16 steps at 120 BPM = 2 s later
	if (fr == 285) {
		if (songA != 1) fail("song should start on B (%d)", songA);
		if (songB != 0) fail("song should move to A after B (%d)", songB);
		gSeqMode = 0; seqStop();
		seqSave(); int keepCur = gPatCur; gSongOn = 0; gSongLen = 0; memset(gPat, 0, sizeof(gPat)); memset(gSeq, 0, sizeof(gSeq));
		seqLoad();
		if (gSongLen != 2 || !gSongOn || gPatCur != keepCur || !stepHasNote(2, gridNote(2))) fail("save + load lost the patterns or song");
	}
	if (fr == 286) {   // NUDGE: step 2 late by 40 %, step 4 early by 40 %; measure when each fires, in 16ths
		gSongOn = 0; memset(gSeq, 0, sizeof(gSeq)); memset(gRowLen, 0, 8); memset(gRowHits, 0, 8); gSeqLen = 8; gSwing = 0; gArp = 0;
		for (int i = 0; i < 8; i++) { gSeq[i].n = 1; gSeq[i].note[0] = 48 + i; gSeq[i].vel = 200; }
		gSeq[2].nudge = 40; gSeq[4].nudge = -40;
		gSeqMode = 1; float t = 0, dt = 0.001f, at[8]; for (int i = 0; i < 8; i++) at[i] = -1;
		for (int k = 0; k < 2000; k++) { seqTick(dt); if (gHeldN) { int i = gHeld[0] - 48; if (i >= 0 && i < 8 && at[i] < 0) at[i] = t * 120 / 60.0f * 4; } t += dt; }
		gSeqMode = 0; seqStop();
		if (fabsf(at[2] - 2.4f) > 0.05f) fail("NUDGE +40: step 3 should play at 2.4 16ths (%.2f)", at[2]);
		if (fabsf(at[4] - 3.6f) > 0.05f) fail("NUDGE -40: step 5 should play at 3.6 16ths (%.2f)", at[4]);
		if (fabsf(at[3] - 3.0f) > 0.05f) fail("step 4 should stay on time (%.2f)", at[3]);
	}
}
int main(void) {
	mkdir("shots", 0777); mkdir("sd", 0777);
	remove("sd/seq.bin");
	loadFont(); FRMAX = 290; app_main();
	printf("seq: grid tap/drag/hold, step locks, euclid row, patterns, song, save\n");
	return gFails ? 1 : 0;
}
