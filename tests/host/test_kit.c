// DRUM KIT patches: every one of the 10 drums must sound on every kit, from MIDI notes (General MIDI) and from the
// on-screen pads; saves the pads screen and a WAV of each kit.
#include "syrup_host.c"
#include "harness.h"
static const int GM[10] = {36, 38, 42, 46, 39, 37, 41, 48, 56, 49};
static int KIT = 0, padHit = 0;
#define T0 20
#define GAP 24   // frames between hits (0.4 s)
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 2) { int st, ix; char nm[16]; static const char* K[4] = {"808 KIT", "909 KIT", "LOFI KIT", "CHIP KIT"}; snprintf(nm, sizeof nm, "%s", K[KIT]); if (findPreset(nm, &st, &ix)) presetLoad(st, ix); else fail("kit: no preset %s", nm); }
	for (int d = 0; d < 10; d++) {
		if (fr == T0 + d * GAP) midiPush(0x90, GM[d], 110);
		if (fr == T0 + d * GAP + 6) midiPush(0x80, GM[d], 0);
	}
	int tp = T0 + 10 * GAP + 10;   // tap the bottom-left pad (KICK) and check the screen
	if (fr == tp || fr == tp + 1 || fr == tp + 2) { TOUCH = 1; TX = 30; TY = KB_Y + 61 + 40; }
	if (fr == tp + 2) { char n[64]; snprintf(n, sizeof n, "shots/kit%d_pads.ppm", KIT); shotBot(n); padHit = 1; }
}
int main(int c, char** v) {
	KIT = c > 1 ? atoi(v[1]) : 0;
	mkdir("shots", 0777); mkdir("wav", 0777);
	loadFont(); FRMAX = T0 + 10 * GAP + 40; app_main();
	static const char* NM[10] = {"KICK", "SNARE", "CL HAT", "OP HAT", "CLAP", "RIM", "LO TOM", "HI TOM", "BELL", "CRASH"};
	printf("kit %d:", KIT);
	for (int d = 0; d < 10; d++) {
		int a = SEC((T0 + d * GAP) / 60.0), b = a + SEC(0.15);
		double r = rmsOf(a, b); printf(" %s %.0f", NM[d], r);
		if (r < 150) fail("kit %d: %s is silent (rms %.0f)", KIT, NM[d], r);
	}
	int a = SEC((T0 + 10 * GAP + 10) / 60.0); double rp = rmsOf(a, a + SEC(0.2));
	printf(" | pad %.0f\n", rp);
	if (rp < 150) fail("kit %d: tapping the KICK pad made no sound", KIT);
	if (clipsOf(0, ON) > 50) fail("kit %d: %d clipped samples", KIT, clipsOf(0, ON));
	char n[64]; snprintf(n, sizeof n, "wav/kit%d.wav", KIT); writeWav(n, 0, ON);
	return gFails ? 1 : 0;
}
