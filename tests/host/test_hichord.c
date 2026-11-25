// HICHORD on the PADS: hold a pad, the circle pad reshapes its chord while it plays - up 7TH, right SUS4, down the
// other colour (major <-> minor), back to the middle: the chord touched
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
#define PADTOUCH do { TOUCH = 1; TX = 2 + 21; TY = PAD_Y + PAD_H - 8; } while (0)
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; CX = CY = 0;
	if (fr == 4) { TOUCH = 1; TX = 6 * 320 / 8 + 20; TY = 8; }
	if (fr == 8) { gScale = 1; gRoot = 0; gPadBase = 60; }
	if (fr >= 12 && fr < 60) PADTOUCH;
	if (fr == 16) { if (gPadN != 3 || gPadNotes[1] - gPadNotes[0] != 4) fail("PAD I: a C major triad (%d notes, third %d)", gPadN, gPadNotes[1] - gPadNotes[0]); }
	if (fr >= 18 && fr < 26) CY = 140;
	if (fr == 24) { if (gPadN != 4) fail("HICHORD up: a 7TH (%d notes)", gPadN); }
	if (fr >= 28 && fr < 36) CX = 140;
	if (fr == 34) { if (gPadNotes[1] - gPadNotes[0] != 5) fail("HICHORD right: SUS4 (third %d)", gPadNotes[1] - gPadNotes[0]); }
	if (fr >= 38 && fr < 46) CY = -140;
	if (fr == 44) { if (gPadNotes[1] - gPadNotes[0] != 3) fail("HICHORD down: C minor (third %d)", gPadNotes[1] - gPadNotes[0]); }
	if (fr == 52) { if (gPadN != 3 || gPadNotes[1] - gPadNotes[0] != 4) fail("HICHORD centre: back to the triad touched (%d, third %d)", gPadN, gPadNotes[1] - gPadNotes[0]); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 64; app_main(); if (!done) fail("didn't run"); printf("hichord: pads + circle pad: 7th, sus4, minor, back\n"); return gFails ? 1 : 0; }
