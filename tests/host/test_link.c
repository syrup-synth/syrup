// LINK (local wireless / IR between consoles): a pretend host runs the same tempo a little ahead, sends its step count
// when each step starts (and heartbeats mid-step). The follower pulls onto it without jumps or double notes; a late
// (the host is only seen once a screen frame, as on the consoles: up to a frame late, ~0.13 of a step at 120 BPM)
// joiner (5 steps out) jumps once; the host's STOP stops the follower; IR's mod-8 step count works the same way.
#include "syrup_host.c"
#include "harness.h"
static int notes = 0, done = 0; static double H0 = -1, OFF = 0.6; static int hostLast = -1, phase = 0, mod = 256; static double M0 = 0;
static void hook(int part, int note) { (void)note; if (part == 0) notes++; }
static double hostPos(void) { return OFF + (DPOS - H0) / 32728.0 * gBPM / 60.0 * 4; }   // the host, in steps
static double mine(void) { float l = stepLenClk(gSeqClock); return gSeqClock + (l > 0 ? gSeqPh / l : 0); }
static double wrapd(double d) { while (d >= mod / 2.0) d -= mod; while (d < -mod / 2.0) d += mod; return d; }
static void hostSend(int fr) {   // a new step: send it; every 30 frames a heartbeat of the same step
	int st = (int)floor(hostPos()); if (st != hostLast) { hostLast = st; linkFollow(st % mod, mod, 120); } else if (fr % 30 == 0) linkFollow(st % mod, mod, 120);
}
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) { gOnsetHook = hook; gBPM = 120; gSwing = 0; memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; for (int i = 0; i < 16; i++) { stepSetNote(i, 60, 1); gSeq[i].vel = 200; } }
	// 1. both running, the host 0.6 of a step ahead: pulls in, no extra notes
	if (fr == 5) { transportToggle(); }
	if (fr == 8) { H0 = DPOS - (mine() - 0) / (gBPM / 60.0 * 4) * 32728.0; OFF = 0.6; notes = 0; phase = 1; M0 = mine(); }
	if (phase == 1) hostSend(fr);
	if (fr == 8 + 240 && phase == 1) {   // 4 s
		double d = wrapd(hostPos() - mine()), steps = mine() - M0;
		printf("link: after 4 s %.3f steps out (was 0.60), %d notes in %.1f steps\n", d, notes, steps);
		if (fabs(d) > 0.15) fail("LINK: the follower should be on the host (%.3f steps out)", d);
		if (notes > (int)steps + 1) fail("LINK: notes fired twice (%d notes in %.1f steps)", notes, steps);
		phase = 2;
	}
	// 2. a late joiner: 5 steps out -> one jump
	if (fr == 260) { OFF += 5; hostLast = -1; }
	if (phase == 2) hostSend(fr);
	if (fr == 300) { double d = wrapd(hostPos() - mine()); if (fabs(d) > 0.3) fail("LINK: 5 steps out should jump onto the host (%.2f)", d); phase = 3; }
	// 3. the host stops
	if (fr == 302) linkFollow(-1, mod, 120);
	if (fr == 306) { if (gSeqMode == 1) fail("LINK: the host's STOP should stop the follower"); }
	// 4. the host starts: the follower starts too; then IR (mod 8)
	if (fr == 310) { mod = 8; hostLast = -1; linkFollow(0, mod, 120); }
	if (fr == 314) { if (gSeqMode != 1) fail("LINK: the host's PLAY should start the follower"); H0 = DPOS - mine() / (gBPM / 60.0 * 4) * 32728.0; OFF = 0.5; phase = 4; }
	if (phase == 4) hostSend(fr);
	if (fr == 314 + 240) { double d = wrapd(hostPos() - mine()); printf("link: IR (mod 8) after 4 s %.3f steps out\n", d); if (fabs(d) > 0.15) fail("LINK IR: should be on the host (%.3f)", d); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 560; app_main(); if (!done) fail("didn't run"); printf("link: follow, late join, stop / start, IR\n"); return gFails ? 1 : 0; }
