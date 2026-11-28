// DEMO TRACK: LOAD SESSION > DEMO TRACK loads the song (the EXAMPLE SESSION, parts muted) and puts its four takes on the tape (demo/take1-4.sytp,
// fetched once into sd/demo), at 156 BPM; PLAY plays them. A take saved and loaded back comes back bit for bit.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 4) { gSeqLen = 16; stepSetNote(0, 60, 1); gBPM = 100; lsOpenSessions(); int k = -1; for (int i = 0; i < gLsN; i++) if (!strcmp(gLsNm[i], "DEMO TRACK")) k = i; if (k < 0) { fail("LOAD SESSION should list DEMO TRACK"); return; } lsPick(k); }
	if (fr == 8) {
		int n = 0; for (int t = 0; t < NTRK; t++) n += gTpHave[t];
		if (n != 4) fail("DEMO TRACK should put 4 takes on the tape (%d)", n);
		if (gBPM != 156) fail("DEMO TRACK should be at 156 BPM (%d)", gBPM);
		if (!gPartMute[0] || !gPartMute[3]) fail("DEMO TRACK: the song's parts should start muted (PLAY = the takes)");
		{ int any = 0; for (int p = 0; p < NPAT; p++) for (int i = 0; i < 64; i++) any |= gPat[p].st[i].n > 0; any |= gSeq[0].n > 0 || gSeqLen > 0; if (!any) fail("DEMO TRACK should bring the song's patterns too"); }
		if (gTapeLen < 32728 * 90) fail("the takes should be the whole song (%d samples)", gTapeLen);
		transportToggle();
	}
	if (fr == 20) {
		if (!gTapePlay || !CS[gTapeCh[0]].on || !CS[gTapeCh[7]].on) fail("PLAY should play all four takes");
		static u8 a[64]; memcpy(a, gTp[2][1] + 100000, 64);
		if (!tapeSaveTake(2, "sd/t3.sytp")) fail("a take should save");
		memset(gTp[2][1] + 100000, 0, 64); gTpHave[2] = 0;
		if (!tapeLoadTake(2, "sd/t3.sytp") || memcmp(a, gTp[2][1] + 100000, 64)) fail("a take should load back as it was saved");
		if (tapeLoadTake(3, "sd/nothere.sytp")) fail("a missing take shouldn't load");
		done = 1;
	}
}
int main(void) { mkdir("sd", 0777); mkdir("sd/demo", 0777);
	for (int t = 1; t <= 4; t++) { char a[64], b[64]; snprintf(a, sizeof a, "../../demo/take%d.sytp", t); snprintf(b, sizeof b, "sd/demo/take%d.sytp", t); FILE* i = fopen(a, "rb"); FILE* o = fopen(b, "wb"); if (!i || !o) { fail("no %s", a); return 1; } static char buf[1 << 16]; size_t n; while ((n = fread(buf, 1, sizeof buf, i)) > 0) fwrite(buf, 1, n, o); fclose(i); fclose(o); }
	loadFont(); FRMAX = 24; app_main(); if (!done) fail("didn't run"); printf("demo: DEMO TRACK, 4 takes on the tape\n"); return gFails ? 1 : 0; }
