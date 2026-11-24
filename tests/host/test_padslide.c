// PADS SLIDE: a drag across the pads plays only the chord the finger settles on, not the ones it crosses
#include "syrup_host.c"
#include "harness.h"
static int seen1 = 0, done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 20) { gPads = 1; gPadMorph = 0.25f; gPadSel = -1; }
	int y = PAD_Y + PAD_H / 2;
	if (fr >= 30 && fr <= 70) { TOUCH = 1; TY = y;
		TX = fr < 40 ? 2 + 21 : fr < 41 ? 2 + 45 + 21 : fr < 42 ? 2 + 90 + 21 : 2 + 135 + 21; }   // pad 1, then over 2 and 3 a frame each, rests on 4
	if (fr >= 40 && fr < 72 && (gPadSel == 1 || gPadSel == 2)) seen1 = 1;
	if (fr == 72) {
		if (seen1) fail("PAD SLIDE: the pads crossed on the way should not play");
		if (gPadSel != 3) fail("PAD SLIDE: the pad it rests on should play (%d)", gPadSel);
	}
	// the PROGRESSION row slides too: tap its first chord, slide over 2 and 3, rest on 4
	static int seenP = 0, at0 = -1;
	if (fr == 80) { gProg = 1; gProgAuto = 0; gProgAt = -1; }
	if (fr >= 90 && fr <= 130) { TOUCH = 1; TY = PROG_Y + 6; TX = fr < 100 ? 128 + 18 : fr < 101 ? 128 + 38 + 18 : fr < 102 ? 128 + 76 + 18 : 128 + 114 + 18; }
	if (fr == 95) at0 = gProgAt;
	if (fr >= 100 && fr < 132 && (gProgAt == 1 || gProgAt == 2)) seenP = 1;
	if (fr == 132) {
		if (at0 != 0) fail("PROGRESSION: a tap should play its first chord (%d)", at0);
		if (seenP) fail("PROGRESSION SLIDE: the chords crossed on the way should not play");
		if (gProgAt != 3 || gPadSel != PROGS[1][3]) fail("PROGRESSION SLIDE: the chord it rests on should play (%d)", gProgAt);
	}
	if (fr == 134) { playSet(NULL, 0, 0.8f); gPadBass = 1; gPadBassSub = 1; gPadSel = 0; gPadN = padChord(48, 0, 0, 1, gPadNotes); padPlay(48, gPadNotes, gPadN, 0.8f, 1); }
	if (fr == 136) {   // PAD BASS SOUND = SUB OSC: the bass note is one SUB layer, the chord the full patch
		int bn = padDeg(48, 0) - 12, ok = 0, chordFull = 0;
		for (int k = 0; k < NVS; k++) { SVoice* v = &gV[k]; if (!v->active || v->isEcho || v->stage == 4) continue;
			if (v->note == bn) ok = v->subOnly && v->nch == 1 && v->role[0] == R_SUB; else if (!v->subOnly && v->nch >= 1) chordFull = 1; }
		if (!ok) fail("PAD BASS SOUND SUB: the bass note should play on the SUB osc alone");
		if (!chordFull) fail("PAD BASS SOUND SUB: the chord should still be the whole patch");
		playSet(NULL, 0, 0.8f); gPadBassSub = 2; gOsc[0].on = 1; padPlay(48, gPadNotes, gPadN, 0.8f, 1);
		int okA = 0; for (int k = 0; k < NVS; k++) { SVoice* v = &gV[k]; if (v->active && !v->isEcho && v->stage != 4 && v->note == bn) { okA = v->nch >= 1; for (int j = 0; j < v->nch; j++) if (v->role[j] > R_A2) okA = 0; } }
		if (!okA) fail("PAD BASS SOUND OSC A: the bass note should be OSC A's layers alone");
		playSet(NULL, 0, 0.8f); gPadBass = 0; gPadBassSub = 1; done = 1;
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 145; app_main(); if (!done) fail("PAD SLIDE: didn't run"); printf("padslide: chords crossed on a slide don't play\n"); return gFails ? 1 : 0; }
