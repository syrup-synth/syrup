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
	if (fr == 52) shotBot("shots/shape_env1.ppm");
	if (fr >= 54 && fr < 62) { TOUCH = 1; TX = 20 + (fr - 54) * 6; TY = SH_Y + 6; }   // drag the attack handle right
	if (fr == 61) { shotBot("shots/shape_env1_drag.ppm"); if (gEnv1.a < 0.05f) fail("shape: dragging the attack handle didn't lengthen the attack (%.3f)", gEnv1.a); }
	if (fr == 64 || fr == 65) { TOUCH = 1; TX = 4 + 2 * 105 + 50; TY = TAB_H + 8; }   // FILTER tab
	if (fr >= 67 && fr < 69) { TOUCH = 1; TX = 200; TY = SH_Y + 10; }   // drag the dot: high cutoff, high resonance
	if (fr == 69) { shotBot("shots/shape_filter.ppm"); if (gCutoff < 0.5f || gRes < 0.6f) fail("shape: the filter dot didn't move cutoff/res (%.2f %.2f)", gCutoff, gRes); }
	if (fr == 72) midiPush(0x80, 45, 0);
	// bottom screen: chord mode, latch, a velocity-lit key, then slide to leave a trail; sequencer playing
	if (fr == 80) { gChord = 1; seqDefault(); gSeqMode = 1; }
	press(fr, 82, KEY_B);   // latch
	if (fr >= 90 && fr < 100) { TOUCH = 1; TX = 60; TY = 230; }        // low on the key: loud
	if (fr >= 100 && fr < 108) { TOUCH = 1; TX = 60 + (fr - 100) * 12; TY = 230; }   // slide right
	if (fr == 108) shotBot("shots/keys_chord_latch_seq.ppm");
	if (fr == 112) press(fr, 112, KEY_R);   // next page: LFO+MOD
	if (fr == 116) { gModView = 1; }
	if (fr == 120) { shotBot("shots/lfo12.ppm"); if (!gModOn) fail("L/R: R from FILT+ENV should reach LFO+MOD"); }
	if (fr == 122 || fr == 123) { TOUCH = 1; TX = 40; TY = 44; }   // LFO 1 shape box: the list opens
	if (fr == 126) { shotBot("shots/lfo12_pick.ppm"); if (gLfPick != 0) fail("LFO 1+2: tapping the shape box should open the list"); }
	// MASTER FX: FX ORDER. Drag LOFI past three blocks, check the order, save it in a patch, reset
	press(fr, 130, KEY_R); press(fr, 136, KEY_R);   // VOICE+FX, MASTER FX
	if (fr == 142) { gLofiBits = 2; gOtt = 0.4f; shotBot("shots/fx_order.ppm"); if (!gChainOn) fail("FX ORDER: the view should be up on MASTER FX"); }
	if (fr >= 144 && fr < 156) { TOUCH = 1; TX = CH_X(0) + 20 + (fr - 144) * 14; TY = CH_Y + 10; }
	if (fr == 150) shotBot("shots/fx_order_drag.ppm");
	if (fr == 158) {
		static const int want[NFXB] = {1, 2, 3, 0, 4};
		for (int i = 0; i < NFXB; i++) if (gFxOrder[i] != want[i]) { fail("FX ORDER: dragging LOFI to slot 4 gave %d %d %d %d %d", gFxOrder[0], gFxOrder[1], gFxOrder[2], gFxOrder[3], gFxOrder[4]); break; }
		shotBot("shots/fx_order_moved.ppm");
		SPatch p; patchCapture(&p); u8 d[NFXB] = {0, 1, 2, 3, 4}; fxOrderSet(d); patchApply(&p);
		if (gFxOrder[3] != 0) fail("FX ORDER: the order didn't come back with the patch");
		u8 bad[NFXB] = {0, 0, 1, 2, 3}; fxOrderSet(bad); if (gFxOrder[1] != 1) fail("FX ORDER: a broken order should fall back to default");
		fxOrderSet(p.fxOrder);
	}
	if (fr == 160 || fr == 161) { TOUCH = 1; TX = 280; TY = TAB_H + 26; }   // RESET
	if (fr == 164) { for (int i = 0; i < NFXB; i++) if (gFxOrder[i] != i) { fail("FX ORDER: RESET didn't restore the default"); break; } }
}
int main(void) {
	mkdir("shots", 0777);
	loadFont(); FRMAX = 170; app_main();
	printf("screens: filter plot, value HUD, keyboard feedback and seq strip saved\n");
	return gFails ? 1 : 0;
}
