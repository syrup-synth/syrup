// PATTERN LOCK: PAT > LOCK keeps a pattern as it is: an edit, RIFF, CLEAR or a COPY> onto it is put back; the tools say
// it's locked; unlock and it changes again; the locks come back after a restart; a new session clears them.
#include "syrup_host.c"
#include "harness.h"
static int done = 0; static Step keep[NSTEP]; static int keepLen;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) { patSwitch(0); seqClear(); gSeq[0].n = 1; gSeq[0].note[0] = 60; gSeq[0].vel = 200; gSeq[4].n = 1; gSeq[4].note[0] = 64; gSeq[4].vel = 200; gSeqLen = 16;
		patLockSet(0, 1); memcpy(keep, gSeq, sizeof keep); keepLen = gSeqLen;
		gSeq[8].n = 1; gSeq[8].note[0] = 67; acidGen(); }   // an edit and a RIFF on the locked pattern
	if (fr == 5) { if (memcmp(keep, gSeq, sizeof keep) || gSeqLen != keepLen) fail("LOCK: the locked pattern should be put back");
		patStore(); gPat[1] = gPat[0]; patSwitch(1); gSeq[2].n = 1; gSeq[2].note[0] = 50; patStore(); gPat[0] = gPat[1]; }   // a COPY> onto it, from B
	if (fr == 7) { if (memcmp(gPat[0].st, keep, sizeof keep)) fail("LOCK: a copy onto the locked pattern should be undone"); patSwitch(0); patLockSet(0, 0); gSeq[8].n = 1; gSeq[8].note[0] = 67; }
	if (fr == 9) { if (gSeq[8].n != 1) fail("UNLOCKED: it should change again"); patLockSet(0, 1); memset(gPatLock, 0, sizeof gPatLock); patLockLoad(); if (!gPatLock[0]) fail("LOCK: should come back after a restart"); sessionClear(); if (gPatLock[0]) fail("a new session should clear the locks"); done = 1; }
}
int main(void) { mkdir("sd", 0777); remove("sd/patlock.bin"); loadFont(); FRMAX = 11; app_main(); if (!done) fail("didn't run"); printf("patlock: pattern lock\n"); return gFails ? 1 : 0; }
