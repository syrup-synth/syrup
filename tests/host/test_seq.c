// Sequencer piano roll: tap / drag-length / tap-to-delete / hold on the roll, the step editor (locks, accent, a rhythm
// row for a pitch), patterns, song mode, save + load, note lengths and nudge in playback. Saves shots.
#include "syrup_host.c"
#include "harness.h"
static void tap(int fr, int at, int x, int y) { if (fr == at) { TOUCH = 1; TX = x; TY = y; } }
#define BTN_X(i) ((i) * 320 / GRID_BTNS + 20)
#define RX(st) (rollX(st) + 8)
#define RY(nt) (rollY(nt) + 5)
static int songA = -1, songB = -1;
static int sqHas(int nt) { for (int i = 0; i < gSqNn; i++) if (gSqN[i].note == nt && !gSqN[i].rel) return 1; return 0; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0; CX = 0;
	if (fr == 3) { int st, ix; if (findPreset("REESE CRATE", &st, &ix)) presetLoad(st, ix); gBPM = 120; gScale = 0; gGridOct = 0; gArp = 0; }
	tap(fr, 5, 5 * 320 / 7 + 20, 8);   // SEQ chip
	if (fr == 8 && !gGridOn) fail("SEQ chip should open the piano roll");
	tap(fr, 10, BTN_X(1), XY_Y + 5);                  // PAT
	tap(fr, 13, 4 + 63 + 30, PV_Y + 33);              // CLEAR
	if (fr == 15) { for (int i = 0; i < NSTEP; i++) if (gSeq[i].n) { fail("CLEAR left notes"); break; } if (!gPatView) fail("PAT should open the pattern view"); }
	tap(fr, 17, BTN_X(1), XY_Y + 5);                  // back to the roll
	if (fr == 19) gRollLow = 46;
	tap(fr, 20, RX(0), RY(48));                       // tap: a note
	if (fr == 23 && !stepHasNote(0, 48)) fail("roll: a tap should place a note");
	if (fr >= 26 && fr < 33) { TOUCH = 1; TX = RX(2) + (fr - 26) * 10; TY = RY(50); }   // drag from an empty cell: a long note
	if (fr == 35) { if (!stepHasNote(2, 50) || noteLen(&gSeq[2], 0) != 4) fail("roll: dragging should make a 4-step note (len %d)", noteLen(&gSeq[2], 0)); shotBot("shots/seq_roll.ppm"); }
	tap(fr, 37, RX(4), RY(50));                       // tap inside the long note: it goes
	if (fr == 39 && stepHasNote(2, 50)) fail("roll: tapping a note should remove it");
	if (fr >= 40 && fr < 80) { TOUCH = 1; TX = RX(0); TY = RY(48); }   // hold: step editor
	if (fr == 80 && (gStepEd != 0 || gStepEdRow != 48)) fail("roll: holding a note should open its step (%d, %d)", gStepEd, gStepEdRow);
	if (fr == 80 && !stepHasNote(0, 48)) fail("roll: holding shouldn't remove the note");
	if (fr >= 82 && fr < 84) { TOUCH = 1; TX = 100; TY = SE_RY + 4 * SE_RH + 8; }   // ACCENT (NOTE tab, row 5): a tap flips it on
	tap(fr, 85, 240, SE_TAB_Y + 5);                    // LOCKS tab
	if (fr >= 87 && fr < 90) { TOUCH = 1; TX = 300; TY = SE_RY + 8; TY += (fr - 87) * 12; }   // CUTOFF all the way up; sliding down doesn't change row
	if (fr >= 91 && fr < 94) { TOUCH = 1; TX = SE_TX + 33; TY = SE_RY + 4 * SE_RH + 8; }   // ROW EUCL ~3 for this pitch
	if (fr == 96) {
		shotBot("shots/seq_step.ppm");
		if (gSeq[0].res != 0) fail("step editor: dragging down off CUTOFF shouldn't touch RES (%d)", gSeq[0].res);
		if (gSeq[0].cut < 90) fail("step editor: CUTOFF lock didn't move (%d)", gSeq[0].cut);
		if (!(gSeq[0].fl & 1)) fail("step editor: ACCENT should switch on");
		int r = rowOfNote(48), hits = 0; if (r >= 0) for (int i = 0; i < 16; i++) hits += rowCellOn(r, i);
		if (r < 0 || gRowHits[r] < 2 || hits != gRowHits[r]) fail("step editor: C3 should get a euclid rhythm (row %d, %d hits)", r, hits);
		if (r >= 0) { gRowHits[r] = 0; gRowLen[r] = 0; }
		seqFireAt(0, 0, 0, 0); if (gLockCut < 0.5f) fail("step lock: firing the step should open the filter (%.2f)", gLockCut);
		sqAllOff(); gLockCut = 0;
		stepSetNote(3, 52, 1); gSeq[3].skip = 100; seqFireAt(3, 3, 0, 0); if (gSqNn) fail("PROB 0%%: the step shouldn't play"); gSeq[3].skip = 0; stepSetNote(3, 52, 0);
	}
	tap(fr, 98, 280, SE_Y + 2);                        // DONE
	if (fr == 101) { gSeq[0].pitch = 3; shotBot("shots/seq_locks.ppm"); gSeq[0].pitch = 0; }
	if (fr == 100 && gStepEd >= 0) fail("DONE should close the step editor");
	tap(fr, 101, BTN_X(1), XY_Y + 5);
	tap(fr, 104, 4 + 30, PV_Y + 33);                  // COPY>
	if (fr == 106) { if (gPatCur != 1) fail("COPY> should move to pattern B (%d)", gPatCur); else if (!stepHasNote(0, 48)) fail("COPY> should copy the notes"); }
	tap(fr, 108, 4 + 63 + 30, PV_Y + 33);             // CLEAR B
	tap(fr, 110, 4 + 3 * 63 + 30, PV_Y + 33);         // +ADD B
	tap(fr, 112, 4 + 5, PV_Y + 15);                   // pattern A
	if (fr == 114 && gPatCur != 0) fail("tapping A should switch to it (%d)", gPatCur);
	if (fr == 114 && !stepHasNote(0, 48)) fail("pattern A lost its notes after switching back");
	tap(fr, 116, 4 + 3 * 63 + 30, PV_Y + 33);         // +ADD A
	tap(fr, 118, 4 + 2 * 63 + 30, PV_Y + 33);         // SONG on
	if (fr == 120) { shotBot("shots/seq_patterns.ppm"); if (gSongLen != 2 || gSong[0] != 1 || gSong[1] != 0 || !gSongOn) fail("song chain should be B A (len %d)", gSongLen); gSeqMode = 1; }
	if (fr == 130) songA = gPatCur;
	if (fr == 130 + 150) songB = gPatCur;
	if (fr == 285) {
		if (songA != 1) fail("song should start on B (%d)", songA);
		if (songB != 0) fail("song should move to A after B (%d)", songB);
		gSeqMode = 0; seqStop();
		seqSave(); int keepCur = gPatCur; gSongOn = 0; gSongLen = 0; memset(gPat, 0, sizeof(gPat)); memset(gSeq, 0, sizeof(gSeq));
		seqLoad();
		if (gSongLen != 2 || !gSongOn || gPatCur != keepCur || !stepHasNote(0, 48)) fail("save + load lost the patterns or song");
	}
	if (fr == 286) {   // playback: a 4-step note holds through step 3 and lets go at the gate of step 4; NUDGE late / early
		gSongOn = 0; memset(gSeq, 0, sizeof(gSeq)); memset(gRowLen, 0, 8); memset(gRowHits, 0, 8); gSeqLen = 8; gSwing = 0; gArp = 0; gSeqGate = 0.6f;
		stepSetNote(0, 60, 1); stepSetLen(0, 60, 4); stepSetNote(4, 62, 1);
		gSeqMode = 1; float t = 0, dt = 0.001f; int held25 = -1, held38 = -1;
		for (int k = 0; k < 1000; k++) { seqTick(dt); float s16 = t * 120 / 60.0f * 4; if (held25 < 0 && s16 >= 2.5f) held25 = sqHas(60); if (held38 < 0 && s16 >= 3.8f) held38 = sqHas(60); t += dt; }
		gSeqMode = 0; seqStop();
		if (held25 != 1) fail("note length: a 4-step note should still sound in step 3");
		if (held38 != 0) fail("note length: it should let go at the gate of its last step");
		memset(gSeq, 0, sizeof(gSeq));
		for (int i = 0; i < 8; i++) stepSetNote(i, 48 + i, 1);
		gSeq[2].nudge = 40; gSeq[4].nudge = -40;
		gSeqMode = 1; t = 0; float at[8]; for (int i = 0; i < 8; i++) at[i] = -1;
		for (int k = 0; k < 2000; k++) { seqTick(dt); for (int i = 0; i < 8; i++) if (at[i] < 0 && sqHas(48 + i)) at[i] = t * 120 / 60.0f * 4; t += dt; }
		gSeqMode = 0; seqStop();
		if (fabsf(at[2] - 2.4f) > 0.05f) fail("NUDGE +40: step 3 should play at 2.4 16ths (%.2f)", at[2]);
		if (fabsf(at[4] - 3.6f) > 0.05f) fail("NUDGE -40: step 5 should play at 3.6 16ths (%.2f)", at[4]);
		if (fabsf(at[3] - 3.0f) > 0.05f) fail("step 4 should stay on time (%.2f)", at[3]);
	}
	if (fr == 287) {   // RATCHET 3: three hits in one step
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 4; stepSetNote(0, 72, 1); gSeq[0].rat = 3;
		unsigned h0 = gSeqHits;
		gSeqMode = 1; for (int k = 0; k < 120; k++) seqTick(0.001f);   // most of step 1 at 120 BPM (125 ms)
		int v = (int)(gSeqHits - h0);
		gSeqMode = 0; seqStop();
		if (v < 3) fail("RATCHET 3: the step should hit three times (%d hits)", v);
		// UNDO after ACID; step editor < / > walks the steps
		memset(gSeq, 0, sizeof(gSeq)); stepSetNote(5, 66, 1); gSeqLen = 16; undoMark(); acidGen();
		gPatView = 1; patViewTouch(20, PV_TOOLS_Y + 6); gPatView = 0;
		if (!stepHasNote(5, 66) || gSeq[0].n) fail("UNDO should bring back the pattern from before ACID");
		gStepEd = 5; gStepEdRow = 66; stepEdTouch(230, SE_Y + 2, 1, 0); stepEdTouch(0, 0, 0, 1); if (gStepEd != 6) fail("step editor ST> should go to step 7 (%d)", gStepEd);
		stepEdTouch(190, SE_Y + 2, 1, 0); stepEdTouch(190, SE_Y + 2, 1, 0); stepEdTouch(0, 0, 0, 1); if (gStepEd != 4) fail("step editor <ST twice should go to step 5 (%d)", gStepEd);
		{ Step b = gSeq[4]; int ch = 0; for (int k = 0; k < 8; k++) { stepEdTouch(140, SE_Y + 2, 1, 0); stepEdTouch(0, 0, 0, 1); if (gSeq[4].vel != b.vel || gSeq[4].nudge != b.nudge || gSeq[4].skip != b.skip) ch = 1; }
		  if (!ch) fail("step editor RND should change the step"); if (gSeq[4].skip > 30 || gSeq[4].nudge < -10 || gSeq[4].nudge > 10) fail("RND went too far"); }
		gStepEd = -1;
		// COPY / PASTE a step; REV / ROT / HUM in PAT, each undoable
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 8; stepSetNote(0, 60, 1); stepSetLen(0, 60, 2); gSeq[0].cut = 40; stepSetNote(3, 64, 1);
		gStepEd = 0; gStepEdRow = 60; stepEdTouch(40, SE_CP_Y + 4, 1, 0); stepEdTouch(0, 0, 0, 1);
		gStepEd = 5; stepEdTouch(200, SE_CP_Y + 4, 1, 0); stepEdTouch(0, 0, 0, 1); gStepEd = -1;
		if (!stepHasNote(5, 60) || gSeq[5].cut != 40 || gSeq[5].len[0] != 2) fail("PASTE should copy the note, length and locks to step 6");
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 8; stepSetNote(0, 60, 1); stepSetLen(0, 60, 2); stepSetNote(3, 64, 1); patStore();
		gPatView = 1; patViewTouch(150, PV_TOOLS_Y + 6);
		if (!stepHasNote(6, 60) || !stepHasNote(4, 64) || gSeq[6].len[0] != 2) fail("REV: the long note should end where it started (step 7), step 4 -> 5");
		patViewTouch(20, PV_TOOLS_Y + 6); if (!stepHasNote(0, 60) || !stepHasNote(3, 64)) fail("UNDO after REV should restore");
		patViewTouch(20, PV_TOOLS_Y + 6); patViewTouch(20, PV_TOOLS_Y + 6);   // redo, undo again
		patViewTouch(212, PV_TOOLS_Y + 6); if (!stepHasNote(1, 60) || !stepHasNote(4, 64) || gSeq[0].n) fail("ROT: everything one step later");
		gSeq[7] = gSeq[4]; memset(&gSeq[4], 0, sizeof(Step)); patViewTouch(212, PV_TOOLS_Y + 6); if (!stepHasNote(0, 64)) fail("ROT should wrap the last step to step 1");
		{ int v0 = gSeq[0].vel; int ch = 0; for (int k = 0; k < 6; k++) { patViewTouch(274, PV_TOOLS_Y + 6); if (gSeq[0].vel != v0) ch = 1; if (gSeq[0].nudge < -50 || gSeq[0].nudge > 50) fail("HUM nudge out of range"); }
		  if (!ch) fail("HUM should change velocities"); if (gSeq[1].n || gSeq[1].vel) fail("HUM should leave empty steps alone"); }
		gPatView = 0;
	}
	// circle pad in the step editor: moves the selected setting, whatever the pad's mode
	{ static int v0; if (fr == 292) { gStickMode = SM_BEND; stepSetNote(2, 60, 1); gStepEd = 2; gStepEdRow = 60; gSeTab = 0; gSeSel = 0; v0 = gSeq[2].vel; }
	  if (fr > 292 && fr < 330) CX = 150;
	  if (fr == 332) { if (gSeq[2].vel <= v0 + 8) fail("step editor: the circle pad should raise VEL (%d -> %d)", v0, gSeq[2].vel); gStepEd = -1; gStickMode = SM_DATA; } }
}
int main(void) {
	mkdir("shots", 0777); mkdir("sd", 0777);
	remove("sd/seq.bin");
	loadFont(); FRMAX = 336; app_main();
	printf("seq: piano roll tap/drag/delete/hold, step locks, accent, euclid pitch row, patterns, song, save, note lengths, nudge, copy/paste, rev/rot/hum\n");
	return gFails ? 1 : 0;
}
