// FILTER SCREAM stress: every scream patch pushed hard (scream + res maxed, fast notes, bends, cutoff sweeps, voice
// stealing, master FX on/off, patch changes while ringing). The engine must keep running, stay finite, and go quiet
// after PANIC. Reported: SCREAM froze the sound engine until power-off.
#include "syrup_host.c"
#include "harness.h"
#include <time.h>
static const char* SP[] = {"SCREAM LEAD", "SCREAM SOLO", "ACID SCREAM", "SUB SCREAM", "SCREAM DUB", "GLITCH STORM", "303 SCREAM", "FOLD SCREAM"};
#define NSP (int)(sizeof SP / sizeof *SP)
#define LEN 300
static unsigned rs = 12345; static int rnd(int n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }
static int onStart, held[4];
static double worst = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; CX = 0; CY = 0;
	static struct timespec t0; struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
	if (fr > 3) { double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9; if (dt > worst) worst = dt; }
	t0 = t1;
	int k = fr / LEN, f = fr % LEN; if (k >= NSP) return;
	if (f == 2) { int st, ix; if (!findPreset(SP[k], &st, &ix)) { fail("no preset %s", SP[k]); return; } presetLoad(st, ix); gQScream = 1; gRes = 1; gDrums = 1; drumsChanged(); }
	if (f >= 6 && f < 220) {
		if (f % 3 == 0) { int n = 24 + rnd(72); int s = rnd(4); if (held[s]) midiPush(0x80, held[s], 0); held[s] = n; midiPush(0x90, n, 30 + rnd(97)); }
		CX = (short)(rnd(311) - 155); CY = (short)(rnd(311) - 155);
		gCutoff = (float)rnd(1000) / 1000.0f; if (f % 40 == 0) gFxCore2 = rnd(2);
		if (f == 120) { int st, ix; findPreset(SP[(k + 1) % NSP], &st, &ix); presetLoad(st, ix); gQScream = 1; gRes = 1; }   // change patch while it rings
		if (f == 160) { int st, ix; findPreset(SP[k], &st, &ix); presetLoad(st, ix); gQScream = 1; gRes = 1; }
	}
	if (f == 220) { for (int s = 0; s < 4; s++) if (held[s]) { midiPush(0x80, held[s], 0); held[s] = 0; } onStart = ON; }
	if (f == 222) panicRest();
	if (f == 230 && ON - onStart < SEC(0.1)) fail("%s: audio clock stopped (%d samples in 10 frames)", SP[k], ON - onStart);
	if (f == 290) {
		for (int i = ON - SEC(1.0); i < ON; i++) if (OUT[2 * i] != OUT[2 * i] || OUT[2 * i + 1] != OUT[2 * i + 1]) { fail("%s: NaN in the output", SP[k]); break; }
		double r = rmsOf(ON - SEC(0.4), ON), r0 = rmsOf(onStart + SEC(0.2), onStart + SEC(0.6));
		for (int c = 0; c < 24; c++) if (CH[c].wb && c != NOISE_CH) { fail("%s: DSP channel %d still playing after PANIC", SP[k], c); break; }
		if (r > 300 || (r > 20 && r > r0 * 0.7)) fail("%s: not dying away after PANIC (%.0f -> %.0f rms)", SP[k], r0, r);
		printf("  %-13s after panic %.1f rms\n", SP[k], r);
	}
}
int main(void) {
	mkdir("sd", 0777); loadFont(); FRMAX = LEN * NSP + 2; app_main();
	if (FR < FRMAX - 1) fail("engine stopped at frame %d", FR);
	if (worst > 0.5) fail("a frame took %.2f s (the engine stalled)", worst);
	printf("scream: %d patches stressed, slowest frame %.0f ms\n", NSP, worst * 1000);
	return gFails ? 1 : 0;
}
