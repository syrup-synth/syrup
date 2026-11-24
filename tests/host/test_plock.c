// PARAMETER LOCKS: hold a note in the roll, pick CUTOFF in the LOCK list, set it; the sequence then plays that step with
// the locked cutoff and puts the patch's back on the next step; B clears them; old seq.bin still loads
#include "syrup_host.c"
#include "harness.h"
#define RX(st) (rollX(st) + 8)
#define RY(nt) (rollY(nt) + 5)
static int done = 0; static float base, during, after;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 5 || fr == 6) { TOUCH = 1; TX = 5 * 320 / 7 + 20; TY = 8; }   // SEQ chip
	if (fr == 9) { gScale = 0; gRollScale = 0; gRollLow = 46; gRollChord = 0; memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 4; stepSetNote(0, 48, 1); stepSetNote(2, 48, 1); gCutoff = 0.7f; base = gCutoff; gBPM = 120; }
	if (fr >= 12 && fr < 60) { TOUCH = 1; TX = RX(2); TY = RY(48); }   // hold step 3's note
	if (fr == 45) { if (gPlkStep != 2) fail("PLOCK: holding a note should open its LOCK list (%d)", gPlkStep); gPlkSel = 0; }   // CUTOFF
	if (fr >= 46 && fr < 56 && (fr & 1)) HD = KEY_DLEFT;   // lower it, 5 presses
	if (fr == 58) { int j = plkFind(&gSeq[2], 0); if (j < 0) fail("PLOCK: step 3 should have a CUTOFF lock"); else if (gSeq[2].lv[j] >= (u8)(base * 255) - 5) fail("PLOCK: the lock should be lower than the patch (%d)", gSeq[2].lv[j]); }
	if (fr == 64) { if (fabsf(gCutoff - base) > 0.01f) fail("PLOCK: letting go should put the patch's cutoff back (%.3f vs %.3f)", gCutoff, base); transportToggle(); }
	// at 120 BPM a step is 0.125 s = 7.5 frames: watch the cutoff over two bars
	if (fr > 70 && fr < 130) { if (gSeqPos == 2) during = gCutoff; if (gSeqPos == 3) after = gCutoff; }
	if (fr == 132) {
		if (during >= base - 0.01f) fail("PLOCK: step 3 should play with the locked cutoff (%.3f vs %.3f)", during, base);
		if (fabsf(after - base) > 0.01f) fail("PLOCK: step 4 should be back to the patch's cutoff (%.3f)", after);
		transportToggle(); if (fabsf(gCutoff - base) > 0.01f) fail("PLOCK: STOP should leave the patch's cutoff (%.3f)", gCutoff);
	}
	if (fr >= 140 && fr < 190) { TOUCH = 1; TX = RX(2); TY = RY(48); }
	if (fr == 180) HD = KEY_B;
	if (fr == 194) { if (gSeq[2].lk[0] || gSeq[2].lk[1] || gSeq[2].lk[2]) fail("PLOCK: hold + B should clear the step's locks"); }
	// MOTION SEQUENCE: REC on, the sequence playing, sweep the cutoff over a bar: each step keeps the value it went by at
	if (fr == 200) { memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 8; for (int k = 0; k < 8; k++) stepSetNote(k, 48, 1); gCutoff = 0.2f; gLiveRec = 1; transportToggle(); }
	if (fr > 202 && fr < 254) gCutoff = 0.2f + (fr - 202) * 0.012f;   // a slow sweep up, one bar (8 steps x 7.5 frames)
	if (fr == 263) { gLiveRec = 0; int n = 0, up = 1, last = -1; for (int k = 1; k < 8; k++) { int j = plkFind(&gSeq[k], 0); if (j >= 0) { n++; if (gSeq[k].lv[j] < last) up = 0; last = gSeq[k].lv[j]; } }
		if (n < 6) fail("MOTION: the sweep should be on most of the bar's steps (%d)", n); if (!up) fail("MOTION: the recorded values should rise with the sweep"); gCutoff = 0.2f; }
	if (fr > 270 && fr < 330) { if (gSeqPos == 6) after = gCutoff; }
	if (fr == 332) { if (after < 0.5f) fail("MOTION: played back, step 7 should have the swept-up cutoff (%.2f)", after); transportToggle(); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 336; app_main(); if (!done) fail("didn't run"); printf("plock: hold a note, lock CUTOFF, played and put back\n"); return gFails ? 1 : 0; }
