// A1-A5 screens: the filter response plot, the value HUD (bipolar and plain), velocity-lit keys with trails,
// chord name, latch frame and the sequencer strip. Saves shots for a look; checks a few pixels changed.
#include "syrup_host.c"
#include "harness.h"
static void press(int fr, int at, u32 k) { if (fr == at || fr == at + 1) HD = k; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 3) { int st, ix; if (findPreset("REESE CRATE", &st, &ix)) presetLoad(st, ix); gCutoff = 0.55f; gRes = 0.6f; }
	if (fr == 6) midiPush(0x90, 45, 100);
	for (int k = 0; k < 4; k++) press(fr, 10 + k * 6, KEY_X);   // to FILT+ENV
	press(fr, 40, KEY_DDOWN);          // CUTOFF row
	press(fr, 46, KEY_DLEFT);          // change it: the HUD shows the value and a bar
	if (fr == 50) shotTop("shots/filter_plot_hud.ppm");
	if (fr == 70) shotTop("shots/filter_plot.ppm");
	if (fr == 72) midiPush(0x80, 45, 0);
	// bottom screen: chord mode, latch, a velocity-lit key, then slide to leave a trail; sequencer playing
	if (fr == 80) { gChord = 1; seqDefault(); gSeqMode = 1; }
	press(fr, 82, KEY_B);   // latch
	if (fr >= 90 && fr < 100) { TOUCH = 1; TX = 60; TY = 230; }        // low on the key: loud
	if (fr >= 100 && fr < 108) { TOUCH = 1; TX = 60 + (fr - 100) * 12; TY = 230; }   // slide right
	if (fr == 108) shotBot("shots/keys_chord_latch_seq.ppm");
}
int main(void) {
	mkdir("shots", 0777);
	loadFont(); FRMAX = 115; app_main();
	printf("screens: filter plot, value HUD, keyboard feedback and seq strip saved\n");
	return gFails ? 1 : 0;
}
