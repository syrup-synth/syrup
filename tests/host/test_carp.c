// CHORD ARP: the CHORD part's chords one note at a time - UP walks the chord low to high, 2 OCT doubles it an octave up,
// only one chord-part voice sounds at once; OFF goes back to the held chord; seq.bin keeps the setting
#include "syrup_host.c"
#include "harness.h"
static int done = 0, seen[128], maxOn = 0, order[64], no = 0, lastN = -1;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 5) { gScale = 0; gRoot = 0; gPadBase = 48; gProg = PROG_CUSTOM; gProgAuto = 1; memset(gCtDeg, -1, sizeof(gCtDeg)); ctSet(0, 0, 64, 0);
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; gBPM = 120; gCtArp = 1; gCtArpRate = 2; gCtArpOct = 1; transportToggle(); }
	if (fr > 8 && fr < 120) {
		int on = 0, nt = -1; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].stage != 4 && gV[i].stage != 3 && gV[i].part == 1) { on++; nt = gV[i].note; }
		if (on > maxOn) maxOn = on;
		if (nt >= 0) { seen[nt] = 1; if (nt != lastN && no < 64) order[no++] = nt; lastN = nt; }
	}
	if (fr == 120) {
		int n = 0; for (int i = 0; i < 128; i++) n += seen[i];
		if (maxOn > 1) fail("ARP: one chord-part note at a time (%d)", maxOn);
		if (n < 6) fail("ARP: 2 OCT should play the chord's notes and their octaves (%d different)", n);
		int up = 0; for (int i = 1; i < no && i < 6; i++) if (order[i] > order[i - 1]) up++;
		if (up < 4) fail("ARP UP: the first notes should climb (%d of 5)", up);
		gCtArp = 0; partChordPlay(gPadNotes, gPadN, 0.8f);
	}
	if (fr == 130) { int on = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].stage != 4 && gV[i].part == 1) on++; if (on < 3) fail("ARP OFF: the chord holds again (%d)", on); }
	if (fr == 132) { transportToggle(); gCtArp = 3; gCtArpRate = 4; gCtArpOct = 2; gSeqSaveDue = 0; seqSave(); gCtArp = 0; gCtArpRate = 0; gCtArpOct = 0; seqLoad();
		if (gCtArp != 3 || gCtArpRate != 4 || gCtArpOct != 2) fail("SAVE: seq.bin should keep the CHORD ARP (%d %d %d)", gCtArp, gCtArpRate, gCtArpOct); gCtArp = 1; gCtArpRate = 2; gProgAuto = 1; transportToggle(); }
	if (fr == 150) panicAll();
	if (fr == 165) { int on = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].stage != 4 && gV[i].part == 1) on++; if (on || gCtArpN) fail("PANIC: the chord arp should stop (%d voices, %d notes)", on, gCtArpN); transportToggle(); gCtArp = 0; done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 170; app_main(); if (!done) fail("didn't run"); printf("carp: CHORD ARP up, octaves, off, saved, panic\n"); return gFails ? 1 : 0; }
