// Playability pass: start on the last page, KNOBS view + XY mode button, circle pad PAGE mode, matrix long-press
// clears a slot, VISUALIZER tiles, HARDWARE sensor view, the HUD's modulation ghost, SPEAKER EQ.
#include "syrup_host.c"
#include "harness.h"
static void press(int fr, int at, u32 k) { if (fr == at || fr == at + 1) HD = k; }
static float cut0, pos0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0; CX = 0; CY = 0;
	if (fr == 3) { int st, ix; if (findPreset("REESE CRATE", &st, &ix)) presetLoad(st, ix); }
	if (fr == 5) { if (!xyShown()) fail("settings: should start on OSC A (saved page) with the XY pad"); gXyMode = 0; }
	// XY mode button -> KNOBS, then drag the CUTOFF knob up
	if (fr == 8) { TOUCH = 1; TX = XYB_X + 20; TY = XY_Y + 6; }
	if (fr == 10) { if (gXyMode != 1) fail("XY mode button should step the XY PAD mode (%d)", gXyMode); gXyMode = 3; }
	if (fr == 12) cut0 = gCutoff;
	if (fr == 27) shotBot("shots/knobs.ppm");
	if (fr >= 14 && fr < 24) { TOUCH = 1; TX = 40; TY = KN_Y + 10 - (fr - 14) * 4; }
	if (fr == 26 && gCutoff < cut0 + 0.2f) fail("KNOBS: dragging CUTOFF up should raise it (%.2f -> %.2f)", cut0, gCutoff);
	if (fr >= 28 && fr < 31) { TOUCH = 1; TX = 4 + 158 + 24 + 100; TY = XY_Y + 74 + 3; }   // macro 2 slider
	if (fr == 33 && gMacro[1] < 0.7f) fail("KNOBS: macro slider should follow the finger (%.2f)", gMacro[1]);
	// circle pad PAGE mode on OSC A: X pushes WT POS
	if (fr == 35) { gStickMode = 1; pos0 = gOsc[0].pos; if (pos0 > 0.8f) gOsc[0].pos = pos0 = 0.2f; }
	if (fr >= 36 && fr < 66) CX = 150;
	if (fr == 68) { if (gOsc[0].pos < pos0 + 0.1f) fail("CIRCLE PAD PAGE: holding right should move WT POS (%.2f -> %.2f)", pos0, gOsc[0].pos); gStickMode = 0; }
	// matrix: long-press the source box clears the slot, a short tap opens the list
	if (fr == 70) { TOUCH = 1; TX = 80; TY = 8; }   // MATRIX chip
	if (fr == 74) { gMod[0].src = SRC_LFO1; gMod[0].dst = DST_CUTOFF; gMod[0].amt = 0.5f; gMod[1].src = SRC_ENV2; gMod[1].amt = 0.3f; }
	if (fr >= 76 && fr < 110) { TOUCH = 1; TX = MX_LX + 20; TY = MX_Y + MX_PITCH + 8; }   // hold slot 2
	if (fr == 112) { if (gMod[1].src != SRC_OFF) fail("MATRIX: holding a slot should clear it"); if (gMxPick >= 0) fail("MATRIX: a hold shouldn't open the list"); }
	if (fr == 114) { TOUCH = 1; TX = MX_LX + 20; TY = MX_Y + 8; }   // tap slot 1
	if (fr == 117) { if (gMxPick != 0) fail("MATRIX: a tap should open the source list (%d)", gMxPick); gMxPick = -1; }
	// HUD with the modulation ghost: FILT+ENV, CUTOFF row, change it
	if (fr == 120) { TOUCH = 1; TX = 4 * 320 / 6 - 20; TY = 8; }   // ENV chip -> FILT+ENV
	if (fr == 123) midiPush(0x90, 45, 100);
	press(fr, 126, KEY_DDOWN); press(fr, 132, KEY_DLEFT);
	if (fr == 136) shotTop("shots/hud_mod.ppm");
	if (fr == 140) midiPush(0x80, 45, 0);
	// VISUALIZER tiles (page 8) and HARDWARE view (page 9)
	for (int k = 0; k < 6; k++) press(fr, 144 + k * 4, KEY_X);   // 4 -> 5 -> 6 -> 10 -> 7 -> 11 -> 8
	if (fr == 172) { if (!gVisOnB) fail("VIS page: tiles should be up"); }
	if (fr == 174) { TOUCH = 1; TX = 4 + 2 * 79 + 30; TY = XY_Y + 32 + 10; }   // row 2 col 3: mode 6
	if (fr == 177) { shotBot("shots/vis_tiles.ppm"); if (gVisMode != 6) fail("VIS tiles: tapping should pick the mode (%d)", gVisMode); }
	press(fr, 180, KEY_X);
	if (fr == 186) { shotBot("shots/hw_view.ppm"); if (!gHwOnB) fail("HARDWARE page: sensor view should be up"); }
}
int main(void) {
	mkdir("shots", 0777); mkdir("sd", 0777);
	FILE* f = fopen("sd/settings.bin", "wb"); int d[4] = {0x53455431, 1, 0, 0}; fwrite(d, sizeof(d), 1, f); fclose(f);
	loadFont(); FRMAX = 190; app_main();
	remove("sd/settings.bin");
	printf("adv: last page, knobs, circle pad, matrix hold, HUD ghost, vis tiles, hardware view\n");
	return gFails ? 1 : 0;
}
