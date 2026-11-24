// ENV 3 / 4 and drawn-LFO TRIG: ENV 3 moves a note's cutoff and lets go with it; a drawn LFO 3 on ONCE starts at its first
// point for every note and holds its last; a v15 user patch still loads (ENV 3 / 4 get defaults); patches keep the new settings
#include "syrup_host.c"
#include "harness.h"
static int done = 0; static float e3peak = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 5) { gEnv3 = (AHDSR){0.05f, 0, 0.2f, 0.0f, 0.1f}; memset(gMod, 0, sizeof(gMod)); gMod[0] = (ModSlot){SRC_ENV3, DST_CUTOFF, 0.8f}; voiceOn(60, 0.9f); }
	if (fr > 5 && fr < 30) for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].note == 60 && gV[i].env3 > e3peak) e3peak = gV[i].env3;
	if (fr == 30) { if (e3peak < 0.9f) fail("ENV3: should reach its peak (%.2f)", e3peak); int ok = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].note == 60) ok = gV[i].env3 < 0.05f; if (!ok) fail("ENV3: sustain 0 should fall back"); voiceOff(60); }
	if (fr == 40) {   // ONCE: a ramp 1 -> -1 painted, slow; each note starts at the top
		for (int i = 0; i < 64; i++) gDl[0][i] = 1 - i / 31.5f; gDlTrig[0] = 2; gDlSync[0] = 0; gDlRate[0] = 2.0f; gDlPh[0] = 0.5f;
		memset(gMod, 0, sizeof(gMod)); gMod[0] = (ModSlot){SRC_LFO3, DST_CUTOFF, 0.5f}; voiceOn(64, 0.9f);
	}
	if (fr == 42) { SVoice* v = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].note == 64) v = &gV[i]; if (!v) fail("ONCE: no voice"); else if (srcValue(SRC_LFO3, v) < 0.7f) fail("ONCE: a new note should start near the first point (%.2f)", srcValue(SRC_LFO3, v)); }
	if (fr == 90) { SVoice* v = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].note == 64) v = &gV[i]; if (v && srcValue(SRC_LFO3, v) > -0.9f) fail("ONCE: after the shape it should hold the last point (%.2f)", srcValue(SRC_LFO3, v)); voiceOff(64); }
	if (fr == 100) {   // the patch keeps them
		SPatch p; patchCapture(&p); if (p.env3.a != gEnv3.a || p.dlTrig[0] != 2) fail("PATCH: ENV 3 / TRIG should be saved");
		gEnv3.a = 1.5f; gDlTrig[0] = 0; patchApply(&p); if (gEnv3.a != 0.05f || gDlTrig[0] != 2) fail("PATCH: ENV 3 / TRIG should load back (%.2f %d)", gEnv3.a, gDlTrig[0]);
	}
	if (fr == 110) {   // a v15 user file: every slot the old size
		SPatch t; patchInit(&t); t.magic = PATCH_MAGIC15; snprintf(t.name, sizeof(t.name), "OLD ONE"); FILE* f = fopen(USER_PATH, "wb");
		for (int i = 0; i < NUSER; i++) fwrite(&t, PATCH_V15_SIZE, 1, f); fclose(f);
		memset(gUser, 0, sizeof(gUser)); userLoad();
		if (gUser[0].magic != PATCH_MAGIC || strcmp(gUser[0].name, "OLD ONE")) fail("V15: an old user file should still load");
		else if (gUser[0].env4.r < 0.1f) fail("V15: ENV 4 should get its default");
		remove(USER_PATH); done = 1;
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 114; app_main(); if (!done) fail("didn't run"); printf("env34: ENV 3 / 4, drawn LFO ONCE, v15 patches\n"); return gFails ? 1 : 0; }
