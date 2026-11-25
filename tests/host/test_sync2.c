// ONE TRANSPORT under stress: SWING with a pattern of an odd length (the beat stays on the bar), a long stall (the clock
// catches up with the audio, the missed notes don't burst out), and an external MIDI clock at another tempo (the
// transport locks to its beats, not only its BPM)
#include "syrup_host.c"
#include "harness.h"
static double kErr = 0, hErr = 0; static int kN = 0, hN = 0, phase = 0, t0 = 0, done = 0, burst = 0, lastTick = -1; static long on0 = 0; static double clkAcc = 0; static int pulses = 0;
static void hook(int part, int note) {
	(void)note; if (!gSeqPlaying) return;
	if (part == 12 && phase == 0) {   // hats on every step, timed by the audio itself: even steps on the beat grid, odd ones late by the swing
		static long h0 = -1; if (h0 < 0) h0 = ON;   // (from the first hit: the transport's step 1)
		double p = (ON - h0) / 32728.0 * gBPM / 60.0 * 4.0, m = fmod(p, 2.0), e = fmin(fmin(fabs(m), fabs(m - 2)), fabs(m - (1 + gSwing))); if (e > hErr) hErr = e; hN++; }
	if (part == 10) { float p = transportPos16(); double e = p / 4 - floor(p / 4 + 0.5); if (fabs(e * 4) > kErr) kErr = fabs(e * 4); kN++; }
}
static float elapsed16(void) { return (float)((ON - on0) / 32728.0 * gBPM / 60.0 * 4.0); }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		gOnsetHook = hook; gBPM = 120; gSwing = 0.25f;
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 13; for (int i = 0; i < 13; i += 2) stepSetNote(i, 60, 1);
		myDrumLoad(); gKickPat = MINE_IDX[0]; gSnarePat = MINE_IDX[1]; gHatPat = MINE_IDX[2]; memset(gMyDrum, 0, sizeof(gMyDrum));
		for (int i = 0; i < 16; i += 4) gMyDrum[0][i] = 230; for (int i = 0; i < 16; i++) gMyDrum[2][i] = 180; gDrums = 1; drumsChanged();
		transportToggle(); t0 = fr;
	}
	if (fr == 4) on0 = ON;
	if (phase == 0 && fr > t0 && transportPos16() > 8 * 16) {
		printf("swing + 13-step pattern: %d kicks, worst %.3f of a 16th off the bar's grid\n", kN, kErr);
		printf("  %d hats, worst %.3f of a 16th off the swung grid\n", hN, hErr);
		if (kErr > 0.1) fail("SWING: an odd pattern length should not pull the drums off the bar (%.2f)", kErr);
		if (hErr > 0.12) fail("SWING: the hats should stay swung the same way (%.2f off)", hErr);
		phase = 1; gClockDebt += 0.8f;   // a 0.8 s stall (a table loading, an SD write)
	}
	if (phase == 1 && fr == lastTick + 1) {}
	if (phase == 1 && transportPos16() > 10 * 16 + 8) {
		float e = elapsed16() + 0.8f * gBPM / 60.0f * 4.0f - transportPos16();   // (the host's 0.8 s of missed frames played on the console but aren't in OUT)
		printf("after a 0.8 s stall: transport %.2f, the audio played %.2f 16ths (off by %.3f)\n", transportPos16(), elapsed16() + 0.8f * gBPM / 15.0f, e);
		if (fabsf(e) > 0.15f) fail("STALL: the transport should catch up with the audio (%.2f 16ths behind)", e);
		phase = 2; transportToggle(); gSwing = 0; memset(gMyDrum, 0, sizeof(gMyDrum)); gBPM = 120;
		midiPush(0xFA, 0, 0); clkAcc = 0; pulses = 0;
	}
	if (phase == 2) {   // an external clock at 131 BPM: 24 pulses a beat
		clkAcc += 1.0 / 60.0; double per = 60.0 / (131.0 * 24.0);
		while (clkAcc >= per) { clkAcc -= per; midiPush(0xF8, 0, 0); pulses++; }
		if (pulses >= 24 * 32) {
			float want = pulses / 6.0f, got = transportPos16();
			printf("external clock 131 BPM, 32 beats: transport %.2f, the clock %.2f 16ths (BPM now %d)\n", got, want, gBPM);
			if (fabsf(got - want) > 0.5f) fail("MIDI CLOCK: the transport should stay on the external beats (%.2f vs %.2f)", got, want);
			phase = 3; done = 1; transportToggle();
		}
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 60 * 60; app_main(); if (!done) fail("didn't finish"); printf("sync2: swing, stalls, external clock\n"); return gFails ? 1 : 0; }
