// UI fuzz: random buttons, taps, drags and circle pad for N frames. Build with -fsanitize=address,undefined in its own
// folder (it writes sd/): gcc -O1 -g -fsanitize=address,undefined -I../../source fuzz.c -o fuzz -lm -lpthread -lcurl; ./fuzz SEED FRAMES
#include "syrup_host.c"
#include "harness.h"
static unsigned rs = 12345;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }
static int holdT = 0, tx, ty;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; CX = 0; CY = 0; gFxCore2 = 0;
	static const u32 K[] = {KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT, KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R};
	unsigned r = rnd() % 100;
	if (r < 25) HD = K[rnd() % 10];
	if (holdT > 0) { holdT--; TOUCH = 1; TX = tx; TY = ty; if (rnd() % 4 == 0) { tx += (int)(rnd() % 21) - 10; ty += (int)(rnd() % 21) - 10; if (tx < 0) tx = 0; if (tx > 319) tx = 319; if (ty < 0) ty = 0; if (ty > 239) ty = 239; } }
	else if (r >= 25 && r < 60) { holdT = 1 + rnd() % 30; tx = rnd() % 320; ty = rnd() % 240; }
	if (r >= 95) { CX = (int)(rnd() % 311) - 155; CY = (int)(rnd() % 311) - 155; }
	if (fr % 5000 == 0) fprintf(stderr, "fr %d\n", fr);
}
int main(int argc, char** argv) { if (argc > 1) rs = atoi(argv[1]); mkdir("sd", 0777); remove("sd/settings.bin"); loadFont(); FRMAX = argc > 2 ? atoi(argv[2]) : 20000; app_main(); printf("fuzz done\n"); return 0; }
