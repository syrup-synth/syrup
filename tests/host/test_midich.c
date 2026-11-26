// MIDI CHANNELS: each part has its own channel in and out, like most synths (SYNTH 1 / ALL, CHORD 2, BASS 3, DRUMS 10).
// Notes in on a part's channel play that part; what each part plays goes out on its channel; MIDI in never echoes out;
// drum hits go out as General MIDI notes with their note-off 50 ms later; the channels are kept in settings.bin.
#include "syrup_host.c"
#include "harness.h"
static int done = 0, lastPart = -9, lastNote = -9;
static void onset(int part, int note) { lastPart = part; lastNote = note; }
static int qFind(u32 from, u8 st, u8 a) { for (u32 r = from; r != gMcoW; r = (r + 1) % MCOQ) if (gMcoSt[r] == st && gMcoA[r] == a) return 1; return 0; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		gOnsetHook = onset;
		struct { u8 st; int part; } IN[4] = {{0x90, 0}, {0x91, 1}, {0x92, 2}, {0x94, 0}};
		for (int i = 0; i < 4; i++) { lastPart = -9; midiApply(IN[i].st, 60 + i, 100, 0);
			if (lastPart != IN[i].part) fail("IN: channel %d should play part %d (got %d)", (IN[i].st & 15) + 1, IN[i].part, lastPart);
			midiApply((u8)(0x80 | (IN[i].st & 15)), 60 + i, 0, 0); }
		// CHORD IN on OFF: channel 2 then plays the SYNTH (ALL takes every free channel)
		gChIn[MP_CHORD] = 0; lastPart = -9; midiApply(0x91, 70, 100, 0); if (lastPart != 0) fail("IN: with CHORD IN off, channel 2 should reach the SYNTH (%d)", lastPart); midiApply(0x81, 70, 0, 0); gChIn[MP_CHORD] = 2;
		// SYNTH IN on 1 only: channel 5 plays nothing
		gChIn[MP_SYNTH] = 1; lastPart = -9; midiApply(0x94, 71, 100, 0); if (lastPart != -9) fail("IN: SYNTH IN 1 shouldn't hear channel 5"); gChIn[MP_SYNTH] = 17;
		// OUT
		gPeerOk = 1; u32 w0 = gMcoW;
		gVoicePart = 1; voiceOn(64, 0.8f); gVoicePart = 0;
		if (!qFind(w0, 0x91, 64)) fail("OUT: a CHORD note should go out on channel 2");
		gVoicePart = 1; voiceFade(64, 0.05f); gVoicePart = 0;
		if (!qFind(w0, 0x81, 64)) fail("OUT: its release should send a note-off on channel 2");
		gVoicePart = 2; voiceOn(40, 0.8f); voiceOff(40); gVoicePart = 0;
		if (!qFind(w0, 0x92, 40) || !qFind(w0, 0x82, 40)) fail("OUT: BASS should send on channel 3");
		voiceOn(72, 0.8f); voiceOff(72);
		if (!qFind(w0, 0x90, 72) || !qFind(w0, 0x80, 72)) fail("OUT: SYNTH should send on channel 1");
		gChOut[MP_SYNTH] = 0; u32 w1 = gMcoW; voiceOn(73, 0.8f); voiceOff(73); if (gMcoW != w1) fail("OUT: SYNTH OUT OFF should send nothing"); gChOut[MP_SYNTH] = 1;
		// no echo: a note that came in over MIDI isn't sent back
		u32 w2 = gMcoW; midiApply(0x91, 66, 100, 0); midiApply(0x81, 66, 0, 0); midiApply(0x90, 67, 100, 0); midiApply(0x80, 67, 0, 0);
		if (gMcoW != w2) fail("OUT: MIDI in should not echo out (%u queued)", (gMcoW - w2) % MCOQ);
		// drums: a GM note on channel 10, its note-off 50 ms later
		u32 w3 = gMcoW; midiDrumOut(1, 0.9f);
		if (!qFind(w3, 0x99, 38)) fail("OUT: a snare hit should send note 38 on channel 10");
		if (qFind(w3, 0x89, 38)) fail("OUT: the drum's note-off shouldn't be at once");
		gDrOffAt[1] = 1; midiDrumOffTick(); if (!qFind(w3, 0x89, 38)) fail("OUT: the drum's note-off should follow");
		// a held note when the session is gone still gets its note-off; nothing new is queued
		voiceOn(80, 0.8f); gPeerOk = 0; u32 w4 = gMcoW; voiceOn(81, 0.8f); if (gMcoW != w4) fail("OUT: nothing should queue with no session");
		voiceOff(80); if (!qFind(w4, 0x80, 80)) fail("OUT: a note that went out should still be ended"); voiceOff(81);
		// settings keep the channels
		gChIn[MP_BASS] = 5; gChOut[MP_DRUMS] = 16; settingsSave(1); gChIn[MP_BASS] = 3; gChOut[MP_DRUMS] = 10; settingsLoad();
		if (gChIn[MP_BASS] != 5 || gChOut[MP_DRUMS] != 16) fail("SETTINGS: the channels should come back (%d %d)", gChIn[MP_BASS], gChOut[MP_DRUMS]);
		gChIn[MP_BASS] = 3; gChOut[MP_DRUMS] = 10; settingsSave(1);
		{ int ok = 0; for (int i = 0; PAGES[9][i].name; i++) if (!strcmp(PAGES[9][i].name, "DRUMS CH OUT")) ok = 1; if (!ok) fail("UI: DRUMS CH OUT should be on the HARDWARE page"); }
		gOnsetHook = 0; done = 1;
	}
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 6; app_main(); if (!done) fail("didn't run"); printf("midich: per-part MIDI channels\n"); return gFails ? 1 : 0; }
