// SEQ RATE 1/32 (steps twice as fast, the drums stay in 16ths, tape lengths stay in bars), REC QUANT 32ND, and the
// CLICK metronome (ALWAYS / RECORDING, on the beat, never sent to the capture that feeds CHIP > FX and the tape).
#include "syrup_host.c"
#include "harness.h"
static int k0, c0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	// 120 BPM: a 16th = 7.5 frames, a bar = 120
	if (fr == 3) { SPatch p; patchInit(&p); patchApply(&p); gBPM = 120; gSwing = 0; seqClear(); gSeqLen = 16; for (int i = 0; i < 16; i++) stepSetNote(i, 60, 1);
		myDrumLoad(); memset(gMyDrum, 0, sizeof gMyDrum); memset(gDrumX, 0, sizeof gDrumX); gMyDrum[0][0] = 230; gKickPat = 7; gSnarePat = 6; gHatPat = 6;
		gSeqRate = 1; gDrums = 1; drumsChanged(); gSeqMode = 1; gClick = 3; }
	if (fr == 10) { k0 = gDrumHitN[0]; c0 = gClickN; }
	if (fr == 250) {
		if (gSeqClock < 60 || gSeqClock > 68) fail("SEQ RATE 1/32: two bars should be about 64 steps (%u)", gSeqClock);
		int k = gDrumHitN[0] - k0; if (k < 2 || k > 3) fail("SEQ RATE 1/32: the drums stay in 16ths, about 2 kicks in 2 bars (%d)", k);
		int c = gClickN - c0; if (c < 7 || c > 9) fail("CLICK ALWAYS: a click a beat, about 8 in 2 bars (%d)", c);
		if (gClickCh >= 0 && (CS[gClickCh].cl != 0 || CS[gClickCh].cr != 0)) fail("CLICK: it must not feed the capture (it would land on tape)");
		gTapeLenSel = 0; int want = 4 * 4 * 32728 / 2; if (abs(tapeSamples() - want) > 400) fail("TAPE 4 BARS at SEQ RATE 1/32 is still 4 bars (%d, want %d)", tapeSamples(), want);
		gTapeLenSel = 5; gSongOn = 0; want = 16 * 32728 / 8 / 2; if (abs(tapeSamples() - want) > 400) fail("TAPE PATTERN at 1/32: 16 steps = half a bar (%d, want %d)", tapeSamples(), want);
		gClick = 2; c0 = gClickN; }
	if (fr == 370 && gClickN != c0) fail("CLICK RECORDING: no clicks when nothing records (%d)", gClickN - c0);
	if (fr == 372) {   // REC QUANT 32ND: a note a quarter step in stays; half way = +50 nudge; three quarters = the next step
		gRecQ = 3; int nd, ah; gSeqPos = 4; float L = stepLen(4);
		gSeqPh = L * 0.1f; int st = recPos(&nd, &ah); if (st != 4 || nd != 0) fail("32ND: just after the step stays on it (%d %d)", st, nd);
		gSeqPh = L * 0.5f; st = recPos(&nd, &ah); if (st != 4 || nd != 50) fail("32ND: half way is the 32nd after (+50) (%d %d)", st, nd);
		gSeqPh = L * 0.8f; st = recPos(&nd, &ah); if (st != 5 || nd != 0) fail("32ND: near the next step goes to it (%d %d)", st, nd);
		gSeqRate = 0; gSeqMode = 0; gDrums = 0; drumsChanged(); }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 380; app_main(); printf("rate: SEQ RATE 1/32, drums in 16ths, tape bars, REC QUANT 32ND, CLICK\n"); return gFails ? 1 : 0; }
