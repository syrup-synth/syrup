// ONE TRANSPORT: the sequence, the drums, the CHORD part (and its ARP), the BASS line and the keys' ARP all start on the
// same step and stay on the same 16th grid - every note-on is timed against the transport and checked over 32 bars
#include "syrup_host.c"
#include "harness.h"
#define NP 5
static const char* PN[NP] = {"SEQ/ARP", "CHORD", "BASS", "KICK", "HAT"};
static double maxErr[NP], sumEarly[NP], sumLate[NP]; static int cnt[NP], early[NP], late[NP], started = 0, done = 0; static float firstPos[NP];
static void hook(int part, int note) {
	(void)note; if (!gSeqPlaying) return;
	int k = part == 0 ? 0 : part == 1 ? 1 : part == 2 ? 2 : part == 10 ? 3 : part == 12 ? 4 : -1; if (k < 0) return;
	float pos = transportPos16(); double e = pos - floor(pos + 0.5); if (fabs(e) > maxErr[k]) maxErr[k] = fabs(e);
	if (!cnt[k]) firstPos[k] = pos;
	cnt[k]++; if (pos < 32) { sumEarly[k] += e; early[k]++; } else if (pos > 32 * 16 - 64) { sumLate[k] += e; late[k]++; }
}
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		gOnsetHook = hook; gBPM = 137; gSwing = 0; gScale = 1; gRoot = 0; gPadBase = 60;
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; for (int i = 0; i < 16; i++) { stepSetNote(i, 72 + (i & 3), 1); gSeq[i].vel = 200; }
		myDrumLoad(); gKickPat = MINE_IDX[0]; gSnarePat = MINE_IDX[1]; gHatPat = MINE_IDX[2]; memset(gMyDrum, 0, sizeof(gMyDrum));
		for (int i = 0; i < 16; i += 4) gMyDrum[0][i] = 230; for (int i = 0; i < 16; i++) gMyDrum[2][i] = 180;
		gDrums = 1; drumsChanged();
		gProg = PROG_CUSTOM; gProgAuto = 1; memset(gCtDeg, -1, sizeof(gCtDeg)); ctSet(0, 0, 16, 0); ctSet(16, 3, 16, 0); ctSet(32, 4, 16, 0); ctSet(48, 0, 16, 0);
		gCtArp = 1; gCtArpRate = 2; gPadBass = 2; gPartMute[1] = 0; gPartMute[2] = 0;
	}
	if (fr == 6) { transportToggle(); started = fr; }
	if (started && !done && transportPos16() > 32 * 16) {
		done = 1; transportToggle();
		for (int k = 0; k < NP; k++) {
			double drift = (late[k] ? sumLate[k] / late[k] : 0) - (early[k] ? sumEarly[k] / early[k] : 0);
			printf("%-8s %4d notes  first at %.3f  worst %.3f of a 16th  drift %+.4f\n", PN[k], cnt[k], firstPos[k], maxErr[k], drift);
			if (!cnt[k]) fail("%s never played", PN[k]);
			if (cnt[k] && firstPos[k] > 0.25f) fail("%s should start on the first step (%.2f)", PN[k], firstPos[k]);
			if (maxErr[k] > 0.1) fail("%s off the grid (%.2f of a 16th)", PN[k], maxErr[k]);
			if (fabs(drift) > 0.03) fail("%s drifts against the transport (%+.3f of a 16th over 32 bars)", PN[k], drift);
		}
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 60 * 60; app_main(); if (!done) fail("didn't finish 32 bars"); printf("sync: one transport, one grid\n"); return gFails ? 1 : 0; }
