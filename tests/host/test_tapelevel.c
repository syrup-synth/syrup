// TAPE LEVEL: a take plays back as loud as it went down (at the 80% level tracks start at), and the SPEAKER EQ
// stays off the take (CHIP > FX put the SUB through its high-pass: thin tapes)
#include "syrup_host.c"
#include "harness.h"
static void tap(int fr, int at, int x, int y) { if (fr == at || fr == at + 1) { TOUCH = 1; TX = x; TY = y; } }
static long s0, s1, p0, p1; static const char* PN;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0; gHeadphones = 0;
	if (fr == 2) { int st, ix; if (findPreset(PN, &st, &ix)) presetLoad(st, ix); else fail("no patch");
		gBPM = 120; gTapeLenSel = 5; gDrums = 0; gPreroll = 0; seqClear(); gSeqLen = 16; for (int k = 0; k < 16; k += 4) { stepSetNote(k, 48, 1); stepSetLen(k, 48, 3); stepSetNote(k, 55, 1); } }
	tap(fr, 3, 5 * 320 / 8 + 20, 8); tap(fr, 6, 1 * 320 / 7 + 20, XY_Y + 5); tap(fr, 9, 160, PV_TAPE_Y + 8);
	tap(fr, 13, 2 * 63 + 30, TP_Y + 8);
	if (fr == 60) s0 = ON; if (fr == 400) s1 = ON;
	if (fr == 520) { if (!gTpHave[0]) fail("no track"); seqClear(); }
	tap(fr, 522, 3 * 63 + 30, TP_Y + 8);
	if (fr == 560) p0 = ON; if (fr == 900) p1 = ON;
}
int main(int c, char** v) { (void)c; (void)v; PN = "DARK 808"; remove("sd/settings.bin"); mkdir("sd", 0777); loadFont(); FRMAX = 905; app_main();
	double a = rmsOf(s0, s1), b = rmsOf(p0, p1); double db = 20 * log10(b / a); if (fabs(db) > 1.0) fail("tape level: the take should play back as loud as it went down (%.1f dB)", db); printf("tapelevel: take vs live %.1f dB\n", db); return gFails ? 1 : 0; }
