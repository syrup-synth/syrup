// Chords: the extra CHORD types, CHORD over MIDI, chords recorded live (MIDI keys together, PADS, a progression
// with BASS), and a held chord carried into the next patch.
#include "syrup_host.c"
#include "harness.h"
static float c0; static int s0, sel0, pr0;
static int stepN(int st) { return gSeq[st].n; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 3) {
		SPatch p; patchInit(&p); patchApply(&p); gVoiceMode = 0; gArp = 0;
		int n[4], k;
		gScale = 0; gChord = 12; k = chordNotes(60, n); if (k != 4 || n[1] != 63 || n[2] != 66 || n[3] != 69) fail("DIM7 should be 60 63 66 69 (%d notes)", k);
		gChord = 15; k = chordNotes(60, n); if (k != 4 || n[3] != 75) fail("7#9 top note should be 75 (%d)", n[3]);
		gScale = 2; gChord = 5; gKeyDeg = 0; k = chordNotes(60, n); if (k != 3) fail("SUS2 in a scale: 3 notes (%d)", k);
		for (int c = 0; c < NCHORD; c++) { gChord = c; k = chordNotes(60, n); if (k < 1 || k > 4) fail("chord %s: %d notes", CHORD_NAMES[c], k); }
		gScale = 0;
		// MIDI with CHORD: one key, the whole chord
		gChord = 2; midiNoteOn(60, 0.8f); if (gMidiHeldN != 1 || gMidiChN[0] != 4) fail("MIDI + MAJ7: one key should hold 4 notes (%d)", gMidiChN[0]);
		midiNoteOff(60); if (gMidiHeldN) fail("MIDI note off should clear it");
		gChord = 0;
		// a held chord survives a patch change
		int ch[3] = {60, 64, 67}; playSet(ch, 3, 0.8f); presetLoad(0, 1); if (gHeldN != 3 || gHeld[2] != 67) fail("a held chord should carry into the next patch (%d held)", gHeldN);
		playSet(NULL, 0, 0.8f);
		patchInit(&p); patchApply(&p); gVoiceMode = 0; gArp = 0; gChord = 0;
		seqClear(); gSeqLen = 16; gBPM = 120; gSeqMode = 1; gRecQ = 0;
	}
	if (fr == 30) {   // live REC: three MIDI keys struck together = one step with a chord
		if (!gSeqPlaying) fail("the sequence should be playing");
		memset(gSeq, 0, sizeof(gSeq)); gLiveRec = 1;
		midiNoteOn(60, 0.8f); midiNoteOn(64, 0.8f); midiNoteOn(67, 0.8f);
		int st = gLvStep; if (stepN(st) != 3) fail("MIDI chord while recording: one step with 3 notes (%d)", stepN(st));
	}
	if (fr == 45) { midiNoteOff(60); midiNoteOff(64); midiNoteOff(67); }
	if (fr == 60) {   // PADS: a pad chord goes in, with BASS under it
		memset(gSeq, 0, sizeof(gSeq)); gPadBass = 1; gPadSel = 0; int base = 48;
		gPadN = padChord(base, 0, 0, 1, gPadNotes); padPlay(base, gPadNotes, gPadN, 0.8f, 1);
		int st = gLvStep; if (stepN(st) != 4) fail("pad + BASS while recording: 4 notes on the step (%d)", stepN(st));
		if (gHeldN != 4 || gHeld[0] != padDeg(base, 0) - 12) fail("BASS: the root an octave down should sound (%d)", gHeld[0]);
		liveUp(); playSet(NULL, 0, 0.8f); gPadBass = 0;
	}
	if (fr == 66) {   // PADS SLIDE: drag from pad 1 to pad 4, the chord follows
		gPadBass = 0; float v; int y = PAD_Y + PAD_H / 2; padsTouch(48, 2 + 22, y, 1, &v); padsTouch(48, 2 + 45 + 22, y, 0, &v); padsTouch(48, 2 + 3 * 45 + 22, y, 0, &v);
		if (gPadSel != 3) fail("PADS: sliding across should play the pad under the finger (%d)", gPadSel);
		{ int moved = 0; for (int k = 0; k < NVS; k++) if (gV[k].active && gV[k].slide == 2) moved++; if (gPadMorph > 0 && moved < 2) fail("PADS MORPH: the voices should glide to the new chord, not restart (%d gliding)", moved); }
		liveUp(); playSet(NULL, 0, 0.8f); gPadSel = -1; memset(gSeq, 0, sizeof(gSeq));
	}
	if (fr == 70) {   // a progression on AUTO records a chord a bar
		memset(gSeq, 0, sizeof(gSeq)); gProg = 9; gProgAuto = 1; gPadBase = 48;
	}
	if (fr == 330) {
		int any = 0; for (int i = 0; i < 16; i++) if (stepN(i) >= 3) any++;
		if (!any) fail("AUTO progression while recording should write chords");
		gProgAuto = 0; gProg = 0; gLiveRec = 0; gSeqMode = 0; seqStop(); playSet(NULL, 0, 0.8f);
	}
	// NEW 3DS CONTROLS: C-stick moves the page's values, ZR plays / stops, ZL arms live REC
	if (fr == 335) { gN3 = 1; gMacro[1] = 0.2f; c0 = gMacro[1]; if (gCurPage != 0) fail("expected the PATCH page (%d)", gCurPage); }
	if (fr >= 336 && fr < 350) { CSY = 150; }
	if (fr == 350) { CSY = 0; if (gMacro[1] <= c0) fail("C-stick up on PATCH should raise MACRO 2 (%.3f -> %.3f)", c0, gMacro[1]); s0 = gSeqMode; }
	if (fr == 352) HD = KEY_ZR;
	if (fr == 354 && gSeqMode == s0) fail("ZR should start / stop the sequence");
	if (fr == 356) HD = KEY_ZR;
	if (fr == 360) { gN3 = 0; HD = KEY_START; }   // the START menu: the circle pad steps through it
	if (fr == 362) sel0 = gStartSel;
	if (fr >= 363 && fr < 366) CY = -150;
	if (fr == 366) { CY = 0; if (gStartSel == sel0) fail("circle pad down should move in the START menu"); }
	if (fr == 368) HD = KEY_B;
	if (fr == 368) {   // ROLL SCALE FOLD and STRUM NOTES SCALE
		int sv = gScale, rv = gRoot, lo = gRollLow, vm = gVoiceMode; gScale = 1; gRoot = 0; gRollScale = 2; gRollLow = 60; gVoiceMode = 0;
		if (rollNote(0) != 60 || rollNote(1) != 62 || rollNote(2) != 64 || rollRow(61) != -1 || rollRow(65) != 3) fail("ROLL SCALE FOLD: rows should be C D E F.. (%d %d %d)", rollNote(0), rollNote(1), rollNote(2));
		gStrumScale = 1; gPadSel = 0; if (strumScaleNote(48, 1) - strumScaleNote(48, 0) != 2 || strumScaleNote(48, 7) - strumScaleNote(48, 0) != 12) fail("STRUM NOTES SCALE: the strings should walk the scale");
		gStrumScale = 0; gRollScale = 1; gScale = sv; gRoot = rv; gRollLow = lo; gVoiceMode = vm; gPadSel = -1;
	}
	if (fr == 369) {   // DRUM HITS table and KIT > TABLE
		float* f = builtinFrames(T_DRUMS); if (!f) fail("DRUM HITS: no table");
		else { float a0 = 0, a7 = 0; for (int k = 0; k < CYC; k++) { a0 += fabsf(f[k]); a7 += fabsf(f[7 * CYC + k]); } if (a0 < 100 || a7 < 100) fail("DRUM HITS: kick frames should have sound (%.0f %.0f)", a0, a7); }
		int n0 = gNFiles; kitToTable(); if (gNFiles != n0 + 1 || gOsc[0].table < T_BUILTIN) fail("KIT > TABLE: a new table on OSC A (files %d -> %d)", n0, gNFiles);
	}
	if (fr == 370) {   // piano roll ERASER: from an empty cell, up first, then across the notes
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; gPatView = 0; gStepEd = -1; gRollLow = 60;
		int row = 3, nt = rollNote(row), y = ROLL_TOP + (ROLL_ROWS - 1 - row) * ROLL_RH + ROLL_RH / 2;
		stepSetNote(4, nt, 1); stepSetNote(6, nt, 1);
		int x0 = ROLL_KW + (int)(2.5f * ROLL_CW), ye = ROLL_TOP + (ROLL_ROWS - 1 - (row + 2)) * ROLL_RH + ROLL_RH / 2;
		gridTouch(x0, ye, 1, 0); gridTouch(x0, ye + 12, 0, 0); gridTouch(x0, y, 0, 0);
		for (int st = 2; st <= 8; st++) gridTouch(ROLL_KW + (int)((st + 0.5f) * ROLL_CW), y, 0, 0);
		gridTouch(0, 0, 0, 1);
		if (gSeq[4].n || gSeq[6].n) fail("ERASER: scribbling across should clear the notes (%d %d)", gSeq[4].n, gSeq[6].n);
		int any = 0; for (int i = 0; i < 16; i++) any += gSeq[i].n; if (any) fail("ERASER shouldn't add notes (%d)", any);
	}
	// PANIC: a long pad, its reverb on FREEZE, grains looping: all gone at once
	if (fr == 376) { int st, ix; if (findPreset("SHIMMER CLOUD", &st, &ix)) presetLoad(st, ix); gRevMix = 0.6f; gMasterFX = 1; midiPush(0x90, 60, 110); midiPush(0x90, 67, 110); }
	if (fr == 420) { gFreeze = 1; }
	if (fr == 440) { midiPush(0x80, 60, 0); midiPush(0x80, 67, 0); panicAll(); pr0 = ON; }
	if (fr == 470) { double r = rmsOf(pr0 + SEC(0.3), ON); if (r > 40) fail("PANIC: everything should be silent half a second later (%.0f rms)", r); if (gFreeze) fail("PANIC should turn FREEZE off"); }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 472; app_main(); printf("chord: NEW 3DS controls, stick in menus, chord types, MIDI chords, live chord REC, pads + BASS, progressions, hold over patch change\n"); return gFails ? 1 : 0; }
