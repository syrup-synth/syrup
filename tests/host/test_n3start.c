// NEW 3DS: the START menu on every page, with the third core running the effects and the C-stick read
#include "syrup_host.c"
#include "harness.h"
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	int ph = fr % 40, pg = fr / 40; CSY = 0; CX = 0; if (pg >= 13) ph = 99;
	if (ph == 0 && pg < NPAGES) gCurPage = pg;
	if (ph == 5) HD = KEY_START;
	if (ph >= 8 && ph < 22 && ph % 2 == 0) HD = KEY_DDOWN;
	if (ph >= 22 && ph < 26) CY = -150; else CY = 0;
	if (ph == 28) HD = KEY_DRIGHT;
	if (ph == 32) HD = KEY_B;
	static int b0, c0;   // C-STICK UP / DOWN: the left list (DRUMS: the beats, SEQ: ROLL CHORD)
	if (fr > 40 * 13 && fr < 40 * 13 + 8) { gN3 = 1; HD = KEY_SELECT; CX = -150; }   // the quick ring: DRUMS
	if (fr == 40 * 13 + 12) { b0 = gBeatSel; c0 = gRollChord; (void)c0; if (gCurPage != 11) fail("quick ring should reach DRUMS (%d)", gCurPage); }
	if (fr >= 40 * 13 + 13 && fr < 40 * 13 + 16) CSY = -150;
	if (fr == 40 * 13 + 18) { CSY = 0; if (gBeatSel != (b0 + 1) % NBEAT) fail("C-STICK DOWN on DRUMS should pick the next beat (%d -> %d)", b0, gBeatSel); }
}
int main(void) { mkdir("sd", 0777); loadFont(); FAKE_NEW3DS = 1; FRMAX = 40 * 14 + 20; app_main(); printf("n3start: START menu on a New 3DS\n"); return gFails ? 1 : 0; }
