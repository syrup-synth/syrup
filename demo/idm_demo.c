// "SYRUP IDM": an Aphex-Twin-flavoured demo made on Syrup's TAPE, four takes over one 60-bar song chain, F minor, 156 BPM.
//   track 1  drums (CHIP > FX) + ANALORD acid        drill'n'bass: 32nd-note snare rushes (RATE X2), ratchets, pitched-down rolls (step TUNE)
//   track 2  SAW 85 pad -> WARP STRINGS in the break
//   track 3  MUSIC BOX melody -> TOY BOX in the break -> SQUELCH lead in drill 2
//   track 4  DEEP SUB -> PREPARED arps in the break
#include "syrup_host.c"
#include "harness.h"
#define BPM 156
#define BARS 60
static int PART = 0, passStart = -1, done = 0, DRUMONLY = 0;
static void note(int st, int n, int len, int vel) { stepSetNote(st, n, 1); stepSetLen(st, n, len); gSeq[st].vel = (u8)vel; }
static void clearPat(void) { memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; }
enum { S_INTRO, S_A, S_DRILL1, S_BREAK, S_DRILL2, S_OUTRO };
static const int SEC0[7] = {0, 8, 16, 32, 40, 56, 60};
static int secOf(int bar) { for (int i = 0; i < 6; i++) if (bar < SEC0[i + 1]) return i; return S_OUTRO; }
static int slow(int s) { return s == S_INTRO || s == S_A || s == S_BREAK || s == S_OUTRO; }
static int chordOf(int bar) { int s = secOf(bar), in = bar - SEC0[s]; return slow(s) ? (in / 2) % 4 : in % 4; }   // 0 Fm9 1 Dbmaj9 2 Bbm9 3 C7sus
enum { F1 = 29, C2 = 36, Db2 = 37, F2 = 41, Ab2 = 44, Bb2 = 46, C3 = 48, Db3 = 49, Eb3 = 51, E3 = 52, F3 = 53, G3 = 55, Ab3 = 56, Bb3 = 58, C4 = 60, Db4 = 61, Eb4 = 63, E4 = 64, F4 = 65, G4 = 67,
	Ab4 = 68, Bb4 = 70, C5 = 72, Db5 = 73, Eb5 = 75, E5 = 76, F5 = 77, G5 = 79, Ab5 = 80, Bb5 = 82, C6 = 84, Db6 = 85, Eb6 = 87, F6 = 89 };
static const int ROOT[4] = {F2, Db2 + 12, Bb2 - 12 + 12, C3 - 12 + 12};
static const int PADV[4][4] = {{Ab3, C4, Eb4, G4}, {F3, Ab3, C4, Eb4}, {Ab3, C4, Db4, F4}, {Bb3, C4, F4, G4}};
static unsigned rs = 12345; static int rnd(int n) { rs = rs * 1103515245u + 12345u; return (int)((rs >> 16) % (unsigned)n); }

// ---------------- track 1: acid ----------------
static const char* AT[2][16] = {
	{"Ra", "R", "O", "-", "7s", "R", "3a", "-", "R", "Os", "5", "R", "3a", "-", "5s", "O"},
	{"Ras", "O", "R", "7", "Ra", "3s", "5", "O", "Ra", "-", "b9", "R", "Os", "7", "5a", "3s"} };
static void buildAcid(int tpl, int ch) {
	clearPat(); int r = (ch == 0 ? F2 : ch == 1 ? Db2 + 12 : ch == 2 ? Bb2 - 12 + 12 : C3 - 12) - 12 + 12;
	r = ch == 0 ? F2 : ch == 1 ? Db3 - 12 : ch == 2 ? Bb2 - 12 : C3 - 12; r += 12;
	for (int k = 0; k < 16; k++) {
		const char* c = AT[tpl][k]; if (c[0] == '-') continue;
		int iv = c[0] == 'O' ? 12 : c[0] == '5' ? 7 : c[0] == '3' ? (ch == 3 ? 5 : ch == 1 ? 4 : 3) : c[0] == '7' ? (ch == 1 ? 11 : 10) : c[0] == 'b' ? 13 : 0;
		int a = strchr(c, 'a') != NULL, s = strchr(c, 's') != NULL;
		note(k, r + iv, s ? 2 : 1, a ? 255 : 150); if (a) gSeq[k].fl |= 1; if (s) gSeq[k].fl |= 2;
	}
}
static int acidPat(int bar) { int s = secOf(bar), ch = chordOf(bar); if (s == S_DRILL1) return ch; if (s == S_DRILL2) return 4 + ch; if (s == S_A && bar - SEC0[s] >= 4) return ch; return 8; }

// ---------------- track 2: pads ----------------
static void buildPad(int i) { clearPat(); if (i >= 1 && i <= 4) for (int k = 0; k < 4; k++) note(0, PADV[i - 1][k], 16, 190 + k * 8);
	if (i >= 5 && i <= 8) { static const int ST[5] = {0, 3, 6, 10, 12}, LN[5] = {2, 2, 3, 1, 3}; for (int j = 0; j < 5; j++) for (int k = 0; k < 4; k++) note(ST[j], PADV[i - 5][k], LN[j], j ? 175 : 215); } }   // drills: offbeat stabs
static int padPat(int bar) { int s = secOf(bar); if (s == S_OUTRO && bar >= 58) return 1; return (s == S_DRILL1 || s == S_DRILL2 ? 5 : 1) + chordOf(bar); }

// ---------------- track 3: melody ----------------
static void buildMel(int i) {
	clearPat();
	switch (i) {   // 1-4 the music box tune (one bar per chord), 5-8 a busier second half, 9-12 toy-box arps, 13-16 squelch
	case 1: note(0, C5, 4, 220); note(4, Ab4, 2, 180); note(6, G4, 2, 170); note(8, Ab4, 4, 200); note(12, C5, 2, 190); note(14, Eb5, 2, 200); break;
	case 2: note(0, F5, 6, 230); note(6, Eb5, 2, 180); note(8, C5, 4, 200); note(12, Ab4, 4, 180); break;
	case 3: note(0, Db5, 4, 220); note(4, C5, 2, 180); note(6, Bb4, 2, 170); note(8, Ab4, 4, 200); note(12, F4, 4, 180); break;
	case 4: note(0, G4, 6, 210); note(6, Ab4, 2, 170); note(8, Bb4, 4, 190); note(12, C5, 2, 180); note(14, E5, 2, 200); break;
	case 5: note(0, C6, 2, 220); note(2, Ab5, 1, 160); note(3, G5, 1, 150); note(4, Ab5, 2, 190); note(6, C5, 2, 170); note(8, Eb5, 3, 200); note(11, F5, 1, 160); note(12, G5, 2, 190); note(14, Ab5, 2, 180); break;
	case 6: note(0, F5, 2, 220); note(2, Ab5, 2, 180); note(4, C6, 4, 210); note(8, Eb6, 2, 200); note(10, Db6, 2, 170); note(12, C6, 4, 190); break;
	case 7: note(0, Db6, 3, 220); note(3, C6, 1, 160); note(4, Bb5, 2, 180); note(6, Ab5, 2, 170); note(8, F5, 2, 200); note(10, Ab5, 2, 170); note(12, Db5, 4, 190); break;
	case 8: note(0, C6, 2, 210); note(2, Bb5, 2, 180); note(4, G5, 4, 190); note(8, E5, 2, 180); note(10, F5, 2, 170); note(12, G5, 2, 190); note(14, C6, 2, 210); break;
	case 9: case 10: case 11: case 12: { const int* v = PADV[i - 9];   // 32nd-feel arps: up an octave every other step, a skip of a step
		for (int k = 0; k < 16; k++) { if (k % 7 == 6) continue; note(k, v[(k * 3) % 4] + 12 + (k & 4 ? 12 : 0), 1, k % 4 ? 160 : 220); } } break;
	case 13: case 14: case 15: case 16: { static const int L[4][16] = {
			{F4, -1, Ab4, C5, -1, Eb5, C5, Ab4, G4, -1, -1, Ab4, C5, Eb5, F5, G5}, {Ab5, -1, F5, -1, Eb5, C5, -1, Ab4, F4, Ab4, C5, F5, -1, Eb5, C5, -1},
			{Db5, F5, -1, Ab5, F5, Db5, -1, C5, Bb4, -1, Ab4, F4, Ab4, Bb4, C5, Db5}, {C5, -1, E5, G5, -1, Bb5, G5, E5, C5, -1, Bb4, G4, -1, E4, F4, G4}};
		for (int k = 0; k < 16; k++) if (L[i - 13][k] >= 0) { note(k, L[i - 13][k], 1, k % 4 == 0 ? 250 : 190); if (k % 5 == 3) gSeq[k].fl |= 2; } } break;
	}
}
static int melPat(int bar) {
	int s = secOf(bar), ch = chordOf(bar), in = bar - SEC0[s];
	if (s == S_INTRO) return in >= 2 && (in & 1) == 0 ? 1 + ch : 0;   // the tune comes in on the chord changes
	if (s == S_A) return (in & 1) == 0 ? 1 + ch : 0;
	if (s == S_DRILL1) return in < 8 ? 1 + ch : 5 + ch;
	if (s == S_BREAK) return 9 + ch;
	if (s == S_DRILL2) return 13 + ch;
	return bar == 56 ? 1 : bar == 58 ? 3 : 0;
}
// ---------------- track 4: sub / prepared ----------------
static void buildSubT(int i) {
	clearPat(); static const int R[4] = {F2, Db2 + 12, Bb2 - 12, C2 + 12};
	if (i >= 1 && i <= 4) note(0, R[i - 1], 16, 220);
	if (i >= 5 && i <= 8) { int r = R[i - 5]; note(0, r, 3, 240); note(3, r, 2, 200); note(6, r + 12, 1, 190); note(7, r, 3, 230); note(11, r, 1, 200); note(12, r + 7, 2, 210); note(14, r, 2, 220); }
	if (i >= 9 && i <= 12) { const int* v = PADV[i - 9]; int seq[8] = {0, 2, 1, 3, 2, 0, 3, 1}; for (int k = 0; k < 8; k++) note(k * 2, v[seq[k]] - 12 + (k > 4 ? 12 : 0), 2, 170 + (k & 1) * 40); }
}
static int subPat(int bar) {
	int s = secOf(bar), ch = chordOf(bar), in = bar - SEC0[s];
	if (s == S_INTRO) return in >= 4 ? 1 + ch : 0;
	if (s == S_A) return 1 + ch;
	if (s == S_DRILL1 || s == S_DRILL2) return 5 + ch;
	if (s == S_BREAK) return 9 + ch;
	return bar < 58 ? 1 + ch : 0;
}
// ---------------- drums (track 1), rewritten every bar ----------------
static void row(int r, const char* p, int L, int rate) {   // p: one char a step: . rest, 1-9 velocity, ratchets as r/t/q (2/3/4 hits), lowercase pitched down
	u8* v = r < 3 ? gMyDrum[r] : gDrumX[r - 3]; memset(v, 0, DSTEP); memset(gDrRat[r], 0, DSTEP); memset(gDrTn[r], 0, DSTEP); memset(gDrCond[r], 0, DSTEP);
	gDrLen[r] = (u8)L; gDrRate[r] = (u8)rate;
	for (int i = 0; i < L && p[i]; i++) {
		char c = p[i]; if (c == '.') continue;
		if (c >= '1' && c <= '9') v[i] = (u8)(60 + (c - '0') * 21);
		else if (c == 'r' || c == 't' || c == 'q') { v[i] = 200; gDrRat[r][i] = (u8)(c == 'r' ? 1 : c == 't' ? 2 : 3); }
		else if (c == 'R' || c == 'T' || c == 'Q') { v[i] = 230; gDrRat[r][i] = (u8)(c == 'R' ? 1 : c == 'T' ? 2 : 3); }
		else if (c == '?') { v[i] = 170; gDrCond[r][i] = 3; }   // 50%
		else if (c == 'x') v[i] = 255;
	}
}
static void tune(int r, int i, int t) { gDrTn[r][i] = (s8)t; }
static void drumsFor(int bar) {
	int s = secOf(bar), in = bar - SEC0[s];
	for (int r = 0; r < 10; r++) row(r, "", 16, 0);
	if (s == S_INTRO) {
		if (in >= 4) { row(5, "..?....?...?..?.", 16, 0); row(8, "........2.......", 16, 0); }
	} else if (s == S_A) {   // half time, dusty
		row(0, "x.........6.....", 16, 0); row(1, "........x.......", 16, 0); row(2, "3.2.3.2.3.2.3.24", 16, 0); row(5, "...2.?.....2..?.", 16, 0);
		if (in == 7) { row(1, "........x...rRtQ", 16, 0); for (int i = 12; i < 16; i++) tune(1, i, -(i - 11) * 3); }
	} else if (s == S_DRILL1 || s == S_DRILL2) {
		int two = s == S_DRILL2, last = (in & 3) == 3;
		row(0, two ? "x..5..x...x.6..." : "x.....6...x.....", 16, 0);
		// snare on a 32nd grid (RATE X2, 32 steps): backbeats, ghost flurries, a roll that falls in pitch at the end of every 4th bar
		static const char* SN[4] = {
			"........x....3..3.......x...2.3.", "........x..2.3..........x.3..2..",
			"....2...x.......3.3.....x.......", "........x.....3.........x.......tQ" };
		int v = rnd(4); row(1, SN[v], 32, 2);
		if (last) { row(1, "........x.......3.3.4.5.6.7.8.9x", 32, 2); for (int i = 16; i < 32; i += 2) { tune(1, i, -(i - 16)); gDrRat[1][i] = (u8)(i > 24 ? 2 : 1); } }
		if (two && (in & 1)) { gDrRat[1][rnd(16) + 8] = 3; gDrRat[1][rnd(8) + 20] = 2; }
		row(2, two ? "4r4342t4r434r4Q4" : "4343r343434r4343", 16, two ? 4 : 0);   // drill 2: hats at 3/2 against the beat
		if (two) row(3, "..........4.....", 16, 0);
		row(5, "...?...2...?.2..", 16, 0);
		if (last) { row(6, "........4.5.6.7.", 16, 0); row(7, "............5.6r", 16, 0); for (int i = 8; i < 16; i++) { tune(6, i, -6 + (i - 8)); tune(7, i, (i - 12) * 2); } }
		if (in == 0) row(9, "x...............", 16, 0);
		if (two && in >= 12) { row(0, "x..5..x.x.x.6.66", 16, 0); }
	} else if (s == S_BREAK) {
		row(8, in & 1 ? "..2.....?..2...." : "........2.....?.", 16, 0); row(5, ".?.....?....?...", 16, 0);
		if (in == 7) { row(1, "................3.3.4.5.6.7.8.9x", 32, 2); for (int i = 16; i < 32; i += 2) tune(1, i, (i - 24)); }
	} else {   // outro: one last fill, then only the ticks
		if (bar == 56) { row(0, "x.....6...x.....", 16, 0); row(1, "........x.......3.4.5.6.7.8.9.xQ", 32, 2); for (int i = 16; i < 32; i++) tune(1, i, -(i - 16)); row(2, "4343r343434r4343", 16, 0); }
		if (bar == 57) row(9, "x...............", 16, 0);
		if (bar >= 57) row(5, "...?.......?....", 16, 0);
	}
}
// ---------------- one pass ----------------
static float smooth(float* v, float to, float k) { *v += (to - *v) * k; return *v; }
static void common(void) { gMaster = 0.62f; gMasterFX = 1; gBPM = BPM; gRoot = 5; gScale = 2; gSwing = 0; gMidiBend = 0; for (int m = 0; m < 4; m++) gMotOn[m] = 0; }
static void load(const char* nm) { int st, ix; if (!findPreset(nm, &st, &ix)) printf("no preset %s\n", nm); presetLoad(st, ix); common(); }
static void setupPass(void) {
	static const char* P[4] = {"ANALORD", "SAW 85", "MUSIC BOX", "DEEP SUB"};
	load(P[PART]);
	gDrums = PART == 0 && DRUMONLY; if (DRUMONLY) { gMasterFX = 0; gMaster = 0.8f; gRevMix = 0; gDlyMix = 0; } if (PART == 0) { gKit = 1; gDrumLevel = 0.75f; myDrumLoad(); gKickPat = MINE_IDX[0]; gSnarePat = MINE_IDX[1]; gHatPat = MINE_IDX[2]; } drumsChanged(); drumsFor(0);
	gThru = DRUMONLY; gThruCut = 1;
	if (PART == 0) { patSwitch(8); clearPat(); patStore(); for (int t = 0; t < 2; t++) for (int c = 0; c < 4; c++) { patSwitch(t * 4 + c); buildAcid(t, c); patStore(); } }
	else for (int i = 0; i < 17; i++) { patSwitch(i); if (PART == 1) buildPad(i); else if (PART == 2) buildMel(i); else buildSubT(i); patStore(); }
	for (int b = 0; b < BARS; b++) gSong[b] = (u8)(PART == 0 ? (DRUMONLY ? 8 : acidPat(b)) : PART == 1 ? padPat(b) : PART == 2 ? melPat(b) : subPat(b));
	gSongLen = BARS; gSongOn = 1; patSwitch(gSong[0]);
	gTapeLenSel = TL_SONG; gTapeArm = PART;
}
static float barPos(void) { return gTapePos / 32728.0f * BPM / 60.0f; }
static void swapFor(int bar) {   // a take changes patch just before a section, like reaching for the browser
	static int cur[4] = {-1, -1, -1, -1}; int s = secOf(bar), want = 0;
	if (PART == 1) want = s == S_BREAK ? 1 : s == S_DRILL1 || s == S_DRILL2 ? 2 : 0;
	if (PART == 2) want = s == S_BREAK ? 1 : s == S_DRILL2 ? 2 : 0;
	if (PART == 3) want = s == S_BREAK;
	if (PART == 0 || want == cur[PART]) return;
	cur[PART] = want;
	static const char* N[4][3] = {{0}, {"SAW 85", "WARP STRINGS", "ALL-IN-ONE"}, {"MUSIC BOX", "TOY BOX", "SQUELCH"}, {"DEEP SUB", "PREPARED"}};
	load(N[PART][want]); gDrums = 0; gThru = 0;
}
static void hands(void) {
	if (gTapeRec < 0 || gTapeState < 3) return;
	float b = barPos(); int bar = (int)(b / 4); if (bar >= BARS) bar = BARS - 1;
	float inBar = b - bar * 4; int s = secOf(bar), in = bar - SEC0[s]; float sb = b - SEC0[s] * 4, t = sb / ((SEC0[s + 1] - SEC0[s]) * 4.0f);
	int nb = (int)((b + 0.25f) / 4); if (nb >= BARS) nb = BARS - 1; swapFor(nb);
	static int lastB = -1; if (PART == 0 && bar != lastB) { lastB = bar; drumsFor(bar); }
	if (PART == 0) gMaster = DRUMONLY ? 0.8f : 0.5f;
	if (PART == 0) {   // the acid's cutoff: a slow ride up through drill 1, wild in drill 2
		static float cut = 0.2f; float c = 0.2f;
		if (s == S_A) c = 0.12f + 0.15f * t;
		else if (s == S_DRILL1) c = 0.22f + 0.3f * t + 0.08f * sinf(b * 3.1416f);
		else if (s == S_DRILL2) { static int lb = -1; static float tg = 0.4f; if (bar != lb) { lb = bar; tg = 0.25f + rnd(100) * 0.005f; } c = tg + 0.12f * sinf(b * 6.2832f * 0.5f); }
		gOfCut = smooth(&cut, c, 0.12f);
	} else if (PART == 1) {   // the pad breathes: filter opens through the intro, closes into the drills
		float c = s == S_INTRO ? 0.35f + 0.3f * t : s == S_DRILL1 || s == S_DRILL2 ? 0.6f : s == S_OUTRO ? 0.62f - 0.3f * t : 0.62f;
		gOfCut = c + 0.04f * sinf(b / 6.0f * 6.2832f); gMaster = 0.7f;
		gMidiBend = 0.03f * sinf(b / 3.3f * 6.2832f);   // cassette wow you can hear
	} else if (PART == 2) {   // a warped music box: slow wobble, and in drill 2 the squelch gets the pad
		gMidiBend = s == S_DRILL2 ? (inBar > 3.5f && (in & 3) == 3 ? -(inBar - 3.5f) * 1.5f : 0) : 0.05f * sinf(b / 2.7f * 6.2832f) + (s == S_OUTRO ? -0.08f * t : 0);
		if (s == S_DRILL2) gOfCut = 0.35f + 0.25f * (0.5f + 0.5f * sinf(b * 6.2832f / 8));
		gMaster = s == S_BREAK ? 0.38f : 0.5f;
	} else {
		if (s == S_DRILL1 || s == S_DRILL2) gMidiBend = (in & 3) == 3 && inBar > 3 ? -(inBar - 3) * 0.6f : 0;   // the sub drops at the end of each phrase
		else gMidiBend = 0;
	}
}
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (done) return;
	if (passStart < 0 && fr >= 3) { setupPass(); passStart = fr; }
	if (passStart >= 0 && fr == passStart + 3) tapeRecGo();
	hands();
	if (0) printf("fr %d rec %d state %d play %d len %d pos %d have %d %d %d %d arm %d\n", fr, gTapeRec, gTapeState, gSeqPlaying, gTapeLen, gTapePos, gTpHave[0], gTpHave[1], gTpHave[2], gTpHave[3], gTapeArm);
	if (passStart >= 0 && fr > passStart + 6 && gTapeRec < 0) {
		if (!gTpHave[PART]) { fail("pass %d didn't record", PART); done = 1; return; }
		printf("track %d on tape (%.1f s)\n", PART + 1, gTapeLen / 32728.0);
		gMidiBend = 0; gThru = 0; gMaster = 0.8f; gDrums = 0;
		if (++PART == 4 || DRUMONLY) { tapeExport(); done = 1; FRMAX = fr + 2; }
		else passStart = -1;
	}
}
int main(int argc, char** argv) { DRUMONLY = argc > 1; mkdir("sd", 0777); loadFont(); FRMAX = 60 * 60 * 12; app_main(); return gFails ? 1 : 0; }
