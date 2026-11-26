// OSC DIST: each oscillator's own distortion (the DRIVE types), baked into its wavetable before band-limiting.
// Hard clip on a sine adds odd harmonics to OSC A only; it saves with the patch and comes back; a v16 patch
// loads with DIST off; OSC > DIST tab shows both oscillators' settings.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static float harm(const float* c, int k) { float re = 0, im = 0; for (int n = 0; n < CYC; n++) { re += c[n] * cosf(6.2831853f * k * n / CYC); im += c[n] * sinf(6.2831853f * k * n / CYC); } return sqrtf(re * re + im * im) / CYC; }
static void rebuild(void) { RebuildJob j; memset(&j, 0, sizeof j); j.chg[0] = j.chg[1] = 1; j.used[0] = j.used[1] = 0x7F; j.pe[0] = gOsc[0].pos; j.pe[1] = gOsc[1].pos; j.we[0] = gOsc[0].warpAmt; j.we[1] = gOsc[1].warpAmt; LightLock_Lock(&gMipsLock); doRebuild(&j); LightLock_Unlock(&gMipsLock); }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		SPatch p; patchInit(&p); p.osc[0].on = p.osc[1].on = 1; p.osc[0].table = p.osc[1].table = T_BASIC; p.osc[0].pos = p.osc[1].pos = 0; p.osc[0].warp = p.osc[1].warp = 0;
		patchApply(&p); gDrive = 0; rebuild();
		float h3a = harm(gOsc[0].cyc, 3), h1 = harm(gOsc[0].cyc, 1);
		gOsc[0].dist = 2; gOsc[0].distAmt = 1.0f; rebuild();   // HARD CLIP on OSC A
		float h3b = harm(gOsc[0].cyc, 3), h3B = harm(gOsc[1].cyc, 3), pk = 0; for (int n = 0; n < CYC; n++) if (fabsf(gOsc[0].cyc[n]) > pk) pk = fabsf(gOsc[0].cyc[n]);
		printf("oscdist: OSC A 3rd harmonic %.4f -> %.4f (fundamental %.3f), OSC B 3rd %.4f, peak %.2f\n", h3a, h3b, h1, h3B, pk);
		if (h3b < h3a + 0.05f) fail("DIST: hard clip should add a 3rd harmonic to OSC A");
		if (h3B > 0.01f) fail("DIST: OSC B should stay clean");
		if (pk > 1.01f) fail("DIST: the cycle should stay in range (%.2f)", pk);
		SPatch q; patchCapture(&q); gOsc[0].dist = 0; gOsc[0].distAmt = 0; patchApply(&q);
		if (gOsc[0].dist != 2 || gOsc[0].distAmt != 1.0f || gOsc[1].dist != 0) fail("DIST: should save with the patch (%d %.2f)", gOsc[0].dist, gOsc[0].distAmt);
		// a v16 user-patch file: DIST off
		SPatch old[NUSER]; memset(old, 0, sizeof old); for (int i = 0; i < NUSER; i++) { patchInit(&old[i]); old[i].magic = PATCH_MAGIC16; old[i].oscDist[0] = 9; }
		FILE* f = fopen("sd/user_presets.bin", "wb"); for (int i = 0; i < NUSER; i++) fwrite(&old[i], PATCH_V16_SIZE, 1, f); fclose(f);
		memset(gUser, 0, sizeof gUser); userLoad();
		if (gUser[0].magic != PATCH_MAGIC || gUser[0].oscDist[0] != 0) fail("DIST: a v16 patch should load with DIST off (magic %08x dist %d)", gUser[0].magic, gUser[0].oscDist[0]);
		remove("sd/user_presets.bin");
		gCurPage = 3; gPnlSub[0] = 1; int n; const PnlSub* S = pnlCur(&n);
		if (n != 6 || strcmp(S->tab, "DIST")) fail("DIST: OSC should have a DIST tab (%d %s)", n, S->tab);
		if (!pnlFind("DIST A AMT", 3) || !pnlFind("DIST B", 3)) fail("DIST: the tab's settings should be there");
		gPageWant = 3;
	}
	if (fr == 8) { gPnlSub[0] = 1; }
	if (fr == 12) { shotBot("shots/osc_dist.ppm"); done = 1; }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 14; app_main(); if (!done) fail("didn't run"); printf("oscdist: per-oscillator distortion\n"); return gFails ? 1 : 0; }
