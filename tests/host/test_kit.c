// DRUM KIT patches: every one of the 10 drums must sound on every kit, from MIDI notes (General MIDI) and from the
// on-screen pads; saves the pads screen and a WAV of each kit.
#include "syrup_host.c"
#include "harness.h"
static const int GMD[10] = {36, 38, 42, 46, 39, 37, 41, 48, 56, 49}, GMP[10] = {82, 70, 69, 80, 73, 75, 64, 63, 60, 76};
static const int* GM = GMD;
static int KIT = 0, padHit = 0;
#define T0 20
#define GAP 24   // frames between hits (0.4 s)
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 2) { int st, ix; char nm[16]; static const char* K[6] = {"808 KIT", "909 KIT", "LOFI KIT", "CHIP KIT", "PERC KIT", "WAVE KIT"}; snprintf(nm, sizeof nm, "%s", K[KIT]); if (findPreset(nm, &st, &ix)) presetLoad(st, ix); else fail("kit: no preset %s", nm); }
	for (int d = 0; d < 10; d++) {
		if (fr == T0 + d * GAP) midiPush(0x90, GM[d], 110);
		if (fr == T0 + d * GAP + 6) midiPush(0x80, GM[d], 0);
	}
	int tp = T0 + 10 * GAP + 10;   // tap the bottom-left pad (KICK) and check the screen
	if (KIT == 0 && fr == tp + 45) { gKickT = 1u << 30; drumHit(0, 1); if (gKickT != 0) fail("sidechain: a kick should restart the KICK pump"); }
	if (KIT == 0 && fr == tp + 47 && (gKickT < 1000 || gKickT > 8000)) fail("sidechain: the kick timer should run with the audio (%u)", (unsigned)gKickT);
	if (KIT == 0 && fr == tp + 50) {   // DRUM POLY: a second kick rings over the first; OFF cuts it; open hat still chokes
		int on0 = 0, on1 = 0, on2 = 0;
		gDrumPoly = 0; drumHit(0, 1); drumHit(0, 1); csndExecCmds(false); for (int i = 0; i < 32; i++) on0 += CS[i].on;
		gDrumPoly = 1; drumHit(0, 1); drumHit(0, 1); csndExecCmds(false); for (int i = 0; i < 32; i++) on1 += CS[i].on;
		if (on1 != on0 + 1) fail("drum poly: the second kick should ring on its own channel (%d -> %d)", on0, on1);
		drumHit(0, 1); csndExecCmds(false); for (int i = 0; i < 32; i++) on2 += CS[i].on;
		if (on2 != on1) fail("drum poly: a third kick should take turns, not add a channel (%d -> %d)", on1, on2);
		int hb = 0; drumHit(2, 1); drumHit(2, 1); drumHit(3, 1); csndExecCmds(false); for (int i = 0; i < 32; i++) hb += CS[i].on;
		if (hb != on2 + 1) fail("drum poly: open hat should choke both closed-hat channels (%d vs %d)", hb, on2 + 1);
	}
	if (fr == tp || fr == tp + 1 || fr == tp + 2) { TOUCH = 1; TX = 30; TY = KB_Y + 61 + 40; }
	if (fr == tp + 2) { char n[64]; snprintf(n, sizeof n, "shots/kit%d_pads.ppm", KIT); shotBot(n); padHit = 1; }
	if (KIT == 4 && fr >= tp + 20 && fr < tp + 32) TILTX = (fr & 1) ? 1.5f : -1.5f;   // shake it
	if (KIT == 4 && fr == tp + 32) TILTX = 0;
	{ static long sum0; if (KIT == 5 && fr == tp + 40) { sum0 = 0; for (int i = 0; i < 4000; i++) sum0 += abs(gDrumSmp[0][i]) * (i % 7 + 1); gOsc[0].pos = gOsc[0].pos < 0.5f ? 0.9f : 0.05f; }
	  if (KIT == 5 && fr == tp + 75) { long s1 = 0; for (int i = 0; i < 4000; i++) s1 += abs(gDrumSmp[0][i]) * (i % 7 + 1); if (s1 == sum0) fail("wave kit: moving WT POS should rebuild the drums from the new frames"); } }
}
int main(int c, char** v) {
	KIT = c > 1 ? atoi(v[1]) : 0; if (KIT == 4) GM = GMP;
	mkdir("shots", 0777); mkdir("wav", 0777);
	loadFont(); FRMAX = T0 + 10 * GAP + 90; app_main();
	static const char* NMD[10] = {"KICK", "SNARE", "CL HAT", "OP HAT", "CLAP", "RIM", "LO TOM", "HI TOM", "BELL", "CRASH"};
	static const char* NMP[10] = {"SHAKER", "MARACA", "CABASA", "TAMB", "GUIRO", "CLAVES", "CONGA L", "CONGA H", "BONGO", "WOOD"};
	const char** NM = KIT == 4 ? NMP : NMD;
	printf("kit %d:", KIT);
	for (int d = 0; d < 10; d++) {
		int a = SEC((T0 + d * GAP) / 60.0), b = a + SEC(0.15);
		double r = rmsOf(a, b); printf(" %s %.0f", NM[d], r);
		if (r < 150) fail("kit %d: %s is silent (rms %.0f)", KIT, NM[d], r);
	}
	int a = SEC((T0 + 10 * GAP + 10) / 60.0); double rp = rmsOf(a, a + SEC(0.2));
	printf(" | pad %.0f\n", rp);
	if (rp < 150) fail("kit %d: tapping the bottom-left pad made no sound", KIT);
	if (KIT == 4) { int s0 = SEC((T0 + 10 * GAP + 10 + 20) / 60.0); double rs = rmsOf(s0, s0 + SEC(0.25)); printf("shake: %.0f\n", rs); if (rs < 150) fail("perc kit: shaking the console played nothing"); }
	if (clipsOf(0, ON) > 50) fail("kit %d: %d clipped samples", KIT, clipsOf(0, ON));
	char n[64]; snprintf(n, sizeof n, "wav/kit%d.wav", KIT); writeWav(n, 0, ON);
	return gFails ? 1 : 0;
}
