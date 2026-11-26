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
EMSCRIPTEN_KEEPALIVE void web_quick(void) { gQuickOpen = !gQuickOpen; }   // QUICK button: the quick-select ring, by tap
EMSCRIPTEN_KEEPALIVE void web_cstick(int x, int y) { W_CSX = (short)x; W_CSY = (short)y; }
// LINK: browser tabs / windows on one device share transport, tempo and the drum machine (shell.html carries it on a BroadcastChannel)
EMSCRIPTEN_KEEPALIVE int web_link_get(void) { return (gSeqMode == 1 ? 1 : 0) | (gDrums ? 2 : 0) | (gBPM << 4); }
EMSCRIPTEN_KEEPALIVE void web_link_set(int st) {   // PLAY / STOP (the one transport: notes + drums) and the BPM
	int play = st & 1, bpm = st >> 4;
	if (bpm >= 40 && bpm <= 240) gBPM = bpm;
	if (play != (gSeqMode == 1)) { if (play) { seqStop(); gSongPos = 0; } transportToggle(); }
}
// MASTER CLOCK: the master sends where its sequence is (in steps since PLAY); a follower pulls its own toward it,
// a little each time (no audible jump), or jumps there if it's more than a step and a half out (joined late)
EMSCRIPTEN_KEEPALIVE double web_link_clock(void) {
	if (!gSeqPlaying) return -1;
	float l = stepLenClk(gSeqClock); return (double)gSeqClock + (l > 0 ? gSeqPh / l : 0);
}
EMSCRIPTEN_KEEPALIVE double web_link_follow(double target) {   // returns how far out it was, in steps
	if (!gSeqPlaying || target < 0) return 0;
	float l = stepLenClk(gSeqClock); double mine = (double)gSeqClock + (l > 0 ? gSeqPh / l : 0), d = target - mine;
	if (fabs(d) > 1.5) {
		long n = (long)floor(target); int L = gSeqLen < 1 ? 1 : gSeqLen;
		gSeqClock = (unsigned)n; gSeqPos = (int)(n % L); gSeqPh = (float)(target - n) * stepLenClk((unsigned)n); gPendFire = -1; gEarlyDone = 0; gFireOff = 0;
		return d;
	}
	float lim = gSeqClock < 16 ? 0.25f : 0.06f;   // just started (PLAY from the other unit got here a trip later): pull in quickly
	float c = (float)d * (gSeqClock < 16 ? 0.6f : 0.3f); if (c > lim) c = lim; if (c < -lim) c = -lim;
	gSeqPh += c * l; if (gSeqPh < 0) gSeqPh = 0;
	return d;
}
// SAMPLES: the page wrote WAVs into sd/kits/WEB; use them
static int W_KITRL = 0;
EMSCRIPTEN_KEEPALIVE void web_kit_reload(void) { W_KITRL = 1; }   // done from the main loop (SCRIPT), not inside the page event
static void webKitReload(void) { kitScanDirs(); for (int i = 0; i < gNKitDirs; i++) if (!strcmp(gKitDirs[i], "WEB")) gKitDir = i; gKit = KIT_SAMPLE; gDrumWant = KIT_SAMPLE + 100; if (!gDrums) { gDrums = 1; drumsChanged(); } say("SAMPLE KIT: YOUR FILES"); }
// CAMERA: the page runs the browser camera while Syrup wants one (CAMERA, CAM > TABLE, CAM WAVE) and puts 160x120 frames here
EMSCRIPTEN_KEEPALIVE int web_cam_want(void) { return gCamOn || gCamWant || gCamVid; }
EMSCRIPTEN_KEEPALIVE u16* web_cam_buf(void) { return W_CAMBUF; }
EMSCRIPTEN_KEEPALIVE void web_cam_frame(void) { W_CAMNEW = 1; }
EMSCRIPTEN_KEEPALIVE void web_midi(int st, int a, int b) { if (st < 0x80) return; midiPush((u8)st, (u8)(a & 127), (u8)(b & 127)); }   // (data bytes are 7-bit)
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
static void webMirDrain(void);
static void SCRIPT(int fr) { (void)fr; webMirDrain(); if (W_KITRL) { W_KITRL = 0; webKitReload(); } HD = W_KEYS | W_LATCH; W_LATCH = 0; TOUCH = W_TOUCH || W_TLATCH; W_TLATCH = 0; TX = W_TX; TY = W_TY; CX = W_CX; CY = W_CY; CSX = W_CSX; CSY = W_CSY; TTOUCH = W_TT || W_TTL; W_TTL = 0; TTX = W_TTX; TTY = W_TTY; }
int main(void) {
	FILE* f = fopen("default_font.bin", "rb"); if (f) { if (fread(FONT, 1, 2048, f) != 2048) {} fclose(f); }
	mkdir("sd", 0777);
	FRMAX = 0x7FFFFFFF; FAKE_NEW3DS = 1;   // the web has the C-stick (a controller's right stick, or the on-screen one) and ZL / ZR
	gFakeHP = true;
	gN3 = 1;   // the browser has the second stick (gamepad right stick / the small one on screen) and ZL / ZR
	app_main();
	return 0;
}

// MIRROR: this page is a muted copy of a 3DS (opened from the 3DS's own address: /mirror, /stream). The page passes on
// what arrives over the WebSocket; the overlays (STREAM) read the state back as a little JSON.
static u8 W_MIR[65536]; static char W_INFO[1024];
EMSCRIPTEN_KEEPALIVE void web_mirror_on(int on) { gMirror = on; if (on) { gSeqMode = 0; seqStop(); } }
EMSCRIPTEN_KEEPALIVE void web_mirror_vis(int on) { gMirVis = on; }   // STREAM: the visualizer whatever page the 3DS is on
EMSCRIPTEN_KEEPALIVE u8* web_mir_buf(void) { return W_MIR; }
// the page hands batches over between frames; they're played from the main loop (SCRIPT), never from inside a page
// event while the loop is paused in a sleep (that broke the paused loop's saved state: crashes)
#define WMQ (1 << 19)
static u8 W_MQ[WMQ]; static int W_MQN = 0;
EMSCRIPTEN_KEEPALIVE void web_mir_feed(int n) { if (n > 0 && n <= (int)sizeof(W_MIR) && W_MQN + n <= WMQ) { memcpy(W_MQ + W_MQN, W_MIR, n); W_MQN += n; } }
static void webMirDrain(void) { if (W_MQN) { int n = W_MQN; W_MQN = 0; mirFeed(W_MQ, n); } }
EMSCRIPTEN_KEEPALIVE int web_th(int k) { const u8* c = k == 0 ? TH->bg : k == 1 ? TH->val : k == 2 ? TH->keyOn : k == 3 ? TH->ink : TH->dim; return c[0] << 16 | c[1] << 8 | c[2]; }
EMSCRIPTEN_KEEPALIVE const char* web_mir_info(void) {
	char nm[32]; int j = 0; for (int i = 0; gPatchName[i] && j < 30; i++) { char ch = gPatchName[i]; if (ch == '"' || ch == '\\') continue; nm[j++] = ch; } nm[j] = 0;
	int o = snprintf(W_INFO, sizeof(W_INFO), "{\"p\":\"%s\",\"b\":%d,\"s\":%d,\"r\":%d,\"n\":[", nm, gBPM, gSeqMode == 1, gLiveRec);
	int first = 1, seen[128] = {0};
	for (int i = 0; i < NVS && o < (int)sizeof(W_INFO) - 8; i++) { SVoice* v = &gV[i]; if (!v->active || v->isEcho || v->stage == 4 || v->note < 0 || v->note > 127 || seen[v->note]) continue; seen[v->note] = 1; o += snprintf(W_INFO + o, sizeof(W_INFO) - o, first ? "%d" : ",%d", v->note); first = 0; }
	snprintf(W_INFO + o, sizeof(W_INFO) - o, "]}");
	return W_INFO;
}
EMSCRIPTEN_KEEPALIVE const char* web_knobs(void) { knobsJson(W_INFO, sizeof(W_INFO)); return W_INFO; }   // PHONE: the KNOBS page's labels
EMSCRIPTEN_KEEPALIVE void web_test_cam(int on) { gCamOn = on; }
EMSCRIPTEN_KEEPALIVE int web_dbg(int k) { if (k == 5) return gCamFrames; return k == 0 ? gStyleCur * 100 + gPresetCur : k == 1 ? gCurPage : k == 2 ? gBrStyle : k == 3 ? (int)gStyle * 100 + gPreset : TOUCH * 1000 + TY; }
