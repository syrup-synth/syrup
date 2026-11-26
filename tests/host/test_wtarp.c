// WT ARP: each arp step moves OSC A's WT POS to the next of WT STEPS positions; UP / DOWN / UP+DOWN walk them in order,
// WT DEPTH scales how far, the table follows in a frame or two (no glide), and the setting saves with the patch.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		int notes[2] = {60, 64}; gArp = 1; gArpOct = 1; gArpN = 2; gArpPool[0] = 60; gArpPool[1] = 64;
		gWtArp = 1; gWtArpSteps = 4; gWtArpDepth = 1; gWtArpIdx = 0; (void)notes;
		float want[6] = {0, 1 / 3.0f, 2 / 3.0f, 1, 0, 1 / 3.0f};
		for (int i = 0; i < 6; i++) { arpStep(); if (fabsf(gWtArpOff - want[i]) > 0.01f) fail("WT ARP UP: step %d at %.2f, wanted %.2f", i, gWtArpOff, want[i]); }
		gWtArp = 2; gWtArpIdx = 0; arpStep(); if (fabsf(gWtArpOff - 1) > 0.01f) fail("WT ARP DOWN should start at the top (%.2f)", gWtArpOff);
		gWtArp = 3; gWtArpIdx = 0; gWtArpDir = 1; float ud[7], wud[7] = {0, 1 / 3.0f, 2 / 3.0f, 1, 2 / 3.0f, 1 / 3.0f, 0};
		for (int i = 0; i < 7; i++) { arpStep(); ud[i] = gWtArpOff; if (fabsf(ud[i] - wud[i]) > 0.01f) fail("WT ARP UP+DOWN step %d %.2f (want %.2f)", i, ud[i], wud[i]); }
		gWtArp = 1; gWtArpDepth = 0.5f; gWtArpIdx = 3; arpStep(); if (fabsf(gWtArpOff - 0.5f) > 0.01f) fail("WT DEPTH 50%%: the last step should be at 0.5 (%.2f)", gWtArpOff);
		allOff(); gArpOnN = 0;
		// the table follows: OSC A's position moves with the step
		gOsc[0].pos = 0; gWtArpDepth = 1; gWtArpIdx = 3; arpStep();
	}
	if (fr > 3 && fr < 11) { gMidiHeldN = 2; gArp = 1; gArpN = 2; gArpPool[0] = 60; gArpPool[1] = 64; gWtArp = 1; gWtArpSteps = 4; gWtArpIdx = 3; arpStep(); }
	static int hit = 0; if (fr > 4 && fr < 12 && gPosEff[0] > 0.5f) hit = 1;   // (OSC A's own WT POS is 0: only the step lifts it)
	if (fr == 12) { if (!hit) fail("WT ARP: OSC A's table should follow the step's position (%.2f)", gPosEff[0]);
		SPatch p; patchCapture(&p); gWtArp = 0; gWtArpSteps = 9; patchApply(&p);
		if (gWtArp != 1 || gWtArpSteps != 4) fail("WT ARP: should save with the patch (%d %d)", gWtArp, gWtArpSteps);
		gMidiHeldN = 0; gArp = 0; gArpN = 0; allOff(); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 14; app_main(); if (!done) fail("didn't run"); printf("wtarp: the wavetable arpeggiator\n"); return gFails ? 1 : 0; }
