// PARTS: the strip under the SEQ roll picks SYNTH / CHORD / BASS / DRUMS. CHORD: a tap in the roll sets that bar's chord
// (a CUSTOM progression that plays on the bars); BASS: a tap writes a chord tone (a USER line); a held part mutes it;
// DRUMS goes to the DRUMS page; the parts are saved in seq.bin
#include "syrup_host.c"
#include "harness.h"
#define STRIP(k) do { TOUCH = 1; TX = (k) * 80 + 40; TY = PART_Y + 5; } while (0)
static int done = 0, root, heard = 0, bassHeard = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 5 || fr == 6) { TOUCH = 1; TX = 5 * 320 / 8 + 20; TY = 8; }   // SEQ chip
	if (fr == 10) { gScale = 0; gRoot = 0; gPadBase = 48; gProg = 0; gProgAuto = 0; gPadBass = 0; memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; stepSetNote(0, 72, 1); stepSetNote(8, 74, 1); root = padDeg(gPadBase, 0); gBPM = 120; }
	if (fr == 12) STRIP(1);
	if (fr == 16) { if (gSeqPart != 1) fail("PARTS: a tap on CHORD should pick it (%d)", gSeqPart); if (PAGES[7] != P_PCHORD) fail("PARTS: the list on top should be the CHORD part's"); shotBot("shots/part_chord_empty.ppm"); }
	// the grid (a page = a bar, 16 steps): bar 1 = I for 8 steps then iii for 8 (drag its length), bar 2 = vi (a tap: 4 steps)
#define CELL(i, d) do { TOUCH = 1; TX = CG_KW + (int)((i) * CG_CW) + 4; TY = CG_Y + (6 - (d)) * CG_RH + 6; } while (0)
	if (fr == 20) CELL(0, 0);
	if (fr == 21) CELL(4, 0); if (fr == 22) CELL(6, 0); if (fr == 23 || fr == 24) CELL(7, 0);
	if (fr == 27) { if (gProg != PROG_CUSTOM || gCtDeg[0] != 0 || gCtLen[0] != 8 || !gProgAuto) fail("CHORD: I for 8 steps in the part's track (%d %d %d)", gProg, gCtDeg[0], gCtLen[0]); }
	if (fr == 28) CELL(8, 2);
	if (fr == 29) CELL(15, 2);
	if (fr == 32) { if (gCtDeg[8] != 2 || gCtLen[8] != 8) fail("CHORD: iii on step 9 for 8 steps (%d %d)", gCtDeg[8], gCtLen[8]); gRollPg = 1; }
	if (fr == 34) CELL(0, 5);
	if (fr == 37) { if (gCtDeg[16] != 5 || gCtLen[16] != 4) fail("CHORD: a tap is a 4-step chord (%d %d)", gCtDeg[16], gCtLen[16]); }
	if (fr == 38) CELL(1, 5);   // tap the chord: gone
	if (fr == 41) { if (gCtDeg[16] != -1) fail("CHORD: a tap on a chord should take it out"); gRollPg = 1; }
	if (fr == 42) CELL(0, 5);
	if (fr == 43) CELL(15, 5);   // vi for the whole bar
	if (fr == 46) { if (gCtLen[16] != 16) fail("CHORD: vi should fill bar 2 (%d)", gCtLen[16]); shotBot("shots/part_chord.ppm"); shotTop("shots/part_chord_top.ppm"); }
	// BASS: an R on step 1 of bar 2 and a 5 on step 9
	if (fr == 47) STRIP(2);
	if (fr == 50) { if (gSeqPart != 2) fail("PARTS: BASS should be picked"); gRollPg = 1; int r = partBassNote(5, 'R'); TOUCH = 1; TX = rollX(16) + 4; TY = rollY(r) + 4; if (rollRow(r) < 0) fail("BASS: the root should be on screen (%d, low %d)", r, gRollLow); }
	if (fr == 53) gRollLow = partBassNote(5, 'R') + 1;   // scroll up a little (the keys drag)
	if (fr == 54) { TOUCH = 1; TX = rollX(24) + 4; TY = rollY(partBassNote(5, '5')) + 4; }
	if (fr == 58) { if (gPadBass != BASS_USER || gBassUser[0] != 'R' || gBassUser[8] != '5') fail("BASS: a USER line with R and 5 (%d %s)", gPadBass, gBassUser); shotBot("shots/part_bass.ppm"); shotTop("shots/part_bass_top.ppm"); }
	// play: the chord part's chords and the bass sound on the bars
	if (fr == 60) { gVoiceMode = 1; transportToggle(); }   // a MONO lead patch: the chords and the bass still play, on their own voices
	if (fr > 60 && fr < 200) { int c = 0, b = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].stage != 4) { if (gV[i].part == 1) c++; if (gV[i].part == 2) b++; } if (c >= 3) heard = 1; if (b) bassHeard = 1; }
	if (fr == 200) { gVoiceMode = 0; if (!heard) fail("PLAY: the chords should sound (3 voices) on a mono patch"); if (!bassHeard) fail("PLAY: the bass should sound on its own voice"); }
	// hold SYNTH: muted (the strip stays where it is)
	if (fr >= 202 && fr < 240) STRIP(0);
	if (fr == 242) { if (!gPartMute[0] || gSeqPart != 2) fail("MUTE: holding SYNTH should mute it and stay on BASS (%d %d)", gPartMute[0], gSeqPart); shotBot("shots/part_mute.ppm"); }
	if (fr == 244) { gSeqSaveDue = 0; seqSave(); gCtDeg[16] = -1; gBassUser[8] = '-'; gPartMute[0] = 0; seqLoad(); if (gCtDeg[16] != 5 || gBassUser[8] != '5' || !gPartMute[0]) fail("SAVE: seq.bin should keep the parts"); }
	if (fr == 250) STRIP(3);
	if (fr == 256) { if (gSeqPart != 3 || !gDrumOnB) fail("PARTS: DRUMS should open the DRUMS page (%d %d)", gSeqPart, gDrumOnB); shotBot("shots/part_drums.ppm"); shotTop("shots/part_drums_top.ppm"); }
	if (fr == 260) STRIP(0);
	if (fr == 266) { if (gSeqPart != 0 || !gGridOn) fail("PARTS: SYNTH should go back to the roll"); gRollLow = 47; shotBot("shots/part_synth.ppm"); shotTop("shots/part_synth_top.ppm"); }
	// RIFF: with chords, 4 bars, every note in the chord under it
	if (fr == 267) { gPartMute[0] = 0; TOUCH = 1; TX = 6 * 320 / GRID_BTNS + 10; TY = XY_Y + 5; }
	if (fr == 268) { int bad = 0, n = 0; for (int st = 0; st < 64; st++) for (int k = 0; k < gSeq[st].n; k++) { n++; int pc = ((gSeq[st].note[k] - gRoot) % 12 + 12) % 12; if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10) bad++; if (partDegAt(st) < 0) bad++; }
		if (gSeqLen != 64 || !n || bad) fail("RIFF: 4 bars, in key, only under chords (len %d, %d notes, %d off)", gSeqLen, n, bad); }
	if (fr == 269) shotBot("shots/part_riff.ppm");
	// FLAVOR: back to CHORD, REC on, the transport playing: hold V, push the stick up (7th) - written as long as it's held
	// NUDGE: the last note written moves a step with <NDG / NDG>
	if (fr == 269) { memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; stepSetNote(4, 60, 1); gSeq[4].vel = 150; gNdSt = 4; gNdNt = 60; gridButtons(3 * 320 / GRID_BTNS + 5); gridButtons(3 * 320 / GRID_BTNS + 5); gridButtons(2 * 320 / GRID_BTNS + 5);
		if (!stepHasNote(5, 60) || stepHasNote(4, 60) || gSeq[5].vel != 150) fail("NUDGE: the note should be one step later, velocity kept"); }
	if (fr == 270) STRIP(1);
	if (fr == 273) { memset(gCtDeg, -1, sizeof(gCtDeg)); ctSet(8, 3, 4, 0); gCgSel = 8; nudgeDo(-1); nudgeDo(-1); if (gCtDeg[6] != 3 || gCtLen[6] != 4 || gCtDeg[8] != -1) fail("NUDGE: the chord should be 2 steps earlier (%d %d)", gCtDeg[6], gCtLen[6]); }
	if (fr == 274) { memset(gCtDeg, -1, sizeof(gCtDeg)); gLiveRec = 1; gRollPg = 0; }
	if (fr >= 276 && fr < 300) { TOUCH = 1; TX = 10; TY = CG_Y + (6 - 4) * CG_RH + 6; if (fr > 280) CY = 120; }
	if (fr == 290) { if (gCgBtn != 4 || gCgQ != 1) fail("FLAVOR: holding V with the stick up = V7 (%d %d)", gCgBtn, gCgQ); shotBot("shots/part_hichord.ppm"); shotTop("shots/part_hichord_top.ppm"); }
	if (fr == 302) { CY = 0; gLiveRec = 0; int a = -1; for (int i = 0; i < CT_N; i++) if (gCtDeg[i] == 4) a = i; if (a < 0) fail("FLAVOR: REC should write the held chord");
		else if (gCtLen[a] < 3 || gCtLen[a] > 5 || gCtQ[a] != 1) fail("FLAVOR: as long as it was held (~3 steps), a 7th (%d %d)", gCtLen[a], gCtQ[a]); transportToggle(); done = 1; }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 306; app_main(); if (!done) fail("didn't run"); printf("parts: CHORD bars, BASS line, mute, DRUMS, saved\n"); return gFails ? 1 : 0; }
