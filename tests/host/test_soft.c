// SOFT LAYERS: a 3+3 unison patch holds 8 notes (hardware: 3), at about the same loudness, and starts / stops clean
#include "syrup_host.c"
#include "harness.h"
static int done = 0, a0, a1, b0, b1;
static int live(void) { int n = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started && !gV[i].isEcho && gV[i].stage != 4) n++; return n; }
static const int CH8[8] = {48, 52, 55, 59, 62, 64, 67, 71};
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 5) { SPatch p; patchInit(&p); p.osc[0].uni = 3; p.osc[0].detune = 15; p.osc[1].on = 1; p.osc[1].uni = 3; p.osc[1].detune = 15; p.osc[1].level = 0.6f; p.subLevel = 0; patchApply(&p); gRevMix = 0; }
	// HARDWARE first: one note, then the chord
	if (fr == 10) { gSoftMode = 0; voiceOn(60, 0.9f); }
	if (fr == 30) { a0 = ON; }
	if (fr == 60) { a1 = ON; voiceOff(60); }
	if (fr == 70) { for (int k = 0; k < 8; k++) voiceOn(CH8[k], 0.8f); }
	if (fr == 80) { int n = live(); printf("hardware: %d of 8 notes\n", n); for (int k = 0; k < 8; k++) voiceOff(CH8[k]); }
	// SOFT
	if (fr == 110) { gSoftMode = 1; voiceOn(60, 0.9f); }
	if (fr == 130) { b0 = ON; }
	if (fr == 160) { b1 = ON; voiceOff(60); }
	if (fr == 170) { for (int k = 0; k < 8; k++) voiceOn(CH8[k], 0.8f); }
	if (fr == 180) { int n = live(); printf("soft: %d of 8 notes, render %.1f%% of a frame\n", n, gSoftCpu * 100); if (n < 8) fail("SOFT LAYERS: a 3+3 patch should hold 8 notes (%d)", n); for (int k = 0; k < 8; k++) voiceOff(CH8[k]); }
	if (fr == 220) {
		double ha = rmsOf(a0, a1), sa = rmsOf(b0, b1); printf("level: hardware %.0f soft %.0f (%.1f dB)\n", ha, sa, 20 * log10(sa / ha));
		if (sa < ha * 0.7 || sa > ha * 1.4) fail("SOFT LAYERS: one note should be about as loud as on hardware (%.0f vs %.0f)", sa, ha);
		writeWav("/tmp/soft.wav", a0 - 1000, ON); done = 1;
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 225; app_main(); if (!done) fail("didn't run"); printf("soft: unison on one channel\n"); return gFails ? 1 : 0; }
