// SESSION NAMES: SAVE SESSION asks for a name (a random one typed in already; the host keyboard types TEST NAME),
// keeps it with the date and time, and LOAD SESSION shows them. A random name is two words that fit the keyboard.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) { system("rm -rf sd/sessions"); for (int i = 0; i < 50; i++) { char n[24]; sessionRandomName(n, sizeof n); if (strlen(n) > 13 || !strchr(n, ' ')) fail("SESSION NAME: '%s' should be two words, 13 at most", n); } }
	if (fr == 4) { gSessDue = 3; }   // nothing saved yet: LOAD SESSION gives the example
	if (fr == 8) { int notes = 0; for (int p = 0; p < NPAT; p++) for (int i = 0; i < NSTEP; i++) notes += gPat[p].st[i].n + (p == gPatCur ? gSeq[i].n : 0);
		printf("sessname: example loaded: '%s', %d BPM, %d notes in the patterns, song %d slots\n", gMsg, gBPM, notes, gSongLen);
		if (!strstr(gMsg, "EXAMPLE")) fail("EXAMPLE: LOAD with nothing saved should load the example (%s)", gMsg);
		if (gBPM != 156 || notes < 20 || gSongLen < 4) fail("EXAMPLE: should be the demo song (%d BPM, %d notes, song %d)", gBPM, notes, gSongLen);
		gSessDue = 1; }
	if (fr == 16) {
		FILE* f = fopen("sd/sessions/S01-name.txt", "rb"); char b[96] = ""; if (f) { size_t n = fread(b, 1, 95, f); b[n] = 0; fclose(f); }
		printf("sessname: S01-name.txt = %s", b);
		if (!strstr(b, "TEST NAME")) fail("SESSION NAME: the typed name should be kept (%s)", b);
		if (!strchr(b, ':')) fail("SESSION NAME: the time should be kept with it (%s)", b);
		if (!strstr(gMsg, "TEST NAME")) fail("SESSION NAME: saving should say the name (%s)", gMsg);
		gSessAt = -1; gSessDue = 3;
	}
	if (fr == 22) { printf("sessname: load says '%s'\n", gMsg); if (!strstr(gMsg, "TEST NAME") || !strstr(gMsg, "1/1")) fail("SESSION NAME: LOAD should show it (%s)", gMsg);
		lsOpenSessions(); if (!gLs || gLsN != 2 || strncmp(gLsNm[0], "TEST NAME", 9) || strcmp(gLsNm[1], "EXAMPLE")) fail("LOAD SESSION list: the saved one, then EXAMPLE (%d: %s / %s)", gLsN, gLsNm[0], gLsNm[1]);
		lsPick(1); }   // the EXAMPLE, picked from the list
	if (fr == 28) { if (!strstr(gMsg, "EXAMPLE") || gBPM != 156) fail("LOAD SESSION list: picking EXAMPLE should load it (%s)", gMsg);
		lsOpenNewSession(); lsPick(1); }   // NEW SESSION, DON'T SAVE
	if (fr == 32) { FILE* f = fopen("sd/sessions/S02-seq.bin", "rb"); if (f) { fclose(f); fail("NEW SESSION DON'T SAVE: nothing should be saved"); }
		int notes = 0; for (int i = 0; i < NSTEP; i++) notes += gSeq[i].n; if (notes || !strstr(gMsg, "NEW SESSION")) fail("NEW SESSION DON'T SAVE: should clear (%d notes, %s)", notes, gMsg); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 34; app_main(); if (!done) fail("didn't run"); printf("sessname: name, date, shown on load\n"); return gFails ? 1 : 0; }
