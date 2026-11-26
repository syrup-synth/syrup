// TAPE SEAM: a take is faded out over its last 3 ms and in over its first 3 ms, so the loop meets near zero (no click
// at the seam); the middle stays as it was.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		int len = 32728 / 2; if (!tapeAlloc(3)) { fail("tape alloc"); return; }
		for (int k = 0; k < 2; k++) { Ima e = {0, 0}; u8* d = gTp[3][k]; for (int i = 0; i < len; i++) { int v = (int)(12000 * sinf(i * 0.0371f)) + 4000, c = imaEnc(&e, v); if (i & 1) d[i >> 1] |= c << 4; else d[i >> 1] = (u8)c; } }
		Ima a = {0, 0}; int mid0 = 0; for (int i = 0; i <= len / 2; i++) { int b = gTp[3][0][i >> 1]; mid0 = imaDec(&a, (i & 1) ? b >> 4 : b & 15); }
		tapeSeam(3, len);
		Ima de = {0, 0}; int first = 0, last = 0, mid = 0, jump = 0, prev = 0;
		for (int i = 0; i < len; i++) { int b = gTp[3][0][i >> 1], v = imaDec(&de, (i & 1) ? b >> 4 : b & 15); if (i == 0) first = v; if (i == len - 1) last = v; if (i == len / 2) mid = v; if (i && abs(v - prev) > jump) jump = abs(v - prev); prev = v; }
		printf("seam: first %d last %d, mid %d (was %d), biggest step %d\n", first, last, mid, mid0, jump);
		if (abs(first) > 300 || abs(last) > 300) fail("SEAM: the take should start and end near zero (%d %d)", first, last);
		if (abs(mid - mid0) > 400) fail("SEAM: the middle should stay (%d vs %d)", mid, mid0);
		if (jump > 2500) fail("SEAM: no jumps (%d)", jump);
		done = 1;
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 5; app_main(); if (!done) fail("didn't run"); printf("seam: tape loop seam\n"); return gFails ? 1 : 0; }
