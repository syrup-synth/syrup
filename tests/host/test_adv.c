// Playability pass: start on the last page, XY modes + PERF sliders + mod ghost, circle pad PAGE mode, matrix long-press
// clears a slot, VISUALIZER tiles, HARDWARE sensor view, the HUD's modulation ghost, SPEAKER EQ.
#include "syrup_host.c"
#include "harness.h"
static void press(int fr, int at, u32 k) { if (fr == at || fr == at + 1) HD = k; }
static float cut0, pos0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0; CX = 0; CY = 0;
	if (fr == 3) { int st, ix; if (findPreset("REESE CRATE", &st, &ix)) presetLoad(st, ix); }
	if (fr == 5) { if (!xyShown()) fail("settings: should start on OSC A (saved page) with the XY pad"); gXyMode = 0; }
	// XY mode button steps the mode; PERF: sliders over the pad; CUTOFF + RES mode moves both
	if (fr == 8) { TOUCH = 1; TX = 300; TY = XY_Y + 6; }
	if (fr == 10) { if (gXyMode != 1) fail("XY mode button should step the XY PAD mode (%d)", gXyMode); gXyMode = 6; }
	if (fr == 12) cut0 = gCutoff;
	if (fr >= 14 && fr < 18) { TOUCH = 1; TX = 4 + 54 + 90; TY = XY_Y + 6; }   // CUTOFF slider near the top
	if (fr == 20 && gCutoff < 0.85f) fail("PERF: CUTOFF slider should follow the finger (%.2f)", gCutoff);
	if (fr >= 22 && fr < 25) { TOUCH = 1; TX = 250; TY = xyTop() + 20; }   // pad: macros 1 + 2
	if (fr == 27) { shotBot("shots/perf.ppm"); if (gMacro[0] < 0.7f || gMacro[1] < 0.6f) fail("PERF: the pad should play macros 1 + 2 (%.2f %.2f)", gMacro[0], gMacro[1]); }
	if (fr == 28) { gXyMode = 3; }
	if (fr >= 29 && fr < 31) { TOUCH = 1; TX = 60; TY = XY_Y + XY_H - 10; }
	if (fr == 33 && (gCutoff > 0.3f || gRes > 0.3f)) fail("CUTOFF + RES mode: bottom left should close both (%.2f %.2f)", gCutoff, gRes);
	if (fr == 34) { gXyMode = 0; gMod[2].src = SRC_LFO1; gMod[2].dst = DST_CUTOFF; gMod[2].amt = 0.8f; midiPush(0x90, 50, 100); }
	if (fr == 60) { shotBot("shots/xy_ghost.ppm"); if (!xyModulated()) fail("XY ghost: an LFO on cutoff should show"); midiPush(0x80, 50, 0); gMod[2].src = SRC_OFF; }
	// circle pad PAGE mode on OSC A: X pushes WT POS
	if (fr == 35) { gStickMode = 1; for (int m = 0; m < NMOD; m++) if (gMod[m].src == SRC_STICK) gMod[m].src = SRC_OFF; pos0 = gOsc[0].pos; if (pos0 > 0.8f) gOsc[0].pos = pos0 = 0.2f; }
	if (fr >= 36 && fr < 66) CX = 150;
	if (fr == 50) { shotTop("shots/stick_hud.ppm"); if (gCallT <= 0 || !strstr(gCallTxt, "WT POS")) fail("CIRCLE PAD PAGE: the pop-up should show what it moves (%s)", gCallTxt); }
	if (fr == 68) { if (gOsc[0].pos < pos0 + 0.1f) fail("CIRCLE PAD PAGE: holding right should move WT POS (%.2f -> %.2f)", pos0, gOsc[0].pos); gMod[7].src = SRC_STICK; gMod[7].amt = 0.5f; pos0 = gOsc[0].pos; }
	if (fr == 69) CX = 150;
	if (fr == 70 && fabsf(gOsc[0].pos - pos0) > 0.001f) fail("CIRCLE PAD PAGE: a patch that routes STICK should keep bend + STICK");
	if (fr == 70) { gStickMode = 0; gMod[7].src = SRC_OFF; gMod[7].amt = 0; }
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
	// keyboard hit areas: black keys reach BLK_GRACE px past their drawn edge; D keeps a strip at the top
	if (fr == 187) {
		int sc = gScale, vm = gVoiceMode; gScale = 0; gVoiceMode = 0; float ww = 320.0f / 14; int bx = (int)(ww - ww * 0.3f), y = KB_Y + 10;
		if (keyAt(bx - 2, y, 48) != 49) fail("keys: 2 px left of C# should still be C# (%d)", keyAt(bx - 2, y, 48));
		if (keyAt(bx - BLK_GRACE - 2, y, 48) != 48) fail("keys: further left should be C");
		int dmid = (int)(1.5f * ww); if (keyAt(dmid, y, 48) != 50) fail("keys: the top middle of D should still be D (%d)", keyAt(dmid, y, 48));
		if (keyAt(bx - 2, KB_Y + BLK_H + 6, 48) != 48) fail("keys: below the black keys it's the white key");
		gScale = sc; gVoiceMode = vm;
	}
	// a held key survives a small slip: touch C# then drift 2 px out of its area
	if (fr >= 188 && fr < 194) { TOUCH = 1; TY = KB_Y + 10; TX = (int)(320.0f / 14 * 0.7f) - BLK_GRACE + (fr >= 191 ? -2 : 1); gScale = 0; }
	if (fr == 195) {   // edits that patchCapture saves must mark the patch edited
		gEdited = 0; gModView = 4; motionTouch(150, MOT_Y + 10, 1, 0); if (!gEdited) fail("edited: drawing a motion lane");
		gEdited = 0; gModView = 2; modViewTouch(100, 100, 1, 0); if (!gEdited) fail("edited: drawing LFO 3");
		gEdited = 0; chainTouch(CH_X(0) + 10, CH_Y + 5, 1, 0); chainTouch(CH_X(2) + 20, CH_Y + 5, 0, 0); chainTouch(0, 0, 0, 1); if (!gEdited) fail("edited: moving an FX block");
		u8 d[NFXB] = {0, 1, 2, 3, 4}; fxOrderSet(d);
		gEdited = 0; gXyMode = 0; xyTouch(100, XY_Y + 40, 1, 0); xyTouch(0, 0, 0, 1); if (!gEdited) fail("edited: the XY pad");
		gModView = 0;
	}
	// LATCH on, tap the same key twice: the second tap restrikes it (a released voice + a new one)
	if (fr == 200) { int st, ix; if (findPreset("WARM FELT", &st, &ix)) presetLoad(st, ix); gPads = 0; gVoiceMode = 0; gArp = 0; gSeqMode = 0; gChord = 0; gScale = 0; gEdited = 0; }
	if (fr == 201) HD = KEY_B;   // LATCH
	if (fr == 204 || fr == 205 || fr == 210 || fr == 211) { TOUCH = 1; TX = 30; TY = KB_Y + 80; }
	{ static int rel0; if (fr == 209 || fr == 214) {
		int rel = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage == 4) rel++;
		if (fr == 209) rel0 = rel; else if (rel <= rel0) fail("keys: with LATCH, tapping the held key again should restrike it");
	} }
	if (fr == 215) HD = KEY_B;
	if (fr == 197) {   // the browser: a GROOVE patch's beat and a DRUMS kit stop when you move on, also into USER
		gStyle = 14; gPreset = 0; presetLoad(14, 0); gEdited = 0; gGuard = 0;
		if (gVoiceMode < 3) fail("browser: a DRUMS kit should play the kit");
		browseStep(1, 0, 1); if (!gDrums || !gBeatFromPatch) fail("browser: a GROOVE patch should bring its beat (%d)", gDrums);
		browseStep(1, 0, 1); if (gDrums) fail("browser: moving GROOVE -> USER should stop the patch's beat");
		gStyle = 14; gPreset = 0; presetLoad(14, 0); gEdited = 0; gGuard = 0;
		gStyle = NSTYLES - 1; gPreset = 0; for (int i = 0; i < NUSER; i++) gUser[i].magic = 0;
		gStyle = 14; browseStep(1, 0, 1); browseStep(1, 0, 1); if (gVoiceMode >= 3 || gDrums) fail("browser: into USER the kit and beat should go (vm %d, drums %d)", gVoiceMode, gDrums);
		if (strcmp(gPatchName, "BOOM BAP BASS")) fail("browser: an empty USER slot keeps the sound (%s)", gPatchName);
		gGuard = 0; gEdited = 0; gStyle = NSTYLES - 1; gPreset = 1; presetLoad(15, 0); gStyle = NSTYLES - 1; gPreset = 1;
		browseStep(0, -1, 1); if (gDrums) fail("browser: an empty USER slot should stop a patch's beat");
	}
	if (fr == 196) {   // DRY check on the FX ORDER view; INIT PATCH comes with its macros wired
		chainTouch(220, TAB_H + 26, 1, 0); chainTouch(0, 0, 0, 1); if (!gDry) fail("DRY: the button should switch every effect off");
		gFx = 3; gFxMix = 0.8f; if (busSend(0) != 0) fail("DRY: no effect sends while it's on");
		chainTouch(220, TAB_H + 26, 1, 0); chainTouch(0, 0, 0, 1); if (gDry) fail("DRY: a second tap brings the effects back");
		SPatch ip; patchInit(&ip); initWire(&ip); patchApply(&ip);
		if (gMod[4].src != SRC_M1 || gMod[4].dst != DST_CUTOFF || gXyMode != 1) fail("INIT PATCH: macro 1 should be wired to the filter, XY on MACRO 1+2");
		gMacro[3] = 0; gFxMixMod = modSum(DST_FXMIX, newestVoice()); if (fxMixEff() > 0.001f || busSend(0) > 0) fail("INIT PATCH: M4 at 0 should be dry");
		gMacro[3] = 0.6f; gFxMixMod = modSum(DST_FXMIX, newestVoice()); if (fabsf(fxMixEff() - 0.6f) > 0.01f) fail("INIT PATCH: M4 should set the FX MIX (%.2f)", fxMixEff());
		gMacro[3] = 0; gFxMixMod = 0;
	}
	if (fr == 193 && !(gHeldN && (gHeld[0] % 12) == 1)) fail("keys: a 2 px slip off C# shouldn't change the note (held %d)", gHeldN ? gHeld[0] : -1);
}
int main(void) {
	mkdir("shots", 0777); mkdir("sd", 0777);
	FILE* f = fopen("sd/settings.bin", "wb"); int d[4] = {0x53455431, 1, 0, 0}; fwrite(d, sizeof(d), 1, f); fclose(f);
	loadFont(); FRMAX = 218; app_main();
	remove("sd/settings.bin");
	printf("adv: last page, XY modes, PERF, circle pad, matrix hold, HUD ghost, vis tiles, hardware view\n");
	return gFails ? 1 : 0;
}
