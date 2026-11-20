// Hardware hacks 2 and 3. CHIP > FX: the second chip's parts must still be heard (once, not twice), follow the
// chip filter, and pick up MASTER FX. Varispeed: LOOP SPEED retimes the loop channels; TAPE STOP runs the loop down.
#include "syrup_host.c"
#include "harness.h"
static int MODE = 0;   // 0 thru off, 1 thru on, 2 thru on + low cutoff, 3 looper varispeed
static double hitRms[4], hitHf[4]; static int hitAt[4] = {30, 60, 90, 120};
static u32 loopTimerFast = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (MODE < 3) {
		if (fr == 3) { int st, ix; if (findPreset("909 KIT", &st, &ix)) presetLoad(st, ix); gMasterFX = 1; gThru = MODE >= 1; gThruCut = MODE == 2 ? 0.25f : 1.0f; }
		for (int k = 0; k < 4; k++) { if (fr == hitAt[k]) midiPush(0x90, k & 1 ? 38 : 36, 110); if (fr == hitAt[k] + 4) midiPush(0x80, k & 1 ? 38 : 36, 0); }
		return;
	}
	// MODE 3: record a 1-bar loop of a held saw, then speed it up, then tape-stop it
	if (fr == 3) { SPatch p; patchInit(&p); patchApply(&p); gBPM = 120; gLoopBars = 0; }
	if (fr == 10) midiPush(0x90, 57, 100);
	if (fr == 12) { gLoopMode = 1; loopModeChanged(); }
	if (fr == 140) midiPush(0x80, 57, 0);
	if (fr == 200) { gLoopSpeed = 12; }
	if (fr == 205 && gLoopCh[0] >= 0) loopTimerFast = CS[gLoopCh[0]].timer;
	if (fr == 230) { gLoopSpeed = 0; gTapeStop = 1; }
}
int main(int c, char** v) {
	MODE = c > 1 ? atoi(v[1]) : 0;
	loadFont(); FRMAX = MODE == 3 ? 360 : 160; app_main();
	if (MODE < 3) {
		printf("chip %s:", MODE == 0 ? "direct" : MODE == 1 ? "thru FX" : "thru FX, cutoff 25%");
		for (int k = 0; k < 4; k++) { int a = SEC(hitAt[k] / 60.0); hitRms[k] = rmsOf(a, a + SEC(0.2)); hitHf[k] = hfRmsOf(a, a + SEC(0.2)); printf(" %s %.0f/hf %.0f", k & 1 ? "snare" : "kick", hitRms[k], hitHf[k]); }
		int a = SEC(130 / 60.0); printf(" | tail %.0f\n", rmsOf(a, a + SEC(0.3)));
		FILE* f = fopen("chip_levels.txt", MODE == 0 ? "w" : "a"); for (int k = 0; k < 4; k++) fprintf(f, "%d %d %.1f %.1f\n", MODE, k, hitRms[k], hitHf[k]); fclose(f);
		for (int k = 0; k < 4; k++) if (hitRms[k] < 150) fail("chip mode %d: hit %d silent", MODE, k);
		if (MODE >= 1) {   // compare with direct
			FILE* g = fopen("chip_levels.txt", "r"); double d[4] = {0}, dh[4] = {0}; int m, k; double r, h; while (fscanf(g, "%d %d %lf %lf", &m, &k, &r, &h) == 4) if (m == 0) { d[k] = r; dh[k] = h; } fclose(g);
			for (k = 0; k < 4; k++) if (d[k] > 0) {
				double db = 20 * log10(hitRms[k] / d[k]);
				if (MODE == 1 && fabs(db) > 4.5) fail("CHIP > FX: hit %d is %+.1f dB vs direct (heard twice, or lost?)", k, db);
				double hdb = 20 * log10(hitHf[k] / dh[k]);
				if (MODE == 2 && (k & 1) && hdb > -10) fail("CHIP > FX: the chip filter at 25%% only cut the snare's highs %+.1f dB", hdb);
				if (MODE == 2 && (k & 1)) printf("  filter: snare highs %+.1f dB\n", hdb);
			}
		}
		char n[64]; snprintf(n, sizeof n, "wav/chip%d.wav", MODE); writeWav(n, 0, ON);
	} else {
		u32 normal = CSND_TIMER(32728);
		int a = SEC(150 / 60.0), b = SEC(345 / 60.0);
		double before = rmsOf(a, a + SEC(0.5)), after = rmsOf(b, b + SEC(0.2));
		printf("loop: timer %u at normal, %u at +12 st | level %.0f before tape stop, %.0f after\n", normal, loopTimerFast, before, after);
		if (!gLoopHave) fail("varispeed: the loop never finished recording");
		if (loopTimerFast < normal / 2 - 2 || loopTimerFast > normal / 2 + 2) fail("varispeed: +12 st should halve the timer (%u -> %u)", normal, loopTimerFast);
		if (before < 150) fail("varispeed: the loop is silent (%.0f)", before);
		if (after > before * 0.2) fail("varispeed: TAPE STOP didn't stop the loop (%.0f -> %.0f)", before, after);
		writeWav("wav/varispeed.wav", 0, ON);
	}
	return gFails ? 1 : 0;
}
