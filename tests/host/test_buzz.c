// Regression test for the frame-rate buzz (a 204.5 Hz tone = one DSP frame of 160 samples) and for the core-2
// master-FX hand-over: a plain sine through MASTER FX, rendered with the chain on core 1 and on core 2.
#include "syrup_host.c"
#include "harness.h"
static int USE_CORE2 = 0, ON_CORE2 = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 2) {
		SPatch p; patchInit(&p); p.osc[0].pos = 0.0f; p.revMix = 0.2f; p.dlyMix = 0.15f; patchApply(&p);
		gMasterFX = 1; gFxCore2 = USE_CORE2;
	}
	if (fr == 10) midiPush(0x90, 57, 100);   // A3, 220 Hz
	if (fr == FRMAX - 2) ON_CORE2 = gFxCore2 && !gFxC2Fail && gFxThread && !(gCore2Lim && gCore2Lim < 50);   // before shutdown stops it
}
int main(int c, char** v) {
	USE_CORE2 = c > 1 && !strcmp(v[1], "core2");
	if (USE_CORE2) FAKE_CPU_LIMIT = 80;
	loadFont(); FRMAX = 10 + 180; app_main();
	int a = SEC(1.0), b = SEC(3.0);
	double r = rmsOf(a, b), worst = -999; double fr = 32728.0 / 160;
	for (int k = 1; k <= 4; k++) { double d = toneDb(a, b, fr * k); if (d > worst) worst = d; }
	printf("%s rms %.1f frame-rate tone %.1f dB  fx %s\n", USE_CORE2 ? "core2" : "core1", r, worst, ON_CORE2 ? "ON CORE 2" : "CORE 1");
	if (c > 2) writeWav(v[2], 0, ON);
	if (r < 300) fail("buzz test: no sound (rms %.1f)", r);
	if (worst > -40) fail("buzz test: frame-rate tone at %.1f dB (limit -40; the old buzz was far above this)", worst);
	if (USE_CORE2 && !ON_CORE2) fail("core-2 FX fell back to core 1 in the simulator");
	return gFails ? 1 : 0;
}
