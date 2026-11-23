// Syrup in a web browser: the same simulator the tests use (tests/host/fake3ds.h), compiled to WebAssembly.
// The page (shell.html) draws both screens, turns taps / mouse into the stylus, on-screen and keyboard buttons
// into the 3DS buttons and circle pad, plays the sound through Web Audio and feeds Web MIDI in.
#define FAKE_WEB 1
#define SYRUP_NO_NET 1
#include "syrup_host.c"
#include <emscripten.h>

static u32 W_KEYS = 0; static int W_TOUCH = 0, W_TX = 0, W_TY = 0; static short W_CX = 0, W_CY = 0, W_CSX = 0, W_CSY = 0;
static int W_AUDIO = 0;   // the page's audio is running (it pulls the ring); until then frames are paced by the clock
static u8 W_TOP[400 * 240 * 4], W_BOT[320 * 240 * 4];

static u32 W_LATCH = 0; static int W_TLATCH = 0;   // a tap shorter than a frame still counts: held for one frame
EMSCRIPTEN_KEEPALIVE void web_input(u32 keys, int touch, int tx, int ty, int cx, int cy) {
	W_LATCH |= keys & ~W_KEYS; if (touch && !W_TOUCH) W_TLATCH = 1;
	W_KEYS = keys; W_TOUCH = touch; W_TX = tx; W_TY = ty; W_CX = (short)cx; W_CY = (short)cy;
}
static int W_TT = 0, W_TTX = 0, W_TTY = 0, W_TTL = 0;
EMSCRIPTEN_KEEPALIVE void web_top_input(int t, int x, int y) { if (t && !W_TT) W_TTL = 1; W_TT = t; W_TTX = x; W_TTY = y; }
EMSCRIPTEN_KEEPALIVE void web_cstick(int x, int y) { W_CSX = (short)x; W_CSY = (short)y; }
// LINK: browser tabs / windows on one device share transport, tempo and the drum machine (shell.html carries it on a BroadcastChannel)
EMSCRIPTEN_KEEPALIVE int web_link_get(void) { return (gSeqMode == 1 ? 1 : 0) | (gDrums ? 2 : 0) | (gBPM << 4); }
EMSCRIPTEN_KEEPALIVE void web_link_set(int st) {
	int play = st & 1, dr = (st >> 1) & 1, bpm = st >> 4;
	if (bpm >= 40 && bpm <= 240) gBPM = bpm;
	if (play && gSeqMode != 1) { seqStop(); gSongPos = 0; gSeqMode = 1; if (gSeqBase < 0) gSeqBase = gridNote(0); }
	else if (!play && gSeqMode == 1) { gSeqMode = 0; seqStop(); }
	if (dr != (gDrums ? 1 : 0)) { gDrums = dr; drumsChanged(); }
}
EMSCRIPTEN_KEEPALIVE void web_midi(int st, int a, int b) { midiPush((u8)st, (u8)a, (u8)b); }
EMSCRIPTEN_KEEPALIVE void web_audio_on(int on) { W_AUDIO = on; }
EMSCRIPTEN_KEEPALIVE int web_audio_avail(void) { return (int)(WRW - WRR); }
EMSCRIPTEN_KEEPALIVE float* web_audio_ring(void) { return WRING; }
EMSCRIPTEN_KEEPALIVE int web_audio_read(void) { return (int)(WRR & 65535); }
EMSCRIPTEN_KEEPALIVE void web_audio_consume(int n) { WRR += (unsigned)n; if (WRW - WRR > 65536) WRR = WRW - 65536; }
EMSCRIPTEN_KEEPALIVE u8* web_top(void) { return W_TOP; }
EMSCRIPTEN_KEEPALIVE u8* web_bot(void) { return W_BOT; }

static void toRGBA(const u8* fb, int w, u8* o) {   // the 3DS framebuffer is column-major, bottom-up, BGR
	for (int x = 0; x < w; x++) { const u8* p = fb + x * 240 * 3; for (int y = 0; y < 240; y++) { const u8* q = p + (239 - y) * 3; u8* d = o + (y * w + x) * 4; d[0] = q[2]; d[1] = q[1]; d[2] = q[0]; d[3] = 255; } }
}
static void webVBlank(void) {   // after each frame: show it, then wait until it's time for the next
	toRGBA(TOPFB, 400, W_TOP); toRGBA(BOTFB, 320, W_BOT);
	EM_ASM({ if (Module.present) Module.present(); });
	if (W_AUDIO) { int guard = 0; while (WRW - WRR > 32728 / 10 && guard++ < 200) emscripten_sleep(4); }   // keep ~100 ms of sound queued
	else { static double next = 0; double now = emscripten_get_now(); if (next < now - 100) next = now; next += 1000.0 / 60; double w = next - now; emscripten_sleep(w > 0 ? (unsigned)w : 0); }
}
static void SCRIPT(int fr) { (void)fr; HD = W_KEYS | W_LATCH; W_LATCH = 0; TOUCH = W_TOUCH || W_TLATCH; W_TLATCH = 0; TX = W_TX; TY = W_TY; CX = W_CX; CY = W_CY; CSX = W_CSX; CSY = W_CSY; TTOUCH = W_TT || W_TTL; W_TTL = 0; TTX = W_TTX; TTY = W_TTY; }
int main(void) {
	FILE* f = fopen("default_font.bin", "rb"); if (f) { if (fread(FONT, 1, 2048, f) != 2048) {} fclose(f); }
	mkdir("sd", 0777);
	FRMAX = 0x7FFFFFFF;
	gFakeHP = true;
	gN3 = 1;   // the browser has the second stick (gamepad right stick / the small one on screen) and ZL / ZR
	app_main();
	return 0;
}
EMSCRIPTEN_KEEPALIVE int web_dbg(int k) { return k == 0 ? gStyleCur * 100 + gPresetCur : k == 1 ? gCurPage : k == 2 ? gBrStyle : k == 3 ? (int)gStyle * 100 + gPreset : TOUCH * 1000 + TY; }
