// One factory patch: C4 held for 1.2 s, then released. Prints style index name rms_attack rms_body peak clips dc.
// run_tests.py runs it for every patch and compares against tests/baseline/levels.txt.
#include "syrup_host.c"
#include "harness.h"
static int ST, IX;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 2) { gFxCore2 = 0; gBPM = 120; presetLoad(ST, IX); gFxCore2 = 0; }
	if (fr == 10) midiPush(0x90, 60, 100);
	if (fr == 10 + 72) midiPush(0x80, 60, 0);
}
int main(int c, char** v) {
	if (c < 3) return 2;
	ST = atoi(v[1]); IX = atoi(v[2]);
	if (ST < 0 || ST >= NSTYLES - 1 || IX < 0 || IX >= MAXPRE || !PRESETS[ST][IX].name) return 3;
	loadFont(); FRMAX = 10 + 100; app_main();
	int t0 = SEC(10 / 60.0);
	double a = rmsOf(t0, t0 + SEC(0.5)), b = rmsOf(t0 + SEC(0.4), t0 + SEC(1.2));
	printf("%d %d %.1f %.1f %d %d %.1f %s\n", ST, IX, a, b, peakOf(0, ON), clipsOf(0, ON), dcOf(t0, t0 + SEC(1.2)), PRESETS[ST][IX].name);
	if (c > 3) writeWav(v[3], 0, ON);
	return 0;
}
