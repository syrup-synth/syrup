// TAPE playing on the second chip must survive every patch (KITCHEN SINK and the CHIP ECHO patches used to kill the
// old looper the same way), and the grain editor works over a tape track
#include "syrup_host.c"
#include "harness.h"
static int st = 0, ix = 0, done = 0, checked = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 2) {   // a 1 s track on tape: a 220 Hz tone, packed like the recorder packs it
		int s, i; findPreset("WARM FELT", &s, &i); presetLoad(s, i);
		if (!tapeAlloc(0)) fail("tape: no memory");
		for (int k = 0; k < 2; k++) { Ima e = {0, 0}; memset(gTp[0][k], 0, 32728 * 15); for (int n = 0; n < 32728; n++) { int c = imaEnc(&e, (s16)(sinf(TWO_PI * 220 * n / 32728.0f) * 12000)); if (n & 1) gTp[0][k][n >> 1] |= c << 4; else gTp[0][k][n >> 1] = c; } }
		gTapeLen = 32728 * 30; gTpHave[0] = 1; gTapeLast = 0; gTpSpd[0] = 8;   /* 30 s: still running at the end; X2 repeats */
	}
	// GRAIN EDITOR over the recorded loop: a touch sets the position and holds it, dragging up grows the grain
	if (fr == 94) { gGrainSrc = 0; gGrains = 1; gGrainSize = 0.3f; grainsChanged(); }
	if (fr == 96) { if (gGrSrcLen <= 64) fail("grain editor: no tape source");
		if (!grainEdTouch(GE_X + GE_W / 2, GE_Y + 30, 1, 0) || !gGrHold) fail("grain editor: a touch on the waveform should hold the grain");
		if (fabsf(gGrPos / gGrSrcLen - 0.5f) > 0.02f) fail("grain editor: touching the middle should put the grain in the middle (%.2f)", gGrPos / gGrSrcLen);
		grainEdTouch(GE_X + GE_W / 4, GE_Y + 10, 0, 0);
		if (fabsf(gGrPos / gGrSrcLen - 0.25f) > 0.02f || gGrainSize < 0.55f) fail("grain editor: drag left and up = earlier and longer (%.2f, size %.2f)", gGrPos / gGrSrcLen, gGrainSize); }
	if (fr == 97) { fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320; drawGrainEd(); shotBot("shots/grain_ed.ppm"); grainEdTouch(0, 0, 0, 1); if (gGrHold) fail("grain editor: letting go should release the hold"); }
	if (fr == 98) { gGrains = 0; grainsChanged(); }
	if (fr == 99) tapeStartPlay(-1);
	if (fr == 100) {
		if (!gTapePlay || gTapeCh[0] < 0 || !CS[gTapeCh[0]].on) fail("tape didn't start");
		if (gTapeCh[0] >= 0 && CS[gTapeCh[0]].timer != CSND_TIMER(32728 * 2)) fail("TRACK SPEED X2 should double the rate (timer %u)", CS[gTapeCh[0]].timer);
		stopCh(BK_CSND, -1); csndExecCmds(false);   // a stale -1 channel must not reach channel 31
		if (gTapeCh[0] >= 0 && !CS[gTapeCh[0]].on) fail("stopCh(-1) stopped the tape's channel");
	}
	if (fr > 100 && !done && fr % 4 == 1) {   // every factory patch in turn, playing a chord
		if (ix >= presetCount(st)) { ix = 0; if (++st >= NSTYLES - 1) { done = 1; return; } }
		presetLoad(st, ix); midiPush(0x90, 60, 100); midiPush(0x90, 64, 100); midiPush(0x90, 67, 100);
	}
	if (fr > 100 && !done && fr % 4 == 3) {
		midiPush(0x80, 60, 0); midiPush(0x80, 64, 0); midiPush(0x80, 67, 0);
		const char* nm = presetName(st, ix);
		for (int k = 0; k < 2; k++) {
			int c = gTapeCh[k];
			if (c < 0 || !CS[c].on) { fail("%s stopped the tape (channel %d)", nm, c); continue; }
			if (gEchoOn && (gEchoCh[0] == c || gEchoCh[1] == c)) fail("%s: CHIP ECHO took tape channel %d", nm, c);
			if (gCsOwner[c] >= 0) fail("%s: a voice took tape channel %d", nm, c);
			for (int d = 0; d < NDCH; d++) if (gDrumCh[d] == c) fail("%s: drums took tape channel %d", nm, c);
		}
		checked++; ix++;
	}
	if (fr == FRMAX - 1 && checked < 50) fail("only %d patches checked", checked);
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 1400; app_main(); printf("test_loop: %d patches, tape intact\n", checked); return gFails ? 1 : 0; }
