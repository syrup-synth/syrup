// MIDI: the raw byte parser (IR delivers one byte at a time, WebSocket / UDP whole frames), then a fuzz of
// everything MIDI can send - garbage bytes, every status, floods of clock, notes never let go - after which
// ALL NOTES OFF + PANIC must leave nothing sounding, the output must stay finite and the engine must keep running.
#include "syrup_host.c"
#include "harness.h"
static unsigned rs = 777; static int rnd(int n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }
static int qn(void) { return (gMqW - gMqR + MQ) % MQ; }
static int qget(u8* o) { if (gMqR == gMqW) return 0; o[0] = gMq[gMqR][0]; o[1] = gMq[gMqR][1]; o[2] = gMq[gMqR][2]; gMqR = (gMqR + 1) % MQ; return 1; }
static void parserTests(void) {
	MidiIn m; memset(&m, 0, sizeof(m)); u8 o[3];
	gMqR = gMqW;   // empty the queue
	const u8 note[] = {0x90, 60, 100};
	for (int i = 0; i < 3; i++) midiParse(&note[i], 1, &m);   // IR: one byte per call
	if (!qget(o) || o[0] != 0x90 || o[1] != 60 || o[2] != 100) fail("MIDI: a note sent one byte at a time should arrive whole");
	const u8 run[] = {62, 90, 64, 0};   // running status: two more notes without the status byte
	midiParse(run, 4, &m);
	if (!qget(o) || o[0] != 0x90 || o[1] != 62) fail("MIDI: running status (1st)"); if (!qget(o) || o[1] != 64 || o[2] != 0) fail("MIDI: running status (2nd)");
	const u8 rt[] = {0x90, 67, 0xF8, 80};   // a clock tick in the middle of a note: both arrive
	midiParse(rt, 4, &m);
	if (!qget(o) || o[0] != 0xF8) fail("MIDI: real-time byte inside a message should come through");
	if (!qget(o) || o[0] != 0x90 || o[1] != 67 || o[2] != 80) fail("MIDI: the note around a clock tick should survive");
	const u8 sx[] = {0xF0, 1, 2, 3, 4, 0xF7, 0xB0, 74, 33};   // sysex skipped, the CC after it intact
	midiParse(sx, 9, &m);
	if (!qget(o) || o[0] != 0xB0 || o[1] != 74 || o[2] != 33) fail("MIDI: a CC after sysex");
	if (qn()) fail("MIDI: nothing else should be queued (%d)", qn());
	const u8 cut[] = {0x90, 70, 0xB0, 1, 2};   // a status byte mid-message drops the half message
	midiParse(cut, 5, &m);
	if (!qget(o) || o[0] != 0xB0 || o[1] != 1 || o[2] != 2) fail("MIDI: a new status mid-message should start over"); if (qn()) fail("MIDI: the cut note shouldn't arrive");
	const u8 split1[] = {0xE0, 0}, split2[] = {64};   // pitch bend split over two packets
	midiParse(split1, 2, &m); midiParse(split2, 1, &m);
	if (!qget(o) || o[0] != 0xE0 || o[2] != 64) fail("MIDI: a message split across packets");
}
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 2) parserTests();
	if (fr == 4) { int st, ix; if (findPreset("WARM FELT", &st, &ix)) presetLoad(st, ix); }
	// MIDI LEARN: arm it on FILT+ENV's RES row, move CC 21, then CC 21 drives RES
	if (fr == 5) { int ri = -1; for (int i = 0; PAGES[4][i].name; i++) if (PAGES[4][i].p == &gRes) ri = i; learnArm(4, ri); midiApply(0xB0, 21, 64, 0);
		if (gLearnP) fail("LEARN: the CC should have been learned"); gRes = 0; midiApply(0xB0, 21, 127, 0); if (gRes < 0.99f) fail("LEARN: CC 21 should now set RES (%.2f)", gRes);
		midiApply(0xB0, 21, 0, 0); if (gRes > 0.01f) fail("LEARN: CC 21 at 0 (%.2f)", gRes);
		memset(gLearnMap, 0, sizeof(gLearnMap)); }
	static MidiIn fz;
	if (fr >= 6 && fr < 900) {   // the fuzz
		u8 b[24]; int n = rnd(24);
		for (int i = 0; i < n; i++) { int r = rnd(100); b[i] = r < 30 ? (u8)(0x80 + rnd(0x80)) : r < 35 ? 0xF8 : (u8)rnd(0x80); }
		midiParse(b, n, &fz);
		for (int k = rnd(4); k > 0; k--) midiPush((u8)(0x80 + rnd(0x80)), (u8)rnd(256), (u8)rnd(256));   // even out-of-range data bytes
		if (fr % 97 == 0) for (int k = 0; k < 300; k++) midiPush(0xF8, 0, 0);   // a clock flood
		if (fr % 131 == 0) midiPush(0xFA, 0, 0); if (fr % 173 == 0) midiPush(0xFC, 0, 0);
		if (fr % 211 == 0) { midiPush(0xB0, 102, 0); }   // patch changes
	}
	if (fr == 900) { midiPush(0xB0, 123, 0); midiPush(0xB0, 120, 0); midiPush(0xFC, 0, 0); for (int c = 0; c < 16; c++) midiPush(0xB0 | c, 123, 0); }
	if (fr == 905) { panicRest(); allOff(); gSeqMode = 0; seqStop(); gArp = 0; }
	if (fr == 1000) {
		int act = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) act++;
		if (act) fail("MIDI fuzz: %d voices still held after ALL NOTES OFF + PANIC", act);
		for (int i = ON - SEC(1.0); i < ON; i++) if (OUT[2 * i] != OUT[2 * i]) { fail("MIDI fuzz: NaN in the output"); break; }
		double r = rmsOf(ON - SEC(0.3), ON); if (r > 400) fail("MIDI fuzz: still loud after everything off (%.0f rms)", r);
		printf("midi: parser ok, fuzz ran %d frames, %.0f rms after panic\n", fr, r);
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 1002; app_main(); if (FR < 1001) fail("engine stopped at frame %d", FR); return gFails ? 1 : 0; }
