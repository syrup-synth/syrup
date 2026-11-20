// The visualizer page in every mode (PLASMA included, its proctex emulated in software): no crash, something drawn.
#include "syrup_host.c"
#include "harness.h"
static int shots = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 3) { int st, ix; if (findPreset("WARM FELT", &st, &ix)) presetLoad(st, ix); gMasterFX = 1; }
	if (fr == 5) midiPush(0x90, 57, 110);
	if (fr >= 10 && fr < 10 + 10 * 6 && (fr - 10) % 6 == 0) HD = KEY_X;   // ten X presses along the tab order: the VISUALIZER
	for (int m = 1; m < NVISM; m++) {
		int at = 110 + m * 20;
		if (fr == at) gVisMode = m;
		if (fr == at + 15) { char n[64]; snprintf(n, sizeof n, "shots/vis%02d_%s.ppm", m, VISM_NAMES[m]); for (char* c = n; *c; c++) if (*c == ' ') *c = '_'; shotTop(n); shots++; }
	}
}
int main(void) {
	mkdir("shots", 0777);
	loadFont(); FRMAX = 110 + NVISM * 20 + 5; app_main();
	printf("vis: %d modes drawn\n", shots);
	if (shots != NVISM - 1) fail("vis: only %d of %d modes drawn", shots, NVISM - 1);
	return gFails ? 1 : 0;
}
