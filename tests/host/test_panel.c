// PANEL: OSC / FILT / FX show their settings as sliders over a small keyboard. A drag sets a slider (through the same path
// as the D-pad), a choice steps with a tap, the sub-pages switch, the small keys play
#include "syrup_host.c"
#include "harness.h"
static int done = 0; static float lv0;
#define CHIP(i) do { TOUCH = 1; TX = (i) * 320 / 8 + 20; TY = 8; } while (0)
static int fx(int i, int n) { int fw = 312 / n; if (fw > 52) fw = 52; return 160 - fw * n / 2 + i * fw + fw / 2; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 5) CHIP(0);
	if (fr == 9) { if (!gPnl || gCurPage != 1) fail("PANEL: OSC should show the sliders (%d %d)", gPnl, gCurPage); shotBot("shots/pnl_osc.ppm"); shotTop("shots/pnl_osc_top.ppm"); lv0 = gOsc[0].level; }
	if (fr >= 10 && fr < 16) { TOUCH = 1; TX = fx(2, 8); TY = PNL_TB - (fr - 10) * 4; }   // LEVEL: drag from the bottom up a bit
	if (fr == 18) { if (gOsc[0].level > 0.4f || gOsc[0].level == lv0) fail("PANEL: dragging LEVEL low should set it (%.2f)", gOsc[0].level); }
	if (fr == 20) { TOUCH = 1; TX = 8 + 308 / 3 + 308 / 3 - 10; TY = PNL_CH_Y + 6; }   // TABLE: the right of it = next
	if (fr == 19) lv0 = gOsc[0].table;
	if (fr == 24) { if (gOsc[0].table == (int)lv0) fail("PANEL: a tap on TABLE's right should step it (%d)", gOsc[0].table); }
	if (fr == 26) { TOUCH = 1; TX = 4 + 312 * 2 / 3 + 30; TY = PNL_SUB_Y + 6; }   // SUB+NOISE
	if (fr == 30) { if (gCurPage != 3) fail("PANEL: SUB+NOISE should be page 3 (%d)", gCurPage); shotBot("shots/pnl_sub.ppm"); }
	if (fr >= 32 && fr <= 35) { TOUCH = 1; TX = 100; TY = 236; }   // a small key, held
	if (fr == 34) { int any = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].stage != 4) any = 1; if (!any) fail("PANEL: the small keys should play"); }
	if (fr == 40) CHIP(1);
	if (fr == 44) { if (gCurPage != 4 || !gPnl) fail("PANEL: FILT"); shotBot("shots/pnl_filt.ppm"); shotTop("shots/pnl_filt_top.ppm"); }
	if (fr == 46) { TOUCH = 1; TX = 4 + 312 / 4 + 30; TY = PNL_SUB_Y + 6; }   // ENV 1
	if (fr >= 50 && fr < 54) { TOUCH = 1; TX = fx(3, 5); TY = PNL_TT + 2; }   // SUS: top
	if (fr == 56) { if (gEnv1.s < 0.95f) fail("PANEL: ENV1 SUS to the top (%.2f)", gEnv1.s); shotBot("shots/pnl_env1.ppm"); }
	if (fr == 58) CHIP(4);
	if (fr == 62) { if (gCurPage != 10 || !gPnl) fail("PANEL: FX (%d)", gCurPage); shotBot("shots/pnl_fx.ppm"); shotTop("shots/pnl_fx_top.ppm"); }
	// FX: the sub-tabs are the chain - drag LOFI (first) to the end
	if (fr == 64) { u8 o[5] = {0, 1, 2, 3, 4}; fxOrderSet(o); }
	if (fr >= 66 && fr < 72) { TOUCH = 1; TX = 20 + (fr - 66) * 56; TY = PNL_SUB_Y + 6; }
	if (fr == 74) { if (gFxOrder[4] != 0 || gFxOrder[0] != 1) fail("FX CHAIN: LOFI dragged to the end (%d %d %d %d %d)", gFxOrder[0], gFxOrder[1], gFxOrder[2], gFxOrder[3], gFxOrder[4]); }
	// the keys' row: OCT+ moves the small keys up, LTCH holds
	if (fr == 76) { TOUCH = 1; TX = 2 + 45 + 10; TY = PNL_KR_Y + 6; }
	// MIX: the strips; drag CHORD's level down, its FX send to 0, solo BASS
	if (fr == 80) CHIP(7);
	if (fr == 84) { if (!gMixOn) fail("MIX: the chip should open the mixer"); }
	if (fr >= 86 && fr < 90) { TOUCH = 1; TX = 320 / 9 + 17; TY = MXV_B - 10; }   // CHORD's level: low
	if (fr == 91) { TOUCH = 1; TX = 150; TY = 30; }   // FX SEND
	if (fr >= 93 && fr < 96) { TOUCH = 1; TX = 106 + 50; TY = MXV_B + 4; }   // CHORD's send: 0
	if (fr == 97) { TOUCH = 1; TX = 40; TY = 30; }   // LEVELS
	if (fr == 99) { TOUCH = 1; TX = 2 * (320 / 9) + 26; TY = MXV_BY + 6; }   // BASS: S
	if (fr == 102) { if (gPartLvl[1] > 0.3f) fail("MIX: CHORD's level should be low (%.2f)", gPartLvl[1]); if (gPartSend[1] > 0.1f) fail("MIX: CHORD's FX send ~0 (%.2f)", gPartSend[1]); if (!gPartSolo[2]) fail("MIX: BASS solo");
		if (partGain(1) != 0 || partGain(2) <= 0) fail("MIX: with BASS soloed, CHORD is silent"); shotBot("shots/mix.ppm"); }
	if (fr == 104) { gPartSolo[2] = 0; gPartLvl[1] = 1; gPartSend[1] = 1; done = 1; }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 106; app_main(); if (!done) fail("didn't run"); printf("panel: sliders, choices, sub-pages, small keys\n"); return gFails ? 1 : 0; }
