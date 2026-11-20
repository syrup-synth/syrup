// Patch format v12: old (v11) user files still load with the new fields at their defaults; patches that carry a
// beat start it, patches that don't leave it alone; CHIP > FX and the XY mode follow every patch; a saved beat
// survives a save and reload.
#include "syrup_host.c"
#include "harness.h"
static void load(const char* nm) { int st, ix; if (findPreset(nm, &st, &ix)) presetLoad(st, ix); else fail("format: no preset %s", nm); }
static void checks(void) {
	if (gUser[0].magic != PATCH_MAGIC) fail("format: a v11 user patch didn't load");
	else {
		if (strcmp(gUser[0].name, "OLD ONE") || fabsf(gUser[0].cutoff - 0.33f) > 1e-6f) fail("format: v11 fields changed on load");
		if (gUser[0].hasBeat || gUser[0].thru || fabsf(gUser[0].thruCut - 1.0f) > 1e-6f || gUser[0].euLen != 16) fail("format: v12 fields of an old patch aren't the defaults");
	}
	// 2. beats come with beat patches only; CHIP > FX and XY follow every patch
	load("BOOM BAP BASS");
	if (!gDrums || gKit != 1 || gKickPat != 6 || gSnarePat != 1 || gHatPat != 5 || gBPM != 90) fail("format: BOOM BAP BASS should bring its beat (drums %d kit %d %d/%d/%d bpm %d)", gDrums, gKit, gKickPat, gSnarePat, gHatPat, gBPM);
	load("WARM FELT");
	if (!gDrums || gKickPat != 6) fail("format: a patch without a beat stopped or changed the beat");
	load("CHIP SWEEP"); if (!gThru || gThruEnv < 0.5f) fail("format: CHIP SWEEP should turn CHIP > FX on");
	load("WARM FELT"); if (gThru) fail("format: CHIP > FX stayed on after a patch without it");
	load("XY MORPH"); if (gXyMode != 1) fail("format: XY MORPH should set the XY pad to MACRO 1+2");
	load("WARM FELT"); if (gXyMode != 0) fail("format: the XY mode stayed on MACRO after a normal patch");
	load("SLOWDOWN"); if (!gTapeKeys || gLoopBars != 1) fail("format: SLOWDOWN should tie the tape to the keys and set 2 bars");
	load("WARM FELT"); if (gTapeKeys) fail("format: tape-follows-keys stayed on");
	// 3. save and reload a patch with a beat
	load("TRAP SP-12"); gEuLen = 9; patchCapture(&gUser[3]); snprintf(gUser[3].name, sizeof gUser[3].name, "MY TRAP"); userSave();
	gDrums = 0; userLoad();
	if (gUser[3].magic != PATCH_MAGIC || !gUser[3].hasBeat || gUser[3].kickPat != 3 || gUser[3].euLen != 9 || gUser[3].bpm != 140) fail("format: a saved beat didn't survive the reload");
	presetLoad(NSTYLES - 1, 3); if (!gDrums || gKickPat != 3) fail("format: loading the saved patch didn't bring its beat back");
	printf("format: v11 migration, beat / CHIP > FX / XY / tape-keys application, save + reload checked\n");
}
static void SCRIPT(int fr) { HD = 0; TOUCH = 0; gFxCore2 = 0; if (fr == 5) checks(); }
int main(void) {
	mkdir("sd", 0777);
	// a v11 file (shorter records, old magic), in place before Syrup starts and reads it
	{ SPatch p; patchInit(&p); p.magic = PATCH_MAGIC11; snprintf(p.name, sizeof p.name, "OLD ONE"); p.cutoff = 0.33f;
	  FILE* f = fopen("sd/user_presets.bin", "wb"); for (int i = 0; i < NUSER; i++) fwrite(&p, PATCH_V11_SIZE, 1, f); fclose(f); }
	loadFont(); FRMAX = 10; app_main();
	remove("sd/user_presets.bin");
	return gFails ? 1 : 0;
}
