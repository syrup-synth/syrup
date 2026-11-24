// START > SAVE SESSION / NEW SESSION / LOAD SESSION
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void menu(int fr, int at, int item) {   // open START, step down to the item, A
	if (fr == at) HD = KEY_START;
	for (int k = 0; k < item; k++) if (fr == at + 2 + k * 2) HD = KEY_DDOWN;
	if (fr == at + 2 + item * 2) HD = KEY_A;
}
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 10) { system("rm -rf sd/sessions"); memset(gSeq, 0, sizeof(gSeq)); stepSetNote(3, 62, 1); myDrumLoad(); memset(gMyDrum, 0, sizeof(gMyDrum)); gMyDrum[0][5] = 230; gKickPat = MINE_IDX[0]; }
	menu(fr, 20, 6);   // SAVE SESSION
	if (fr == 60) { if (!fopen("sd/sessions/S01-seq.bin", "rb")) fail("SAVE SESSION: no S01 files"); }
	menu(fr, 70, 8); menu(fr, 100, 8);   // NEW SESSION, twice
	if (fr == 140) { if (stepHasNote(3, 62) || gMyDrum[0][5]) fail("NEW SESSION: the notes and drums should be gone"); if (!fopen("sd/sessions/S02-seq.bin", "rb")) fail("NEW SESSION: the old one should be saved first (S02)"); }
	menu(fr, 150, 7);   // LOAD SESSION: the newest (S02)
	if (fr == 180) { if (!stepHasNote(3, 62) || !gMyDrum[0][5]) fail("LOAD SESSION: the notes and drums should come back (%d %d)", stepHasNote(3, 62), gMyDrum[0][5]); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 185; app_main(); if (!done) fail("didn't run"); printf("session: save, new, load\n"); return gFails ? 1 : 0; }
