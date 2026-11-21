// LOOPER: a recorded loop must survive every patch (KITCHEN SINK and the CHIP ECHO patches used to kill it)
#include "syrup_host.c"
#include "harness.h"
static int st = 0, ix = 0, done = 0, checked = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 2) { int s, i; findPreset("WARM FELT", &s, &i); presetLoad(s, i); gLoopBars = 0; gBPM = 240; gLoopMode = 1; loopModeChanged(); }
	if (fr >= 4 && fr < 60 && fr % 10 == 4) midiPush(0x90, 60 + fr % 7, 100);
	if (fr >= 4 && fr < 60 && fr % 10 == 8) midiPush(0x80, 60 + (fr - 4) % 7, 0);
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
