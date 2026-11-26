// Release-review fixes: SEQ RATE changed mid-play keeps the transport where it was (no jump for synced LFOs / arp /
// gate); the arp plays its first step again after STOP + PLAY; FX clears asked for by the main loop are done by the
// effects path; a user patch save happens with the voice lock let go.
#include "syrup_host.c"
#include "harness.h"
static int arpNotes = 0, done = 0; static float before = -1;
static void hook(int part, int note) { (void)note; if (part == 0 && gArp) arpNotes++; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) { gOnsetHook = hook; gBPM = 120; gSwing = 0.2f; memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; gSeqRate = 0; transportToggle(); }
	// 1. SEQ RATE mid-play
	if (fr == 40) { before = transportPos16(); gSeqRate = 1; }
	if (fr == 41) { float after = transportPos16(), dt = 1.0f / 60 * 120 / 60.0f * 4;   // one frame is ~0.13 of a 16th
		printf("fixes: SEQ RATE 1/16 -> 1/32 mid-play: %.3f -> %.3f 16ths\n", before, after);
		if (after < before || after - before > dt * 2.5f) fail("SEQ RATE: the transport jumped (%.3f -> %.3f)", before, after); }
	if (fr == 60) { before = transportPos16(); gSeqRate = 2; }
	if (fr == 61) { float after = transportPos16(); printf("fixes: SEQ RATE 1/32 -> 1/8: %.3f -> %.3f\n", before, after);
		if (after < before || after - before > 0.5f) fail("SEQ RATE: the transport jumped (%.3f -> %.3f)", before, after); gSeqRate = 0; }
	// 2. the arp's first step after STOP + PLAY (driven by hand: the host's keyboard state would let the held chord go)
	if (fr == 70) {
		transportToggle(); seqTick(0.0001f);   // stop
		gArp = 1; gArpRate = 2; gArpPool[0] = 60; gArpPool[1] = 64; gArpPool[2] = 67;
		transportToggle(); seqTick(0.0001f); gArpN = 3; gArpFresh = 0; arpNotes = 0; arpTick(0.0001f);   // play: step 0 of the arp (the chord held through it)
		int first = arpNotes;
		transportToggle(); seqTick(0.0001f);   // stop inside that first step, chord still held
		transportToggle(); seqTick(0.0001f); gArpN = 3; gArpFresh = 0; arpNotes = 0; arpTick(0.0001f);   // play again
		int again = arpNotes; arpTick(0.0001f); int twice = arpNotes;
		printf("fixes: arp first step: %d note(s) on PLAY, %d on PLAY again, %d after a second tick\n", first, again, twice);
		if (again < 1) fail("ARP: the first step after STOP + PLAY should play");
		if (twice != again) fail("ARP: the first step played twice");
		transportToggle(); seqTick(0.0001f); gArpN = 0; gArp = 0;
	}
	// 3. FX clears are done by the effects path
	if (fr == 82) { fxTailsClear(); ofReset(); if (!gFxClrReq) fail("FX: the main loop should only ask for the clear"); }
	if (fr == 86) { if (gFxClrReq) fail("FX: the effects path should have done the clear (%d left)", gFxClrReq); }
	// 4. a user patch save: asked under the lock, written after
	if (fr == 88) { patchCapture(&gUser[0]); gUserSaveDue = 1; }
	if (fr == 91) { if (gUserSaveDue) fail("USER SAVE: should have been written by now"); FILE* f = fopen("sd/user_presets.bin", "rb"); if (!f) fail("USER SAVE: no file"); else fclose(f); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 94; app_main(); if (!done) fail("didn't run"); printf("fixes: rate change, arp restart, FX clears, user save\n"); return gFails ? 1 : 0; }
