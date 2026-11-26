// YOUR OWN BEAT IS SAFE: BEAT presets step freely over an empty grid or over the last preset, but a grid you've
// edited needs a second pick to be replaced (SELECT+L still undoes). The drum SOUND's RESET needs a second tap.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		myDrumLoad(); memset(gMyDrum, 0, sizeof gMyDrum); memset(gDrumX, 0, sizeof gDrumX);
		if (!beatMayPaste()) fail("BEAT: an empty grid should take a preset at once"); beatPaste(2);
		if (!beatMayPaste()) fail("BEAT: the last preset should be replaced at once"); beatPaste(3);
		gMyDrum[0][5] = 200;   // your own edit
		u32 before = drumGridHash();
		if (beatMayPaste()) fail("BEAT: your own beat should be asked about first");
		if (drumGridHash() != before) fail("BEAT: the first pick should leave your beat alone");
		if (!strstr(gMsg, "AGAIN")) fail("BEAT: should say how to replace it (%s)", gMsg);
		if (!beatMayPaste()) fail("BEAT: a second pick should replace it"); beatPaste(4);
		// RESET: the first tap only asks
		gDV[0].decay = 150; gDgEd = 2; gDgEdR = 0;
	}
	if (fr == 5) { dgEdTouch(220, DE_Y + 6, 1); if (gDV[0].decay != 150) fail("RESET: one tap shouldn't reset the sound"); dgEdTouch(220, DE_Y + 6, 1); if (gDV[0].decay != 150) fail("RESET: a bounce (two presses at once) shouldn't reset it"); }
	if (fr == 30) { dgEdTouch(220, DE_Y + 6, 1); if (gDV[0].decay == 150) fail("RESET: the second tap should reset it"); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 34; app_main(); if (!done) fail("didn't run"); printf("beatsafe: own beat asked first, RESET two taps\n"); return gFails ? 1 : 0; }
