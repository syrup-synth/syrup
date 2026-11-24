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
		gPadBass = 0; float v; int y = PAD_Y + PAD_H / 2; padsTouch(48, 2 + 22, y, 1, &v); padsTouch(48, 2 + 45 + 22, y, 0, &v); padsTouch(48, 2 + 3 * 45 + 22, y, 0, &v); padsSettle(1);   // (a slide plays the pad it settles on)
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
	if (fr == 335) { gN3 = 1; gMacro[0] = 0.2f; c0 = gMacro[0]; if (gCurPage != 0) fail("expected the PATCH page (%d)", gCurPage); }
	if (fr >= 336 && fr < 350) { CSX = 150; }   // (up / down is the browser's on PATCH)
	if (fr == 350) { CSX = 0; if (gMacro[0] <= c0) fail("C-stick right on PATCH should raise MACRO 1 (%.3f -> %.3f)", c0, gMacro[0]); s0 = gSeqMode; }
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
		int n0 = gNFiles; kitToTable(); for (int t = 0; t < 400 && gWtJob; t++) usleep(10000);   /* (the table is made on the SAVER thread) */ if (gNFiles != n0 + 1 || gOsc[0].table < T_BUILTIN) fail("KIT > TABLE: a new table on OSC A (files %d -> %d)", n0, gNFiles);
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
	if (fr == 371) { gPads = 1; gPadBass = 2; gPadBase = 48; gVoiceMode = 0; gArp = 0; float v; padsTouch(48, 2 + 22, PAD_Y + PAD_H / 2, 1, &v); }
	if (fr == 375) { int want = padDeg(48, 0) - 12, got = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].note == want && gV[i].stage != 4) got = 1;
		if (!got) fail("BASS PULSE: the bass line should be playing the root an octave down (%d)", want); playSet(NULL, 0, 0.8f); gPads = 0; gPadBass = 0; gPadSel = -1; }
	// PANIC: a long pad, its reverb on FREEZE, grains looping: all gone at once
	if (fr == 376) { int st, ix; if (findPreset("SHIMMER CLOUD", &st, &ix)) presetLoad(st, ix); gRevMix = 0.6f; gMasterFX = 1; midiPush(0x90, 60, 110); midiPush(0x90, 67, 110); }
	if (fr == 420) { gFreeze = 1; }
	if (fr == 440) { midiPush(0x80, 60, 0); midiPush(0x80, 67, 0); panicAll(); pr0 = ON; }
	if (fr == 471) {   // CHORD SLIDE: a chord step with SLIDE glides into the next chord (no restrike), and a pad MORPH while step-recording marks it
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; gSeqTr = 0; gArp = 0; gScale = 0; sqAllOff(); gSlideOn = 0; for (int i = 0; i < NVS; i++) gV[i].active = 0;
		stepSetNote(0, 60, 1); stepSetNote(0, 64, 1); stepSetNote(0, 67, 1); gSeq[0].fl |= 2; gSeq[0].vel = 200;
		stepSetNote(1, 62, 1); stepSetNote(1, 65, 1); stepSetNote(1, 69, 1); gSeq[1].vel = 200;
		seqFireAt(0, 0, 0, 0); unsigned h0 = gSeqHits; seqFireAt(1, 1, 0, 0);
		if (gSeqHits != h0) fail("CHORD SLIDE: the next chord shouldn't restrike (%u new notes)", gSeqHits - h0);
		int want[3] = {62, 65, 69}, ok = 0; for (int w = 0; w < 3; w++) for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].note == want[w] && gV[i].slide == 2) { ok++; break; }
		if (ok != 3) fail("CHORD SLIDE: the voices should bend to D F A by MORPH (%d of 3)", ok);
		sqAllOff(); gSlideOn = 0;
		memset(gSeq, 0, sizeof(gSeq)); gSeqMode = 2; gRecPos = 0; gLiveRec = 0; gPadBass = 0; gHeldN = 0;
		int c1[3] = {60, 64, 67}, c2[3] = {62, 65, 69}; padPlay(48, c1, 3, 0.8f, 2); padPlay(48, c2, 3, 0.8f, 2);
		if (!(gSeq[0].fl & 2) || (gSeq[1].fl & 2)) fail("CHORD SLIDE REC: morphing pads should mark the first step SLIDE (%d %d)", gSeq[0].fl, gSeq[1].fl);
		gSeqMode = 0; playSet(NULL, 0, 0.8f);
	}
	// the phone remote: CHORDS page pads (CC 105, played by the main loop like a tap), KNOBS (CC 20-27: the page's settings)
	static float kv0;
	if (fr == 472) { gLearnLoaded = 1; memset(gLearnMap, 0, sizeof(gLearnMap)); gArp = 0; gVoiceMode = 0; playSet(NULL, 0, 0.8f); allOff(); midiPush(0xB0, 105, 1 + 4 * 3); const Param* kp = knobParam(gCurPage, 0); kv0 = kp ? paramNorm(kp) : -1; midiPush(0xB0, 20, kv0 > 0.5f ? 0 : 127); }
	if (fr == 475) { int any = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) any++;
		if (gPadSel != 4 || any < 3) fail("REMOTE PADS: CC 105 should play pad V (sel %d, %d voices)", gPadSel, any);
		const Param* kp = knobParam(gCurPage, 0); if (!kp || fabsf(paramNorm(kp) - kv0) < 0.3f) fail("REMOTE KNOBS: CC 20 should turn the page's first setting (%s)", kp ? kp->name : "none");
		midiPush(0xB0, 105, 0); }
	if (fr == 478) { int any = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) any++; if (any) fail("REMOTE PADS: CC 105 0 should let go (%d)", any); }
	if (fr == 479) {   // the drum grid: one tap on an empty grid is ONE hit (a factory row used to paste itself first); ACC accents one hit
		myDrumLoad(); memset(gMyDrum, 0, sizeof(gMyDrum)); memset(gDrumX, 0, sizeof(gDrumX)); memset(gDrAcc, 0, sizeof(gDrAcc)); gKickPat = MINE_IDX[0]; gSnarePat = MINE_IDX[1]; gHatPat = MINE_IDX[2]; gDgPg = 0;   // an empty grid, as it looks
		 gAccMode = 0;
		int cx = DG_LX + (int)(4.5f * (320 - DG_LX - 2) / 16.0f), cy = DG_TOP + DG_RH / 2;
		drumGridTouch(cx, cy, 1); drumGridRelease();
		int n = 0; for (int r = 0; r < 3; r++) for (int i = 0; i < DSTEP; i++) n += gMyDrum[r][i] != 0;
		if (n != 1 || !gMyDrum[0][4]) fail("DRUM GRID: one tap on an empty grid should add one hit (%d hits)", n);
		gAccMode = 1; drumGridTouch(cx, cy, 1); drumGridRelease(); gAccMode = 0;
		int na = 0; for (int r = 0; r < 10; r++) for (int i = 0; i < DSTEP; i++) na += gDrAcc[r][i];
		if (!gDrAcc[0][4] || na != 1) fail("ACCENT: ACC + a tap should accent that one hit (%d accents)", na);
	}
	if (fr == 470) { double r = rmsOf(pr0 + SEC(0.3), ON); if (r > 40) fail("PANIC: everything should be silent half a second later (%.0f rms)", r); if (gFreeze) fail("PANIC should turn FREEZE off"); }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 482; app_main(); printf("chord: NEW 3DS controls, stick in menus, chord types, MIDI chords, live chord REC, pads + BASS, progressions, hold over patch change\n"); return gFails ? 1 : 0; }
