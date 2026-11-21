// LOOPER: a recorded loop must survive every patch (KITCHEN SINK and the CHIP ECHO patches used to kill it)
#include "syrup_host.c"
#include "harness.h"
static int st = 0, ix = 0, done = 0, checked = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 2) { int s, i; findPreset("WARM FELT", &s, &i); presetLoad(s, i); gLoopBars = 0; gBPM = 240; gLoopMode = 1; loopModeChanged(); }
	if (fr >= 4 && fr < 60 && fr % 10 == 4) midiPush(0x90, 60 + fr % 7, 100);
	if (fr >= 4 && fr < 60 && fr % 10 == 8) midiPush(0x80, 60 + (fr - 4) % 7, 0);
	// GRAIN EDITOR over the recorded loop: a touch sets the position and holds it, dragging up grows the grain
	if (fr == 94) { gGrainSrc = 0; gGrains = 1; gGrainSize = 0.3f; grainsChanged(); }
	if (fr == 96) { if (gGrSrcLen <= 64) fail("grain editor: no loop source");
		if (!grainEdTouch(GE_X + GE_W / 2, GE_Y + 30, 1, 0) || !gGrHold) fail("grain editor: a touch on the waveform should hold the grain");
		if (fabsf(gGrPos / gGrSrcLen - 0.5f) > 0.02f) fail("grain editor: touching the middle should put the grain in the middle (%.2f)", gGrPos / gGrSrcLen);
		grainEdTouch(GE_X + GE_W / 4, GE_Y + 10, 0, 0);
		if (fabsf(gGrPos / gGrSrcLen - 0.25f) > 0.02f || gGrainSize < 0.55f) fail("grain editor: drag left and up = earlier and longer (%.2f, size %.2f)", gGrPos / gGrSrcLen, gGrainSize); }
	if (fr == 97) { fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320; drawGrainEd(); shotBot("shots/grain_ed.ppm"); grainEdTouch(0, 0, 0, 1); if (gGrHold) fail("grain editor: letting go should release the hold"); }
	if (fr == 98) { gGrains = 0; grainsChanged(); }
	if (fr == 100) {
		if (gLoopMode != 2 || !gLoopHave || gLoopCh[0] < 0) fail("loop didn't record");
		stopCh(BK_CSND, -1); csndExecCmds(false);   // a stale -1 channel must not reach channel 31
		if (!CS[gLoopCh[0]].on) fail("stopCh(-1) stopped the looper's channel");
	}
	if (fr > 100 && !done && fr % 4 == 1) {   // every factory patch in turn, playing a chord
		if (ix >= presetCount(st)) { ix = 0; if (++st >= NSTYLES - 1) { done = 1; return; } }
		presetLoad(st, ix); midiPush(0x90, 60, 100); midiPush(0x90, 64, 100); midiPush(0x90, 67, 100);
	}
	if (fr > 100 && !done && fr % 4 == 3) {
		midiPush(0x80, 60, 0); midiPush(0x80, 64, 0); midiPush(0x80, 67, 0);
		const char* nm = presetName(st, ix);
		for (int k = 0; k < 2; k++) {
			int c = gLoopCh[k];
			if (!CS[c].on) fail("%s stopped the loop (channel %d)", nm, c);
			if (gEchoOn && (gEchoCh[0] == c || gEchoCh[1] == c)) fail("%s: CHIP ECHO took loop channel %d", nm, c);
			if (gCsOwner[c] >= 0) fail("%s: a voice took loop channel %d", nm, c);
			for (int d = 0; d < NDCH; d++) if (gDrumCh[d] == c) fail("%s: drums took loop channel %d", nm, c);
		}
		checked++; ix++;
	}
	if (fr == FRMAX - 1 && checked < 50) fail("only %d patches checked", checked);
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 1400; app_main(); printf("test_loop: %d patches, loop intact\n", checked); return gFails ? 1 : 0; }
