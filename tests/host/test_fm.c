// FM ENGINE: 4 operators on an oscillator's SOFT channel. A lone carrier is a pure sine; a modulator adds harmonics; its
// envelope's decay makes the note darker over time; FM AMOUNT and velocity scale the brightness; feedback adds grit;
// every algorithm renders in range; an FM note always takes the SOFT path (even with LAYERS on HARDWARE); the settings
// save with the patch, a v17 patch loads with ENGINE WAVE; OSC > FM and OPS tabs edit them (OP picks the operator).
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
#define P 64   // the test note's period in samples: 511.375 Hz at 32728
static float harm(const float* x, int n, int k) { float re = 0, im = 0; for (int i = 0; i < n; i++) { re += x[i] * cosf(6.2831853f * k * i / P); im += x[i] * sinf(6.2831853f * k * i / P); } return sqrtf(re * re + im * im) * 2 / n; }
static float bright(const float* x, int n) { float h1 = harm(x, n, 1), hi = 0; for (int k = 2; k <= 12; k++) hi += harm(x, n, k); return hi / (h1 + 1e-6f); }
static SoftCh T;
static void setup(float vel) {
	memset(&T, 0, sizeof T); fmWaveInit(); T.n = 1; T.fm[0] = 1; T.inc[0] = (u32)(4294967296.0 / P); T.gl[0] = T.gr[0] = T.cl[0] = T.cr[0] = 1;
	T.fp = gFm; T.fgate = 1; T.fvel = vel; T.famt = 1; for (int j = 0; j < FM_OPS; j++) { T.fe[j] = 0; T.fst[j] = 1; }
	T.in = SOFT_FADE; T.armed = 1;
}
static float gBuf[SOFT_BLK * 80];
static void render(int blocks) {   // the left channel, as floats
	s16 out[SOFT_BLK * 2];
	for (int b = 0; b < blocks; b++) { softBlockS(&T, out); for (int i = 0; i < SOFT_BLK; i++) gBuf[b * SOFT_BLK + i] = out[i * 2] / 32768.0f; }
}
static float span(int from, int n) { float lo = 0, hi = 0; for (int i = from; i < from + n; i++) { if (gBuf[i] > hi) hi = gBuf[i]; if (gBuf[i] < lo) lo = gBuf[i]; } return hi - lo; }
#define WIN (P * 20)
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		FmP base = FM_DEF; gFm = base;
		// a lone carrier: a sine
		gFm.lvl[1] = 0; setup(1); render(20);
		float b0 = bright(gBuf + SOFT_BLK * 10, WIN), pk0 = span(SOFT_BLK * 10, WIN);
		printf("fm: carrier alone: harmonics/fundamental %.3f, level %.2f\n", b0, pk0);
		if (pk0 < 0.05f) fail("FM: a carrier should sound (%.3f)", pk0);
		if (b0 > 0.02f) fail("FM: a lone carrier should be a sine (%.3f)", b0);
		// a modulator: harmonics, fading with its envelope
		gFm = base; gFm.lvl[1] = 0.8f; gFm.d[1] = 0.25f; gFm.s[1] = 0; gFm.d[0] = 4; setup(1); render(80);
		float bA = bright(gBuf + SOFT_BLK * 2, WIN), bB = bright(gBuf + SOFT_BLK * 70, WIN);
		printf("fm: 2 > 1 brightness %.3f early, %.3f after the modulator's decay\n", bA, bB);
		if (bA < 0.3f) fail("FM: a modulator should add harmonics (%.3f)", bA);
		if (bB > bA * 0.5f) fail("FM: the modulator's decay should darken the note (%.3f -> %.3f)", bA, bB);
		// velocity and FM AMOUNT scale the modulators
		gFm = base; gFm.lvl[1] = 0.6f; gFm.s[1] = 1; gFm.vel = 1;
		setup(1); render(20); float vHi = bright(gBuf + SOFT_BLK * 6, WIN);
		setup(0.2f); render(20); float vLo = bright(gBuf + SOFT_BLK * 6, WIN);
		setup(1); T.famt = 0; render(20); float a0 = bright(gBuf + SOFT_BLK * 6, WIN);
		printf("fm: velocity 1 / 0.2 brightness %.3f / %.3f, FM AMOUNT 0: %.3f\n", vHi, vLo, a0);
		if (vLo > vHi * 0.6f) fail("FM VEL: softer notes should be darker (%.3f vs %.3f)", vLo, vHi);
		if (a0 > 0.02f) fail("FM AMOUNT at 0 should leave the carrier alone (%.3f)", a0);
		// feedback on OP 4 (algorithm 4>3>2>1, all on)
		gFm = base; gFm.lvl[1] = 0.5f; gFm.lvl[2] = 0.5f; gFm.lvl[3] = 0.5f; for (int j = 0; j < 4; j++) gFm.s[j] = 1;
		setup(1); render(20); float f0 = bright(gBuf + SOFT_BLK * 6, WIN);
		gFm.fb = 1; setup(1); render(20); float f1 = bright(gBuf + SOFT_BLK * 6, WIN);
		printf("fm: feedback 0 / 1 brightness %.3f / %.3f\n", f0, f1);
		if (fabsf(f1 - f0) < 0.05f) fail("FEEDBACK should change the sound");
		// every algorithm in range, every wave
		for (int a = 0; a < FM_NALG; a++) for (int w = 0; w < FM_NWAVE; w++) {
			gFm = base; gFm.algo = a; gFm.fb = 0.7f; for (int j = 0; j < 4; j++) { gFm.lvl[j] = 1; gFm.s[j] = 1; gFm.wave[j] = w; gFm.ratio[j] = j * 3; }
			setup(1); render(20); float pk = 0; for (int i = 0; i < SOFT_BLK * 10; i++) if (fabsf(gBuf[i]) > pk) pk = fabsf(gBuf[i]);
			if (pk < 0.02f || pk > 0.995f) fail("FM: algorithm %s / %s out of range (peak %.3f)", FMALG_NAMES[a], FMWAVE_NAMES[w], pk);
		}
		// release: the gate lets the operators go
		gFm = base; gFm.r[0] = 0.05f; setup(1); render(6); T.fgate = 0; render(30);
		if (span(SOFT_BLK * 25, SOFT_BLK * 4) > 0.01f) fail("FM: released, the carrier should fade (%.3f)", span(SOFT_BLK * 25, SOFT_BLK * 4));
		gFm = base;
		// a note on an FM oscillator takes the SOFT path, LAYERS on HARDWARE or not
		{ SPatch p; patchInit(&p); p.oscEngine[0] = 1; patchApply(&p); gSoftMode = 0; voiceOn(60, 0.8f); SVoice* v = NULL;
		  for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].note == 60) v = &gV[i];
		  if (!v || v->role[0] != R_SOFT) fail("FM: the note should be on a SOFT channel (role %d)", v ? v->role[0] : -1);
		  else if (!gSoftC[v->ch[0]].fm[0]) fail("FM: its layer should render FM");
		  voiceOff(60); gSoftMode = 1; }
		// the patch keeps it; v17 loads with WAVE
		gOsc[0].engine = 1; gFm.algo = 5; gFm.ratio[2] = 7; SPatch q; patchCapture(&q); gOsc[0].engine = 0; gFm = base; patchApply(&q);
		if (gOsc[0].engine != 1 || gFm.algo != 5 || gFm.ratio[2] != 7) fail("FM: should save with the patch (%d %d %d)", gOsc[0].engine, gFm.algo, gFm.ratio[2]);
		SPatch old[NUSER]; memset(old, 0, sizeof old); for (int i = 0; i < NUSER; i++) { patchInit(&old[i]); old[i].magic = PATCH_MAGIC17; old[i].oscEngine[0] = 1; old[i].fm.algo = 99; }
		FILE* f = fopen("sd/user_presets.bin", "wb"); for (int i = 0; i < NUSER; i++) fwrite(&old[i], PATCH_V17_SIZE, 1, f); fclose(f);
		memset(gUser, 0, sizeof gUser); userLoad(); remove("sd/user_presets.bin");
		if (gUser[0].magic != PATCH_MAGIC || gUser[0].oscEngine[0] != 0 || gUser[0].fm.algo != 0) fail("FM: a v17 patch should load with ENGINE WAVE (magic %08x engine %d)", gUser[0].magic, gUser[0].oscEngine[0]);
		{ SPatch bad; patchInit(&bad); bad.fm.algo = 40; bad.fm.ratio[0] = -3; bad.fm.lvl[1] = 9; bad.fm.a[2] = NAN; patchApply(&bad);
		  if (gFm.algo != 0 || gFm.ratio[0] != 1 || gFm.lvl[1] > 1 || gFm.a[2] != gFm.a[2]) fail("FM: a bad patch should be kept in range"); }
		// the presets
		const char* NM[4] = {"FM TINE PIANO", "FM BELL", "FM SLAP BASS", "FM BRASS"}; int found = 0;
		for (int st = 0; st < NSTYLES - 1; st++) for (int i = 0; i < MAXPRE && PRESETS[st][i].name; i++) for (int k = 0; k < 4; k++) if (!strcmp(PRESETS[st][i].name, NM[k])) {
			found++; SPatch pp; patchInit(&pp); PRESETS[st][i].fn(&pp); if (pp.oscEngine[0] != 1) fail("PRESET %s should use the FM engine", NM[k]); }
		if (found != 4) fail("FM: 4 FM presets expected (%d)", found);
		// tabs
		gCurPage = 3; gPnlSub[0] = 1; int n; const PnlSub* S = pnlCur(&n);
		if (n != 5 || strcmp(S->tab, "FM")) fail("FM: OSC should have an FM tab (%d %s)", n, S->tab);
		gPnlSub[0] = 2; S = pnlCur(&n); if (strcmp(S->tab, "ENV")) fail("FM: OSC should have an ENV tab"); if (pnlFaders(S) != 16) fail("ENV: 16 sliders, two rows (%d)", pnlFaders(S));
		{ int cx, top, bot, w; pnlGeom(16, 13, &cx, &top, &bot, &w); if (pnlAt(16, cx, top + 5) != 13) fail("two rows: a touch on slider 13 should find it (%d)", pnlAt(16, cx, top + 5)); }
		gFmSel = 2; Param* pl = pnlFind("OP LEVEL", 3); if (!pl || pl->p != &gFm.lvl[2]) fail("OPS: OP LEVEL should edit operator 3");
		gFmSel = 0; pl = pnlFind("OP RATIO", 3); if (!pl || pl->p != &gFm.ratio[0]) fail("OPS: OP RATIO should follow OP");
		if (!pnlFind("ENGINE A", 3) || !pnlFind("ALGO", 3) || !pnlFind("OP4 REL", 3)) fail("FM: settings missing");
		{ SPatch p; patchInit(&p); k_fmtine(&p); patchApply(&p); }
		gPnlSub[0] = 1; gPageWant = 3;
	}
		if (fr == 10) shotBot("shots/fm_tab.ppm");
	if (fr == 12) { gPnlSub[0] = 2; gCurPage = 1; gPageWant = 3; }
	if (fr == 16) shotBot("shots/fm_env.ppm");
	// the FM presets through the whole engine, next to a wavetable keys patch: a level in the same range, no clipping
	static void (*const PF_[5])(SPatch*) = {k_epsat, k_fmtine, u_fmbell, b_fmslap, l_fmbrass}; static const int NT[5] = {60, 60, 72, 36, 60};
	static int a0; static double rm[5];
	if (fr >= 20 && fr < 20 + 5 * 50) { int i = (fr - 20) / 50, k = (fr - 20) % 50;
		if (k == 0) { SPatch p; patchInit(&p); PF_[i](&p); patchApply(&p); }
		if (k == 4) { a0 = ON; voiceOn(NT[i], 0.9f); }
		if (k == 30) { rm[i] = rmsOf(a0, ON); int pk = peakOf(a0, ON), cl = clipsOf(a0, ON); printf("fm: preset %d rms %.0f peak %d clips %d\n", i, rm[i], pk, cl);
			if (cl > 0) fail("FM preset %d clips", i); if (i && rm[i] < 300) fail("FM preset %d too quiet (%.0f)", i, rm[i]); voiceOff(NT[i]); } }
	if (fr == 20 + 5 * 50) { writeWav("/tmp/fm_presets.wav", 0, ON); done = 1; }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 20 + 5 * 50 + 2; app_main(); if (!done) fail("didn't run"); printf("fm: the FM engine\n"); return gFails ? 1 : 0; }
