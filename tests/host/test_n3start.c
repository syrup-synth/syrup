// NEW 3DS: the START menu on every page, with the third core running the effects and the C-stick read
#include "syrup_host.c"
#include "harness.h"
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	int ph = fr % 40, pg = fr / 40;
	if (ph == 0 && pg < NPAGES) gCurPage = pg;
	if (ph == 5) HD = KEY_START;
	if (ph >= 8 && ph < 22 && ph % 2 == 0) HD = KEY_DDOWN;
	if (ph >= 22 && ph < 26) CY = -150; else CY = 0;
	if (ph == 28) HD = KEY_DRIGHT;
	if (ph == 32) HD = KEY_B;
}
int main(void) { mkdir("sd", 0777); loadFont(); FAKE_NEW3DS = 1; FRMAX = 40 * 14; app_main(); printf("n3start: START menu on a New 3DS\n"); return gFails ? 1 : 0; }
