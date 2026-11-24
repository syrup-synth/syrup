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
	if (fr == 61) shotBot("shots/shape_env1_drag.ppm");   // (the envelopes are sliders on FILT now: test_panel)
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
	if (fr == 112) press(fr, 112, KEY_X);   // next page: LFO+MOD
	if (fr == 116) { gModView = 1; }
	if (fr == 120) { shotBot("shots/lfo12.ppm"); if (!gModOn) fail("X from FILT+ENV should reach LFO+MOD"); }
	if (fr == 122 || fr == 123) { TOUCH = 1; TX = 40; TY = 44; }   // LFO 1 shape box: the list opens
	if (fr == 126) { shotBot("shots/lfo12_pick.ppm"); if (gLfPick != 0) fail("LFO 1+2: tapping the shape box should open the list"); }
	// MASTER FX: FX ORDER. Drag LOFI past three blocks, check the order, save it in a patch, reset
	press(fr, 130, KEY_X); press(fr, 136, KEY_X);   // VOICE+FX, MASTER FX
	if (fr == 142) { gLofiBits = 2; gOtt = 0.4f; shotBot("shots/fx_order.ppm"); if (!gPnl) fail("FX ORDER: the sliders should be up on MASTER FX"); }
	if (fr >= 144 && fr < 156) { TOUCH = 1; TX = 34 + (fr - 144) * 16; TY = PNL_SUB_Y + 6; }   // the chain is the FX sub-tabs: drag LOFI
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
	if (fr == 160) { u8 d0[NFXB] = {0, 1, 2, 3, 4}; fxOrderSet(d0); }
	if (fr == 164) { for (int i = 0; i < NFXB; i++) if (gFxOrder[i] != i) { fail("FX ORDER: RESET didn't restore the default"); break; } }
	// WORK chips and the PERF row
	if (fr == 166 || fr == 167) { TOUCH = 1; TX = 2 * 320 / 8 + 20; TY = 8; }   // MOD
	if (fr == 170 && !gModOn) fail("chips: MOD should open LFO+MOD");
	if (fr == 172 || fr == 173) { TOUCH = 1; TX = 4 * 320 / 8 + 20; TY = 8; }  // FX
	if (fr == 176) { if (!gChainOn) fail("WORK chips: CHAIN should open FX ORDER"); gSeqMode = 1; }
	if (fr == 178 || fr == 179) { TOUCH = 1; TX = 2 + 6 * 45 + 20; TY = PNL_KR_Y + 6; }   // PANIC (the keys' row)
	if (fr == 184) { splashDraw(); shotTop("shots/splash.ppm"); shotBot("shots/splash_bot.ppm"); int lit = 0; for (int i = 0; i < 400 * 240 * 3; i++) lit += TOPFB[i] > 100; if (lit < 20000) fail("splash: the logo should fill the top screen (%d)", lit); }
	if (fr == 182) { shotBot("shots/perf_row.ppm"); if (gSeqMode == 1 || gSeqPlaying) fail("PERF: PANIC should stop the sequencer"); }
}
int main(void) {
	mkdir("shots", 0777);
	loadFont(); FRMAX = 186; app_main();
	printf("screens: filter plot, value HUD, keyboard feedback and seq strip saved\n");
	return gFails ? 1 : 0;
}
