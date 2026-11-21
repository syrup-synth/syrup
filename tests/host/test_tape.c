// TAPE: bounce the sequence to tracks. Record track 1 (quarter notes), change the pattern, record track 2 (off-beats)
// while track 1 plays along; both must line up with the sequence's start. Then PLAY, SAVE WAV and ERASE.
#include "syrup_host.c"
#include "harness.h"
static void tap(int fr, int at, int x, int y) { if (fr == at || fr == at + 1) { TOUCH = 1; TX = x; TY = y; } }
static int onset(int t, int from) {   // first sample past a threshold on track t (left), decoded from the tape
	Ima s = {0, 0}; for (int p = 0; p < gTapeLen; p++) { int b = gTp[t][0][p >> 1], v = imaDec(&s, (p & 1) ? b >> 4 : b & 15); if (p >= from && abs(v) > 1500) return p; }
	return -1;
}
static long fsize(const char* p) { FILE* f = fopen(p, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }
static int playOk = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 2) { int st, ix; if (findPreset("SOFT PLUCK", &st, &ix)) presetLoad(st, ix); else fail("tape: no SOFT PLUCK");
		gBPM = 240; gTapeLenSel = 0; gDrums = 0; seqClear(); gSeqLen = 16; for (int k = 0; k < 16; k += 4) stepSetNote(k, 60, 1); }
	tap(fr, 3, 5 * 320 / 7 + 20, 8);                       // SEQ chip
	tap(fr, 6, 1 * 320 / 7 + 20, XY_Y + 5);                // PAT
	tap(fr, 9, 160, PV_Y + 110);                           // TAPE >>>
	if (fr == 12 && gPatView != 2) fail("tape: PAT > TAPE should open the tape view (%d)", gPatView);
	tap(fr, 13, 2 * 63 + 30, TP_Y + 8);                    // REC
	if (fr == 20 && gTapeRec != 0) fail("tape: REC should record track 1 (%d)", gTapeRec);
	if (fr == 60) shotBot("shots/tape_rec.ppm");
	if (fr == 300) {
		if (!gTpHave[0] || gTapeRec >= 0) fail("tape: track 1 should be on tape after 4 bars (have %d rec %d)", gTpHave[0], gTapeRec);
		int want = 4 * 4 * 32728 / 4; if (abs(gTapeLen - want) > 400) fail("tape: 4 bars at 240 should be %d samples (%d)", want, gTapeLen);
		if (gTapeArm != 1) fail("tape: the next empty track should be armed (%d)", gTapeArm);
		seqClear(); gSeqLen = 16; for (int k = 2; k < 16; k += 4) stepSetNote(k, 67, 1);   // off-beats for track 2
	}
	tap(fr, 302, 2 * 63 + 30, TP_Y + 8);                   // REC track 2
	if (fr == 330) { if (!gTapePlay || gTapeCh[1] < 0 || !CS[gTapeCh[0]].on) fail("tape: track 1 should play while track 2 records"); shotBot("shots/tape_overdub.ppm"); }
	if (fr == 590) {
		if (!gTpHave[1]) fail("tape: track 2 should be on tape");
		int a = onset(0, 0), b = onset(1, 0), step = 32728 / 16;   // a 16th at 240 = 2045 samples
		if (a < 0 || a > 700) fail("tape: track 1 should start on the sequence's first step (onset %d)", a);
		if (b < 0 || abs(b - 2 * step - a) > 300) fail("tape: track 2's off-beat should land 2 steps after track 1's beat (%d vs %d)", b, a);
		printf("tape: onsets %d, %d (two 16ths = %d)\n", a, b, 2 * step);
	}
	tap(fr, 592, 3 * 63 + 30, TP_Y + 8);                   // PLAY
	if (fr == 600) { playOk = gTapePlay && CS[gTapeCh[0]].on && CS[gTapeCh[2]].on; if (!playOk) fail("tape: PLAY should start both tracks"); }
	if (fr == 610) shotBot("shots/tape_play.ppm");
	tap(fr, 612, 3 * 63 + 30, TP_Y + 8);                   // STOP
	tap(fr, 616, 4 * 63 + 30, TP_Y + 8);                   // SAVE WAV
	if (fr == 620) {
		long w = 44 + (long)gTapeLen * 4;
		if (fsize("sd/tape/track1.wav") != w || fsize("sd/tape/track2.wav") != w || fsize("sd/tape/mix.wav") != w) fail("tape: SAVE WAV should write track1, track2 and mix (%ld %ld %ld, want %ld)", fsize("sd/tape/track1.wav"), fsize("sd/tape/track2.wav"), fsize("sd/tape/mix.wav"), w);
		if (fsize("sd/tape/track3.wav") >= 0) fail("tape: an empty track shouldn't be saved");
	}
	tap(fr, 622, 300, TP_RY + 1 * TP_RH + 8); tap(fr, 625, 300, TP_RY + 1 * TP_RH + 8);   // X twice: erase track 2
	if (fr == 630 && gTpHave[1]) fail("tape: X twice should erase track 2");
	if (fr == 632 && (gTapeLen == 0 || !gTpHave[0])) fail("tape: track 1 should stay");
}
int main(void) {
	mkdir("shots", 0777); mkdir("sd", 0777); remove("sd/tape/track1.wav"); remove("sd/tape/track2.wav"); remove("sd/tape/track3.wav"); remove("sd/tape/mix.wav");
	loadFont(); FRMAX = 640; app_main();
	printf("tape: 2 tracks bounced, played, saved, erased\n");
	return gFails ? 1 : 0;
}
