// Controls: the preset browser (D-pad), saving + naming a patch, and the MATRIX touch screen.
// Also saves both screens of every page to shots/ for a look.
#include "syrup_host.c"
#include "harness.h"
static int step = 0, pageShots = 0;
static void press(int fr, int at, u32 k) { if (fr == at || fr == at + 1) HD = k; }
static void tap(int fr, int at, int x, int y) { if (fr == at || fr == at + 1) { TOUCH = 1; TX = x; TY = y; } }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	// --- browser: from the start page, left twice, down three times, check the patch loaded
	if (fr == 15) { gStyle = 0; gPreset = 0; presetLoad(0, 0); }
	press(fr, 20, KEY_DRIGHT); press(fr, 30, KEY_DLEFT);
	press(fr, 40, KEY_DDOWN); press(fr, 50, KEY_DDOWN); press(fr, 60, KEY_DDOWN);
	if (fr == 70) { if (gStyle != 0 || gPreset != 3 || strcmp(gPatchName, PRESETS[0][3].name)) fail("browser: expected BASS #4 %s, got style %d patch %d '%s'", PRESETS[0][3].name, gStyle, gPreset, gPatchName); shotTop("shots/browser.ppm"); }
	press(fr, 72, KEY_DLEFT);   // wraps to USER
	if (fr == 80 && gStyle != NSTYLES - 1) fail("browser: left from BASS should wrap to USER, got style %d", gStyle);
	if (fr == 80 && strcmp(gPatchName, PRESETS[0][3].name)) fail("browser: an empty USER slot should keep the sound, got '%s'", gPatchName);
	// --- save + name: A twice
	press(fr, 90, KEY_A);
	if (fr == 95 && gUser[0].magic == PATCH_MAGIC) fail("save: the first A should only ask");
	press(fr, 100, KEY_A);
	if (fr == 110) {
		if (gUser[0].magic != PATCH_MAGIC) fail("save: nothing in USER 1 after A A");
		else if (strcmp(gUser[0].name, "TEST NAME ?")) fail("save: name should be 'TEST NAME ?', got '%s'", gUser[0].name);
		shotTop("shots/user.ppm");
	}
	// --- go to the MOD page (X steps pages) and open MATRIX
	if (fr >= 120 && fr < 220 && !gModOn && fr % 4 == 0) HD = KEY_X;
	if (fr == 222) { if (!gModOn) fail("matrix: never reached the MOD page"); gModView = 0; gMod[2].src = SRC_LFO3; gMod[2].dst = DST_BLEV; gMod[2].amt = 0.3f; }
	if (fr == 226) shotBot("shots/matrix.ppm");
	tap(fr, 230, 50, MX_Y + 2 * MX_PITCH + 8);    // slot 3 source box
	if (fr == 236) { if (gMxPick != 2 || gMxPickDst) fail("matrix: tapping a source box should open the source list"); shotBot("shots/matrix_sources.ppm"); }
	tap(fr, 240, 160, 53 + 8 * 19 + 5);           // CAM LIGHT (row 9, middle column)
	if (fr == 246 && gMod[2].src != SRC_CAML) fail("matrix: picking CAM LIGHT gave %s", SRC_NAMES[gMod[2].src]);
	tap(fr, 250, 260, MX_Y + 2 * MX_PITCH + 8);   // slot 3 destination box
	tap(fr, 256, 50, 60);                         // A WT POS
	if (fr == 262 && gMod[2].dst != DST_APOS) fail("matrix: picking A WT POS gave %s", DST_NAMES[gMod[2].dst]);
	if (fr >= 270 && fr < 290) { TOUCH = 1; TX = 160 + (fr - 270) * 2; TY = MX_Y + 2 * MX_PITCH + 9; }   // drag right
	if (fr == 289) shotBot("shots/matrix_drag.ppm");
	if (fr == 295 && (gMod[2].amt < 0.8f || gMod[2].amt > 0.9f)) fail("matrix: dragging to +38 px should give ~+0.86, got %.2f", gMod[2].amt);
	// --- every page, both screens
	if (fr >= 300 && fr < 300 + 12 * 6) {
		int k = (fr - 300) / 6, f = (fr - 300) % 6;
		if (f == 0 && k) HD = KEY_X;
		if (f == 4) { char n[64]; snprintf(n, sizeof n, "shots/page%02d_top.ppm", k); shotTop(n); snprintf(n, sizeof n, "shots/page%02d_bot.ppm", k); shotBot(n); pageShots++; }
	}
}
int main(void) {
	mkdir("shots", 0777); remove("sd/user_presets.bin");
	loadFont(); FRMAX = 300 + 12 * 6 + 2; app_main();
	printf("ui: %d page shots, %d failures\n", pageShots, gFails);
	return gFails ? 1 : 0;
}
