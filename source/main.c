// SYRUP v2 - Serum-style wavetable synth for Nintendo 2DS / 3DS (homebrew, libctru)
//
// The DSP plays the oscillators: every voice layer is a DSP channel looping a single-cycle
// waveform at the note's pitch, through the channel's own biquad filter. The ARM rebuilds the
// cycles only when something changes (or is modulated): morph between wavetable frames at
// WT POS, apply the warp (Serum's list, incl. FM/AM/RM from the other oscillator, baked into
// the cycle), drive, then FFT into band-limited copies so high notes don't alias.
//
// Layout follows Serum: OSC A, OSC B, SUB, NOISE, FILTER, ENV1 (amp, AHDSR), ENV2, LFO1-2,
// a 4-slot mod matrix, voicing (poly/mono/legato + glide) and FX (chorus, echo, space, drive).
// Paraphonic note: WT POS / WARP modulation is shared by all notes (one cycle per oscillator);
// filter, pitch, level and pan modulation are per note.
//
// The waveform rebuilds run on the second CPU core (worker thread on core 1).
//
// Controls:
//   D-pad U/D select parameter, L/R change it (hold to repeat)   X / Y  next / previous page
//   Bottom tabs: HELP, < PAGE, PAGE >, LATCH   XY pad: X = OSC A WT POS, Y = filter cutoff
//   Keyboard: play (lower on a key = louder), slide to glide
//   A chord mode   B latch   L / R octave   SELECT play/stop   START menu (save, browser, panic, help, theme, quit)
//   Circle pad: left/right pitch bend, up/down = STICK mod source

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <stddef.h>
#ifndef SYRUP_VER
#define SYRUP_VER "dev"
#endif
#ifndef SYRUP_BUILD
#define SYRUP_BUILD 0   // the build number: how many commits the source has (CI sets it)
#endif
#define STR2(x) #x
#define STR1(x) STR2(x)
#define BUILD_STR STR1(SYRUP_BUILD)
#include <malloc.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#ifndef SYRUP_NO_NET
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#endif

#define TWO_PI   6.28318530718f
#define CYC      2048
#define MAXFR    256
#define NLEV     7
#define NCH      24
#define DSPRATE  32728.0f

static const int LEVN[NLEV] = {2048, 1024, 512, 256, 128, 64, 32};

// ---------------------------------------------------------------- FFT
typedef struct { float re, im; } cpx;
static float twRe[CYC / 2], twIm[CYC / 2];
static cpx gTmp[CYC];

static void fftInit(void) { for (int k = 0; k < CYC / 2; k++) { twRe[k] = cosf(TWO_PI * k / CYC); twIm[k] = -sinf(TWO_PI * k / CYC); } }

static void fft(cpx* a, int n, int inv) {
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) { cpx t = a[i]; a[i] = a[j]; a[j] = t; }
	}
	for (int len = 2; len <= n; len <<= 1) {
		int step = CYC / len, h = len >> 1;
		for (int i = 0; i < n; i += len)
			for (int k = 0; k < h; k++) {
				float wr = twRe[k * step], wi = inv ? -twIm[k * step] : twIm[k * step];
				cpx u = a[i + k], v = a[i + k + h];
				float vr = v.re * wr - v.im * wi, vi = v.re * wi + v.im * wr;
				a[i + k].re = u.re + vr; a[i + k].im = u.im + vi;
				a[i + k + h].re = u.re - vr; a[i + k + h].im = u.im - vi;
			}
	}
}

static int gGpuOK = 0, gGpuFx = 1, gGpuHung = 0, gGpuFrames = 0, gGpuCopy = 1;
static int gDrums, gKit, gKickPat, gSnarePat, gHatPat; static float gDrumLevel;
// v12 patch fields live with the looper, CHIP > FX and the XY pad further down
static int gSeqRate = 0;   // SEQ RATE (see seqDiv)
static int gTapeAlign; static int gXyMode, gThru, gTapeKeys; static float gThruCut, gThruRes, gThruEnv, gThruLfo, gEchoWow;
// ACCENT (TR-808 style): a step column marked ACC plays every drum on it louder, by ACCENT (DRUMS page)
#define DSTEP 64   // drum rows: up to 64 steps (4 pages of 16)
static u8 gDrAcc[DSTEP]; static float gDrAccAmt = 0.6f; static int gAccMode = 0;
#define NKIT 7
#define KIT_SAMPLE 6
static float gPadMorph = 0.25f;   // PADS: sliding glides to the next chord over this long (0 = retrigger)
static int gStrumScale = 0; static const char* STRN_NAMES[2] = {"CHORD", "SCALE"};   // PADS strum strip: the chord's notes, or the scale's
static int gRollScale = 1; static const char* RSC_NAMES[3] = {"OFF", "SHOW", "FOLD"};   // the piano roll and the SCALE
static volatile int gDrumWant;
// SAMPLE kit: WAV files from a folder under /3ds/syrup/kits (one folder = one kit); gKitDirP names them for KIT FOLDER
static int gKitDir = 0, gNKitDirs = 0; static char gKitDirs[16][32], gKitRaw[16][64]; static const char* gKitDirP[16] = {"(NONE)"};
static void drumsChanged(void); static void drumRecChanged(void); static void outlineP(int x, int y, int w, int h, const u8* c); static void drawPageStrip(void); static void gestureDemo(int X, int Y, int roll);
static int gBeatFromPatch;   // the running beat came with the current patch (and stops when you leave it); touching a drum control makes it yours
// chip quirks: DSP GRIT (the DSP's interpolation off), FILTER SCREAM (its biquad pushed past stable), MIX OVERDRIVE (its
// channel volumes past full, clipped at the 16-bit ceiling), ECHO PITCH (the chip echo replayed at another rate than it records)
static int gQGrit = 0, gCsEchoPitch = 0; static float gQScream = 0, gQMixDrive = 0; static const char* GRIT_NAMES[3] = {"OFF", "LINEAR", "RAW"};
static float gChipLevel, gCsEcho, gCsEchoFb, gOrbit, gOrbitRate, gGrainSize, gGrainScan, gGrainSpray, gGrainPitch, gGrainLevel; static int gChipDuty, gChipOct, gCsEchoT, gGrains, gGrainSrc; static const char* GRAIN_MODES[3]; static const char* GRAIN_SRCS[3];
static const char* DUTY_NAMES[8]; static const char* CSECHO_T_NAMES[4];
static u8 gMot[4][64]; static int gMotTgt[4], gMotOn[4], gMotRec, gMotBars; static float gMotPh;
static int gCamVid, gMidiNet, gAddEdit, gOfType; static float gOfCut, gOfRes, gOfDrive, gOfEnv; static const char* OFT_NAMES[14]; static float gWtScan; static const char* VID_NAMES[3];
static void chipSet(float lv);
static void grainsChanged(void);
static int gEdStick = 0, gRcS = 0, gRcN = 60, gDcR = 0, gDcI = 0, gDgSelF = 0; static u64 gCurT = 0, gDvRebuildT = 0;
static int curShown(void) { return gCurT && osGetTime() - gCurT < 5000; }
static int EU_IDX_(int r) { return r ? 7 : 8; }   // EUCLID in the KICK / SNARE + HAT lists
#define NBEAT_ 30
static const char* BEAT_NAMES_[NBEAT_] = {"HOUSE", "BOOM BAP", "TRAP", "DNB", "BREAK", "DISCO", "FUNK", "HALFTIME", "DEMBOW", "AFROBEAT", "2-STEP", "ELECTRO", "ROCK", "SHUFFLE", "CUMBIA", "BOSSA", "SAMBA", "RUMBA", "BEGUINE", "CHA-CHA", "TANGO", "MAMBO", "MARCH", "SWING", "SLOW ROCK", "WALTZ", "REGGAE", "FOXTROT", "TECHNO", "GO-GO"};
static int gDrumSel[3] = {1, 1, 1}; static int gBeatSel = 0; static void beatPaste(int b); static void drumPaste(int r, int v, int undo); static void drumGridMark(void); static const char* KIT_NAMES[7]; static const char* KICKP_NAMES[9]; static const char* SNAREP_NAMES[8]; static const char* HATP_NAMES[8]; static int gEuHits[3], gEuRot[3], gEuLen; static int gDrumRec; static const char* DREC_NAMES[4];
// live drum shaping: TUNE (semitones, applied at each hit: no rebuild), CHANCE (each hit of the row plays this often), MUTE (rows, bit 1 kick 2 snare 4 hat)
static float gDrumTune[3] = {0, 0, 0}, gDrumChance[3] = {1, 1, 1}; static int gDrumMute = 0; static int gDrumPoly;
static const char* DMUTE_NAMES[8] = {"NONE", "KICK", "SNARE", "KICK+SNARE", "HAT", "KICK+HAT", "SNARE+HAT", "ALL"};
#define NVISM 11
static const char* VISM_NAMES[NVISM] = {"AUTO", "WAVETABLE", "SCOPES", "ORBIT", "SPECTRUM", "TRACKER", "TRACKER VU", "TUNNEL", "KALEIDO", "RIPPLES", "PLASMA"};
static const char* AB_NAMES[2] = {"OSC A", "OSC B"};
static int gVisMode = 0, gVisOsc = 0, gVisOverlay = 1;
static float gVisBeat = 0;
static int gVisTrails = 0, gVisParts = 1, gVisBurst = 0;
#define NTHEMES 20
static const char* THEME_NAMES[NTHEMES] = {"ORIGINAL", "PIP GREEN", "DOS BLUE", "AMBER EGA", "MAGENTA", "STICKER", "IMPULSE",
	"CYBER", "SOLARIZED", "GB POCKET", "VAPORWAVE", "CGA", "SINCLAIR", "X11", "NORD", "ACID", "BBS", "HERCULES", "HERC AMBER", "HERC GREEN"};
static int gTheme = 16,   /* BBS: the default look */ gScan = 0, gBright = 0;   // gBright: screen brightness 1..5, 0 = leave the system setting
static const char* BRIGHT_NAMES[6] = {"SYSTEM", "1 DIM", "2", "3", "4", "5 BRIGHT"};
   // graphics-chip visuals (see visFrame); off by default while being tested on hardware
static int gInAudioTick = 0;
// crash guard: a marker file holds the last milestone reached; it is removed on a clean exit.
// If it is still there at launch, the last run crashed -> SAFE MODE (new hardware features off).
static const char* CRASH_PATH = "sdmc:/3ds/syrup/running.txt";
static int gSafeMode = 0, gAudioClock = 1;
static char gCrashStage[40] = "";
static void crashMark(const char* stage) {
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777);
	FILE* f = fopen(CRASH_PATH, "w");
	if (f) { fputs(stage, f); fclose(f); }
}
static void crashCheck(void) {
	FILE* nc = fopen("sdmc:/3ds/syrup/nogpucopy", "r"); if (nc) { gGpuCopy = 0; fclose(nc); }
	FILE* f = fopen(CRASH_PATH, "r");
	if (!f) return;
	if (!fgets(gCrashStage, sizeof(gCrashStage), f)) snprintf(gCrashStage, sizeof(gCrashStage), "START");
	if (!strncmp(gCrashStage, "GPU COPY", 8) && strncmp(gCrashStage, "GPU COPY OK", 11)) { FILE* n = fopen("sdmc:/3ds/syrup/nogpucopy", "w"); if (n) fclose(n); }   // hung in the GPU copy: CPU copies from now on
	fclose(f);
	remove(CRASH_PATH);   // one safe start after a crash or a power-off, not every start from then on
#ifdef FAKE_WEB
	return;   // the web page never "exits": closing the tab isn't a crash
#endif
	gSafeMode = 1;
}        // set while the audio thread runs the arp/seq clock (it holds gMipsLock)
static LightLock gMipsLock; static volatile int gTickNoMips = 0;   // audio tick running without gMipsLock (a rebuild holds it)   // guards the shared FFT buffers (worker on core 1 vs main core)
static Thread gWorker = NULL;
#define SYRUP_VERSION "1.0 BETA"
// safe saves: write NAME.new, then swap it in, so pulling the power mid-save can never leave a half-written file
static FILE* safeOpen(const char* path) { char t[96]; snprintf(t, sizeof(t), "%s.new", path); return fopen(t, "wb"); }
static int safeClose(FILE* f, const char* path, int ok) {
	char t[96]; snprintf(t, sizeof(t), "%s.new", path);
	if (fflush(f) != 0) ok = 0;
	if (fclose(f) != 0) ok = 0;
	if (!ok) { remove(t); return 0; }
	remove(path); return rename(t, path) == 0;
}
static FILE* safeRead(const char* path) {   // a save that lost power between the remove and the rename left only NAME.new: use it
	FILE* f = fopen(path, "rb"); if (f) return f;
	char t[96]; snprintf(t, sizeof(t), "%s.new", path);
	f = fopen(t, "rb"); if (f) { fclose(f); rename(t, path); return fopen(path, "rb"); }
	return NULL;
}
static Result gCore2Rc = 0, gCore2ThRc = 0; static u32 gCore2Lim = 0;   // why core 2 is off (HARDWARE page)
static void probeMain(void* a) { (void)a; svcExitThread(); }
static volatile u64 gCore1Busy = 0;   // system ticks core 1 spent rebuilding (HARDWARE page meter)

// a band-limited single-cycle source: spectrum + 7 mip levels in linear memory
typedef struct {
	cpx spec[CYC];
	s16* lev[NLEV];
	float norm; int normInit;
	u32 built;      // levels written since the last spectrum change
} Mips;

static void mipsInit(Mips* m) {
	int tot = 0; for (int L = 0; L < NLEV; L++) tot += LEVN[L];
	s16* mem = (s16*)linearAlloc(tot * sizeof(s16));
	memset(mem, 0, tot * sizeof(s16));
	for (int L = 0, o = 0; L < NLEV; L++) { m->lev[L] = mem + o; o += LEVN[L]; }
	m->norm = 1; m->normInit = 0; m->built = 0;
}

static volatile int gInWorker; static void fxService(void);   // the core-1 worker serves effects frames between steps
static void mipsBuildLevel(Mips* m, int L) {
	int N = LEVN[L], H = N / 4;
	memset(gTmp, 0, sizeof(cpx) * N);
	for (int k = 1; k <= H; k++) { gTmp[k] = m->spec[k]; gTmp[N - k].re = m->spec[k].re; gTmp[N - k].im = -m->spec[k].im; }
	fft(gTmp, N, 1);
	float sc = m->norm / CYC * 32767.0f;
	for (int n = 0; n < N; n++) {
		float v = gTmp[n].re * sc;
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		m->lev[L][n] = (s16)v;
	}
	DSP_FlushDataCache(m->lev[L], N * sizeof(s16));
	m->built |= 1u << L;
	if (gInWorker) fxService();   // a waiting effects frame goes between mip levels
}

// new cycle -> spectrum, normalise (from the 512-harmonic peak), rebuild the levels in use
static void mipsSet(Mips* m, const float* cyc, u32 needMask) {
	for (int n = 0; n < CYC; n++) { m->spec[n].re = cyc[n]; m->spec[n].im = 0; }
	fft(m->spec, CYC, 0);
	if (gInWorker) fxService();
	m->spec[0].re = m->spec[0].im = 0;
	memset(gTmp, 0, sizeof(cpx) * CYC);
	for (int k = 1; k <= 512; k++) { gTmp[k] = m->spec[k]; gTmp[CYC - k].re = m->spec[k].re; gTmp[CYC - k].im = -m->spec[k].im; }
	fft(gTmp, CYC, 1);
	float pk = 0; for (int n = 0; n < CYC; n++) { float v = fabsf(gTmp[n].re); if (v > pk) pk = v; }
	float target = pk > 1e-3f ? 0.92f * CYC / pk : 1.0f;
	if (!m->normInit) { m->norm = target; m->normInit = 1; } else m->norm += (target - m->norm) * 0.5f;
	m->built = 0;
	for (int L = 0; L < NLEV; L++) if (needMask & (1u << L)) mipsBuildLevel(m, L);
}

// ---------------------------------------------------------------- wavetables
enum { T_BASIC, T_PWM, T_HARMONIX, T_VOWELS, T_SYNC, T_GROWL, T_DIGITAL, T_PLUCK, T_DRUMS, T_BUILTIN };
static const char* BUILTIN_NAMES[T_BUILTIN] = {"BASIC SHAPES", "PWM", "HARMONIX", "VOWELS", "SYNC SAW", "FM GROWL", "DIGITAL", "KARPLUS", "DRUM HITS"};
#define MAXFILES 64
static char gFiles[MAXFILES][64];
static int gNFiles = 0;

static float hash01(int a, int b) {
	unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

typedef struct { float* frames; int nFrames; float peak; char name[40]; } Table;

// DRUM HITS (built-in table) and KIT > TABLE: 8 drums, 8 frames each, attack to tail. Frame 8g is drum g's first
// moment, frame 8g+7 its tail: WT POS swept by a fast envelope plays the hit back, at any pitch, band-limited.
#define DT_N 8   // drums
#define DT_F 8   // frames per drum
static void drumSig(int g, float* o, int n) {   // a cheap analog-style drum, 32728 Hz: KICK SNARE TOM CLAP HAT RIM COWBELL ZAP
	const float sr = 32728; float ph = 0, ph2 = 0, bp = 0, lp = 0, hp = 0; unsigned r = 22222u + g * 777u;
	float sq[6] = {0}; static const float HF[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
	for (int i = 0; i < n; i++) {
		float t = i / sr, v = 0; r = r * 1664525u + 1013904223u; float nz = (int)(r >> 9) / 4194304.0f - 1.0f;
		switch (g) {
		case 0: { float f = 48 + 110 * expf(-t * 28); ph += f / sr; v = sinf(TWO_PI * ph) * expf(-t * 5) + (t < 0.004f ? nz * 0.5f : 0); v = tanhf(v * 1.6f); break; }   // KICK
		case 1: { float f = 185 * (1 + 0.4f * expf(-t * 40)); ph += f / sr; ph2 += f * 1.78f / sr; bp += 0.45f * (nz - bp);
			v = (0.55f * sinf(TWO_PI * ph) + 0.3f * sinf(TWO_PI * ph2)) * expf(-t * 18) + (nz - bp) * 0.8f * expf(-t * 11); break; }   // SNARE
		case 2: { float f = 95 * (1 + 0.6f * expf(-t * 18)); ph += f / sr; v = sinf(TWO_PI * ph) * expf(-t * 6) + nz * 0.05f * expf(-t * 40); break; }   // TOM
		case 3: { float e = t < 0.03f ? expf(-fmodf(t, 0.01f) * 300) : expf(-(t - 0.03f) * 14); bp += 0.25f * (nz - bp); hp = nz - bp; lp += 0.5f * (hp - lp); v = lp * e * 1.6f; break; }   // CLAP
		case 4: { float m = 0; for (int k = 0; k < 6; k++) { sq[k] += HF[k] * 1.9f / sr; if (sq[k] >= 1) sq[k] -= 1; m += sq[k] < 0.5f ? 1 : -1; }
			lp += 0.9f * (m / 6 - lp); hp = m / 6 - lp; v = (hp + nz * 0.3f) * expf(-t * 9); break; }   // HAT (six squares, like the 808)
		case 5: { ph += 1700 / sr; ph2 += 465 / sr; v = (0.6f * sinf(TWO_PI * ph) + 0.6f * sinf(TWO_PI * ph2)) * expf(-t * 55) + nz * 0.3f * expf(-t * 120); break; }   // RIM
		case 6: { ph += 540 / sr; ph2 += 800 / sr; float m = (fmodf(ph, 1) < 0.5f ? 1 : -1) + (fmodf(ph2, 1) < 0.5f ? 1 : -1); bp += 0.3f * (m - bp); v = (m - bp) * 0.5f * (0.4f * expf(-t * 60) + 0.6f * expf(-t * 9)); break; }   // COWBELL
		default: { float f = 1800 * expf(-t * 22) + 60; ph += f / sr; v = (fmodf(ph, 1) < 0.5f ? 0.8f : -0.8f) * expf(-t * 7); break; }   // ZAP
		}
		o[i] = v;
	}
}
static void drumTableFrames(float* fr, float* const* sig, const int* len) {   // DT_N hits -> 64 frames
	for (int g = 0; g < DT_N; g++) for (int j = 0; j < DT_F; j++) {
		float* f = fr + (g * DT_F + j) * CYC; const float* x = sig[g]; int L = len[g], W = L / 6 < 1500 ? L / 6 : 1500; if (W < 64) W = L;
		int st = (int)(powf(j / (float)(DT_F - 1), 1.6f) * (L - W) * 0.85f);
		for (int k = 0; k < CYC; k++) { float u = k * (W - 1) / (float)CYC; int a = st + (int)u; float fa = u - (int)u; f[k] = a + 1 < L ? x[a] * (1 - fa) + x[a + 1] * fa : 0; }
		float dc = 0; for (int k = 0; k < CYC; k++) dc += f[k]; dc /= CYC; for (int k = 0; k < CYC; k++) f[k] -= dc;
		for (int k = 0; k < 128; k++) { float w = k / 128.0f; f[CYC - 128 + k] = f[CYC - 128 + k] * (1 - w) + f[k] * w; }   // the end leads back into the start: it loops
		float pk = 1e-6f; for (int k = 0; k < CYC; k++) if (fabsf(f[k]) > pk) pk = fabsf(f[k]); for (int k = 0; k < CYC; k++) f[k] /= pk;   // each frame full scale: the envelope plays the level
	}
}
static void genDrumTable(Table* tb) {
	static float buf[DT_N][13000]; float* sig[DT_N]; int len[DT_N];
	for (int g = 0; g < DT_N; g++) { len[g] = g == 0 || g == 2 ? 13000 : 9000; drumSig(g, buf[g], len[g]); sig[g] = buf[g]; }
	drumTableFrames(tb->frames, sig, len); tb->nFrames = 64;
}
static void genBuiltin(Table* tb, int t) {
	if (t == T_DRUMS) { genDrumTable(tb); snprintf(tb->name, sizeof(tb->name), "%s", BUILTIN_NAMES[t]); return; }
	tb->nFrames = 64;
	for (int f = 0; f < 64; f++) {
		float* fr = tb->frames + f * CYC;
		float m = f / 63.0f;
		if (t == T_HARMONIX || t == T_VOWELS || t == T_DIGITAL || t == T_PLUCK) {
			static float amp[512];   // KARPLUS uses 511 harmonics
			memset(amp, 0, sizeof(amp));
			int nh = 256;
			if (t == T_HARMONIX) { int cnt = 1 + f; for (int k = 1; k <= cnt; k++) amp[k] = 1.0f / k; nh = cnt; }
			if (t == T_VOWELS) {
				static const float V[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
				float seg = m * 4.0f; int vi = (int)seg; float u = seg - vi; if (vi >= 4) { vi = 3; u = 1; }
				float F[3]; for (int j = 0; j < 3; j++) F[j] = V[vi][j] * (1 - u) + V[vi + 1][j] * u;
				static const float G[3] = {1.0f, 0.6f, 0.35f}, BW[3] = {110, 160, 220};
				for (int k = 1; k <= 64; k++) {
					float hz = k * 110.0f, a = 0;
					for (int j = 0; j < 3; j++) { float d = (hz - F[j]) / BW[j]; a += G[j] * expf(-d * d); }
					amp[k] = a * powf(k, -0.3f) + 0.02f / k;
				}
				nh = 64;
			}
			if (t == T_DIGITAL) {
				float seg = m * 7.0f; int a0 = (int)seg; float u = seg - a0;
				for (int k = 1; k <= 96; k++) {
					float h = hash01(k, a0) * (1 - u) + hash01(k, a0 + 1) * u;
					amp[k] = (h > 0.55f ? h : 0.0f) / sqrtf((float)k);
				}
				amp[1] = 0.6f; nh = 96;
			}
			if (t == T_PLUCK) {   // KARPLUS: a plucked string, frame by frame. A noise burst goes round a delay line whose two-point
				// average takes cos(pi k / N) off harmonic k every trip (N = 100: a string around 440 Hz). Frame f = 6 f trips in:
				// frame 0 is the bright burst, frame 63 is nearly the bare fundamental. Sweep WT POS 0 -> 1 for the pluck.
				for (int k = 1; k <= 511; k++) {
					float d = cosf(3.14159265f * k / 100.0f); d = d < 0 ? -d : d;
					amp[k] = (0.45f + 0.55f * hash01(k, 11)) * powf(d, 6.0f * f) / sqrtf((float)k) * 3.0f;
				}
				nh = 511;
			}
			// sum of sines via one inverse FFT (was ~100k sinf calls per frame)
			static cpx gb[CYC];
			memset(gb, 0, sizeof(gb));
			for (int k = 1; k <= nh && k < CYC / 2; k++) if (amp[k] != 0) {
				float ph = t == T_DIGITAL ? k * 1.3f : t == T_PLUCK ? hash01(k, 5) * TWO_PI : 0, a = amp[k] * 0.5f;
				gb[k].re = a * sinf(ph); gb[k].im = -a * cosf(ph);
				gb[CYC - k].re = gb[k].re; gb[CYC - k].im = -gb[k].im;
			}
			fft(gb, CYC, 1);
			for (int n = 0; n < CYC; n++) fr[n] = gb[n].re;
			if (t == T_PLUCK) { float pk = 1e-6f; for (int n = 0; n < CYC; n++) pk = fabsf(fr[n]) > pk ? fabsf(fr[n]) : pk; for (int n = 0; n < CYC; n++) fr[n] /= pk; }   // each frame full scale: the envelope does the fading
			continue;
		}
		for (int n = 0; n < CYC; n++) {
			float p = n / (float)CYC, s = 0;
			switch (t) {
			case T_BASIC: {
				float a[4] = {sinf(TWO_PI * p), 4.0f * fabsf(p - 0.5f) - 1.0f, 2.0f * p - 1.0f, p < 0.5f ? 1.0f : -1.0f};
				float seg = m * 3.0f; int si = (int)seg; float u = seg - si;
				if (si >= 3) { si = 2; u = 1; }
				s = a[si] * (1 - u) + a[si + 1] * u;
				break; }
			case T_PWM: { float w = 0.5f - 0.47f * m; s = p < w ? 1.0f : -1.0f; break; }
			case T_SYNC: { float q = p * (1.0f + 7.0f * m); q -= floorf(q); s = (2 * q - 1) * (1.0f - p * 0.3f); break; }
			case T_GROWL: { float I = 6.0f * m; s = sinf(TWO_PI * p + I * sinf(TWO_PI * 2 * p) + 0.5f * I * sinf(TWO_PI * 3 * p)); break; }
			}
			fr[n] = s;
		}
	}
	snprintf(tb->name, sizeof(tb->name), "%s", BUILTIN_NAMES[t]);
}

static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }

// Serum wavetable: RIFF WAV (PCM16/24/32f), frame size from the 'clm ' chunk ("<!>2048 ..."), default 2048
static int loadWav(Table* tb, const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	if (len < 44 || len > 16 * 1024 * 1024) { fclose(f); return 0; }
	u8* b = (u8*)malloc(len);
	if (!b) { fclose(f); return 0; }
	fread(b, 1, len, f); fclose(f);
	int ok = 0, fmt = 0, ch = 1, bits = 16, frameSize = CYC;
	const u8* data = NULL; u32 dlen = 0;
	if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
		long o = 12;
		while (o + 8 <= len) {
			u32 cl = rd32(b + o + 4);
			if (!memcmp(b + o, "fmt ", 4)) { fmt = rd16(b + o + 8); ch = rd16(b + o + 10); bits = rd16(b + o + 22); }
			else if (!memcmp(b + o, "clm ", 4) && cl >= 7) { int fs = atoi((const char*)b + o + 8 + 3); if (fs >= 64 && fs <= 8192) frameSize = fs; }
			else if (!memcmp(b + o, "data", 4)) { data = b + o + 8; dlen = cl; if (o + 8 + (long)dlen > len) dlen = len - o - 8; }
			o += 8 + cl + (cl & 1);
		}
	}
	if (data && ch >= 1 && (bits == 16 || bits == 24 || bits == 32)) {
		int bps = bits / 8 * ch;
		u32 total = dlen / bps;
		int nf = total / frameSize;
		if (nf > MAXFR) nf = MAXFR;
		if (nf >= 1) {
			for (int fi = 0; fi < nf; fi++) {
				float* fr = tb->frames + fi * CYC;
				for (int n = 0; n < CYC; n++) {
					float x = n * (float)frameSize / CYC; int i0 = (int)x; float u = x - i0;
					float v[2];
					for (int k = 0; k < 2; k++) {
						int idx = fi * frameSize + ((i0 + k) % frameSize);
						const u8* s = data + (u32)idx * bps;
						if (bits == 16) v[k] = (s16)rd16(s) / 32768.0f;
						else if (bits == 24) v[k] = ((int)((s[0] << 8) | (s[1] << 16) | ((u32)s[2] << 24)) >> 8) / 8388608.0f;
						else if (fmt == 3) { float fv; memcpy(&fv, s, 4); v[k] = fv; }
						else v[k] = (int)rd32(s) / 2147483648.0f;
					}
					fr[n] = v[0] * (1 - u) + v[1] * u;
				}
			}
			tb->nFrames = nf; ok = 1;
		}
	}
	free(b);
	return ok;
}

// BUSY BOY's waveform, measured from a recording of the Korg sound: harmonic levels (dB) of 93 cycles on G#3..D4.
// The 3rd harmonic leads on every note, the 4th close behind, with a bright knot around the 10th / 11th. Written once to
// the SD card as a wavetable (8 frames, darker -> brighter; frame 5 is the measured one) so it lists with the others.
static const float BUSY_DB[40] = {-31.9f, -30.2f, -0.1f, -10.3f, -25.1f, -41.4f, -32.7f, -23.9f, -33.0f, -13.0f, -23.6f, -30.7f, -38.6f, -44.0f, -33.1f, -33.1f, -28.0f, -37.7f, -44.8f, -39.1f, -46.9f, -41.3f, -38.1f, -35.5f, -40.3f, -46.7f, -34.3f, -44.0f, -41.7f, -44.1f, -43.9f, -46.3f, -44.8f, -47.1f, -41.6f, -44.9f, -45.5f, -49.5f, -52.6f, -47.9f};
static void makeBusyTable(void) {
	const char* path = "sdmc:/3ds/syrup/BUSY BOY.wav";
	FILE* t = fopen(path, "rb"); if (t) { fclose(t); return; }
	const int FR = 8, N = 2048;
	s16* out = (s16*)malloc(FR * N * 2); if (!out) return;
	static const float PH[40] = {0, 2.1f, 4.4f, 1.3f, 5.6f, 0.7f, 3.9f, 2.8f, 0.2f, 4.9f, 1.8f, 3.3f, 5.1f, 0.9f, 2.4f, 4.1f, 1.1f, 5.9f, 3.6f, 0.4f,
		2.9f, 4.6f, 1.6f, 5.3f, 0.1f, 3.1f, 4.8f, 2.2f, 5.7f, 1.4f, 3.8f, 0.6f, 2.6f, 4.3f, 1.9f, 5.5f, 0.3f, 3.4f, 4.0f, 2.0f};
	float* fr = (float*)malloc(N * sizeof(float)); if (!fr) { free(out); return; }
	for (int f = 0; f < FR; f++) {
		float tilt = (f - 5) * 0.6f;   // dB per harmonic above the 4th: frame 5 = as measured
		float pk = 0;
		for (int n = 0; n < N; n++) fr[n] = 0;
		for (int k = 1; k <= 40; k++) {
			float db = BUSY_DB[k - 1] + (k > 4 ? tilt * (k - 4) : 0); if (db < -70) continue;
			float a = powf(10.0f, db / 20.0f), w = TWO_PI * k / N;
			for (int n = 0; n < N; n++) fr[n] += a * sinf(w * n + PH[k - 1]);
		}
		for (int n = 0; n < N; n++) if (fabsf(fr[n]) > pk) pk = fabsf(fr[n]);
		for (int n = 0; n < N; n++) out[f * N + n] = (s16)(fr[n] / (pk + 1e-6f) * 30000);
	}
	free(fr);
	FILE* w = fopen(path, "wb");
	if (w) {
		u32 data = FR * N * 2; const char clm[16] = "<!>2048 10000000";
		u32 hdr[11] = {0x46464952, 36 + 8 + 16 + data, 0x45564157, 0x20746d66, 16, 0x00010001, 44100, 88200, 0x00100002, 0x206d6c63, 16};
		fwrite(hdr, 4, 11, w); fwrite(clm, 1, 16, w);
		u32 dh[2] = {0x61746164, data}; fwrite(dh, 4, 2, w); fwrite(out, 2, FR * N, w); fclose(w);
	}
	free(out);
}
static void scanTables(void) {
	gNFiles = 0;
	DIR* d = opendir("sdmc:/3ds/syrup");
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) && gNFiles < MAXFILES) {
		int l = strlen(e->d_name);
		if (l > 4 && !strcasecmp(e->d_name + l - 4, ".wav")) snprintf(gFiles[gNFiles++], 64, "%s", e->d_name);
	}
	closedir(d);
}

// cache: built-in tables are generated once; the last few SD wavetables stay in memory
static float* gBuiltCache[T_BUILTIN];
static float* builtinFrames(int t) {   // generate (once) and return a built-in table's 64 frames
	if (t < 0 || t >= T_BUILTIN) t = 0;
	if (!gBuiltCache[t]) {
		Table tmp; tmp.frames = (float*)malloc(sizeof(float) * CYC * 64);
		if (!tmp.frames) return NULL;
		genBuiltin(&tmp, t); gBuiltCache[t] = tmp.frames;
	}
	return gBuiltCache[t];
}
#define WCACHE 4
static struct { int idx, nFrames; float* frames; char name[40]; unsigned used; } gWCache[WCACHE];
static unsigned gWClock = 0;
static void tableSelect(Table* tb, int idx) {
	if (idx < T_BUILTIN) {
		if (gBuiltCache[idx]) { memcpy(tb->frames, gBuiltCache[idx], sizeof(float) * CYC * 64); tb->nFrames = 64; snprintf(tb->name, sizeof(tb->name), "%s", BUILTIN_NAMES[idx]); }
		else {
			genBuiltin(tb, idx);
			gBuiltCache[idx] = (float*)malloc(sizeof(float) * CYC * 64);
			if (gBuiltCache[idx]) memcpy(gBuiltCache[idx], tb->frames, sizeof(float) * CYC * 64);
		}
	} else {
		for (int i = 0; i < WCACHE; i++) if (gWCache[i].frames && gWCache[i].idx == idx) {
			memcpy(tb->frames, gWCache[i].frames, sizeof(float) * CYC * gWCache[i].nFrames);
			tb->nFrames = gWCache[i].nFrames; snprintf(tb->name, sizeof(tb->name), "%s", gWCache[i].name); gWCache[i].used = ++gWClock;
			goto peak;
		}
		char path[128]; snprintf(path, sizeof(path), "sdmc:/3ds/syrup/%s", gFiles[idx - T_BUILTIN]);
		if (loadWav(tb, path)) {
			char nm[64]; snprintf(nm, sizeof(nm), "%s", gFiles[idx - T_BUILTIN]);
			nm[strlen(nm) - 4] = 0;
			for (char* c = nm; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
			snprintf(tb->name, sizeof(tb->name), "%.22s", nm);
			int slot = 0; for (int i = 1; i < WCACHE; i++) if (gWCache[i].used < gWCache[slot].used) slot = i;
			free(gWCache[slot].frames);
			gWCache[slot].frames = (float*)malloc(sizeof(float) * CYC * tb->nFrames);
			if (gWCache[slot].frames) {
				memcpy(gWCache[slot].frames, tb->frames, sizeof(float) * CYC * tb->nFrames);
				gWCache[slot].idx = idx; gWCache[slot].nFrames = tb->nFrames; gWCache[slot].used = ++gWClock;
				snprintf(gWCache[slot].name, sizeof(gWCache[slot].name), "%s", tb->name);
			}
		} else { genBuiltin(tb, T_BASIC); snprintf(tb->name, sizeof(tb->name), "LOAD FAILED"); }
	}
peak:;
	float pk = 0;
	for (int i = 0; i < tb->nFrames * CYC; i += 4) { float v = fabsf(tb->frames[i]); if (v > pk) pk = v; }
	tb->peak = pk > 1e-4f ? pk : 1.0f;
}

static float* builtinFrames(int t);
// ---------------------------------------------------------------- patch parameters
enum { W_OFF, W_SYNC, W_WSYNC, W_BENDP, W_BENDM, W_BENDPM, W_PWM, W_ASYMP, W_ASYMM, W_ASYMPM, W_FLIP, W_MIRROR,
       W_REMAP1, W_REMAP2, W_REMAP3, W_REMAP4, W_QUANT, W_FM, W_AM, W_RM, W_COUNT };
static const char* WARP_NAMES[W_COUNT] = {"OFF", "SYNC", "WIN SYNC", "BEND+", "BEND-", "BEND+/-", "PWM", "ASYM+", "ASYM-",
	"ASYM+/-", "FLIP", "MIRROR", "REMAP 1", "REMAP 2", "REMAP 3", "REMAP 4", "QUANTIZE", "FM <OSC", "AM <OSC", "RM <OSC"};

typedef struct {
	int on, table, warp, oct, semi, uni;
	float pos, warpAmt, level, fine, detune, blend, width;
	Table tb; Mips mips;
	float raw[CYC], cyc[CYC];
} Osc;
static Osc gOsc[2];

static const char* SUB_SHAPES[6] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "PULSE 25", "ROUNDED"};
static const char* NOISE_TYPES[6] = {"WHITE", "PINK", "CRACKLE", "BROWN", "BLUE", "CORRUPT"};
static const char* FILT_TYPES[5] = {"LOW PASS", "HIGH PASS", "BAND PASS", "NOTCH", "PEAK"};
#define NLFOSH 13
enum { LS_SINE, LS_TRI, LS_SAW, LS_SQR, LS_SH, LS_STEPS, LS_BOUNCE, LS_EXPUP, LS_EXPDN, LS_PULSE, LS_WTA, LS_WTB, LS_TABLE };
static const char* LFO_SHAPES[NLFOSH] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "S&H", "STEPS", "BOUNCE", "EXP RISE", "EXP FALL", "PULSE 25%", "WT OSC A", "WT OSC B", "WAVETABLE"};
#define NSYNC 10
static const char* SYNC_NAMES[NSYNC] = {"OFF", "4 BARS", "2 BARS", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/8T", "1/16T"};
static const float SYNC_BEATS[NSYNC] = {0, 16, 8, 4, 2, 1, 0.5f, 0.25f, 1.0f / 3, 1.0f / 6};
static const char* LFO_MODES[2] = {"FREE", "TRIG"};
enum { SRC_OFF, SRC_LFO1, SRC_LFO2, SRC_ENV2, SRC_VEL, SRC_STICK, SRC_XY, SRC_TILTX, SRC_TILTY, SRC_MIC, SRC_TWIST, SRC_SHAKE, SRC_CAML, SRC_CAMC, SRC_M1, SRC_M2, SRC_M3, SRC_M4, SRC_CHAOSX, SRC_CHAOSY, SRC_RAND, SRC_NOTE, SRC_LFO3, SRC_LFO4, SRC_SPINX, SRC_SPINY, SRC_MICBRT, SRC_CAMMOVE, SRC_PRESS, SRC_MSLIDE, SRC_STK2X, SRC_STK2Y, SRC_COUNT };
static const char* SRC_NAMES[SRC_COUNT] = {"OFF", "LFO 1", "LFO 2", "ENV 2", "VELOCITY", "STICK", "XY Y", "TILT L/R", "TILT F/B", "BREATH", "TWIST", "SHAKE", "CAM LIGHT", "CAM COLOR", "MACRO 1", "MACRO 2", "MACRO 3", "MACRO 4", "CHAOS X", "CHAOS Y", "RANDOM", "NOTE", "LFO 3", "LFO 4", "SPIN F/B", "SPIN L/R", "VOICE BRIGHT", "CAM MOTION", "PRESSURE", "MPE SLIDE", "STICK 2 X", "STICK 2 Y"};   // PRESSURE: aftertouch (per note with poly AT / MPE); MPE SLIDE: CC 74 per note
// LFO 3 and 4 are drawn: 64 points you paint with the stylus on the MOD page
static float gDl[2][64]; static float gDlRate[2] = {1.0f, 0.5f}, gDlPh[2], gDlVal[2]; static int gDlSync[2] = {0, 0};
static float gChaosX = 0, gChaosY = 0, gChaosRate = 0.3f;   // a Lorenz attractor, slowly wandering, never repeating
static float gMacro[4] = {0, 0, 0, 0};
#define NMOD 8
static int gModCurve[NMOD];
static const char* CURVE_NAMES[4] = {"LINEAR", "EXP", "LOG", "S-CURVE"};
static float gTiltX, gTiltY, gMicEnv, gTwist, gShake, gShakeD, gCamLight, gCamColor;
static float gSpinX, gSpinY, gMicBright, gCamMove;   // gyro pitch/roll speed, voice brightness ("ss" vs "oo"), how much the picture changes
static int gHeadphones;   // (0 until the first check)
enum { DST_APOS, DST_BPOS, DST_AWARP, DST_BWARP, DST_CUTOFF, DST_RES, DST_PITCH, DST_ALEV, DST_BLEV, DST_PAN, DST_FXP, DST_AMP, DST_OFCUT, DST_FXMIX, DST_GRPOS, DST_COUNT };
static const char* DST_NAMES[DST_COUNT] = {"A WT POS", "B WT POS", "A WARP", "B WARP", "CUTOFF", "RES", "PITCH", "A LEVEL", "B LEVEL", "PAN", "FX COLOR", "VOLUME", "OUT CUTOFF", "FX MIX", "GRAIN POS"};   // GRAIN POS: an LFO sweeps where the grains read   // VOLUME: when routed, the source IS the loudness (breath control)
static const char* VOICE_MODES[10] = {"POLY", "MONO", "LEGATO", "808 KIT", "909 KIT", "LOFI KIT", "CHIP KIT", "PERC KIT", "WAVE KIT", "SAMPLE KIT"};   // KIT: keys play the drum kit
static const char* FX_NAMES[4] = {"OFF", "ECHO", "SPACE", "REVERB"};
static const char* ECHO_T_NAMES[3] = {"250MS", "375MS", "500MS"};
static const float ECHO_T[3] = {0.25f, 0.375f, 0.5f};
static const char* DRIVE_TYPES[15] = {"OFF", "TANH", "HARD CLIP", "SINE FOLD", "RECTIFY", "TUBE", "DIODE", "ASYM", "LIN FOLD", "ZERO SQUARE", "HALF RECT", "BITCRUSH", "CUBIC", "X-SHAPE", "OVERDRIVE"};
#define NCHORD 16
static const char* CHORD_NAMES[NCHORD] = {"OFF", "MIN7", "MAJ7", "POWER", "SUS", "SUS2", "ADD9", "6TH", "9TH", "11TH", "13TH", "QUARTAL", "DIM7", "M7B5", "AUG", "7#9"};
// semitones off the key (no SCALE, or a chord that is the same in any key: POWER and the last four)
static const int CHORDS[NCHORD][4] = {{0, -1, -1, -1}, {0, 3, 7, 10}, {0, 4, 7, 11}, {0, 7, 12, -1}, {0, 5, 7, 12}, {0, 2, 7, -1}, {0, 4, 7, 14}, {0, 4, 7, 9},
	{0, 4, 10, 14}, {0, 10, 14, 17}, {0, 4, 10, 21}, {0, 5, 10, 15}, {0, 3, 6, 9}, {0, 3, 6, 10}, {0, 4, 8, 12}, {0, 4, 10, 15}};
// with a SCALE: scale steps above the key, so the chord stays in key (-1 = a fixed chord: use CHORDS)
static const s8 CHORD_DEG[NCHORD][4] = {{0, -1, -1, -1}, {0, 2, 4, 6}, {0, 2, 4, -1}, {-1, -1, -1, -1}, {0, 3, 4, -1}, {0, 1, 4, -1}, {0, 2, 4, 8}, {0, 2, 4, 5},
	{0, 2, 6, 8}, {0, 6, 8, 10}, {0, 2, 6, 12}, {0, 3, 6, 9}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}};

typedef struct { float a, h, d, s, r; } AHDSR;
#define LFON 256
static int gBPM;
typedef struct { int shape, mode; float rate; float ph, val, sh; int sync, table; float pos; float tab[LFON]; int tabKey; } LFO;   // tab: sampled wave for table shapes
typedef struct { int src, dst; float amt; } ModSlot;

static int gSubShape = 0, gSubOct = -1, gNoiseType = 0, gFiltType = 0;
static float gSubLevel = 0.0f, gNoiseLevel = 0.0f;
static float gCutoff = 0.75f, gRes = 0.1f, gKeytrack = 0.3f;
static AHDSR gEnv1 = {0.005f, 0.0f, 0.6f, 0.7f, 0.35f}, gEnv2 = {0.002f, 0.0f, 0.45f, 0.2f, 0.4f};
static LFO gLfo[2] = {{0, 0, 0.6f, 0, 0, 0}, {1, 1, 4.0f, 0, 0, 0}};
static int gLfoCC[2] = {0, 0};   // LFO1/2 CC OUT: send the LFO as this CC (0 = off) to the Network MIDI session, channel 1
static ModSlot gMod[8] = {{SRC_ENV2, DST_CUTOFF, 0.35f}, {SRC_STICK, DST_APOS, 0.5f}, {SRC_LFO1, DST_BPOS, 0.0f}, {SRC_OFF, DST_PITCH, 0.0f}};
static int gVoiceMode = 0, gChord = 0, gFx = 0, gEchoT = 1, gDrive = 0, gBendRange = 2;
static float gGlide = 0.0f, gChorus = 0.3f, gDriveAmt = 0.4f, gMaster = 0.8f;

// ---------------------------------------------------------------- oscillator cycle build
static inline float rd(const float* c, float p) {
	p -= floorf(p);
	float x = p * CYC; int i = (int)x; float u = x - i; int j = (i + 1) & (CYC - 1); i &= CYC - 1;
	return c[i] + (c[j] - c[i]) * u;
}

static void oscMorph(Osc* o, float pos) {   // raw = interpolated table frame at pos
	Table* tb = &o->tb;
	float fp = pos * (tb->nFrames - 1); int f0 = (int)fp; if (f0 > tb->nFrames - 2) f0 = tb->nFrames > 1 ? tb->nFrames - 2 : 0;
	float t = tb->nFrames > 1 ? fp - f0 : 0;
	const float* A = tb->frames + f0 * CYC; const float* B = tb->frames + (tb->nFrames > 1 ? f0 + 1 : f0) * CYC;
	for (int n = 0; n < CYC; n++) o->raw[n] = A[n] + (B[n] - A[n]) * t;
}

static float drive(float s) {
	float d = 1.0f + gDriveAmt * 9.0f;
	switch (gDrive) {
	case 1: return tanhf(s * d) / tanhf(d);
	case 2: { float v = s * d; return v > 1 ? 1 : (v < -1 ? -1 : v); }
	case 3: return sinf(s * d * 1.5708f);
	case 4: return (fabsf(s) * 2.0f - 0.6f) * gDriveAmt + s * (1 - gDriveAmt);
	case 5: { float v = s * d; return (v >= 0 ? tanhf(v) : tanhf(v * 0.6f) / 0.6f * 0.75f) / tanhf(d) * 0.9f; }   // tube: uneven halves
	case 6: { float v = s * d; return v > 0 ? (1 - expf(-v)) / (1 - expf(-d)) : v * 0.15f; }                   // diode: one side conducts
	case 7: { float v = s * d + gDriveAmt * 0.8f; return tanhf(v) / tanhf(d + 0.8f); }                         // asymmetric bias
	case 8: { float v = s * (1 + gDriveAmt * 5); v = fmodf(v + 1, 4.0f); if (v < 0) v += 4; return v < 2 ? v - 1 : 3 - v; }   // triangle fold
	case 9: { float v = s * d; v = v * fabsf(v); return v > 1 ? 1 : v < -1 ? -1 : v; }                       // zero square
	case 10: return s > 0 ? s : s * (1 - gDriveAmt);                                                                   // half-wave rectify
	case 11: { float q = (float)(1 << (int)(1 + (1 - gDriveAmt) * 6)); return roundf(s * q) / q; }                  // bitcrush the cycle
	case 12: { float v = s * (1 + gDriveAmt * 2); if (v > 1) v = 1; if (v < -1) v = -1; return 1.5f * v - 0.5f * v * v * v; }   // cubic soft clip
	case 13: { float a2 = gDriveAmt; return s * (1 - a2) + a2 * (4 * s * s * s - 3 * s); }                            // X-SHAPE: Chebyshev T3 blend
	case 14: { float v = s * d * 1.5f; return (v / (1 + fabsf(v))) * (1 + 1 / d); }                                    // overdrive
	}
	return s;
}

// warp + drive into o->cyc; other = the other oscillator (for FM/AM/RM), ratio = its pitch ratio (whole octaves)
static void oscWarp(Osc* o, float amt, const Osc* other, float ratio) {
	const float* R = o->raw;
	for (int n = 0; n < CYC; n++) {
		float p = n / (float)CYC, q = p, s, x = 2 * p - 1;
		int w = o->warp; float a = amt;
		if (w == W_BENDPM) { if (a < 0.5f) { w = W_BENDM; a = (0.5f - a) * 2; } else { w = W_BENDP; a = (a - 0.5f) * 2; } }
		if (w == W_ASYMPM) { if (a < 0.5f) { w = W_ASYMM; a = (0.5f - a) * 2; } else { w = W_ASYMP; a = (a - 0.5f) * 2; } }
		switch (w) {
		case W_SYNC: case W_WSYNC: q = p * (1.0f + 15.0f * a); break;
		case W_BENDP: q = 0.5f + 0.5f * (x < 0 ? -1 : 1) * powf(fabsf(x), 1.0f + 3.0f * a); break;
		case W_BENDM: q = 0.5f + 0.5f * (x < 0 ? -1 : 1) * powf(fabsf(x), 1.0f / (1.0f + 3.0f * a)); break;
		case W_ASYMP: q = powf(p, 1.0f + 3.0f * a); break;
		case W_ASYMM: q = powf(p, 1.0f / (1.0f + 3.0f * a)); break;
		case W_MIRROR: q = p < 0.5f ? 2 * p : 2 - 2 * p; break;
		case W_REMAP1: q = p + a * 0.25f * sinf(TWO_PI * p); break;
		case W_REMAP2: { float h = p * 2 - floorf(p * 2); float qh = h + a * 0.35f * sinf(TWO_PI * h); q = (floorf(p * 2) + qh) * 0.5f; break; }
		case W_REMAP3: q = (1 - a) * p + a * (1 - cosf(3.14159265f * p)) * 0.5f; break;
		case W_REMAP4: { float h = p * 4 - floorf(p * 4); int sg = (int)(p * 4); float hm = (sg & 1) ? 1 - h : h; q = (1 - a) * p + a * (sg + hm) * 0.25f; break; }
		case W_QUANT: { float steps = CYC * powf(2.0f, -10.0f * a); if (steps < 2) steps = 2; q = floorf(p * steps) / steps; break; }
		case W_FM: q = p + a * 0.3f * rd(other->raw, p * ratio); break;
		}
		if (w == W_PWM) { float wd = 1.0f - 0.95f * a; s = p < wd ? rd(R, p / wd) : 0.0f; }
		else s = rd(R, q);
		if (w == W_MIRROR) s = s * a + rd(R, p) * (1 - a);
		if (w == W_WSYNC) s *= 0.5f - 0.5f * cosf(TWO_PI * p);
		if (w == W_FLIP && p > a) s = -s;
		if (w == W_AM) s *= 1.0f + a * rd(other->raw, p * ratio);
		if (w == W_RM) s *= (1 - a) + a * rd(other->raw, p * ratio) * 1.5f;
		if (gDrive) s = drive(s);
		o->cyc[n] = s;
	}
}

// ---------------------------------------------------------------- sub + noise sources
static Mips gSubMips;
static s16* gNoise; static int gNoiseLen = 16384;
static float gTmpCyc[CYC];

static void buildSub(void) {
	for (int n = 0; n < CYC; n++) {
		float p = n / (float)CYC, s = 0;
		switch (gSubShape) {
		case 0: s = sinf(TWO_PI * p); break;
		case 1: s = 4.0f * fabsf(p - 0.5f) - 1.0f; break;
		case 2: s = 2 * p - 1; break;
		case 3: s = p < 0.5f ? 1 : -1; break;
		case 4: s = p < 0.25f ? 1 : -1; s += 0.5f; break;
		case 5: s = sinf(TWO_PI * p) + 0.35f * sinf(2 * TWO_PI * p) + 0.12f * sinf(3 * TWO_PI * p); s *= 0.8f; break;   // a fuller, organ-ish sine
		}
		gTmpCyc[n] = s;
	}
	gSubMips.normInit = 0;
	mipsSet(&gSubMips, gTmpCyc, 0x7f);
}

static ndspAdpcmData gNoiseAd; static int gNoiseOn; static int gNoiseWasCorrupt = 0;
static u16 gNoiseCoefs[16];
static void noiseChannel(int want);
static void buildNoise(void) {
	unsigned seed = 12345; float b0 = 0, b1 = 0, b2 = 0, br = 0, lw = 0;
	if (gNoiseType == 5) {   // CORRUPT: junk handed to the DSP's own ADPCM decoder: headers with predictors past the table,
		u8* b = (u8*)gNoise;  // scales pushed high, wild coefficient pairs. The decoder's errors are the sound: crunchy, glitchy bursts.
		for (int i = 0; i < gNoiseLen * 2; i++) {
			seed = seed * 1664525u + 1013904223u;
			b[i] = (i & 7) ? (u8)(seed >> 24) : (u8)(((seed >> 20) & 0x70) | (8 + ((seed >> 27) & 7)));
		}
		for (int i = 0; i < 16; i++) { seed = seed * 1664525u + 1013904223u; gNoiseCoefs[i] = (u16)(((s16)((seed >> 16) & 0xFFFF)) / 3); }
		gNoiseAd.index = b[0]; gNoiseAd.history0 = 0; gNoiseAd.history1 = 0;
		DSP_FlushDataCache(gNoise, gNoiseLen * sizeof(s16));
		gNoiseWasCorrupt = 1;
		if (gNoiseOn) { noiseChannel(0); noiseChannel(1); }
		return;
	}
	for (int i = 0; i < gNoiseLen; i++) {
		seed = seed * 1664525u + 1013904223u;
		float w = ((seed >> 9) & 0xffff) / 32768.0f - 1.0f, s = w;
		if (gNoiseType == 1) { b0 = 0.99765f * b0 + w * 0.0990460f; b1 = 0.96300f * b1 + w * 0.2965164f; b2 = 0.57000f * b2 + w * 1.0526913f; s = (b0 + b1 + b2 + w * 0.1848f) * 0.3f; }
		if (gNoiseType == 2) {   // vinyl: soft hiss + sparse crackles
			s = w * 0.08f;
			if (((seed >> 3) & 1023) < 3) s += (((seed >> 13) & 1) ? 1.0f : -1.0f) * (0.4f + 0.6f * (((seed >> 17) & 255) / 255.0f));
		}
		if (gNoiseType == 3) { br = br * 0.985f + w * 0.12f; s = br * 2.2f; }   // brown: deep rumble
		if (gNoiseType == 4) { s = (w - lw) * 0.6f; lw = w; }                 // blue: bright fizz
		if (s > 1) s = 1;
		if (s < -1) s = -1;
		gNoise[i] = (s16)(s * 30000);
	}
	DSP_FlushDataCache(gNoise, gNoiseLen * sizeof(s16));
	if (gNoiseWasCorrupt && gNoiseOn) { noiseChannel(0); noiseChannel(1); }   // back to plain PCM
	gNoiseWasCorrupt = 0;
}

// ---------------------------------------------------------------- voices: DSP + CSND channels
// DSP (24 ch, filtered): OSC A / OSC B unison layers, plus one shared NOISE channel for all notes.
// CSND (~24 ch, no filter): SUB layers and the ECHO / SPACE / REVERB taps. Their "filter" is
// the mip level: a darker (fewer-harmonic) copy of the cycle chosen to match the cutoff.
#define NVS 48
#define NCS 32
#define NOISE_CH 23
enum { R_A0, R_A1, R_A2, R_B0, R_B1, R_B2, R_SUB, R_TAP, R_COUNT };
enum { BK_DSP, BK_CSND };
typedef struct {
	int active, started, note, stage, stage2, isEcho, nch;
	int ch[R_COUNT], bk[R_COUNT], role[R_COUNT], lev[R_COUNT];
	float env, env2, freq, cur, gain, pan, cutMul, startDelay, gate, held, vel, hold1, hold2, relMul, tapDet;
	unsigned parent, id, age;
	int chipCh;   // PSG square channel for the CHIP layer, -1 if none
	int slide;    // 303 slide: glide to the new pitch quickly, no retrigger
	float rnd;    // RANDOM mod source, set per note
	int mch;      // the MIDI channel the note came in on (-1: not MIDI) - MPE sends each note on its own
	float mbend, press, mslide;   // per note: MPE bend (semitones), pressure (aftertouch), slide (CC 74), 0..1
} SVoice;
static SVoice gV[NVS];
static float gPressAll = 0, gSlideAll = 0;   // the latest channel pressure / CC 74 (for notes not on their own channel)
static int gMidiChNow = -1, gMpe = 0;   // the channel of the MIDI note being started; MPE mode (VOICE+FX)
static float gMpeRange = 48;
static int gDspOwner[NCH], gCsOwner[NCS];
static unsigned gAge = 0, gIdc = 0;
static ndspWaveBuf gWB[NCH];
// SOFT START: a note's first few cycles play from a one-shot copy that fades in from silence, then the looping cycle
// takes over at full level. The level used to step up at the DSP's 5 ms frames from wherever the wave happened to
// be: a click on every note of a pure, plucky sound (STEEL PAN, louder with chords).
#define FADE_MAX 4096
static ndspWaveBuf gFadeWB[NCH]; static s16* gFadeBuf[NCH]; static u8 gFadeIn[NCH];
static int gAudioOK = 0, gCsndOK = 0, gNoiseOn = 0; static int gDspAuto;
static float gStick = 0, gXYmod = 0, gChorusPh = 0;
static int gXyGlide = 2; static const char* XYG_NAMES[4] = {"OFF", "FAST", "MEDIUM", "SLOW"}; static float gXyTx = -1, gXyTy = -1;   // XY GLIDE: the dot follows your finger instead of jumping to it
static float gStick2X = 0, gStick2Y = 0; static int gN3 = -1;   // NEW 3DS CONTROLS: the C-stick (STICK 2) and ZL / ZR; -1 = not decided yet (on by itself on a New 3DS / New 2DS XL)
static float gAutoGain = 1.0f;
static float gRevSize = 0.5f, gRevDamp = 0.5f, gFxMix = 0.5f, gFxMixMod = 0;   // gFxMixMod: what the matrix adds (FX MIX dest)
static float fxMixEff(void) { float v = gFxMix + gFxMixMod; return v < 0 ? 0 : v > 1 ? 1 : v; }

static float mtof(float n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }
static int levelFor(float f) { for (int L = 0; L < NLEV; L++) if (f * LEVN[L] <= 2.0f * DSPRATE) return L; return NLEV - 1; }
// darkest level whose harmonics still reach `cut` (CSND layers have no filter); CSND needs >= ~1.1 kHz rate
static int levelDamped(float f, float cut) {
	int L = levelFor(f);
	while (L + 1 < NLEV && f * LEVN[L + 1] * 0.25f >= cut && f * LEVN[L + 1] >= 1100.0f) L++;
	return L;
}
static float oscTune(int o) { return powf(2.0f, gOsc[o].oct + gOsc[o].semi / 12.0f + gOsc[o].fine / 1200.0f); }
static float baseCutoff(void) { return 60.0f * powf(15000.0f / 60.0f, gCutoff); }

static void uniParams(const Osc* o, int u, float* det, float* pan, float* gain) {
	int n = o->uni;
	if (n == 1) { *det = 0; *pan = 0.5f; *gain = 1; return; }
	if (n == 2) { *det = (u == 0 ? -0.5f : 0.5f) * o->detune; *pan = 0.5f + (u == 0 ? -0.5f : 0.5f) * o->width; *gain = 0.71f; return; }
	if (u == 0) { *det = 0; *pan = 0.5f; *gain = 1.0f - 0.7f * o->blend; }
	else { *det = (u == 1 ? -1.0f : 1.0f) * o->detune; *pan = 0.5f + (u == 1 ? -0.5f : 0.5f) * o->width; *gain = 0.35f + 0.5f * o->blend; }
}

static u8 gChClean[24];   // filter state already zeroed while the channel sat idle
static u8 gWbPend[NCH];   // a voice layer waiting to start with its first volume
// VOLUME RAMPS: the voices' volumes are worked out 60 times a second, but the DSP applies a new volume as a step at
// its next 5 ms frame. On a sine (G-FUNK SINE, EP SATURDAY) each 16 ms step was a click, worst on note changes.
// The main loop now sets targets and the audio tick walks every channel there over ~3 DSP frames.
static float gMixTgt[NCH][12], gMixCur[NCH][12]; static u8 gMixLive[NCH];
// TRANCE GATE: the voices chopped in time with the clock by a 16-step on / off pattern. Each lit step opens
// for its first 60%; DEPTH is how far it closes (100% = silence between). Runs in the audio tick with the ramps.
#define NGATE 8
static int gGatePat = 0, gGateRate = 0; static float gGateDepth = 1.0f, gGateG = 1.0f, gGatePh = 0;
static const char* GATE_NAMES[NGATE] = {"OFF", "16THS", "8THS", "OFFBEAT", "TRANCE 1", "TRANCE 2", "HALF", "STUTTER"};
static const char* GRATE_NAMES[3] = {"1/16", "1/8", "1/32"};
static const u16 GATEP[NGATE] = {0, 0xFFFF, 0xAAAA, 0x5555, 0xB6DB, 0xEEEE, 0xF0F0, 0xFAB5};
static int seqStepNow(void);   // the sequencer's step while it plays, else -1 (defined with the sequencer)
static void gateTick(float dt, int seqStep) {   // audio tick, every 5 ms piece
	static int lastSeq = -1;
	if (!gGatePat) { gGateG = 1; return; }
	float r = gGateRate == 1 ? 0.5f : gGateRate == 2 ? 2.0f : 1.0f;
	gGatePh += dt * gBPM / 60.0f * 4.0f * r;
	if (seqStep >= 0 && seqStep != lastSeq && gGateRate == 0 && gSeqRate == 0) gGatePh = (float)seqStep;   // lock to the sequencer's steps
	lastSeq = seqStep;
	if (gGatePh >= 16) gGatePh -= 16 * floorf(gGatePh / 16);
	int st = (int)gGatePh; float fr = gGatePh - st;
	int open = (GATEP[gGatePat] & (0x8000 >> st)) && fr < 0.6f;
	float tgt = open ? 1.0f : 1.0f - gGateDepth;
	gGateG += (tgt - gGateG) * 0.5f;   // ~10 ms edges: a chop, not a click
}
static void mixRampTick(void) {   // audio tick, voice lock held
	static float lastG = -1, lastGate = 1; float g = gAutoGain * gGateG; int gch = g != lastG; lastG = g; (void)lastGate;
	for (int c = 0; c < NCH; c++) if (gMixLive[c]) {
		int moved = gch;
		for (int k = 0; k < 12; k++) { float d = gMixTgt[c][k] - gMixCur[c][k]; if (d > 1e-5f || d < -1e-5f) { gMixCur[c][k] += fabsf(d) < 0.002f ? d : d * (d > 0 ? 0.5f : 0.75f); /* up a little slower than down: no added sustain */ moved = 1; } }
		if (moved) { float m[12]; for (int k = 0; k < 12; k++) m[k] = gMixCur[c][k] * g; ndspChnSetMix(c, m); }
	}
}
static void stopCh(int bk, int ch) {
	if (bk == BK_DSP) {
		ndspChnWaveBufClear(ch); gDspOwner[ch] = -1; if (ch >= 0 && ch < NCH) { gWbPend[ch] = 0; gMixLive[ch] = 0; }
		if (gQScream > 0.001f && ch < 24) { ndspChnIirBiquadSetParamsCustomFilter(ch, 1.0f, 0, 0, 0, 0, 0); gChClean[ch] = 1; }   // flush while silent: no gap on the next note
	}
	else { if (ch < 0 || ch >= NCS) return; if (gCsndOK) CSND_SetPlayState(ch, 0); gCsOwner[ch] = -1; }   // (-1 would land on channel 31: the looper)
}

static void chipStop(SVoice* v);
static void freeVoice(SVoice* v) {
	chipStop(v);
	if (v->started) for (int i = 0; i < v->nch; i++) if (v->ch[i] >= 0) { stopCh(v->bk[i], v->ch[i]); v->ch[i] = -1; }
	v->active = 0; v->started = 0;
}

#define THRU_CH0 19   // CHIP > FX plays the second chip's capture on DSP channels 19 (left) and 20 (right)
static volatile int gThruOn = 0;
static int dspUsable(int c) { return !(gNoiseOn && c == NOISE_CH) && !(gGrains && (c == 21 || c == 22)) && !(gThruOn && (c == THRU_CH0 || c == THRU_CH0 + 1)); }
static int tapeReserved(int c);
static int drumReserved(int c);
static int chipReserved(int c);
static int echoReserved(int c);
static int csUsable(int c) { return gCsndOK && (csndChannels & BIT(c)) && !tapeReserved(c) && !drumReserved(c) && !chipReserved(c) && !echoReserved(c); }
static int freeCount(int bk) {
	int n = 0;
	if (bk == BK_DSP) { for (int c = 0; c < NCH; c++) if (dspUsable(c) && gDspOwner[c] < 0) n++; }
	else for (int c = 0; c < NCS; c++) if (csUsable(c) && gCsOwner[c] < 0) n++;
	return n;
}
static int voiceUses(const SVoice* v, int bk) { for (int i = 0; i < v->nch; i++) if (v->bk[i] == bk) return 1; return 0; }

// make room for n channels on a backend: oldest tap, then oldest releasing voice, then oldest
static int stealFor(int bk, int n, int self) {
	while (freeCount(bk) < n) {
		int vi = -1; unsigned best = 0xffffffff;
		for (int pass = 0; pass < 3 && vi < 0; pass++)
			for (int i = 0; i < NVS; i++) {
				SVoice* v = &gV[i];
				if (i == self || !v->active || !v->started || !voiceUses(v, bk)) continue;
				if (pass == 0 && !v->isEcho) continue;
				if (pass == 1 && v->stage != 4) continue;
				if (v->age < best) { best = v->age; vi = i; }
			}
		if (vi < 0) return 0;
		freeVoice(&gV[vi]);
	}
	return 1;
}
static int takeCh(int bk, int vi) {
	if (bk == BK_DSP) { for (int c = 0; c < NCH; c++) if (dspUsable(c) && gDspOwner[c] < 0) { gDspOwner[c] = vi; return c; } }
	else for (int c = 0; c < NCS; c++) if (csUsable(c) && gCsOwner[c] < 0) { gCsOwner[c] = vi; return c; }
	return -1;
}

static int gFilt2 = 0, gLowCut = 2;
static const char* FILT2_NAMES[3] = {"OFF", "+6DB SLOPE", "LOW CUT"};
static const char* LOWCUT_NAMES[6] = {"30HZ", "60HZ", "120HZ", "250HZ", "500HZ", "1KHZ"};
static const float LOWCUT_HZ[6] = {30, 60, 120, 250, 500, 1000};
static int masterActive(void);
static u8 gScreamFlush[24]; static volatile int gDspStalled = 0;   // CONTAINMENT: one tick of a zero filter clears a voice's stale self-oscillation before its next note
static void filterSet(int ch, float hz, float res) {
	if (hz < 40) hz = 40;
	if (hz > 15000) hz = 15000;
	float q = 0.707f + res * 9.0f;
	if (gQScream > 0.001f && gFiltType == 0 && ch != NOISE_CH && masterActive()) {   // only with the containment in the path
		// FILTER SCREAM: poles at radius r around the cutoff. Past r = 1 the DSP's fixed-point filter rings by itself and
		// saturates: a screaming self-oscillation the stock filter types never allow.
		if (ch < 24 && gScreamFlush[ch]) {   // b = a = 0: output 0 and both state words 0 after one sample
			gScreamFlush[ch] = 0;
			ndspChnIirBiquadSetParamsCustomFilter(ch, 1.0f, 0, 0, 0, 0, 0); ndspChnIirBiquadSetEnable(ch, true); ndspChnIirMonoSetEnable(ch, false);
			return;
		}
		// Poles stay just INSIDE the unit circle (r < 1). Past 1 the DSP's filter saturated on its own and, on hardware,
		// could lock the DSP up: no sound, PANIC dead, the app unable to close (Matt: only a power-off cleared it). At
		// r = 0.999 it still rings like mad off every note (a Q in the hundreds) but always dies away.
		float w = TWO_PI * hz / DSPRATE, cw = cosf(w), r = 0.975f + gQScream * 0.022f + res * 0.002f;
		if (r > 0.999f) r = 0.999f;
		float rmax = 0.9995f / (cw > 0.5f ? cw : 0.5f);   // ceiling: keep a1 inside the DSP's Q14 range (past it the coefficient wraps and the filter latches)
		if (r > rmax) r = rmax;
		float a1 = -2.0f * r * cw, a2 = r * r;
		float b0 = (1.0f + a1 + a2) * 0.9f + 0.003f;   // (+3.5 dB: makes up the level the old unstable ring added)
		ndspChnIirBiquadSetParamsCustomFilter(ch, 1.0f, a1, a2, b0, 0, 0);
		ndspChnIirBiquadSetEnable(ch, true);
		ndspChnIirMonoSetEnable(ch, false);
		return;
	}
	switch (gFiltType) {
	case 0: ndspChnIirBiquadSetParamsLowPassFilter(ch, hz, q); break;
	case 1: ndspChnIirBiquadSetParamsHighPassFilter(ch, hz, q); break;
	case 2: ndspChnIirBiquadSetParamsBandPassFilter(ch, hz, 0.5f + res * 6.0f); break;
	case 3: ndspChnIirBiquadSetParamsNotchFilter(ch, hz, 0.5f + res * 6.0f); break;
	case 4: ndspChnIirBiquadSetParamsPeakingEqualizer(ch, hz, 0.7f + res * 6.0f, 4.0f); break;
	}
	ndspChnIirBiquadSetEnable(ch, true);
	// second DSP filter stage (one-pole): steeper low pass, or a low cut under the main filter
	if (gFilt2 == 1 && gFiltType == 0) { ndspChnIirMonoSetParamsLowPassFilter(ch, hz * 1.3f); ndspChnIirMonoSetEnable(ch, true); }
	else if (gFilt2 == 2) { ndspChnIirMonoSetParamsHighPassFilter(ch, LOWCUT_HZ[gLowCut]); ndspChnIirMonoSetEnable(ch, true); }
	else ndspChnIirMonoSetEnable(ch, false);
}

static Mips* roleMips(int role) {
	if (role == R_SUB) return &gSubMips;
	if (role == R_TAP) return &gOsc[gOsc[0].on ? 0 : 1].mips;
	return &gOsc[role >= R_B0].mips;
}
static float roleFreq(const SVoice* v, int role) {
	float f = v->cur;
	if (role <= R_A2) return f * oscTune(0);
	if (role <= R_B2) return f * oscTune(1);
	if (role == R_SUB) return f * powf(2.0f, (float)gSubOct);
	return f * oscTune(gOsc[0].on ? 0 : 1);
}

static void startLayer(SVoice* v, int vi, int i, int role, int bk) {
	int ch = takeCh(bk, vi);
	v->ch[i] = ch; v->bk[i] = bk; v->role[i] = role;
	float f = roleFreq(v, role);
	v->lev[i] = (role == R_TAP || (role == R_SUB && bk == BK_CSND)) ? levelDamped(f, baseCutoff() * v->cutMul + 200) : levelFor(f * 1.03f);
	Mips* m = roleMips(role);
	if (!gInAudioTick) LightLock_Lock(&gMipsLock);   // the audio clock already holds it
	if (!(m->built & (1u << v->lev[i]))) {
		if (gInAudioTick) {   // no FFT in the audio thread: borrow the nearest smoother level that's built
			int L = v->lev[i] + 1; while (L < NLEV && !(m->built & (1u << L))) L++;
			if (L < NLEV) v->lev[i] = L; else if (!gTickNoMips) mipsBuildLevel(m, v->lev[i]);   // (without the lock the worker owns the FFT buffers: play what's there)
		} else mipsBuildLevel(m, v->lev[i]);
	}
	if (!gInAudioTick) LightLock_Unlock(&gMipsLock);
	int N = LEVN[v->lev[i]];
	if (bk == BK_DSP) {
		float mix[12] = {0};
		ndspChnSetMix(ch, mix);
		ndspChnSetRate(ch, f * N);
		ndspChnSetInterp(ch, gQGrit == 2 ? NDSP_INTERP_NONE : gQGrit == 1 ? NDSP_INTERP_LINEAR : NDSP_INTERP_POLYPHASE);
		if (gQScream > 0.001f && ch < 24 && !gChClean[ch]) gScreamFlush[ch] = 1;   // stolen while ringing: flush now (one 5 ms gap)
		if (ch < 24) gChClean[ch] = 0;
		filterSet(ch, 2000, 0);
		memset(&gWB[ch], 0, sizeof(ndspWaveBuf));
		gWB[ch].data_vaddr = m->lev[v->lev[i]];
		gWB[ch].nsamples = N;
		gWB[ch].looping = true;
		int soft = 0;
		if (!v->isEcho && ch < NCH) {   // SOFT START
			if (!gFadeBuf[ch]) gFadeBuf[ch] = (s16*)linearAlloc(FADE_MAX * 2);
			if (gFadeBuf[ch] && N > 0 && N <= FADE_MAX) {
				// k whole cycles fading in (~4 ms), then the looping cycle carries on from its start: seamless. Both start
				// together with the note's first level (gWbPend), so the level goes straight on and only the data fades
				int z = 0, k = (int)ceilf(0.004f * f); if (k < 1) k = 1; if (k > 8) k = 8;
				while (k > 1 && z + k * N > FADE_MAX) k--;
				if (z + k * N > FADE_MAX) z = FADE_MAX - k * N; if (z < 0) z = 0;
				int fl = k * N, tot = z + fl; const s16* src = m->lev[v->lev[i]];
				for (int j = 0; j < z; j++) gFadeBuf[ch][j] = 0;
				for (int j = 0; j < fl; j++) { float u = j / (float)fl; gFadeBuf[ch][z + j] = (s16)(src[j % N] * u * u * (3 - 2 * u)); }
				DSP_FlushDataCache(gFadeBuf[ch], tot * 2);
				memset(&gFadeWB[ch], 0, sizeof(ndspWaveBuf)); gFadeWB[ch].data_vaddr = gFadeBuf[ch]; gFadeWB[ch].nsamples = tot; gFadeWB[ch].looping = false;
				soft = 1;
			}
		}
		memset(gMixCur[ch], 0, sizeof(gMixCur[ch])); memset(gMixTgt[ch], 0, sizeof(gMixTgt[ch])); gMixLive[ch] = !v->isEcho;
		if (soft) { gFadeIn[ch] = 1; gWbPend[ch] = 1; }   // the buffers go on with the first level (voicesUpdate)
		else { gFadeIn[ch] = 0; ndspChnWaveBufAdd(ch, &gWB[ch]); }   // starts silent; the audio tick ramps it in
	} else {
		u32 pa = osConvertVirtToPhys(m->lev[v->lev[i]]);
		u32 timer = CSND_TIMER(f * N);
		if (timer < 0x42) timer = 0x42;
		if (timer > 0xFFFF) timer = 0xFFFF;
		u32 flags = SOUND_FORMAT_16BIT | SOUND_REPEAT | (gQGrit == 2 ? 0 : SOUND_LINEAR_INTERP) | SOUND_ENABLE | SOUND_CHANNEL(ch) | (timer << 16);
		CSND_SetChnRegs(flags, pa, pa, N * 2, 0, 0);
	}
}

static void chipStart(SVoice* v, int vi);
static void startVoice(int vi) {
	SVoice* v = &gV[vi];
	int roles[R_COUNT], bks[R_COUNT], nr = 0, nd = 0, nc = 0;
	if (v->isEcho) { if (!gOsc[0].on && !gOsc[1].on) { v->active = 0; return; } roles[nr] = R_TAP; bks[nr++] = gCsndOK ? BK_CSND : BK_DSP; }
	else {
		for (int o = 0; o < 2; o++) if (gOsc[o].on) for (int u = 0; u < gOsc[o].uni; u++) { roles[nr] = (o ? R_B0 : R_A0) + u; bks[nr++] = BK_DSP; }
		if (gSubLevel > 0.01f) { roles[nr] = R_SUB; bks[nr++] = gCsndOK ? BK_CSND : BK_DSP; }
	}
	for (int i = 0; i < nr; i++) { if (bks[i] == BK_DSP) nd++; else nc++; }
	if ((nr == 0 && (v->isEcho || gChipLevel <= 0.001f)) || (nd && !stealFor(BK_DSP, nd, vi)) || (nc && !stealFor(BK_CSND, nc, vi))) { v->active = 0; return; }
	v->nch = nr;
	for (int i = 0; i < nr; i++) startLayer(v, vi, i, roles[i], bks[i]);
	v->started = 1;
	chipStart(v, vi);
}

static SVoice* newVoice(void) {
	for (int i = 0; i < NVS; i++) if (!gV[i].active) return &gV[i];
	int vi = 0; unsigned best = 0xffffffff;
	for (int i = 0; i < NVS; i++) if (gV[i].isEcho && gV[i].age < best) { best = gV[i].age; vi = i; }
	if (best == 0xffffffff) for (int i = 0; i < NVS; i++) if (gV[i].age < best) { best = gV[i].age; vi = i; }
	freeVoice(&gV[vi]);
	return &gV[vi];
}

static void initVoice(SVoice* v) {
	memset(v, 0, sizeof(*v));
	v->chipCh = -1;
	for (int i = 0; i < R_COUNT; i++) v->ch[i] = -1;
	v->active = 1; v->stage = 1; v->stage2 = 1; v->age = gAge++; v->id = ++gIdc; v->gate = -1;
	v->gain = 1; v->cutMul = 1; v->relMul = 1; v->mch = -1;
}

static void lfoTrig(void) { for (int i = 0; i < 2; i++) if (gLfo[i].mode == 1) gLfo[i].ph = 0; }

static int busActive(void);
static int gDry = 0;   // DRY check (FX ORDER view): every effect off for a listen, the OUT FILTER and limiter stay; not saved
static void addTaps(SVoice* v) {
	if (gDry || fxMixEff() < 0.002f) return;
	if (busActive() && (gFx == 1 || gFx == 3)) return;   // reverb / echo run on the DSP effect buses
	int ne = 0; float t[8], g[8], p[8], c[8], rm[8], dt[8];
	float mix = fxMixEff() * 2.0f;
	if (gFx == 1) { float d = ECHO_T[gEchoT]; for (int k = 0; k < 3; k++) { t[k] = d * (k + 1); g[k] = 0.6f * powf(0.55f, (float)k); p[k] = (k & 1) ? 0.45f : -0.45f; c[k] = powf(0.7f, (float)(k + 1)); rm[k] = 1; dt[k] = 0; } ne = 3; }
	if (gFx == 2) { static const float T[6] = {0.041f, 0.067f, 0.109f, 0.157f, 0.233f, 0.331f};
		for (int k = 0; k < 6; k++) { t[k] = T[k]; g[k] = 0.5f * powf(0.8f, (float)k); p[k] = (k & 1) ? 0.4f : -0.4f; c[k] = powf(0.82f, (float)(k + 1)); rm[k] = 1; dt[k] = 0; } ne = 6; }
	if (gFx == 3) {   // REVERB: 8 diffuse, detuned, darkening taps with long releases
		static const float M[8] = {1.0f, 1.7f, 2.6f, 3.7f, 5.0f, 6.6f, 8.5f, 11.0f};
		float base = 0.018f + gRevSize * 0.05f, damp = 0.55f + 0.4f * (1.0f - gRevDamp);
		for (int k = 0; k < 8; k++) {
			t[k] = base * M[k]; g[k] = 0.42f * powf(0.84f, (float)k);
			p[k] = ((k & 1) ? 1.0f : -1.0f) * (0.2f + 0.05f * k); c[k] = powf(damp, (float)(k + 1));
			rm[k] = 1.0f + gRevSize * 5.0f; dt[k] = ((k & 1) ? 1.0f : -1.0f) * (2.0f + k * 0.8f);
		}
		ne = 8;
	}
	for (int k = 0; k < ne; k++) {
		SVoice* e = newVoice(); initVoice(e);
		e->note = v->note; e->freq = v->freq; e->cur = v->freq; e->vel = v->vel;
		e->gain = g[k] * mix; e->pan = p[k]; e->cutMul = c[k]; e->startDelay = t[k]; e->isEcho = 1; e->parent = v->id;
		e->relMul = rm[k]; e->tapDet = dt[k];
	}
}

// ---- MIRROR (Wi-Fi): what this 3DS plays goes out as small events to a browser running Syrup as a muted copy, which
// draws the top screen (and can sound) in step: notes, drum hits, the live mod values, the page you're on, the patch
// when it changes. gMirOn: a mirror is listening (3DS); gMirror: this is the mirror (web). Events: type, u16 length, data.
static volatile int gMirOn = 0, gMirNew = 0, gMirClients = 0; static int gMirror = 0;
#define MIRQ 65536
static u8 gMirQ[MIRQ]; static volatile u32 gMirW = 0, gMirR = 0; static volatile int gMirSpin = 0;
static void mirEmit(u8 t, const void* p, int n) {   // any thread; never waits long (a full ring drops: the next snapshot repairs)
	if (!gMirOn || n < 0 || n > 16000) return;
	int tries = 0; while (__sync_lock_test_and_set(&gMirSpin, 1)) { if (++tries > 4000) return; }
	u32 w = gMirW, need = 3 + (u32)n;
	if (((gMirR - w - 1) & (MIRQ - 1)) < need) { __sync_lock_release(&gMirSpin); return; }
	gMirQ[w] = t; gMirQ[(w + 1) & (MIRQ - 1)] = (u8)(n & 255); gMirQ[(w + 2) & (MIRQ - 1)] = (u8)(n >> 8);
	for (int i = 0; i < n; i++) gMirQ[(w + 3 + i) & (MIRQ - 1)] = ((const u8*)p)[i];
	__sync_synchronize(); gMirW = (w + need) & (MIRQ - 1);
	__sync_lock_release(&gMirSpin);
}
static void mirNote(u8 t, int note, float x) { if (!gMirOn) return; u8 b[2] = {(u8)(note < 0 ? 0 : note > 127 ? 127 : note), (u8)(x <= 0 ? 0 : x >= 1.7f ? 255 : x * 150)}; mirEmit(t, b, 2); }
static void mirRetune(int from, int to, int kind) { if (!gMirOn) return; u8 b[3] = {(u8)from, (u8)to, (u8)kind}; mirEmit('G', b, 3); }
static void kitNoteOn(int note, float vel); static void kitHitSound(int d, float vel);
static float gNoteVel[128], gKeyFade[128];   // keyboard feedback: how hard each held note was hit; the glow a released key leaves
static int gChordKey = -1;
static void voiceOn(int note, float vel) {
	mirNote('N', note, vel);
	if (note >= 0 && note < 128) gNoteVel[note] = vel;
	if (!gAudioOK) return;
	if (gVoiceMode >= 3) { kitNoteOn(note, vel); return; }   // DRUM KIT patch: a key is a drum hit
	if (gVoiceMode != 0) {   // mono / legato: one voice
		SVoice* m = NULL;
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) { m = &gV[i]; break; }
		if (m && gVoiceMode == 2) { m->note = note; m->freq = mtof((float)note); return; }   // legato: just glide
		float from = m ? m->cur : 0;
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho) { gV[i].stage = gV[i].stage2 = 4; gV[i].relMul = 0.05f / (gEnv1.r + 0.0005f); }   // the old note fades out over 50 ms (cutting it dead clicked; volumes move at 60 Hz)
		SVoice* v = newVoice(); initVoice(v);
		v->note = note; v->freq = mtof((float)note); v->cur = (from > 0 && gGlide > 0) ? from : v->freq; v->vel = vel; v->mch = gMidiChNow;
		lfoTrig(); startVoice(v - gV);
		if (v->active) addTaps(v);
		return;
	}
	SVoice* v = newVoice(); initVoice(v);
	v->note = note; v->freq = mtof((float)note); v->cur = v->freq; v->vel = vel; v->rnd = (rand() % 2001) / 1000.0f - 1; v->mch = gMidiChNow;
	lfoTrig();
	startVoice(v - gV);
	if (v->active) addTaps(v);
}

static void voiceOff(int note) {
	mirNote('O', note, 0);
	for (int i = 0; i < NVS; i++) {
		SVoice* v = &gV[i];
		if (!v->active || v->isEcho || v->note != note || v->stage == 4) continue;
		v->stage = 4; v->stage2 = 4;
		for (int k = 0; k < NVS; k++) if (gV[k].active && gV[k].isEcho && gV[k].parent == v->id && gV[k].gate < 0) gV[k].gate = v->held;
	}
}
static void allOff(void) { mirEmit('A', NULL, 0); for (int i = 0; i < NVS; i++) if (gV[i].active) { if (gV[i].isEcho && !gV[i].started) gV[i].active = 0; else gV[i].stage = gV[i].stage2 = 4; } }

static void envStep(float* e, int* st, const AHDSR* E, float dt, float* hold, float relMul) {
	switch (*st) {
	case 1: *e += dt / (E->a + 0.0005f); if (*e >= 1) { *e = 1; *st = E->h > 0 ? 5 : 2; *hold = 0; } break;
	case 5: *hold += dt; if (*hold >= E->h) *st = 2; break;
	case 2: *e -= dt * (1 - E->s) / (E->d + 0.0005f); if (*e <= E->s) { *e = E->s; *st = 3; } break;
	case 4: *e -= dt / (E->r * relMul + 0.0005f); if (*e < 0) *e = 0; break;
	}
}

// shape of an LFO at phase p (also used to draw it)
static float lfoShapeAt(const LFO* l, float p) {
	switch (l->shape) {
	case LS_SINE: return sinf(TWO_PI * p);
	case LS_TRI: return 1.0f - 4.0f * fabsf(p - 0.5f);
	case LS_SAW: return 2 * p - 1;
	case LS_SQR: return p < 0.5f ? 1 : -1;
	case LS_SH: return l->sh;
	case LS_STEPS: { static const float ST[8] = {-1, 0.43f, -0.29f, 1, -0.71f, 0.14f, 0.71f, -0.43f}; return ST[(int)(p * 8) & 7]; }
	case LS_BOUNCE: return 2 * fabsf(sinf(TWO_PI * p)) - 1;
	case LS_EXPUP: return 2 * (expf(4 * p) - 1) / (expf(4) - 1) - 1;
	case LS_EXPDN: return 2 * expf(-5 * p) - 1;
	case LS_PULSE: return p < 0.25f ? 1 : -1;
	default: { float x = p * LFON; int i = (int)x; float u = x - i; return l->tab[i % LFON] * (1 - u) + l->tab[(i + 1) % LFON] * u; }
	}
}
// ---- wavetable LFO shapes: one frame of a built-in table, or the live cycle of OSC A / B
static void lfoFill(LFO* l, const float* cyc, float peak) {
	float pk = 1e-6f;
	for (int i = 0; i < LFON; i++) { l->tab[i] = cyc[i * CYC / LFON]; float a = fabsf(l->tab[i]); if (a > pk) pk = a; }
	(void)peak;
	for (int i = 0; i < LFON; i++) l->tab[i] /= pk;
}
static float lfoValue(LFO* l) { return lfoShapeAt(l, l->ph); }
static float lfoRateHz(const LFO* l, int bpm) { return l->sync > 0 ? bpm / 60.0f / SYNC_BEATS[l->sync] : l->rate; }

static float srcValue(int src, const SVoice* v) {
	switch (src) {
	case SRC_LFO1: return gLfo[0].val;
	case SRC_LFO2: return gLfo[1].val;
	case SRC_ENV2: return v ? v->env2 : 0;
	case SRC_VEL: return v ? v->vel : 0;
	case SRC_STICK: return gStick;
	case SRC_XY: return gXYmod;
	case SRC_TILTX: return gTiltX;
	case SRC_TILTY: return gTiltY;
	case SRC_MIC: return gMicEnv;
	case SRC_TWIST: return gTwist;
	case SRC_SHAKE: return gShake;
	case SRC_CAML: return gCamLight;
	case SRC_CAMC: return gCamColor;
	case SRC_M1: case SRC_M2: case SRC_M3: case SRC_M4: return gMacro[src - SRC_M1];
	case SRC_CHAOSX: return gChaosX;
	case SRC_CHAOSY: return gChaosY;
	case SRC_RAND: return v ? v->rnd : 0;   // a new random value for every note
	case SRC_NOTE: { float x = v ? (v->note - 60) / 24.0f : 0; return x > 1 ? 1 : x < -1 ? -1 : x; }   // keytrack: C3 = 0
	case SRC_LFO3: return gDlVal[0];
	case SRC_LFO4: return gDlVal[1];
	case SRC_SPINX: return gSpinX;
	case SRC_SPINY: return gSpinY;
	case SRC_MICBRT: return gMicBright;
	case SRC_CAMMOVE: return gCamMove;
	case SRC_PRESS: return v && v->mch >= 0 ? (v->press > gPressAll ? v->press : gPressAll) : gPressAll;
	case SRC_STK2X: return gStick2X;
	case SRC_STK2Y: return gStick2Y;
	case SRC_MSLIDE: return v && v->mch >= 0 ? v->mslide : gSlideAll;
	}
	return 0;
}

static float gLockCut, gLockRes;   // sequencer step locks (set further down)
static float modSum(int dst, const SVoice* v) {
	float s = 0;
	for (int i = 0; i < NMOD; i++) if (gMod[i].src != SRC_OFF && gMod[i].dst == dst) {
		float x = srcValue(gMod[i].src, v), a = fabsf(x);
		switch (gModCurve[i]) {   // response curves
		case 1: x = x * a; break;
		case 2: x = x < 0 ? -sqrtf(a) : sqrtf(a); break;
		case 3: { float u = (x + 1) * 0.5f; u = u * u * (3 - 2 * u); x = u * 2 - 1; } break;
		}
		s += x * gMod[i].amt;
	}
	if (dst == DST_CUTOFF) s += gLockCut; else if (dst == DST_RES) s += gLockRes;   // sequencer step locks
	return s;
}

static void noiseChannel(int want) {   // shared noise layer on DSP channel NOISE_CH
	if (want == gNoiseOn) return;
	if (want) {
		if (gDspOwner[NOISE_CH] >= 0) freeVoice(&gV[gDspOwner[NOISE_CH]]);
		gNoiseOn = 1;
		float mix[12] = {0};
		ndspChnSetMix(NOISE_CH, mix);
		ndspChnSetRate(NOISE_CH, DSPRATE);
		memset(&gWB[NOISE_CH], 0, sizeof(ndspWaveBuf));
		gWB[NOISE_CH].data_vaddr = gNoise; gWB[NOISE_CH].nsamples = gNoiseLen; gWB[NOISE_CH].looping = true;
		if (gNoiseType == 5) {   // CORRUPT: the DSP decodes junk ADPCM (14 samples per 8-byte frame)
			ndspChnSetFormat(NOISE_CH, NDSP_FORMAT_MONO_ADPCM); ndspChnSetAdpcmCoefs(NOISE_CH, gNoiseCoefs);
			gWB[NOISE_CH].nsamples = gNoiseLen * 2 / 8 * 14; gWB[NOISE_CH].adpcm_data = &gNoiseAd;
		} else ndspChnSetFormat(NOISE_CH, NDSP_FORMAT_MONO_PCM16);
		ndspChnWaveBufAdd(NOISE_CH, &gWB[NOISE_CH]);
	} else { ndspChnWaveBufClear(NOISE_CH); gNoiseOn = 0; }
}

static float busSend(int bus);
static int masterActive(void);
static int chipRate(float f);
static u32 csSend(float l, float r);
static int gOutMode;
static inline u32 csMono(u32 v) { if (gOutMode != 3) return v; u32 a = ((v & 0xFFFF) + (v >> 16)) / 2; return a | (a << 16); }   // OUTPUT MONO: the second chip's left / right averaged (the DSP does its own)
static void voicesUpdate(float dt, float bend) {
	for (int i = 0; i < 2; i++) {
		LFO* l = &gLfo[i];
		if (l->shape == LS_WTA || l->shape == LS_WTB) lfoFill(l, gOsc[l->shape - LS_WTA].cyc, 1);   // follows the live cycle
		else if (l->shape == LS_TABLE) {
			int fi = (int)(l->pos * 63 + 0.5f), key = l->table * 100 + fi;
			if (key != l->tabKey) { float* fr = builtinFrames(l->table); if (fr) { lfoFill(l, fr + fi * CYC, 1); l->tabKey = key; } }
		}
		float prev = l->ph; l->ph += dt * lfoRateHz(l, gBPM); if (l->ph >= 1) l->ph -= floorf(l->ph);
		if (l->ph < prev) l->sh = hash01((int)(gAge + i * 77), (int)(l->rate * 100 + gIdc)) * 2 - 1;
		l->val = lfoValue(l);
	}
	for (int k = 0; k < 2; k++) {   // drawn LFOs: read the painted shape, smoothly
		float hz = gDlSync[k] > 0 ? gBPM / 60.0f / SYNC_BEATS[gDlSync[k]] : gDlRate[k];
		gDlPh[k] += dt * hz; if (gDlPh[k] >= 1) gDlPh[k] -= floorf(gDlPh[k]);
		float x = gDlPh[k] * 64; int i = (int)x; float u = x - i;
		gDlVal[k] = gDl[k][i & 63] * (1 - u) + gDl[k][(i + 1) & 63] * u;
	}
	{   // chaos: Lorenz system, normalised to about -1..1
		static float x = 0.1f, y = 0, z = 0;
		float h = dt * (0.2f + gChaosRate * 2.5f); if (h > 0.03f) h = 0.03f;
		for (int k = 0; k < 2; k++) { float dx = 10 * (y - x), dy = x * (28 - z) - y, dz = x * y - 2.6667f * z; x += dx * h * 0.5f; y += dy * h * 0.5f; z += dz * h * 0.5f; }
		gChaosX = x / 20.0f; gChaosY = (z - 25) / 25.0f;
		if (gChaosX > 1) gChaosX = 1;
		if (gChaosX < -1) gChaosX = -1;
		if (gChaosY > 1) gChaosY = 1;
		if (gChaosY < -1) gChaosY = -1;
	}
	gChorusPh += dt * 0.7f * TWO_PI;
	noiseChannel(gNoiseLevel > 0.01f);
	static float dL[NCH], dR[NCH], cL[NCS], cR[NCS];
	for (int c = 0; c < NCH; c++) dL[c] = dR[c] = -1;
	for (int c = 0; c < NCS; c++) cL[c] = cR[c] = -1;
	float gl = gGlide > 0 ? 1.0f - expf(-dt / (gGlide * 0.35f)) : 1.0f;
	float noiseAmt = 0;

	for (int i = 0; i < NVS; i++) {
		SVoice* v = &gV[i];
		if (!v->active) continue;
		if (!v->started) { v->startDelay -= dt; if (v->startDelay <= 0) startVoice(i); if (!v->started) continue; }
		if (v->stage != 4) v->held += dt;
		if (v->gate >= 0 && v->stage != 4) { v->gate -= dt; if (v->gate <= 0) v->stage = v->stage2 = 4; }
		envStep(&v->env, &v->stage, &gEnv1, dt, &v->hold1, v->relMul);
		envStep(&v->env2, &v->stage2, &gEnv2, dt, &v->hold2, 1);
		if (v->stage == 4 && v->env <= 0) { freeVoice(v); continue; }
		if (v->stage == 3 && gEnv1.s <= 0.001f) { freeVoice(v); continue; }
		v->cur += (v->freq - v->cur) * (v->slide == 2 ? 1.0f - expf(-dt / (gPadMorph * 0.35f + 0.001f)) : v->slide ? 1.0f - expf(-dt / 0.03f) : gl);   // 2: a pad MORPH

		float pitch = (gMpe && v->mch > 0 ? 0 : bend * gBendRange) + v->mbend + modSum(DST_PITCH, v) * 12.0f;   // MPE: each note bends on its own channel
		float pm = powf(2.0f, pitch / 12.0f);
		float cutoff = baseCutoff() * powf(2.0f, modSum(DST_CUTOFF, v) * 5.0f);
		cutoff *= powf(v->cur / 261.6f, gKeytrack) * v->cutMul;
		float res = gRes + modSum(DST_RES, v);
		if (res < 0) res = 0;
		if (res > 1) res = 1;
		// level compensation: resonance boosts the peak (LP/HP), band pass thins the level
		float fcomp = gFiltType <= 1 ? 1.0f / (1.0f + res * 3.5f) : (gFiltType == 2 ? 2.2f : 1.0f);
		float panMod = modSum(DST_PAN, v) * 0.5f;
		float aLev = gOsc[0].level * (1 + modSum(DST_ALEV, v)); if (aLev < 0) aLev = 0;
		float bLev = gOsc[1].level * (1 + modSum(DST_BLEV, v)); if (bLev < 0) bLev = 0;
		float vg = v->env * v->env * v->gain * gMaster * (0.35f + 0.65f * v->vel) * 1.5f;
		{ int ar = 0; for (int m = 0; m < NMOD; m++) if (gMod[m].src != SRC_OFF && gMod[m].dst == DST_AMP) ar = 1;
		  if (ar) { float am = modSum(DST_AMP, v); if (am < 0) am = 0; if (am > 1.5f) am = 1.5f; vg *= am; } }
		if (!v->isEcho) noiseAmt += v->env * v->env * (0.35f + 0.65f * v->vel);

		for (int k = 0; k < v->nch; k++) {
			int ch = v->ch[k], role = v->role[k]; if (ch < 0) continue;
			float det = 0, pan = 0.5f, ug = 1, lv = 1, f = roleFreq(v, role);
			if (role <= R_B2) {
				int o = role >= R_B0, u = role - (o ? R_B0 : R_A0);
				uniParams(&gOsc[o], u, &det, &pan, &ug);
				lv = o ? bLev : aLev;
				ug /= sqrtf((float)gOsc[o].uni);
				if (gChorus > 0 && gOsc[o].uni > 1 && u > 0) det += gChorus * 12.0f * sinf(gChorusPh + u * 2.1f + o);
			} else if (role == R_SUB) lv = gSubLevel;
			else { lv = gOsc[0].on ? aLev : bLev; det = v->tapDet; }
			pan += v->pan + panMod;
			if (pan < 0) pan = 0;
			if (pan > 1) pan = 1;
			float g = vg * ug * lv;
			if (v->bk[k] == BK_DSP) g *= fcomp;
			float L = g * sqrtf(1 - pan) * 1.41f, R = g * sqrtf(pan) * 1.41f;
			float rate = f * pm * powf(2.0f, det / 1200.0f) * LEVN[v->lev[k]];
			if (v->bk[k] == BK_DSP) {
				dL[ch] = L; dR[ch] = R;
				ndspChnSetRate(ch, rate);
				filterSet(ch, role == R_SUB ? cutoff * 0.6f + 300 : cutoff, res);
			} else {
				cL[ch] = L; cR[ch] = R;
				u32 timer = CSND_TIMER(rate); if (timer < 0x42) timer = 0x42; if (timer > 0xFFFF) timer = 0xFFFF;
				CSND_SetTimer(ch, timer);
			}
		}
		if (v->chipCh >= 8 && v->chipCh <= 13) {   // CHIP layer: hardware square, pitch and level from the same voice
			float pan = v->pan + panMod + 0.5f; if (pan < 0) pan = 0; if (pan > 1) pan = 1;
			float g = vg * gChipLevel * 0.6f;
			cL[v->chipCh] = g * sqrtf(1 - pan) * 1.41f; cR[v->chipCh] = g * sqrtf(pan) * 1.41f;
			u32 timer = CSND_TIMER(chipRate(v->cur * pm)); if (timer < 0x42) timer = 0x42; if (timer > 0xFFFF) timer = 0xFFFF;
			CSND_SetTimer(v->chipCh, timer);
		}
	}
	if (gNoiseOn) {
		float nc = gFiltType <= 1 ? 1.0f / (1.0f + gRes * 3.5f) : (gFiltType == 2 ? 2.5f : 1.0f);
		float g = gNoiseLevel * 0.8f * gMaster * nc * (noiseAmt > 1.2f ? 1.2f : noiseAmt);
		dL[NOISE_CH] = dR[NOISE_CH] = g;
		filterSet(NOISE_CH, baseCutoff() * powf(2.0f, modSum(DST_CUTOFF, NULL) * 5.0f), gRes);
	}
	// limiter across every channel (DSP + CSND): estimate the summed peak, scale everything together
	float sl = 0, sr = 0, al = 0, ar = 0;
	for (int c = 0; c < NCH; c++) if (dL[c] >= 0) { sl += dL[c] * dL[c]; sr += dR[c] * dR[c]; al += dL[c]; ar += dR[c]; }
	for (int c = 0; c < NCS; c++) if (cL[c] >= 0) { sl += cL[c] * cL[c]; sr += cR[c] * cR[c]; al += cL[c]; ar += cR[c]; }
	float estL = sqrtf(sl) * 0.9f + al * 0.5f, estR = sqrtf(sr) * 0.9f + ar * 0.5f;
	float est = estL > estR ? estL : estR;
	float target = est > 0.8f ? 0.8f / est : 1.0f;
	gAutoGain += (target - gAutoGain) * (target < gAutoGain ? 0.6f : 0.05f);
	for (int c = 0; c < NCH; c++) if (dL[c] >= 0) {
		float mix[12] = {0};
		mix[0] = dL[c] * gAutoGain; mix[1] = dR[c] * gAutoGain;
		if (gQMixDrive > 0.001f && masterActive()) { float k = 1.0f + gQMixDrive * 15.0f; mix[0] *= k; mix[1] *= k; }   // MIX OVERDRIVE
		if (masterActive()) { mix[4] = mix[0]; mix[5] = mix[1]; mix[0] = mix[1] = 0; }   // whole sound through the master chain
		else {
			float s0 = busSend(0), s1 = busSend(1);   // per-channel sends to the DSP effect buses
			mix[4] = mix[0] * s0; mix[5] = mix[1] * s0;
			mix[8] = mix[0] * s1; mix[9] = mix[1] * s1;
		}
		int ow = gDspOwner[c], echo = ow >= 0 && ow < NVS && gV[ow].isEcho;   // echo taps: short blips, no ramp (it cost them 3 dB)
		if (gWbPend[c] && (mix[0] + mix[1] + mix[4] + mix[5] + mix[8] + mix[9]) > 1e-4f) {   // the note's first level: start it now
			gWbPend[c] = 0;
			if (gFadeIn[c]) {   // SOFT START: the level straight on, the one-shot fades the wave in, the loop follows
				float ig = gAutoGain > 1e-4f ? 1.0f / gAutoGain : 0; for (int k = 0; k < 12; k++) gMixCur[c][k] = mix[k] * ig;
				ndspChnSetMix(c, mix); ndspChnWaveBufAdd(c, &gFadeWB[c]); ndspChnWaveBufAdd(c, &gWB[c]); gFadeIn[c] = 0;
			} else { memset(gMixCur[c], 0, sizeof(gMixCur[c])); ndspChnSetMix(c, gMixCur[c]); ndspChnWaveBufAdd(c, &gWB[c]); }
			gMixLive[c] = !echo;
		}
		{ float ig = gAutoGain > 1e-4f ? 1.0f / gAutoGain : 0; for (int k = 0; k < 12; k++) gMixTgt[c][k] = mix[k] * ig; }   // the ramp moves the envelope; the limiter gain applies straight
		if (!gMixLive[c] || !gAudioClock) { memcpy(gMixCur[c], gMixTgt[c], sizeof(mix)); ndspChnSetMix(c, mix); }   // channels outside the voices (noise) and no audio clock: as before
	}
	int any = 0;
	for (int c = 0; c < NCS; c++) if (cL[c] >= 0) {
		float l = cL[c] * gAutoGain, r = cR[c] * gAutoGain;
		if (l > 1) l = 1;
		if (r > 1) r = 1;
		u32 vol = (u32)(l * 0x8000) | ((u32)(r * 0x8000) << 16);
		CSND_SetVol(c, gThruOn ? 0 : csMono(vol), csSend(l, r)); any = 1;   // CHIP > FX: heard only through the DSP
	}
	if (gCsndOK && any) csndExecCmds(false);
}

// ---------------------------------------------------------------- DSP effect buses (aux 0 = reverb, aux 1 = ping-pong delay)
// Each DSP channel sends to the two aux buses at its own level (mix[4..5], mix[8..9]); the DSP
// sums them and hands them to the audio thread every 160 samples, where the effects run, and
// mixes the result back in. If the DSP never delivers send audio, Syrup falls back to the
// per-note tap effects.
static int gBusOK = 0, gBusFail = 0;
static volatile int gBusSeen = 0;
static float gBusWait = 0;
static float gEchoFb = 0.45f;
static int busActive(void) { return gBusOK && !gBusFail; }

#define NCOMB 8
#define NAP 4
typedef struct { float* b; int n, ln, i; float st; } Comb;   // ln: active length (room / plate / spring use shorter loops)
typedef struct { float* b; int n, i; } APass;
static Comb gCbL[NCOMB], gCbR[NCOMB];
static APass gApL[NAP], gApR[NAP];
static float* gDlL; static float* gDlR;
#define DLMAX 26000   // ~0.8 s
static int gDlW = 0;
static float gDlLp[2];

static int fxAllocLine(float** b, int n) { *b = (float*)calloc(n, sizeof(float)); return *b != NULL; }
static int busFxInit(void) {
	static const int CL[NCOMB] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, AL[NAP] = {556, 441, 341, 225};
	const float sc = DSPRATE / 44100.0f;
	for (int i = 0; i < NCOMB; i++) {
		gCbL[i].n = gCbL[i].ln = (int)(CL[i] * sc); gCbR[i].n = gCbR[i].ln = (int)((CL[i] + 23) * sc);
		if (!fxAllocLine(&gCbL[i].b, gCbL[i].n) || !fxAllocLine(&gCbR[i].b, gCbR[i].n)) return 0;
	}
	for (int i = 0; i < NAP; i++) {
		gApL[i].n = (int)(AL[i] * sc); gApR[i].n = (int)((AL[i] + 23) * sc);
		if (!fxAllocLine(&gApL[i].b, gApL[i].n) || !fxAllocLine(&gApR[i].b, gApR[i].n)) return 0;
	}
	return fxAllocLine(&gDlL, DLMAX) && fxAllocLine(&gDlR, DLMAX);
}
static inline float comb(Comb* c, float in, float fb, float damp) {
	float y = c->b[c->i];
	c->st = y * (1 - damp) + c->st * damp;
	c->b[c->i] = in + c->st * fb;
	if (++c->i >= c->ln) c->i = 0;
	return y;
}
static inline float apass(APass* a, float in) {
	float b = a->b[a->i], y = b - in;
	a->b[a->i] = in + b * 0.5f;
	if (++a->i >= a->n) a->i = 0;
	return y;
}
// ---- reverb types (all from the same comb/allpass bank; the type sets loop lengths, feedback, tone and dynamics)
static int gRevType = 0;
#define NREVT 6
static const char* REVT_NAMES[NREVT] = {"HALL", "ROOM", "PLATE", "SPRING", "GATED", "SWELL"};
static const float REVT_LEN[NREVT] = {1.0f, 0.42f, 0.72f, 0.38f, 0.9f, 1.0f};
#define NSPR 6
static float gSprBuf[2][NSPR][104]; static int gSprI[2][NSPR];
static const int SPR_N[NSPR] = {37, 53, 61, 71, 83, 94};
static float gRevEnv = 0, gRevGate = 0, gRevHold = 0, gRevHp[2], gRevLp[2];
static void revTypeApply(void) {   // audio side picks this up; clear the loops so a change never bursts
	static int last = -1;
	if (last == gRevType) return;
	last = gRevType;
	for (int i = 0; i < NCOMB; i++) {
		Comb* c[2] = {&gCbL[i], &gCbR[i]};
		for (int k = 0; k < 2; k++) { if (!c[k]->b) continue; c[k]->ln = (int)(c[k]->n * REVT_LEN[gRevType]); if (c[k]->ln < 8) c[k]->ln = 8; c[k]->i = 0; c[k]->st = 0; memset(c[k]->b, 0, c[k]->n * sizeof(float)); }
	}
	memset(gSprBuf, 0, sizeof(gSprBuf));
}
// per-type feedback / damping from REV SIZE and REV DAMP
static void revParams(float* fb, float* damp) {
	float sz = gRevSize, dm = gRevDamp;
	switch (gRevType) {
	case 1: *fb = 0.62f + 0.24f * sz; *damp = 0.3f + 0.55f * dm; break;    // ROOM: short, woody
	case 2: *fb = 0.80f + 0.18f * sz; *damp = 0.03f + 0.25f * dm; break;   // PLATE: dense, bright
	case 3: *fb = 0.86f + 0.1f * sz; *damp = 0.2f + 0.4f * dm; break;      // SPRING: rings on
	case 4: *fb = 0.86f + 0.1f * sz; *damp = 0.1f + 0.4f * dm; break;      // GATED: big tail, cut short
	case 5: *fb = 0.84f + 0.14f * sz; *damp = 0.15f + 0.6f * dm; break;    // SWELL
	default: *fb = 0.70f + 0.28f * sz; *damp = 0.15f + 0.6f * dm; break;   // HALL
	}
}
// per-sample: input shaping before the combs, output shaping after the allpasses
static inline float revIn(float in) {
	float a = fabsf(in) * 40;
	gRevEnv += (a - gRevEnv) * (a > gRevEnv ? 0.02f : 0.0004f);
	return in;
}
static inline void revOut(float* l, float* r) {
	if (gRevType == 3) {   // SPRING: dispersive allpass chain (the "boing"), band-limited like a real tank
		float* v[2] = {l, r};
		for (int k = 0; k < 2; k++) {
			float x = *v[k];
			for (int j = 0; j < NSPR; j++) {
				float* b = gSprBuf[k][j]; int i = gSprI[k][j], nn = SPR_N[j] + k * 3;
				float d = b[i], y = d - 0.62f * x; b[i] = x + 0.62f * y; x = y;
				if (++i >= nn) i = 0;
				gSprI[k][j] = i;
			}
			gRevLp[k] += (x - gRevLp[k]) * 0.35f; gRevHp[k] += (gRevLp[k] - gRevHp[k]) * 0.03f;
			*v[k] = (gRevLp[k] - gRevHp[k]) * 1.3f;
		}
	} else if (gRevType == 4) {   // GATED: open while the input is loud (plus a short hold), then slam shut
		if (gRevEnv > 0.25f) { gRevHold = 0.22f * 32728; }
		float tgt = gRevHold > 0 ? 1 : 0; if (gRevHold > 0) gRevHold -= 1;
		gRevGate += (tgt - gRevGate) * (tgt > gRevGate ? 0.05f : 0.004f);
		*l *= gRevGate * 0.85f; *r *= gRevGate * 0.85f;
	} else if (gRevType == 5) {   // SWELL: the tail is ducked while you play and blooms up after, like a reversed reverb
		float d = 1 - (gRevEnv > 1 ? 1 : gRevEnv) * 0.95f;
		gRevGate += (d - gRevGate) * 0.0006f;
		*l *= gRevGate * 1.1f; *r *= gRevGate * 1.1f;
	} else if (gRevType == 1) { *l *= 1.25f; *r *= 1.25f; }
}
static void auxReverb(void* data, int n, void* samples[4]) {
	(void)data;
	s32* L = (s32*)samples[0]; s32* R = (s32*)samples[1];
	float fb, damp; revParams(&fb, &damp);
	int seen = 0;
	for (int s = 0; s < n; s++) {
		if (L[s] | R[s]) seen = 1;
		float in = (L[s] + R[s]) * (0.5f / 32768.0f) * 0.03f, ol = 0, orr = 0;
		for (int i = 0; i < NCOMB; i++) { ol += comb(&gCbL[i], in, fb, damp); orr += comb(&gCbR[i], in, fb, damp); }
		for (int i = 0; i < NAP; i++) { ol = apass(&gApL[i], ol); orr = apass(&gApR[i], orr); }
		float yl = ol * 3.0f * 32768.0f, yr = orr * 3.0f * 32768.0f;
		if (yl > 2e9f) yl = 2e9f;
		if (yl < -2e9f) yl = -2e9f;
		if (yr > 2e9f) yr = 2e9f;
		if (yr < -2e9f) yr = -2e9f;
		L[s] = (s32)yl; R[s] = (s32)yr;
	}
	memset(samples[2], 0, n * 4); memset(samples[3], 0, n * 4);
	if (seen) gBusSeen = 1;
}
static float delayTime(void) { return ECHO_T[gEchoT]; }
static int gMasterFX;
static void auxDelay(void* data, int n, void* samples[4]) {
	(void)data;
	if (gMasterFX) { for (int c = 0; c < 4; c++) memset(samples[c], 0, n * 4); return; }   // delay lives in the master chain
	s32* L = (s32*)samples[0]; s32* R = (s32*)samples[1];
	int d = (int)(delayTime() * DSPRATE); if (d >= DLMAX) d = DLMAX - 1;
	float fb = gEchoFb;
	int seen = 0;
	for (int s = 0; s < n; s++) {
		if (L[s] | R[s]) seen = 1;
		float in = (L[s] + R[s]) * (0.5f / 32768.0f);
		int r = gDlW - d; if (r < 0) r += DLMAX;
		float yl = gDlL[r], yr = gDlR[r];
		gDlLp[0] += (yr - gDlLp[0]) * 0.45f; gDlLp[1] += (yl - gDlLp[1]) * 0.45f;   // darker repeats
		gDlL[gDlW] = in + gDlLp[0] * fb;   // ping-pong: L <- R, R <- L
		gDlR[gDlW] = gDlLp[1] * fb;
		if (++gDlW >= DLMAX) gDlW = 0;
		L[s] = (s32)(yl * 32768.0f * 0.8f); R[s] = (s32)(yr * 32768.0f * 0.8f);
	}
	memset(samples[2], 0, n * 4); memset(samples[3], 0, n * 4);
	if (seen) gBusSeen = 1;
}
static void auxMaster(void* data, int n, void* samples[4]);
static int gSurActive = 0;
static int gMasterFX;
static int masterInit(void);
// FX ON CORE 2: the master chain runs on the second core, one DSP frame (160 samples, ~5 ms) behind. The DSP's audio
// thread hands each frame over and takes back the one finished last time, so the main core never waits on the effects.
// If core 2 ever misses a frame, the chain falls back to running here (and says so).
static int gFxCore2 = 1, gFxC2Fail = 0;
static Thread gFxThread = NULL;
static LightEvent gFxGo, gFxDone;
static volatile int gFxQuit = 0, gFxN = 0;
static int gFxBusy = 0, gFxMiss = 0, gFxRetry = 0, gFxRetries = 0;   // audio thread only
// Where the chain runs off the DSP thread: on a New 3DS / New 2DS its third core (gFxThread, core id 2); otherwise inside
// the wavetable worker on core 1 (gFxOnWorker). libctru allows only ONE thread on core 1, so a separate effects thread
// there never started on an old 2DS. The worker serves a waiting effects frame before, between and after rebuild steps.
static volatile int gFxOnWorker = 0, gFxPending = 0, gInWorker = 0;
static LightEvent gJobEv;
static void fxRunFrame(void);
static s32 gFxIn[2][160], gFxOut[2][160], gFxZ[2][160];
static void fxRunFrame(void) {
	u64 t0 = svcGetSystemTick();
	void* smp[4] = {gFxIn[0], gFxIn[1], gFxZ[0], gFxZ[1]};
	auxMaster(NULL, gFxN, smp);
	memcpy(gFxOut[0], gFxIn[0], gFxN * 4); memcpy(gFxOut[1], gFxIn[1], gFxN * 4);
	__atomic_fetch_add(&gCore1Busy, svcGetSystemTick() - t0, __ATOMIC_RELAXED);
	LightEvent_Signal(&gFxDone);
}
static void fxService(void) {   // worker: run a waiting effects frame now
	if (__atomic_exchange_n(&gFxPending, 0, __ATOMIC_ACQ_REL)) fxRunFrame();
}
static void fxMain(void* a) {
	(void)a;
	for (;;) {
		LightEvent_Wait(&gFxGo);
		if (gFxQuit) break;
		fxRunFrame();
	}
}
static void fxCore2Start(void) {
	if (gFxThread || gFxOnWorker) return;
	LightEvent_Init(&gFxGo, RESET_ONESHOT); LightEvent_Init(&gFxDone, RESET_ONESHOT);
	gFxBusy = 0; gFxMiss = 0; gFxPending = 0;
	memset(gFxOut, 0, sizeof(gFxOut));
	bool n3 = false; APT_CheckNew3DS(&n3);
	if (n3) gFxThread = threadCreate(fxMain, NULL, 0x4000, 0x18, 2, false);   // New 3DS / New 2DS: a core of its own
	if (!gFxThread) gFxOnWorker = 1;   // otherwise share core 1's one thread with the wavetable worker
}
static void fxCore2Stop(void) {
	if (gFxOnWorker) { gFxOnWorker = 0; gFxPending = 0; gFxBusy = 0; return; }
	if (!gFxThread) return;
	gFxQuit = 1; LightEvent_Signal(&gFxGo); threadJoin(gFxThread, 1000000000ULL); threadFree(gFxThread); gFxThread = NULL; gFxQuit = 0; gFxBusy = 0;
}
// One writer at a time: while core 2 still has a frame (gFxBusy), the chain's state is its, and this thread never runs
// auxMaster. A miss outputs silence for that frame; 3 misses in a row fall back to core 1 once core 2 has finished,
// and core 2 is tried again every ~10 s (3 times). With only 30% of core 2 (some systems), it stays on core 1.
#define FX_WAIT_NS 4500000LL   // frame is 160 / 32728 Hz = 4.89 ms
static int fxCollect(s64 ns) {   // 1 = core 2's last frame is done (or none was out)
	if (!gFxBusy) return 1;
	if (LightEvent_WaitTimeout(&gFxDone, ns) != 0) return 0;
	gFxBusy = 0; return 1;
}
static volatile float gAudioRms = 0, gAudioPeak = 0;   // the effects bus level, for the visuals
static void aux0(void* d, int n, void* s[4]) {
	{ const s32* L = (const s32*)s[0]; const s32* R = (const s32*)s[1]; float sum = 0, pk = 0;
		for (int i = 0; i < n; i += 4) { float l = L[i] / 32768.0f, r = R[i] / 32768.0f; sum += l * l + r * r; float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r); if (a > pk) pk = a; }
		gAudioRms = sqrtf(sum / (n / 2 > 0 ? n / 2 : 1)); gAudioPeak = pk; }
	if (!gMasterFX) { if (fxCollect(0)) auxReverb(d, n, s); else for (int k = 0; k < 4; k++) memset(s[k], 0, n * 4); return; }
	if (!gFxCore2) { gFxC2Fail = 0; gFxMiss = 0; gFxRetries = 0; }   // switching it off and on again retries core 2
	if (gFxC2Fail && gFxRetries < 3 && ++gFxRetry > 2000) { gFxRetry = 0; gFxRetries++; gFxC2Fail = 0; gFxMiss = 0; }
	int local = !gFxCore2 || !(gFxThread || gFxOnWorker) || gFxC2Fail || n > 160 || (gCore2Lim && gCore2Lim < 50);
	if (local) {
		if (!fxCollect(0)) { for (int k = 0; k < 4; k++) memset(s[k], 0, n * 4); return; }
		auxMaster(d, n, s); return;
	}
	s32* L = (s32*)s[0]; s32* R = (s32*)s[1];
	if (!fxCollect(FX_WAIT_NS)) {   // core 2 is late: keep its frame, play silence here
		if (++gFxMiss >= 3) { gFxC2Fail = 1; gFxRetry = 0; }
		memset(L, 0, n * 4); memset(R, 0, n * 4); memset(s[2], 0, n * 4); memset(s[3], 0, n * 4);
		return;
	}
	gFxMiss = 0;
	int prevN = gFxN;
	memcpy(gFxIn[0], L, n * 4); memcpy(gFxIn[1], R, n * 4);
	if (prevN == n) { memcpy(L, gFxOut[0], n * 4); memcpy(R, gFxOut[1], n * 4); }
	else { memset(L, 0, n * 4); memset(R, 0, n * 4); }
	memset(s[2], 0, n * 4); memset(s[3], 0, n * 4);
	gFxN = n; gFxBusy = 1;
	if (gFxOnWorker) { __atomic_store_n(&gFxPending, 1, __ATOMIC_RELEASE); LightEvent_Signal(&gJobEv); } else LightEvent_Signal(&gFxGo);
}
static void busInit(void) {
	if (!busFxInit()) return;
	if (!masterInit()) gMasterFX = 0;
	ndspAuxSetCallback(0, aux0, NULL); ndspAuxSetCallback(1, auxDelay, NULL);
	for (int i = 0; i < 2; i++) { ndspAuxSetFrontBypass(i, false); ndspAuxSetVolume(i, 1.0f); ndspAuxSetEnable(i, true); }
	gBusOK = 1;
}
// sends for the current FX choice: REVERB -> bus 0, ECHO -> bus 1 (SPACE stays on taps)
static float busSend(int bus) {
	if (gDry || !busActive() || (gMasterFX && bus == 1)) return 0;
	if (bus == 0 && gFx == 3) return fxMixEff() * 1.4f;
	if (bus == 1 && gFx == 1) return fxMixEff() * 1.2f;
	return 0;
}

// ================================================================ master effects chain (on DSP aux bus 0)
// With MASTER FX on, every DSP channel goes to aux bus 0 only; the audio thread runs this chain on the
// whole mix every 160 samples and the DSP mixes the result back: lofi -> mod FX -> EQ -> tape delay ->
// reverb (shimmer / freeze) -> OTT -> sidechain pump -> limiter. CSND sounds (sub, drums, looper) stay dry.
static int gMasterFX = 1, gModFx = 0, gDlySync = 0, gShimmer = 0, gFreeze = 0, gPumpRate = 1, gLofiBits = 0;
static float gModRate = 0.4f, gModDepth = 0.5f, gModMix = 0.5f, gFxColor = 0.5f;
static float gEqLow = 0, gEqMid = 0, gEqHigh = 0;
static int gDlyType = 0; static float gRevK = 0, gDlHp[2];
#define NDLYT 6
static const char* DLYT_NAMES[NDLYT] = {"TAPE", "DIGITAL", "PING-PONG", "DUB", "LO-FI", "REVERSE"};
static void dlyTypeChanged(void) {   // clear the line so switching type never replays old repeats through the new path
	if (gDlL) memset(gDlL, 0, DLMAX * sizeof(float));
	if (gDlR) memset(gDlR, 0, DLMAX * sizeof(float));
	gDlLp[0] = gDlLp[1] = gDlHp[0] = gDlHp[1] = 0; gRevK = 0;
}
static float gDlyMix = 0, gDlyWow = 0.25f, gDlySat = 0.3f, gRevMix = 0, gOtt = 0, gPump = 0, gLofiRate = 0;
static volatile float gFxModVal = 0;   // FX COLOR modulation from the mod matrix (set by the main loop)
static float gBeatPh = 0;              // quarter notes, advanced by the audio clock (sidechain pump)
static volatile u32 gFxTicks = 0;      // CPU time of the chain per frame (for the meter)
static const char* MODFX_NAMES[8] = {"OFF", "CHORUS", "FLANGER", "PHASER", "COMB", "FORMANT", "HYPER", "DIMENSION"};
static const char* DSYNC_NAMES[7] = {"FREE", "1/16", "1/8", "1/8 DOT", "1/4", "1/4 DOT", "1/2"};
static const float DSYNC_BEATS[7] = {0, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
static const char* PUMP_NAMES[4] = {"1/2", "1/4", "1/8", "KICK"};   // KICK: ducks on every kick the drums actually play
static volatile u32 gKickT = 1u << 30;   // samples since the last kick hit
static const char* BITS_NAMES[6] = {"OFF", "12 BIT", "10 BIT", "8 BIT", "6 BIT", "4 BIT"};
static int masterActive(void) { return gMasterFX && busActive(); }

typedef struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; } BQ;
static inline float bq(BQ* f, int c, float x) { float y = f->b0 * x + f->z1[c]; f->z1[c] = f->b1 * x - f->a1 * y + f->z2[c]; f->z2[c] = f->b2 * x - f->a2 * y; return y; }
static void bqSet(BQ* f, int type, float hz, float q, float db) {   // RBJ: 0 low shelf, 1 peak, 2 high shelf, 3 band pass
	float A = powf(10, db / 40), w = TWO_PI * hz / DSPRATE, cw = cosf(w), sw = sinf(w), al = sw / (2 * q), b0, b1, b2, a0, a1, a2;
	if (type == 1) { b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; }
	else if (type == 3) { b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; }
	else {
		float sa = 2 * sqrtf(A) * al, s = type == 0 ? 1 : -1;   // RBJ shelf sign: +1 low, -1 high
		b0 = A * ((A + 1) - s * (A - 1) * cw + sa); b1 = 2 * s * A * ((A - 1) - s * (A + 1) * cw); b2 = A * ((A + 1) - s * (A - 1) * cw - sa);
		a0 = (A + 1) + s * (A - 1) * cw + sa; a1 = -2 * s * ((A - 1) + s * (A + 1) * cw); a2 = (A + 1) + s * (A - 1) * cw - sa;
	}
	f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0; f->a1 = a1 / a0; f->a2 = a2 / a0;
}

// buffers
#define MDL 2048        // mod FX delay line
#define OFDL 2048       // OUT FILTER comb / flanger line
static float* gOfDl[2];
#define PSN 4096        // shimmer pitch shifter
static float* gMdl[2]; static int gMdlW = 0;
static float* gPs; static int gPsW = 0; static float gPsR = 0;
static BQ gEq[3], gForm[3];
static float gModPh = 0, gApZ[2][4], gPhFb[2], gCombFb[2], gLofiHold[2]; static int gLofiCnt = 0;
static float gWowPh = 0, gFlutPh = 0;
static float gBandLp1[2][2], gBandLp2[2][2], gBandEnv[3], gBandGain[3] = {1, 1, 1};
static float gLimEnv = 0;
static int masterInit(void) {
	for (int c = 0; c < 2; c++) if (!(gMdl[c] = (float*)calloc(MDL, sizeof(float)))) return 0;
	for (int c = 0; c < 2; c++) gOfDl[c] = (float*)calloc(OFDL, sizeof(float));
	return (gPs = (float*)calloc(PSN, sizeof(float))) != NULL;
}
static inline float rdFrac(const float* b, int n, float pos) { while (pos < 0) pos += n; int i = (int)pos; float u = pos - i; return b[i % n] * (1 - u) + b[(i + 1) % n] * u; }


// ---- OUT FILTER: one stereo CPU filter on the summed voices (paraphonic, like SHRUB's), after the per-note DSP
// filters. Stacked on the per-note 12 dB it makes a 24-36 dB slope; the ladders have the resonant, driven character
// the DSP's clean biquads can't. Cutoff can be modulated (OUT CUTOFF destination) and follows the newest note's ENV 2.
#define NOFT 14
static const char* OFT_NAMES[NOFT] = {"OFF", "LADDER 24", "LADDER 12", "ACID 18", "DIRTY", "CREAMY 24", "SVF LP", "SVF HP", "SVF BP", "SVF NOTCH", "COMB +", "COMB -", "PHASER", "FLANGER"};
static int gOfType = 0; static float gOfCut = 0.6f, gOfRes = 0.2f, gOfDrive = 0.2f, gOfEnv = 0; static volatile float gOfMod = 0;
static float gOfS[2][4], gOfSv[2][2], gOfAp[2][6], gOfFb[2]; static int gOfW = 0;
static inline float ftanh(float x) { if (x > 3) return 1; if (x < -3) return -1; float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
static void ofReset(void) { memset(gOfS, 0, sizeof(gOfS)); memset(gOfSv, 0, sizeof(gOfSv)); memset(gOfAp, 0, sizeof(gOfAp)); gOfFb[0] = gOfFb[1] = 0; for (int c = 0; c < 2; c++) if (gOfDl[c]) memset(gOfDl[c], 0, OFDL * sizeof(float)); }
static void ofBlock(s32* L, s32* R, int n) {   // audio thread, inside auxMaster
	if (!gOfType) return;
	if (gOfType >= 10 && !gOfDl[0]) return;
	static float sm = -1; float tc = gOfCut + gOfMod;
	if (tc < 0) tc = 0;
	if (tc > 1) tc = 1;
	if (sm < 0) sm = tc;
	sm += (tc - sm) * 0.35f;   // per-block smoothing: no zipper on fast mod
	float hz = 30.0f * powf(533.0f, sm);            // 30 Hz .. 16 kHz
	if (hz > 14000) hz = 14000;
	float g = 1 - expf(-TWO_PI * hz / DSPRATE), res = gOfRes, drv = 1 + gOfDrive * 6;
	float f = 2 * sinf(3.14159f * hz / DSPRATE / 2), q = 1.9f - res * 1.85f;   // SVF
	float d = DSPRATE / hz; if (d > OFDL - 4) d = OFDL - 4; if (d < 2) d = 2;
	float apc = (1 - tanf(3.14159f * hz / DSPRATE)) / (1 + tanf(3.14159f * hz / DSPRATE));
	float comp = 1 + res * 2.2f;   // ladders lose bass as resonance rises: give some back
	float hn = g / (2 - g), hn2 = hn * hn;   // one stage's gain at Nyquist: the feedback's one-sample delay flips its sign there,
	// so past loop gain 1 the ladder rings at 16 kHz instead of the cutoff (high cutoff + high resonance). The caps below keep it under.
	for (int i = 0; i < n; i++) {
		float x[2] = {L[i] / 32768.0f, R[i] / 32768.0f};
		for (int c = 0; c < 2; c++) {
			float in = x[c] * drv, y = in; float* s4 = gOfS[c];
			switch (gOfType) {
			case 1: case 2: case 5: {   // ladder: four one-pole stages with saturated resonance feedback
				float k = gOfType == 5 ? res * 2.6f : res * 4.1f, kmax = 0.9f / (gOfType == 2 ? hn2 : hn2 * hn2); if (k > kmax) k = kmax;
				float u = ftanh(in - k * (gOfType == 2 ? s4[1] : s4[3]) + (gOfType == 5 ? 0 : 0.0f));
				s4[0] += g * (u - s4[0]); s4[1] += g * (ftanh(s4[0]) - s4[1]); s4[2] += g * (s4[1] - s4[2]); s4[3] += g * (s4[2] - s4[3]);
				y = (gOfType == 2 ? s4[1] : s4[3]) * comp;
				if (gOfType == 5) y = (s4[3] * 0.85f + s4[2] * 0.15f) * (1 + res);   // creamy: softer knee, rounder peak
				break; }
			case 3: {   // acid: three stages, asymmetric squelch
				float k = res * 3.3f, kmax = 0.9f / (hn2 * hn); if (k > kmax) k = kmax;
				float u = ftanh(in - k * s4[2]) + 0.02f * s4[2] * s4[2];
				s4[0] += g * (u - s4[0]); s4[1] += g * (s4[0] - s4[1]); s4[2] += g * (ftanh(s4[1] * 1.2f) / 1.2f - s4[2]);
				y = s4[2] * comp; break; }
			case 4: {   // dirty: 2-pole state variable with the resonance path clipping hard
				float* sv = gOfSv[c]; float lp = sv[1] + f * sv[0], hp = in - lp - (q * 0.6f) * ftanh(sv[0] * 2.5f), bp = f * hp + sv[0];
				sv[0] = ftanh(bp); sv[1] = lp; y = lp * 0.85f; break; }
			case 6: case 7: case 8: case 9: {   // clean state variable
				float* sv = gOfSv[c]; float lp = sv[1] + f * sv[0], hp = in - lp - q * sv[0], bp = f * hp + sv[0];
				sv[0] = bp; sv[1] = lp;
				y = (gOfType == 6 ? lp : gOfType == 7 ? hp : gOfType == 8 ? bp : lp + hp) * (0.85f - 0.3f * res); break; }
			case 10: case 11: {   // comb at the cutoff's period: + = metallic peaks, - = hollow
				float dl = rdFrac(gOfDl[c], OFDL, gOfW - d), sg = gOfType == 10 ? 1 : -1;
				gOfDl[c][gOfW] = ftanh(in + sg * dl * (0.3f + res * 0.65f)); y = (in + sg * dl) * 0.6f; break; }
			case 12: {   // phaser: six allpasses at the cutoff, resonance = feedback
				float v = in + gOfFb[c] * res * 0.8f;
				for (int k2 = 0; k2 < 6; k2++) { float o = -apc * v + gOfAp[c][k2]; gOfAp[c][k2] = v + apc * o; v = o; }
				gOfFb[c] = v; y = (in + v) * 0.6f; break; }
			default: {   // flanger: very short comb with negative feedback
				float dd = d * 0.25f; if (dd < 2) dd = 2;
				float dl = rdFrac(gOfDl[c], OFDL, gOfW - dd); gOfDl[c][gOfW] = in - dl * res * 0.85f; y = (in + dl) * 0.6f; break; }
			}
			y /= (drv > 1 ? 0.55f + 0.45f * drv : 1);   // keep loudness roughly steady as DRIVE goes up
			x[c] = y;
		}
		if (++gOfW >= OFDL) gOfW = 0;
		L[i] = (s32)(x[0] * 32768.0f); R[i] = (s32)(x[1] * 32768.0f);
	}
}
// FX ORDER: the master chain's blocks, in the order they run. 0 LOFI, 1 MOD FX, 2 EQ, 3 SPACE (delay + reverb, fed in
// parallel), 4 OTT. Saved with the patch; SIDECHAIN PUMP and the limiter always come after these.
#define NFXB 5
static int gFxOrder[NFXB] = {0, 1, 2, 3, 4};
static const char* FXB_NAMES[NFXB] = {"LOFI", "MOD FX", "EQ", "SPACE", "OTT"};
static void fxOrderSet(const u8* o) {   // only a full permutation is taken; anything else = default order
	int seen = 0, t[NFXB];
	for (int i = 0; i < NFXB; i++) { if (o[i] >= NFXB || (seen >> o[i] & 1)) { for (int k = 0; k < NFXB; k++) t[k] = k; goto ok; } seen |= 1 << o[i]; t[i] = o[i]; }
ok:	for (int i = 0; i < NFXB; i++) __atomic_store_n(&gFxOrder[i], t[i], __ATOMIC_RELAXED);
}
static int gSpkEq = 2;   // HARDWARE > SPEAKER EQ: OFF / ON / AUTO (on whenever the headphones are out); saved in settings.bin
static const char* SPK_NAMES[3] = {"OFF", "ON", "AUTO"};
static int gHeadphones;
static void auxMaster(void* data, int n, void* samples[4]) {
	int fxo[NFXB]; for (int i = 0; i < NFXB; i++) fxo[i] = __atomic_load_n(&gFxOrder[i], __ATOMIC_RELAXED);
	(void)data;
	u64 t0 = svcGetSystemTick();
	s32* L = (s32*)samples[0]; s32* R = (s32*)samples[1];
	int seen = 0;
	// per-frame parameter work
	static float lastEq[3] = {99, 99, 99};
	if (lastEq[0] != gEqLow || lastEq[1] != gEqMid || lastEq[2] != gEqHigh) {
		bqSet(&gEq[0], 0, 150, 0.7f, gEqLow); bqSet(&gEq[1], 1, 1200, 0.8f, gEqMid); bqSet(&gEq[2], 2, 6000, 0.7f, gEqHigh);
		lastEq[0] = gEqLow; lastEq[1] = gEqMid; lastEq[2] = gEqHigh;
	}
	if (gQMixDrive > 0.001f) {   // MIX OVERDRIVE: the boosted channels hit the DSP mix's 16-bit ceiling, then come back down
		float k = 1.0f / (1.0f + gQMixDrive * 4.0f);
		for (int s = 0; s < n; s++) {
			s32 l = L[s], r = R[s]; if (l > 32767) l = 32767; if (l < -32768) l = -32768; if (r > 32767) r = 32767; if (r < -32768) r = -32768;
			L[s] = (s32)(l * k); R[s] = (s32)(r * k);
		}
	}
	{   // CONTAINMENT: DC block (a latched filter or clipped mix leaves an offset) and a ceiling before the effects
		static float dx[2], dy[2];
		for (int s = 0; s < n; s++) {
			s32* io[2] = {&L[s], &R[s]};
			for (int c = 0; c < 2; c++) {
				float x = (float)*io[c], y = x - dx[c] + 0.9985f * dy[c]; dx[c] = x;
				if (!(y == y) || y > 131072 || y < -131072) y = y > 0 ? 131072 : (y < 0 ? -131072 : 0);   // NaN / runaway
				dy[c] = y; *io[c] = (s32)y;
			}
		}
	}
	ofBlock(L, R, n);   // OUT FILTER first, then the effects
	float color = gFxColor + gFxModVal; if (color < 0) color = 0; if (color > 1) color = 1;
	if (gModFx == 5) {   // formant: morph through A E I O U
		static const float VF[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
		float x = color * 4; int i = (int)x; if (i > 3) i = 3; float u = x - i;
		for (int k = 0; k < 3; k++) bqSet(&gForm[k], 3, VF[i][k] * (1 - u) + VF[i + 1][k] * u, 6 + k * 2, 0);
	}
	float dSec = gDlySync ? DSYNC_BEATS[gDlySync] * 60.0f / gBPM : ECHO_T[gEchoT];
	if (dSec > 0.78f) dSec = 0.78f;
	float dSmp = dSec * DSPRATE, dMix = (gDlyMix + (gFx == 1 ? fxMixEff() : 0)) * 1.2f, rMix = (gRevMix + (gFx == 3 ? fxMixEff() : 0)) * 1.4f;
	float fb, damp; revParams(&fb, &damp); if (gFreeze) { fb = 1.0f; damp = 0; }
	int bits = gLofiBits ? 14 - gLofiBits * 2 : 0, hold = 1 + (int)(gLofiRate * 15);
	float pumpRate = gPumpRate == 0 ? 0.5f : (gPumpRate == 1 ? 1.0f : 2.0f), beatInc = gBPM / 60.0f / DSPRATE;
	float combHz = 60 * powf(2, color * 6), combD = DSPRATE / combHz;
	for (int s = 0; s < n; s++) {
		if (L[s] | R[s]) seen = 1;
		float x[2] = {L[s] / 32768.0f, R[s] / 32768.0f};
		// the movable blocks, in FX ORDER (pump and limiter always come last)
		if (!gDry) for (int fk = 0; fk < NFXB; fk++) switch (fxo[fk]) {
		case 0: {
			// lofi
			if (bits || hold > 1) {
				if (gLofiCnt++ % hold == 0) for (int c = 0; c < 2; c++) gLofiHold[c] = bits ? roundf(x[c] * (1 << (bits - 1))) / (1 << (bits - 1)) : x[c];
				x[0] = gLofiHold[0]; x[1] = gLofiHold[1];
			}
		} break;
		case 1: {
			// modulation FX
			if (gModFx) {
				gModPh += gModRate * (0.2f + color) / DSPRATE; if (gModPh >= 1) gModPh -= 1;
				for (int c = 0; c < 2; c++) gMdl[c][gMdlW] = x[c];
				for (int c = 0; c < 2; c++) {
					float lfo = sinf(TWO_PI * (gModPh + c * 0.25f)), y = x[c];
					if (gModFx == 1) y = rdFrac(gMdl[c], MDL, gMdlW - (0.012f + 0.006f * gModDepth * lfo) * DSPRATE);
					else if (gModFx == 2) { y = rdFrac(gMdl[c], MDL, gMdlW - (0.0015f + 0.0035f * gModDepth * (lfo * 0.5f + 0.5f)) * DSPRATE); gMdl[c][gMdlW] = x[c] + y * 0.6f; }
					else if (gModFx == 3) {   // 4 first-order allpass stages
						float a = 0.2f + 0.7f * gModDepth * (lfo * 0.5f + 0.5f), v = x[c] + gPhFb[c] * 0.5f;
						for (int k = 0; k < 4; k++) { float o = -a * v + gApZ[c][k]; gApZ[c][k] = v + a * o; v = o; }
						gPhFb[c] = v; y = v;
					} else if (gModFx == 4) { y = rdFrac(gMdl[c], MDL, gMdlW - combD); gMdl[c][gMdlW] = x[c] + y * (0.6f + 0.35f * gModDepth); }
					else if (gModFx == 5) { y = (bq(&gForm[0], c, x[c]) + 0.7f * bq(&gForm[1], c, x[c]) + 0.4f * bq(&gForm[2], c, x[c])) * 1.6f; }
					else if (gModFx == 6) {   // HYPER: five drifting taps per side, like extra unison voices on the whole mix
						static const float RT[5] = {0.13f, 0.21f, 0.29f, 0.37f, 0.47f}, BD[5] = {0.007f, 0.011f, 0.016f, 0.021f, 0.027f};
						static float hp[5]; float a = 0;
						for (int k = 0; k < 5; k++) {
							if (c == 0) { hp[k] += RT[k] * (0.5f + gModRate) / DSPRATE; if (hp[k] >= 1) hp[k] -= 1; }
							float m = sinf(TWO_PI * (hp[k] + c * 0.37f + k * 0.19f));
							a += rdFrac(gMdl[c], MDL, gMdlW - (BD[k] + 0.004f * gModDepth * m) * DSPRATE);
						}
						y = a * 0.42f;
					} else {   // DIMENSION: two short, slowly moving delays crossed between the sides - wide, not wobbly
						float m = sinf(TWO_PI * gModPh * 0.5f);
						float d1 = rdFrac(gMdl[c], MDL, gMdlW - (0.0085f + 0.0012f * gModDepth * m) * DSPRATE);
						float d2 = rdFrac(gMdl[c ^ 1], MDL, gMdlW - (0.0118f - 0.0012f * gModDepth * m) * DSPRATE);
						y = (d1 - 0.6f * d2) * 0.9f + x[c] * 0.3f;
					}
					x[c] = x[c] * (1 - gModMix) + y * gModMix;
				}
				if (++gMdlW >= MDL) gMdlW = 0;
			}
		} break;
		case 2: {
			// EQ
			if (gEqLow != 0 || gEqMid != 0 || gEqHigh != 0) for (int c = 0; c < 2; c++) x[c] = bq(&gEq[2], c, bq(&gEq[1], c, bq(&gEq[0], c, x[c])));
		} break;
		case 3: {
			// tape delay: wow + flutter on the read head, saturation and darkening in the feedback
			float mono = (x[0] + x[1]) * 0.5f, wet[2] = {0, 0};
			static int dlyTail = 0, revTail = 0;
			if (dMix > 0.001f) dlyTail = 32728 * 6; else if (dlyTail > 0) dlyTail--;
			if (rMix > 0.001f || gFreeze) revTail = 32728 * 8; else if (revTail > 0) revTail--;
			if (dlyTail > 0) {
				gWowPh += 0.6f / DSPRATE; gFlutPh += 7.0f / DSPRATE; if (gWowPh > 1) gWowPh -= 1; if (gFlutPh > 1) gFlutPh -= 1;
				float mod = gDlyWow * (0.004f * sinf(TWO_PI * gWowPh) + 0.0006f * sinf(TWO_PI * gFlutPh)) * DSPRATE;
				int dt = gDlyType;
				if (dt != 0 && dt != 3) mod = 0;   // only TAPE and DUB wobble
				float yl, yr;
				if (dt == 5) {   // REVERSE: each slice of DELAY TIME comes back played backwards, windowed so it doesn't click
					float D = dSmp < 64 ? 64 : dSmp; if (D * 2 > DLMAX - 4) D = (DLMAX - 4) / 2;
					gRevK += 1; if (gRevK >= D) gRevK -= D;
					float w = sinf(3.14159f * gRevK / D), w2 = sinf(3.14159f * fmodf(gRevK + D / 2, D) / D);
					float r1 = gDlW - 2 * gRevK, r2 = gDlW - 2 * fmodf(gRevK + D / 2, D);
					yl = rdFrac(gDlL, DLMAX, r1) * w + rdFrac(gDlL, DLMAX, r2) * w2;
					yr = rdFrac(gDlR, DLMAX, r1 - 37) * w + rdFrac(gDlR, DLMAX, r2 - 37) * w2;
				} else { yl = rdFrac(gDlL, DLMAX, gDlW - dSmp - mod); yr = rdFrac(gDlR, DLMAX, gDlW - dSmp - (dt == 1 ? dSmp * 0.012f : 0) - mod); }
				float sat = 1 + gDlySat * 3, inL, inR;
				switch (dt) {
				case 1:   // DIGITAL: clean stereo repeats, no crossing, full bandwidth
					inL = mono * dMix + yl * gEchoFb; inR = mono * dMix + yr * gEchoFb; break;
				case 2:   // PING-PONG: clean, bounces left / right
					inL = mono * dMix + yr * gEchoFb; inR = yl * gEchoFb; break;
				case 3: {   // DUB: band-passed, saturated, long and swampy (feedback can push past 100% gently)
					gDlLp[0] += (yr - gDlLp[0]) * 0.22f; gDlLp[1] += (yl - gDlLp[1]) * 0.22f;
					gDlHp[0] += (gDlLp[0] - gDlHp[0]) * 0.02f; gDlHp[1] += (gDlLp[1] - gDlHp[1]) * 0.02f;
					float fb = gEchoFb * 1.15f, s2 = 1 + gDlySat * 4 + 1;
					inL = tanhf((mono * dMix + (gDlLp[0] - gDlHp[0]) * fb) * s2) / s2 * 1.3f; inR = tanhf((gDlLp[1] - gDlHp[1]) * fb * s2) / s2 * 1.3f; break; }
				case 4: {   // LO-FI: each repeat goes through a sample-rate and bit crusher, getting grittier
					static float hold[2]; static int ctr = 0;
					if (++ctr >= 4) { ctr = 0; hold[0] = yr; hold[1] = yl; }
					float q = 1.0f / 48;
					float cl = floorf(hold[0] / q + 0.5f) * q, cr = floorf(hold[1] / q + 0.5f) * q;
					gDlLp[0] += (cl - gDlLp[0]) * 0.3f; gDlLp[1] += (cr - gDlLp[1]) * 0.3f;
					inL = mono * dMix + gDlLp[0] * gEchoFb; inR = gDlLp[1] * gEchoFb; break; }
				case 5:   // REVERSE: the dry signal is recorded, the reversed read is the output; light feedback of the reversed sound
					inL = mono * dMix + yl * gEchoFb * 0.6f; inR = mono * dMix + yr * gEchoFb * 0.6f; break;
				default:   // TAPE: ping-pong, darker and more saturated each pass
					gDlLp[0] += (yr - gDlLp[0]) * 0.45f; gDlLp[1] += (yl - gDlLp[1]) * 0.45f;
					inL = tanhf((mono * dMix + gDlLp[0] * gEchoFb) * sat) / sat; inR = tanhf(gDlLp[1] * gEchoFb * sat) / sat; break;
				}
				if (inL > 4) inL = 4;
				if (inL < -4) inL = -4;
				if (inR > 4) inR = 4;
				if (inR < -4) inR = -4;
				gDlL[gDlW] = inL; gDlR[gDlW] = inR;
				if (++gDlW >= DLMAX) gDlW = 0;
				wet[0] += yl * 0.8f; wet[1] += yr * 0.8f;
			}
			// reverb (+ shimmer: an octave-up copy of the tail fed back in; freeze: endless tail, no new input)
			if (revTail > 0) {
				float in = gFreeze ? 0 : revIn(mono * rMix * 0.025f), ol = 0, orr = 0;
				if (gShimmer) {
					float a = rdFrac(gPs, PSN, gPsR), b = rdFrac(gPs, PSN, gPsR + PSN / 2), ph = fmodf(gPsW - gPsR + PSN, PSN) / PSN;
					float w = 1 - fabsf(ph * 2 - 1);
					in += tanhf(a * w + b * (1 - w)) * 0.004f;   // loop gain < 1 so the octave tail always dies away
					gPsR += 2; if (gPsR >= PSN) gPsR -= PSN;
				}
				for (int i = 0; i < NCOMB; i++) { ol += comb(&gCbL[i], in, fb, damp); orr += comb(&gCbR[i], in, fb, damp); }
				for (int i = 0; i < NAP; i++) { ol = apass(&gApL[i], ol); orr = apass(&gApR[i], orr); }
				if (!gFreeze) revOut(&ol, &orr);
				if (gShimmer) { gPs[gPsW] = (ol + orr) * 0.5f; if (++gPsW >= PSN) gPsW = 0; }
				wet[0] += ol * 1.6f; wet[1] += orr * 1.6f;
			}
			x[0] += wet[0]; x[1] += wet[1];
		} break;
		case 4: {
			// OTT: three bands, each pushed down above and pulled up below a target level
			if (gOtt > 0.001f) {
				float band[3][2];
				for (int c = 0; c < 2; c++) {
					gBandLp1[0][c] += (x[c] - gBandLp1[0][c]) * 0.023f; gBandLp1[1][c] += (gBandLp1[0][c] - gBandLp1[1][c]) * 0.023f;   // ~120 Hz
					gBandLp2[0][c] += (x[c] - gBandLp2[0][c]) * 0.38f; gBandLp2[1][c] += (gBandLp2[0][c] - gBandLp2[1][c]) * 0.38f;     // ~2.5 kHz
					band[0][c] = gBandLp1[1][c]; band[2][c] = x[c] - gBandLp2[1][c]; band[1][c] = x[c] - band[0][c] - band[2][c];
				}
				for (int b = 0; b < 3; b++) {
					float e = fabsf(band[b][0]) + fabsf(band[b][1]);
					gBandEnv[b] += (e - gBandEnv[b]) * (e > gBandEnv[b] ? 0.02f : 0.0008f);
				}
				if ((s & 15) == 0) for (int b = 0; b < 3; b++) {   // gains per 16 samples
					float env = gBandEnv[b], T = 0.18f, g;
					if (env < 1e-4f) g = 1;
					else if (env > T) g = powf(T / env, 0.75f);
					else { g = powf(T / env, 0.66f); if (g > 6) g = 6; }
					gBandGain[b] = 1 + gOtt * (g - 1);
				}
				for (int c = 0; c < 2; c++) x[c] = (band[0][c] * gBandGain[0] + band[1][c] * gBandGain[1] + band[2][c] * gBandGain[2]) * (1 + gOtt * 0.3f);
			}
		} break;
		}
		if (gSpkEq == 1 || (gSpkEq == 2 && !gHeadphones)) {   // SPEAKER EQ: one-pole high-pass ~120 Hz and low-pass ~12 kHz for the console's small speaker
			static float hpS[2], lpS[2];
			for (int c = 0; c < 2; c++) { hpS[c] += (x[c] - hpS[c]) * 0.0228f; float h = x[c] - hpS[c]; lpS[c] += (h - lpS[c]) * 0.9f; x[c] = lpS[c]; }
		}
		// sidechain pump: duck on each beat, recover through it
		if (gPump > 0.001f && !gDry) {
			float ph;
			if (gPumpRate == 3) { ph = (gKickT + s) * beatInc * 1.6f; if (ph > 1) ph = 1; }   // KICK: from the last kick, recovering over ~a quarter at this tempo
			else { ph = (gBeatPh + s * beatInc) * pumpRate; ph -= floorf(ph); }
			float r = ph < 0.45f ? ph / 0.45f : 1; r = r * r * (3 - 2 * r);
			float g = 1 - gPump * (1 - r);
			x[0] *= g; x[1] *= g;
		}
		// limiter
		float pk = fabsf(x[0]) > fabsf(x[1]) ? fabsf(x[0]) : fabsf(x[1]);
		if (pk > gLimEnv) gLimEnv = pk; else gLimEnv += (pk - gLimEnv) * 0.0004f;
		float lg = gLimEnv > 0.95f ? 0.95f / gLimEnv : 1;
		L[s] = (s32)(x[0] * lg * 32768.0f); R[s] = (s32)(x[1] * lg * 32768.0f);
	}
	gBeatPh += n * beatInc; if (gBeatPh > 4096) gBeatPh -= 4096;
	if (gKickT < (1u << 30)) gKickT += n;
	// 3D ORBIT: in surround the DSP places the rear channels behind you; the sound circles the listener
	if (gOrbit > 0.001f && gSurActive) {
		static float oph = 0; oph += n / 32728.0f * gOrbitRate; if (oph >= 1) oph -= 1;
		float a = oph * TWO_PI, pan = 0.5f + 0.5f * sinf(a), fb = 0.5f + 0.5f * cosf(a), am = gOrbit;
		float gF = sqrtf(fb), gB = sqrtf(1 - fb), gl = sqrtf(2 * (1 - pan)), gr = sqrtf(2 * pan);
		float fl = (1 - am) + am * gF * gl, fr = (1 - am) + am * gF * gr, bl = am * gB * gl, br = am * gB * gr;
		s32* BL = (s32*)samples[2]; s32* BR = (s32*)samples[3];
		for (int s = 0; s < n; s++) { float l = L[s], r = R[s]; L[s] = (s32)(l * fl); R[s] = (s32)(r * fr); BL[s] = (s32)(l * bl); BR[s] = (s32)(r * br); }
	} else { memset(samples[2], 0, n * 4); memset(samples[3], 0, n * 4); }
	if (seen) gBusSeen = 1;
	gFxTicks = (u32)(svcGetSystemTick() - t0);
}

// ---------------------------------------------------------------- output: surround + second filter stage
static int gOutMode = 0, gSurDepth = 3;   // STEREO / SURROUND
static const char* OUT_NAMES[4] = {"STEREO", "SURROUND", "AUTO", "MONO"};   // AUTO: surround on headphones; MONO: one speaker / a mono cable
static const char* DEPTH_NAMES[4] = {"25%", "50%", "75%", "100%"};
static void outputApply(void) {
	if (!gAudioOK) return;
	int sur = gOutMode == 1 || (gOutMode == 2 && gHeadphones);
	ndspSetOutputMode(gOutMode == 3 ? NDSP_OUTPUT_MONO : sur ? NDSP_OUTPUT_SURROUND : NDSP_OUTPUT_STEREO); gSurActive = sur && gOutMode != 3;
	ndspSurroundSetDepth((u16)((gSurDepth + 1) * 0x7FFF / 4));
	ndspSetClippingMode(NDSP_CLIP_SOFT);
}

// ---------------------------------------------------------------- motion + breath (accelerometer, microphone)
static float gTiltCX = 0, gTiltCY = 0;
static int gTiltCal = 0, gMicOK = 0;
static u8* gMicBuf = NULL; static u32 gMicSize = 0;
static void sensorsInit(void) {
	HIDUSER_EnableAccelerometer();
	gMicBuf = (u8*)memalign(0x1000, 0x8000);
	if (gMicBuf && R_SUCCEEDED(micInit(gMicBuf, 0x8000))) {
		gMicSize = micGetSampleDataSize();
		gMicOK = R_SUCCEEDED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, gMicSize, true));
	}
}
static void sensorsExit(void) {
	if (gMicOK) MICU_StopSampling();
	if (gMicBuf) { micExit(); free(gMicBuf); }
	HIDUSER_DisableAccelerometer();
}
static void sensorsUpdate(float dt) {
	accelVector a; hidAccelRead(&a);
	float ax = a.x / 512.0f, ay = atan2f((float)a.z, (float)a.y);   // roll (1 g ~ 512), pitch angle
	if (gTiltCal < 20) { gTiltCX += (ax - gTiltCX) * 0.3f; gTiltCY += (ay - gTiltCY) * 0.3f; gTiltCal++; }   // centre = how it's held at start
	float tx = (ax - gTiltCX) * 2.2f, ty = (ay - gTiltCY) * 1.6f;
	if (ty > 3.14159f) ty -= 6.28318f;
	if (ty < -3.14159f) ty += 6.28318f;
	if (fabsf(tx) < 0.06f) tx = 0;
	if (fabsf(ty) < 0.06f) ty = 0;
	if (tx > 1) tx = 1;
	if (tx < -1) tx = -1;
	if (ty > 1) ty = 1;
	if (ty < -1) ty = -1;
	float k = 1.0f - expf(-dt / 0.05f);
	gTiltX += (tx - gTiltX) * k; gTiltY += (ty - gTiltY) * k;
	if (gMicOK) {   // breath / voice level: RMS of the last ~30 ms
		u32 end = micGetLastSampleOffset() / 2, N = gMicSize / 2;
		const s16* m = (const s16*)gMicBuf;
		float sum = 0;
		int zc = 0; s16 prev = m[(end + N - 1) % N];
		for (int i = 0; i < 512; i++) { s16 sm = m[(end + N - 1 - i) % N]; float v = sm / 32768.0f; sum += v * v; if ((sm ^ prev) < 0 && (sm > 200 || sm < -200)) zc++; prev = sm; }
		float br = gMicEnv > 0.05f ? ((float)zc / 512 - 0.03f) / 0.3f : 0;   // zero crossings: hiss and "s" are bright, "oo" is dark
		br = br < 0 ? 0 : br > 1 ? 1 : br; gMicBright += (br - gMicBright) * 0.3f;
		float rms = sqrtf(sum / 512) - 0.012f;   // small noise gate
		float lvl = rms / 0.12f; if (lvl < 0) lvl = 0; if (lvl > 1) lvl = 1;
		gMicEnv += (lvl - gMicEnv) * (lvl > gMicEnv ? 0.6f : 0.12f);
	}
}
static void tiltRecenter(void) { gTiltCal = 0; }

// ---------------------------------------------------------------- scales, arpeggiator, step sequencer
static const char* NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* kitPadName(int d); static int kitDrumOf(int nt);   // a kit patch: which drum plays this note (-1 = none / not a kit)
#define NSCALES 51
static const char* SCALE_NAMES[NSCALES] = {"CHROMATIC", "MAJOR", "MINOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN", "HARM MINOR", "MAJ PENTA", "MIN PENTA", "BLUES", "WHOLE TONE", "JAPANESE", "ARABIC", "LOCRIAN", "MELODIC MIN", "HARM MAJOR", "DOUBLE HARM", "HUNGARIAN MIN", "HUNGARIAN MAJ", "NEAPOLITAN MIN", "NEAPOLITAN MAJ", "ENIGMATIC", "PERSIAN", "PHRYG DOMINANT", "LYDIAN DOM", "LYDIAN AUG", "SUPER LOCRIAN", "DORIAN #4", "MAJ BLUES", "BEBOP MAJOR", "BEBOP DOM", "BEBOP MINOR", "DIMINISHED", "DIM HALF-WHOLE", "AUGMENTED", "PROMETHEUS", "TRITONE", "HIRAJOSHI", "IN SEN", "IWATO", "KUMOI", "PELOG", "SLENDRO", "EGYPTIAN", "CHINESE", "MONGOLIAN", "GYPSY", "SPANISH 8", "ALGERIAN", "RAGA BHAIRAV"};
static const s8 SCALES[NSCALES][13] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1}, {0, 2, 4, 5, 7, 9, 11, -1}, {0, 2, 3, 5, 7, 8, 10, -1}, {0, 2, 3, 5, 7, 9, 10, -1}, {0, 1, 3, 5, 7, 8, 10, -1}, {0, 2, 4, 6, 7, 9, 11, -1}, {0, 2, 4, 5, 7, 9, 10, -1}, {0, 2, 3, 5, 7, 8, 11, -1}, {0, 2, 4, 7, 9, -1}, {0, 3, 5, 7, 10, -1}, {0, 3, 5, 6, 7, 10, -1}, {0, 2, 4, 6, 8, 10, -1}, {0, 1, 5, 7, 8, -1}, {0, 1, 4, 5, 7, 8, 11, -1}, {0, 1, 3, 5, 6, 8, 10, -1}, {0, 2, 3, 5, 7, 9, 11, -1}, {0, 2, 4, 5, 7, 8, 11, -1}, {0, 1, 4, 5, 7, 8, 11, -1}, {0, 2, 3, 6, 7, 8, 11, -1}, {0, 3, 4, 6, 7, 9, 10, -1}, {0, 1, 3, 5, 7, 8, 11, -1}, {0, 1, 3, 5, 7, 9, 11, -1}, {0, 1, 4, 6, 8, 10, 11, -1}, {0, 1, 4, 5, 6, 8, 11, -1}, {0, 1, 4, 5, 7, 8, 10, -1}, {0, 2, 4, 6, 7, 9, 10, -1}, {0, 2, 4, 6, 8, 9, 11, -1}, {0, 1, 3, 4, 6, 8, 10, -1}, {0, 2, 3, 6, 7, 9, 10, -1}, {0, 2, 3, 4, 7, 9, -1}, {0, 2, 4, 5, 7, 8, 9, 11, -1}, {0, 2, 4, 5, 7, 9, 10, 11, -1}, {0, 2, 3, 4, 5, 7, 9, 10, -1}, {0, 2, 3, 5, 6, 8, 9, 11, -1}, {0, 1, 3, 4, 6, 7, 9, 10, -1}, {0, 3, 4, 7, 8, 11, -1}, {0, 2, 4, 6, 9, 10, -1}, {0, 1, 4, 6, 7, 10, -1}, {0, 2, 3, 7, 8, -1}, {0, 1, 5, 7, 10, -1}, {0, 1, 5, 6, 10, -1}, {0, 2, 3, 7, 9, -1}, {0, 1, 3, 7, 8, -1}, {0, 2, 5, 7, 9, -1}, {0, 2, 5, 7, 10, -1}, {0, 4, 6, 7, 11, -1}, {0, 2, 4, 7, 9, -1}, {0, 2, 3, 6, 7, 8, 10, -1}, {0, 1, 3, 4, 5, 6, 8, 10, -1}, {0, 2, 3, 6, 7, 8, 11, -1}, {0, 1, 4, 5, 7, 8, 11, -1}};
static int gRoot = 0, gScale = 0;
static int scaleLen(void) { int n = 0; while (n < 12 && SCALES[gScale][n] >= 0) n++; return n; }
static int scaleNote(int rootNote, int deg) {   // degree -> MIDI note (any degree, negative ok)
	int n = scaleLen(), o = deg >= 0 ? deg / n : -((-deg + n - 1) / n);
	return rootNote + 12 * o + SCALES[gScale][deg - o * n];
}
static int inScale(int note) { int pc = ((note - gRoot) % 12 + 12) % 12; for (int i = 0; SCALES[gScale][i] >= 0; i++) if (SCALES[gScale][i] == pc) return 1; return 0; }
static int snapScale(int note) { if (!gScale) return note; for (int d = 0; d < 6; d++) { if (inScale(note - d)) return note - d; if (inScale(note + d)) return note + d; } return note; }
static int gKeyDeg = 0;   // degree of the last pad pressed in scale mode

static int gArp;   // (set below with the arp)
// chord tones: chromatic chords, or chords stacked inside the scale when a scale is on
static int chordNotes(int key, int* out) {
	if (gVoiceMode >= 3) { out[0] = key; return 1; }   // drum kit: one hit per key
	if ((gVoiceMode == 1 || gVoiceMode == 2) && !gArp) { out[0] = key; return 1; }   // MONO / LEGATO: one voice can't hold a chord - play the key you hit (an arp still walks the chord)
	int n = 0, c = gChord < 0 || gChord >= NCHORD ? 0 : gChord;
	if (!gScale || CHORD_DEG[c][0] < 0) { for (int i = 0; i < 4; i++) if (CHORDS[c][i] >= 0) out[n++] = key + CHORDS[c][i]; return n; }
	int rootNote = key - scaleNote(0, gKeyDeg);   // = base + root
	for (int i = 0; i < 4; i++) if (CHORD_DEG[c][i] >= 0) out[n++] = rootNote + scaleNote(0, gKeyDeg + CHORD_DEG[c][i]);
	return n;
}

// ---- tempo + arpeggiator
#define NARPM 7
static const char* ARP_NAMES[NARPM] = {"OFF", "UP", "DOWN", "UP+DOWN", "RANDOM", "AS PLAYED", "CHORD"};
static const char* RATE_NAMES[6] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float RATE_BEATS[6] = {1.0f, 0.5f, 1.0f / 3, 0.25f, 1.0f / 6, 0.125f};
static int gArp = 0, gArpRate = 3, gArpOct = 1, gBPM = 120;
static float gArpGate = 0.5f;
static float gArpStrum = 0; static int gArpStrumDir = 0;   // ARP STRUM (ms between strings) + direction: the CHORD arp rolls its notes like a strummed guitar
#define NSTRD 4
static const char* STRUMDIR_NAMES[NSTRD] = {"DOWN", "UP", "DOWN+UP", "RANDOM"};
static int gStrQ[8], gStrN = 0, gStrUp = 0; static float gStrT[8], gStrV[8];   // strummed notes still to sound (time left, velocity)
static int gArpPool[8], gArpN = 0;   // notes the arp is cycling through (held keys or the current sequencer step)
static float gArpVel = 0.8f, gArpPh = 0;
static int gArpIdx = 0, gArpDir = 1, gArpFresh = 0;
static int gArpOn[8], gArpOnN = 0;
static int gBeatFlash = 0;   // set on each arp/seq step (UI blink)

static void voiceFade(int note, float sec);
static void arpReleaseX(int damp) {   // damp (the next CHORD strum coming): the last one stops in 50 ms, like a guitar restruck -
	// its long release used to pile up under the next chords until channels were stolen mid-note (the clicks on STRUM 8THS)
	for (int i = 0; i < gArpOnN; i++) { if (damp && gArp == 6) voiceFade(gArpOn[i], 0.05f); else voiceOff(gArpOn[i]); }
	gArpOnN = 0; gStrN = 0;
}
static void arpRelease(void) { arpReleaseX(0); }
static void arpSetPool(const int* notes, int n, float vel) {
	if (n > 8) n = 8;
	int was = gArpN;
	for (int i = 0; i < n; i++) gArpPool[i] = notes[i];
	gArpN = n; gArpVel = vel;
	if (!was && n) { gArpIdx = 0; gArpDir = 1; gArpFresh = 1; }   // start on the first press
	if (!n) arpRelease();
}
static void arpStep(void) {
	arpReleaseX(1);
	int s[8], n = gArpN;
	for (int i = 0; i < n; i++) s[i] = gArpPool[i];
	if (gArp == 6) {   // CHORD: retrigger the whole chord (strummed when ARP STRUM is up)
		if (gArpStrum < 0.5f || n < 2) { for (int i = 0; i < n; i++) { voiceOn(s[i], gArpVel); gArpOn[gArpOnN++] = s[i]; } return; }
		for (int i = 1; i < n; i++) for (int j = i; j > 0 && s[j] < s[j - 1]; j--) { int t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
		int up = gArpStrumDir == 1 || (gArpStrumDir == 2 && gStrUp);   // a down stroke goes low string to high
		if (gArpStrumDir == 2) gStrUp = !gStrUp;
		if (up) for (int i = 0; i < n / 2; i++) { int t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
		if (gArpStrumDir == 3) for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1), t = s[i]; s[i] = s[j]; s[j] = t; }
		float step = 60.0f / gBPM * RATE_BEATS[gArpRate], room = step * (gArpGate < 0.99f ? gArpGate : 1) * 0.9f;
		float gap = gArpStrum / 1000.0f; if (gap * (n - 1) > room) gap = room / (n - 1);   // the whole strum fits inside the note
		float v0 = gArpVel * (up ? 0.85f : 1.0f);   // up strokes land a little lighter
		voiceOn(s[0], v0); gArpOn[gArpOnN++] = s[0];
		for (int i = 1; i < n; i++) { gStrQ[gStrN] = s[i]; gStrT[gStrN] = gap * i; gStrV[gStrN] = v0 * (1.0f - 0.05f * i); gStrN++; }
		return;
	}
	if (gArp != 5) for (int i = 1; i < n; i++) for (int j = i; j > 0 && s[j] < s[j - 1]; j--) { int t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
	int tot = n * gArpOct, k;
	if (tot <= 0) return;
	if (gArp == 4) k = rand() % tot;
	else if (gArp == 3 && tot > 1) {
		if (gArpIdx >= tot) { gArpIdx = tot - 2; gArpDir = -1; }
		if (gArpIdx < 0) { gArpIdx = 1; gArpDir = 1; }
		k = gArpIdx; gArpIdx += gArpDir;
	} else { k = gArpIdx % tot; gArpIdx = (gArpIdx + 1) % tot; }
	if (gArp == 2) k = tot - 1 - k;
	int note = s[k % n] + 12 * (k / n);
	voiceOn(note, gArpVel); gArpOn[gArpOnN++] = note;
}
static void arpTick(float dt) {
	if (!gArp || !gArpN) { if (gArpOnN || gStrN) arpRelease(); gArpPh = 0; return; }
	for (int i = 0; i < gStrN; i++) if ((gStrT[i] -= dt) <= 0) {   // strummed strings arriving
		if (gArpOnN < 8) { voiceOn(gStrQ[i], gStrV[i]); gArpOn[gArpOnN++] = gStrQ[i]; }
		gStrN--; gStrQ[i] = gStrQ[gStrN]; gStrT[i] = gStrT[gStrN]; gStrV[i] = gStrV[gStrN]; i--;
	}
	float stepsPerSec = gBPM / 60.0f / RATE_BEATS[gArpRate];
	if (gArpFresh) { gArpFresh = 0; gArpPh = 0; arpStep(); gBeatFlash = 1; return; }
	float before = gArpPh;
	gArpPh += dt * stepsPerSec;
	if (before < gArpGate && gArpPh >= gArpGate && gArpGate < 0.99f) arpRelease();
	if (gArpPh >= 1) { gArpPh -= floorf(gArpPh); arpStep(); gBeatFlash = 1; }
}

// ---- clock: the arp and sequencer run in the audio thread, once per DSP frame (160 samples, ~4.9 ms)
static LightLock gVoiceLock;   // voices are touched by the main loop and by the audio thread
static float gClockDebt = 0; static volatile int gClockSkips = 0;   // HARDWARE page shows skips
static void arpTick(float dt);
static void seqTick(float dt); static void tapeSeqStarted(void); static void clickTick(void);
static void drumTick(float dt); static void shakeQTick(void);
static int tqDrain(int inTick); static int mqDrain(int inTick); static volatile int gTickMidi; static volatile int gTqUi;
static void audioTick(void* data) {
	(void)data;
	if (!gAudioClock) return;
	// time comes from the DSP's own frame counter, not from how often this callback ran: when the ndsp thread or
	// the voice lock makes us miss frames, the clock still advances by exactly the audio that played, so the
	// drums, sequencer and arp stay locked to the loop and tape (which the second chip plays in real time)
#ifdef SYRUP_HOST
	int nf = 1;
#else
	extern u16 ndspFrameId; static u16 lastId; static int init = 0;
	u16 id = ndspFrameId; int nf = (u16)(id - lastId); if (id < lastId) nf -= 2;   // (the id skips 0 and 1 when it wraps)
	if (!init || nf < 0 || nf > 2000) nf = 1;
	if (nf == 0) return;   // no new frame yet
#endif
#ifdef SYRUP_HOST
	#define CLK_MISS gClockDebt += 160.0f / DSPRATE
#else
	#define CLK_MISS (void)0
#endif
	if (LightLock_TryLock(&gVoiceLock)) { CLK_MISS; return; }   // main loop busy: the frames are counted next time
	int mipsOk = !LightLock_TryLock(&gMipsLock);   // a wavetable rebuild (XY, circle pad, motion moving WT POS) used to hold the clock here:
	gTickNoMips = !mipsOk;                          // now the clock runs anyway and notes it starts just skip building tables
#ifndef SYRUP_HOST
	lastId = id; init = 1;
#endif
	float dt = nf * 160.0f / DSPRATE + gClockDebt; gClockDebt = 0;
	if (dt > 0.25f) { dt = 160.0f / DSPRATE; gClockSkips++; }   // a very long stall drops time rather than firing a burst
	gInAudioTick = 1;
	if (tqDrain(1)) gTqUi = 1;   // FM-1 LINK's timed notes, on time to the tick
	gTickMidi = 1; if (mqDrain(1)) gTqUi = 1;   // network, IR and remote MIDI: as soon as it arrives, not at the next screen frame
	while (dt > 1e-6f) {   // missed frames: catch up in frame-sized pieces so every step still fires, in order
		float d = dt > 160.0f / DSPRATE ? 160.0f / DSPRATE : dt; dt -= d;
		if (!gMirror) { arpTick(d); seqTick(d); }   // a mirror plays only what the 3DS sends
		drumTick(d);
		clickTick();
		shakeQTick();
		gateTick(d, seqStepNow());
	}
	mixRampTick();
	gInAudioTick = 0; gTickNoMips = 0;
	if (mipsOk) LightLock_Unlock(&gMipsLock);
	LightLock_Unlock(&gVoiceLock);
}

// ---- held notes (keys or sequencer) -> voices or arp
static int gHeld[8], gHeldN = 0;
static int gMidiHeldN;
static float gHeldVel = 0.8f;
static void playSet(const int* notes, int n, float vel) {
	gHeldVel = vel;
	for (int i = 0; i < gHeldN; i++) voiceOff(gHeld[i]);
	gHeldN = 0;
	if (gArp) { arpSetPool(notes, n, vel); return; }
	if (gArpN) arpSetPool(NULL, 0, vel);
	for (int i = 0; i < n && i < 8; i++) { gHeld[gHeldN++] = notes[i]; voiceOn(notes[i], vel); }
}

// ---- 16-step sequencer (step record, keys transpose while playing)
// fl: 1 accent, 2 slide into the next step. Per-step locks (0 = none): gate % (0 = SEQ GATE), skip = chance in % that
// the step doesn't play, cut / res -100..100 on top of the patch, pitch in semitones, nudge -50..50 % of a step
typedef struct { s8 n; u8 note[4]; u8 vel; u8 fl; u8 gate, skip; s8 cut, res, pitch, nudge; u8 len[4]; u8 rat; } StepV2;   // seq.bin before conditions
typedef struct { s8 n; u8 note[4]; u8 vel; u8 fl; u8 gate, skip; s8 cut, res, pitch, nudge; u8 len[4]; u8 rat; u8 cond; } Step;   // cond: an Elektron-style trig condition (COND_NAMES)   // len: steps each note lasts (0 = 1); rat: hits in the step (0/1 = one, 2-4 = a roll)
typedef struct { s8 n; u8 note[4]; u8 vel; u8 fl; u8 gate, skip; s8 cut, res, pitch, nudge; } StepV1;   // seq.bin before note lengths
#define NSTEP 64   // up to 4 pages of 16
#define NSTEP16 16   // the step count of files from before pages
static const char* SEQ_NAMES[3] = {"OFF", "PLAY", "REC"};
static Step gSeq[NSTEP];
static int gSeqMode = 0, gSeqLen = 8, gSeqPos = -1, gRecPos = 0, gSeqBase = -1, gSeqTr = 0;
static float gSeqGate = 0.6f, gSwing = 0.0f, gSeqPh = 0;
static int gSeqPlaying = 0;
static int gSeqKeys = 0; static const char* SEQKEYS_NAMES[2] = {"PLAY", "TRANSPOSE"};   // while the sequence runs the keys play along (and the tape hears them), or transpose it
// patterns + song. gSeq / gSeqLen / gSeqBase / gRow* are the pattern being played and edited; gPat holds all 16.
// Rows of the grid can run their own length (polymeter) or an euclid rhythm instead of the steps.
#define NPAT 16
#define NSONG 64
typedef struct { Step st[NSTEP]; s16 len, base; u8 rowLen[8], rowHits[8], rowRot[8], rowNote[8]; } SeqPat;
typedef struct { Step st[NSTEP16]; s16 len, base; u8 rowLen[8], rowHits[8], rowRot[8], rowNote[8]; } SeqPat16;   // before pages
typedef struct { StepV2 st[NSTEP16]; s16 len, base; u8 rowLen[8], rowHits[8], rowRot[8], rowNote[8]; } SeqPatV2;   // rowNote: the pitch a rhythm row plays
typedef struct { StepV1 st[NSTEP16]; s16 len, base; u8 rowLen[8], rowHits[8], rowRot[8]; } SeqPatV1;
static SeqPat gPat[NPAT];
static int gPatCur = 0, gPatNext = -1;
static u8 gSong[NSONG]; static int gSongLen = 0, gSongPos = 0, gSongOn = 0;
static u8 gRowLen[8], gRowHits[8], gRowRot[8], gRowNote[8];
static unsigned gSeqClock = 0;   // steps since PLAY: polymeter rows count from this
// PREROLL + CLICK: a count-in before TAPE REC and live sequencer REC (clicks on the second chip: never on tape)
static const char* PRE_NAMES[3] = {"OFF", "1 BAR", "2 BARS"}; static const char* CLICK_NAMES[4] = {"OFF", "COUNT-IN", "RECORDING", "ALWAYS"};
// CLICK (the metronome): COUNT-IN only, RECORDING = the count-in and while the tape or live REC records, ALWAYS =
// whenever the sequence or the drums run. It plays on the second chip's own output, so it is never on the tape.
static float gClickLevel = 0.7f;
static int gPreroll = 1, gClick = 1, gPreFor = 0, gLiveRec = 0; static float gPreBeats = 0, gPreDone = 0;
// REC QUANT: where live notes land (16TH, 8TH, or OFF = your exact timing kept as the step's NUDGE); TAP REC: knocks on the table (mic) while live REC
static const char* RQ_NAMES[4] = {"16TH", "8TH", "OFF (NUDGE)", "32ND"}; static const char* TAPR_NAMES[3] = {"OFF", "NOTES", "VELOCITY"};
static int gRecQ = 0, gTapRec = 0, gTapNote = -1;
// SHAKE QUANT (PERC KIT's shaker): OFF plays as you shake; 16TH / 8TH holds the hit for the next step while the
// sequencer or the drums are running (with nothing running it plays at once)
static int gShakeQ = 1; static volatile float gShakePend = 0; static const char* SHQ_NAMES[3] = {"OFF", "16TH", "8TH"};
static float gLockCut = 0, gLockRes = 0, gStepGate = 0.6f, gFireOff = 0;   // the playing step's locks
static int gPendFire = -1, gEarlyDone = 0;
static void patStore(void) {
	SeqPat* p = &gPat[gPatCur]; memcpy(p->st, gSeq, sizeof(gSeq)); p->len = gSeqLen; p->base = gSeqBase;
	memcpy(p->rowLen, gRowLen, 8); memcpy(p->rowHits, gRowHits, 8); memcpy(p->rowRot, gRowRot, 8); memcpy(p->rowNote, gRowNote, 8);
}
static void patSwitch(int i) {   // store the working pattern, load pattern i
	if (i < 0 || i >= NPAT) return;
	patStore(); gPatCur = i; SeqPat* p = &gPat[i];
	memcpy(gSeq, p->st, sizeof(gSeq)); gSeqLen = p->len < 1 ? 16 : p->len > NSTEP ? NSTEP : p->len; gSeqBase = p->base;
	memcpy(gRowLen, p->rowLen, 8); memcpy(gRowHits, p->rowHits, 8); memcpy(gRowRot, p->rowRot, 8); memcpy(gRowNote, p->rowNote, 8);
	if (gSeqPos >= gSeqLen) gSeqPos = 0;
}
static int patUsed(int i) { const SeqPat* p = i == gPatCur ? NULL : &gPat[i]; for (int k = 0; k < NSTEP; k++) if ((p ? p->st[k].n : gSeq[k].n)) return 1; return 0; }
static const char* SEQ_PATH = "sdmc:/3ds/syrup/seq.bin";
#define SEQ_MAGIC 0x53455131   // "SEQ1"
static SeqPat gSnapPat[NPAT]; static u8 gSnapSong[NSONG]; static int gSnapHdr[4];
static void seqSnap(void) {   // copy what gets saved (hold the voice lock: the audio tick can switch patterns)
	patStore(); memcpy(gSnapPat, gPat, sizeof(gPat)); memcpy(gSnapSong, gSong, sizeof(gSong));
	gSnapHdr[0] = gPatCur; gSnapHdr[1] = gSongLen; gSnapHdr[2] = gSongOn; gSnapHdr[3] = (int)sizeof(SeqPat);
}
static void seqWriteSnap(void) {   // the slow part, done without the lock; skipped when nothing changed since the last write
	static SeqPat prevPat[NPAT]; static u8 prevSong[NSONG]; static int prevHdr[4], have = 0;
	if (have && !memcmp(prevPat, gSnapPat, sizeof(prevPat)) && !memcmp(prevSong, gSnapSong, sizeof(prevSong)) && !memcmp(prevHdr, gSnapHdr, sizeof(prevHdr))) return;
	memcpy(prevPat, gSnapPat, sizeof(prevPat)); memcpy(prevSong, gSnapSong, sizeof(prevSong)); memcpy(prevHdr, gSnapHdr, sizeof(prevHdr)); have = 1;
	FILE* f = safeOpen(SEQ_PATH); if (!f) return;
	u32 m = SEQ_MAGIC; int ok = fwrite(&m, 4, 1, f) == 1 && fwrite(gSnapHdr, sizeof(gSnapHdr), 1, f) == 1 && fwrite(gSnapPat, sizeof(gSnapPat), 1, f) == 1 && fwrite(gSnapSong, sizeof(gSnapSong), 1, f) == 1;
	safeClose(f, SEQ_PATH, ok);
}
static void seqSave(void) { seqSnap(); seqWriteSnap(); }
static void seqLoad(void) {
	FILE* f = safeRead(SEQ_PATH); if (!f) return;
	u32 m = 0; int hdr[4]; static SeqPat tmp[NPAT]; static SeqPatV1 old[NPAT]; u8 song[NSONG]; int ok = 0;
	if (fread(&m, 4, 1, f) == 1 && m == SEQ_MAGIC && fread(hdr, sizeof(hdr), 1, f) == 1) {
		if (hdr[3] == (int)sizeof(SeqPat)) ok = fread(tmp, sizeof(tmp), 1, f) == 1;
		else if (hdr[3] == (int)sizeof(SeqPat16)) {   // before pages: 16 steps, the rest of the 64 empty
			static SeqPat16 s16[NPAT];
			if (fread(s16, sizeof(s16), 1, f) == 1) { memset(tmp, 0, sizeof(tmp)); ok = 1;
				for (int p = 0; p < NPAT; p++) { memcpy(tmp[p].st, s16[p].st, sizeof(s16[p].st)); tmp[p].len = s16[p].len; tmp[p].base = s16[p].base;
					memcpy(tmp[p].rowLen, s16[p].rowLen, 8); memcpy(tmp[p].rowHits, s16[p].rowHits, 8); memcpy(tmp[p].rowRot, s16[p].rowRot, 8); memcpy(tmp[p].rowNote, s16[p].rowNote, 8); } }
		}
		else if (hdr[3] == (int)sizeof(SeqPatV2)) {   // before trig conditions: the same steps, every one ALWAYS
			static SeqPatV2 v2[NPAT];
			if (fread(v2, sizeof(v2), 1, f) == 1) { memset(tmp, 0, sizeof(tmp)); ok = 1;
				for (int p = 0; p < NPAT; p++) { for (int k = 0; k < NSTEP16; k++) memcpy(&tmp[p].st[k], &v2[p].st[k], sizeof(StepV2));
					tmp[p].len = v2[p].len; tmp[p].base = v2[p].base; memcpy(tmp[p].rowLen, v2[p].rowLen, 8); memcpy(tmp[p].rowHits, v2[p].rowHits, 8); memcpy(tmp[p].rowRot, v2[p].rowRot, 8); memcpy(tmp[p].rowNote, v2[p].rowNote, 8); } }
		}
		else if (hdr[3] == (int)sizeof(SeqPatV1) && fread(old, sizeof(old), 1, f) == 1) {   // before note lengths: every note one step, rows keep their pitch
			static const int MAJ[8] = {0, 2, 4, 5, 7, 9, 11, 12};
			memset(tmp, 0, sizeof(tmp));
			for (int p = 0; p < NPAT; p++) {
				for (int k = 0; k < NSTEP16; k++) memcpy(&tmp[p].st[k], &old[p].st[k], sizeof(StepV1));
				tmp[p].len = old[p].len; tmp[p].base = old[p].base;
				memcpy(tmp[p].rowLen, old[p].rowLen, 8); memcpy(tmp[p].rowHits, old[p].rowHits, 8); memcpy(tmp[p].rowRot, old[p].rowRot, 8);
				for (int r = 0; r < 8; r++) tmp[p].rowNote[r] = (u8)(48 + MAJ[r]);
			}
			ok = 1;
		}
		memset(song, 0, sizeof(song)); ok = ok && fread(song, 1, sizeof(song), f) >= 32;   // files from before the song grew to 64 hold 32
	}
	if (ok) {
		memcpy(gPat, tmp, sizeof(gPat)); memcpy(gSong, song, sizeof(gSong));
		gSongLen = hdr[1] < 0 ? 0 : hdr[1] > NSONG ? NSONG : hdr[1]; gSongOn = hdr[2] & 1;
		for (int i = 0; i < gSongLen; i++) if (gSong[i] >= NPAT) gSong[i] = 0;
		int c = hdr[0] < 0 || hdr[0] >= NPAT ? 0 : hdr[0]; gPatCur = c;
		memcpy(gSeq, gPat[c].st, sizeof(gSeq)); gSeqLen = gPat[c].len < 1 ? 16 : gPat[c].len > NSTEP ? NSTEP : gPat[c].len; gSeqBase = gPat[c].base;
		memcpy(gRowLen, gPat[c].rowLen, 8); memcpy(gRowHits, gPat[c].rowHits, 8); memcpy(gRowRot, gPat[c].rowRot, 8); memcpy(gRowNote, gPat[c].rowNote, 8);
	}
	fclose(f);
}

static void seqDefault(void) {   // a little pattern so PLAY makes sound straight away
	static const int pat[8] = {0, -1, 7, 12, -1, 10, 7, 3};
	memset(gSeq, 0, sizeof(gSeq));
	for (int i = 0; i < 8; i++) if (pat[i] >= 0) { gSeq[i].n = 1; gSeq[i].note[0] = 48 + pat[i]; gSeq[i].vel = 200; }
	gSeqLen = 8; gSeqBase = 48;
	memset(gRowLen, 0, 8); memset(gRowHits, 0, 8); memset(gRowRot, 0, 8); memset(gRowNote, 0, 8);
	static int init = 0; if (!init) { init = 1; for (int i = 0; i < NPAT; i++) { memset(&gPat[i], 0, sizeof(SeqPat)); gPat[i].len = 16; gPat[i].base = -1; } }
}
static void seqRecord(const int* notes, int n, float vel) {
	Step* s = &gSeq[gRecPos];
	s->n = n > 4 ? 4 : n;
	for (int i = 0; i < s->n; i++) s->note[i] = notes[i] < 0 ? 0 : (notes[i] > 127 ? 127 : notes[i]);
	s->vel = (u8)(vel * 255);
	if (gRecPos == 0) gSeqBase = notes[0];
	gRecPos++;
	gSeqLen = gRecPos;
	if (gRecPos >= NSTEP) gRecPos = 0;
}
static void seqRest(void) { gSeq[gRecPos].n = 0; gRecPos++; gSeqLen = gRecPos; if (gRecPos >= NSTEP) gRecPos = 0; }
static void seqClear(void) { memset(gSeq, 0, sizeof(gSeq)); gRecPos = 0; gSeqLen = 1; gSeqBase = -1; gSeqTr = 0; memset(gRowLen, 0, 8); memset(gRowHits, 0, 8); memset(gRowRot, 0, 8); memset(gRowNote, 0, 8); }
static int gSlideOn;
// the sequencer plays its own notes (not through the keys' held list), each with its own length:
// a note lasts len steps and lets go at the step's GATE in its last step; SLIDE keeps it and bends it to the next note
typedef struct { int note, left, rel, slide; float thr; } SqN;
static SqN gSqN[16]; static int gSqNn = 0;
static unsigned gSeqHits = 0;   // notes the sequencer has struck (tests count it)
static void sqRelease(SqN* q) { if (!q->rel) { voiceOff(q->note); q->rel = 1; } }
static void sqCompact(void) { int k = 0; for (int i = 0; i < gSqNn; i++) if (!(gSqN[i].rel && gSqN[i].left <= 0)) gSqN[k++] = gSqN[i]; gSqNn = k; }
static void sqAllOff(void) { for (int i = 0; i < gSqNn; i++) sqRelease(&gSqN[i]); gSqNn = 0; }
static int gSlideOn;   // the step before asked to slide into this one
static void seqStop(void) { if (gSeqPlaying) { sqAllOff(); if (gArp) playSet(NULL, 0, 0.8f); } gSlideOn = 0; gSeqPlaying = 0; gSeqPos = -1; gSeqPh = 0; gLockCut = gLockRes = 0; gPendFire = -1; gEarlyDone = 0; gFireOff = 0; }
static int gridNote(int row);
static int rowCustom(int r) { return gRowLen[r] || gRowHits[r]; }
static int rowLenOf(int r) { int L = gRowLen[r] ? gRowLen[r] : gSeqLen; return L < 1 ? 1 : L > NSTEP ? NSTEP : L; }
static int stepHasNote(int st, int nt) { for (int k = 0; k < gSeq[st].n; k++) if (gSeq[st].note[k] == nt) return 1; return 0; }
static int noteLen(const Step* s, int k) { return s->len[k] ? s->len[k] : 1; }
static int euclidHit(int i, int len, int hits, int rot) { if (hits <= 0) return 0; if (hits >= len) return 1; i = ((i - rot) % len + len) % len; return (i * hits) % len < hits; }
static int rowCellOn(int r, int i) {   // does rhythm row r play at its own step i?
	int L = rowLenOf(r); i %= L;
	return gRowHits[r] ? euclidHit(i, L, gRowHits[r], gRowRot[r]) : stepHasNote(i, gRowNote[r]);
}
static int rowOfNote(int nt) { for (int r = 0; r < 8; r++) if (rowCustom(r) && gRowNote[r] == nt) return r; return -1; }
static int rowSlotFor(int nt) {   // the rhythm row for this pitch, or a free one for it (-1 if all 8 are used)
	int r = rowOfNote(nt); if (r >= 0) return r;
	for (r = 0; r < 8; r++) if (!rowCustom(r)) { gRowNote[r] = (u8)nt; return r; }
	return -1;
}
static int noteOnCustomRow(int nt) { return rowOfNote(nt) >= 0; }
// SEQ RATE: what one sequencer step is. 1/32 runs the steps twice as fast (a 16-step pattern is half a bar), 1/8 half
static const char* SEQRATE_NAMES[3] = {"1/16", "1/32", "1/8"};
static float seqDiv(void) { return gSeqRate == 1 ? 0.5f : gSeqRate == 2 ? 2.0f : 1.0f; }   // a step, in 16ths
static float stepLen(int i) { return ((i & 1) ? 1.0f - gSwing : 1.0f + gSwing) * seqDiv(); }   // in 16ths
// trig conditions, shared by the sequencer and the drums: 0 ALWAYS, 1-5 chance, 6-14 A:B (loop A of every B), FILL,
// !FILL, PRE / !PRE (the last condition here), NEI / !NEI (the neighbour's), 1ST / !1ST (the first loop after start)
#define NCOND 23
static int gDrFill;
static int condTest(int c, int cyc, int* pre, int nei) {
	int ok = 1;
	switch (c) {
	case 0: return 1;
	case 1: ok = rand() % 100 < 90; break; case 2: ok = rand() % 100 < 75; break; case 3: ok = rand() % 100 < 50; break; case 4: ok = rand() % 100 < 25; break; case 5: ok = rand() % 100 < 10; break;
	case 6: case 7: ok = cyc % 2 == c - 6; break;
	case 8: case 9: case 10: ok = cyc % 3 == c - 8; break;
	case 11: case 12: case 13: case 14: ok = cyc % 4 == c - 11; break;
	case 15: ok = gDrFill; break; case 16: ok = !gDrFill; break;
	case 17: ok = *pre; break; case 18: ok = !*pre; break;
	case 19: ok = nei; break; case 20: ok = !nei; break;
	case 21: ok = cyc == 0; break; default: ok = cyc != 0; break;
	}
	if (c < 17 || c > 20) *pre = ok;   // PRE / NEI look at the last condition that wasn't itself PRE / NEI
	return ok;
}
static int gSeqPre = 0;
static void seqFireAt(int idx, unsigned clock, int early, float off0) {
	Step* s = &gSeq[idx];
	gLockCut = s->cut * 0.006f; gLockRes = s->res * 0.01f;
	float g = s->gate ? s->gate * 0.01f : gSeqGate; gStepGate = g;
	int notes[8], lens[8], n = 0, skip = s->skip && rand() % 100 < s->skip;
	if (!skip && s->cond && s->n) { int L = gSeqLen < 1 ? 1 : gSeqLen; skip = !condTest(s->cond, (int)(clock / L), &gSeqPre, gSeqPre); }   // IF: the step's condition
	if (!skip) {
		for (int i = 0; i < s->n && n < 8; i++) { if (noteOnCustomRow(s->note[i])) continue; lens[n] = noteLen(s, i); notes[n++] = snapScale(s->note[i] + gSeqTr + s->pitch); }
		for (int r = 0; r < 8 && n < 8; r++) if (rowCustom(r) && rowCellOn(r, clock % rowLenOf(r))) { lens[n] = 1; notes[n++] = snapScale(gRowNote[r] + gSeqTr + s->pitch); }
	}
	float vel = s->n ? s->vel / 255.0f : 0.8f;
	if (s->fl & 1) vel = 1.0f;   // accent
	gBeatFlash = 1;
	if (gArp) { playSet(notes, n, vel); gSlideOn = n && (s->fl & 2); return; }   // the arp takes the step's notes as its pool
	int sl = -1, nsl = 0; for (int i = 0; i < gSqNn; i++) if (gSqN[i].slide && !gSqN[i].rel) { sl = i; nsl++; }
	if (gSlideOn && n >= 1 && nsl >= 1) {   // slide: keep the voices, bend them to the new notes (chords pair low to high, like the pads' MORPH)
		int fi[16], m = 0; for (int i = 0; i < gSqNn; i++) if (gSqN[i].slide && !gSqN[i].rel) fi[m++] = i;
		for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) if (gSqN[fi[b]].note < gSqN[fi[a]].note) { int t = fi[a]; fi[a] = fi[b]; fi[b] = t; }
		int ord[8]; for (int j = 0; j < n; j++) ord[j] = j;
		for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (notes[ord[b]] < notes[ord[a]]) { int t = ord[a]; ord[a] = ord[b]; ord[b] = t; }
		int chord = n > 1 || m > 1, k = m < n ? m : n, sl2 = (s->fl & 2) != 0;
		for (int a = 0; a < k; a++) {
			SqN* q = &gSqN[fi[a]]; int j = ord[a], from = q->note;
			for (int i = 0; i < NVS; i++) { SVoice* v = &gV[i]; if (v->active && !v->isEcho && v->note == from && v->stage < 4 && v->slide != 3) { v->note = notes[j]; v->freq = mtof((float)notes[j]); v->slide = 3; } }
			mirRetune(from, notes[j], chord ? 2 : 1); q->note = notes[j]; q->left = lens[j] + early; q->slide = sl2; q->thr = (lens[j] == 1 ? off0 : 0) + stepLen((idx + lens[j] - 1) % NSTEP) * g;
		}
		for (int i = 0; i < NVS; i++) if (gV[i].slide == 3) gV[i].slide = chord ? 2 : 1;   // chords glide by PAD MORPH, single notes by GLIDE
		for (int a = k; a < m; a++) { sqRelease(&gSqN[fi[a]]); gSqN[fi[a]].left = 0; gSqN[fi[a]].slide = 0; }   // spare voices let go
		for (int a = k; a < n && gSqNn < 16; a++) { int j = ord[a]; voiceOn(notes[j], vel); gSeqHits++; SqN* q = &gSqN[gSqNn++]; q->note = notes[j]; q->left = lens[j] + early; q->rel = 0; q->slide = sl2; q->thr = (lens[j] == 1 ? off0 : 0) + stepLen((idx + lens[j] - 1) % NSTEP) * g; }
		sqCompact(); gSlideOn = sl2; return;
	}
	for (int i = 0; i < gSqNn; i++) if (gSqN[i].slide) { sqRelease(&gSqN[i]); gSqN[i].left = 0; gSqN[i].slide = 0; }   // a slide into something else just ends
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < gSqNn; i++) if (gSqN[i].note == notes[j] && !gSqN[i].rel) { sqRelease(&gSqN[i]); gSqN[i].left = 0; }   // the same pitch again: restrike
		sqCompact();
		if (gSqNn >= 16) break;
		voiceOn(notes[j], vel); gSeqHits++;
		SqN* q = &gSqN[gSqNn++]; q->note = notes[j]; q->left = lens[j] + early; q->rel = 0; q->slide = (s->fl & 2) != 0;
		q->thr = (lens[j] == 1 ? off0 : 0) + stepLen((idx + lens[j] - 1) % NSTEP) * g;
	}
	gSlideOn = n && (s->fl & 2);
}
static void seqFire(void) { seqFireAt(gSeqPos, gSeqClock, 0, gFireOff); }
static void seqAdvance(void) {   // to the next step; at the end of the pattern the song or a queued pattern takes over
	int len = gSeqLen < 1 ? 1 : gSeqLen;
	gSeqPos++; gSeqClock++;
	if (gSeqPos >= len) {
		gSeqPos = 0;
		if (gSongOn && gSongLen > 0) { gSongPos = (gSongPos + 1) % gSongLen; if (gSong[gSongPos] != gPatCur) patSwitch(gSong[gSongPos]); }
		else if (gPatNext >= 0) { patSwitch(gPatNext); gPatNext = -1; }
	}
}
static int seqStepNow(void) { return gSeqPlaying ? gSeqPos : -1; }
static void seqTick(float dt) {
	if (gSeqMode != 1) { if (gSeqPlaying) seqStop(); return; }
	if (!gSeqPlaying) {
		gSeqPlaying = 1; gSeqPos = 0; gSeqPh = 0; gSeqClock = 0; gPendFire = -1; gEarlyDone = 0; gFireOff = 0; gSqNn = 0;
		tapeSeqStarted();
		if (gSongOn && gSongLen > 0) { gSongPos = 0; if (gSong[0] != gPatCur) patSwitch(gSong[0]); }
		else if (gPatNext >= 0) { patSwitch(gPatNext); gPatNext = -1; }
		if (gSeq[0].nudge > 0) { gPendFire = 0; gFireOff = gSeq[0].nudge * 0.01f * stepLen(0); } else seqFire();
		return;
	}
	float len = stepLen(gSeqPos);
	float before = gSeqPh;
	gSeqPh += dt * gBPM / 60.0f * 4.0f;
	if (gPendFire >= 0 && gSeqPh >= gFireOff) { gPendFire = -1; seqFire(); }   // a late step
	{ int rat = gSeqPos >= 0 ? gSeq[gSeqPos].rat : 0;   // RATCHET: the step's one-step notes hit again, evenly through the step
	  if (rat > 1 && gPendFire < 0 && !gArp) for (int k = 1; k < rat; k++) { float at = gFireOff + len * k / rat; if (before < at && gSeqPh >= at) {
		Step* s = &gSeq[gSeqPos]; float vel = (s->fl & 1) ? 1.0f : s->vel / 255.0f * 0.85f;
		for (int i = 0; i < s->n; i++) if (noteLen(s, i) == 1) { int nt = snapScale(s->note[i] + gSeqTr + s->pitch);
			for (int j = 0; j < gSqNn; j++) if (gSqN[j].note == nt) sqRelease(&gSqN[j]);
			sqCompact(); if (gSqNn < 16) { voiceOn(nt, vel); gSeqHits++; SqN* q = &gSqN[gSqNn++]; q->note = nt; q->left = 1; q->rel = 0; q->slide = 0; q->thr = at + len / rat * (s->gate ? s->gate * 0.01f : gSeqGate); } }
	  } } }
	if (!gArp) for (int i = 0; i < gSqNn; i++) { SqN* q = &gSqN[i]; if (q->left == 1 && !q->slide && !q->rel && before < q->thr && gSeqPh >= q->thr) sqRelease(q); }   // notes ending in this step let go at the gate
	else if (!gSlideOn && gPendFire < 0 && !gEarlyDone && before < gFireOff + len * gStepGate && gSeqPh >= gFireOff + len * gStepGate) playSet(NULL, 0, 0.8f);
	int L = gSeqLen < 1 ? 1 : gSeqLen, nx = gSeqPos + 1;
	if (!gEarlyDone && nx < L && gSeq[nx].nudge < 0 && gSeqPh >= len + gSeq[nx].nudge * 0.01f * len && gSeqPh < len) {   // an early step
		gEarlyDone = 1; seqFireAt(nx, gSeqClock + 1, 1, gSeq[nx].nudge * 0.01f * len);
	}
	if (gSeqPh >= len) {
		gSeqPh -= len; seqAdvance();
		for (int i = 0; i < gSqNn; i++) { SqN* q = &gSqN[i]; q->left--; if (q->left <= 0 && !q->slide) sqRelease(q); }
		sqCompact();
		Step* s = &gSeq[gSeqPos];
		if (gEarlyDone) gEarlyDone = 0;
		else if (s->nudge > 0) { gPendFire = gSeqPos; gFireOff = s->nudge * 0.01f * stepLen(gSeqPos); }
		else { gFireOff = 0; seqFire(); }
		for (int i = 0; i < gSqNn; i++) { SqN* q = &gSqN[i]; if (q->left == 1 && !q->slide && !q->rel && q->thr <= gSeqPh) sqRelease(q); }   // a gate that ends before the step starts (early notes)
	}
}

// ---------------------------------------------------------------- presets (organised by style, like Serum's browser)
typedef struct {
	int on, table, warp, oct, semi, uni;
	float pos, warpAmt, level, fine, detune, blend, width;
} OscP;
typedef struct {
	u32 magic;
	char name[20];
	OscP osc[2];
	int subShape, subOct, noiseType, filtType;
	float subLevel, noiseLevel, cutoff, res, keytrack;
	AHDSR env1, env2;
	int lfoShape[2], lfoMode[2]; float lfoRate[2];
	ModSlot mod[4];
	int voiceMode, chord, fx, echoT, drive, bendRange;
	float glide, chorus, revSize, revDamp, fxMix, driveAmt, master;
	int arp, arpRate, arpOct, bpm;   // bpm 0 = keep the current tempo
	float arpGate;
	int lfoSync[2], lfoTable[2]; float lfoPos[2];   // v3
	ModSlot mod2[4]; int modCurve[8]; float macro[4];   // v4
	int modFx, dlySync, shimmer, freeze, pumpRate, lofiBits;
	float modRate, modDepth, modMix, fxColor, eqLow, eqMid, eqHigh, dlyMix, dlyWow, dlySat, revMix, ott, pump, lofiRate;
	float chipLevel, csEcho, csEchoFb, orbit, orbitRate; int chipDuty, chipOct, csEchoT;   // v5
	int revType, dlyType;   // v6
	int grainMode, grainSrc; float grainSize, grainScan, grainSpray, grainPitch, grainLevel;   // v7
	int ofType; float ofCut, ofRes, ofDrive, ofEnv;   // v8
	s8 dl[2][64]; float dlRate[2]; int dlSync[2];   // v9: drawn LFO 3 / 4
	u8 mot[4][64]; int motTgt[4], motOn[4], motBars;   // v10: motion lanes
	int qGrit, csEchoPitch; float qScream, qMixDrive;   // v11: chip quirks
	// v12: a patch can bring a beat (hasBeat), a loop setup (hasLoop), CHIP > FX, tape wow and the XY pad mode
	int hasBeat, drums, kit, kickPat, snarePat, hatPat, euHits[3], euRot[3], euLen, drumRec; float drumLevel, swing;
	int hasLoop, loopBars; float loopSpeed, loopWow;
	int thru, xyMode, tapeKeys; float thruCut, thruRes, thruEnv, thruLfo, echoWow;
	u8 fxOrder[8];   // v13: master FX chain order (first 5 used)
	int arpStrumDir; float arpStrum;   // v14: ARP STRUM
	int gatePat, gateRate; float gateDepth;   // v15: TRANCE GATE
} SPatch;
#define PATCH_MAGIC 0x5359523F   // "SYR?" (v15)
#define PATCH_MAGIC14 0x5359523E // v14 (no trance gate)
#define PATCH_V14_SIZE offsetof(SPatch, gatePat)
#define PATCH_MAGIC13 0x5359523D // v13 (no arp strum)
#define PATCH_V13_SIZE offsetof(SPatch, arpStrumDir)
#define PATCH_MAGIC12 0x5359523C // v12 (fixed FX order)
#define PATCH_V12_SIZE offsetof(SPatch, fxOrder)
#define PATCH_MAGIC11 0x5359523B // v11 (no beat / loop / CHIP > FX / XY mode)
#define PATCH_V11_SIZE offsetof(SPatch, hasBeat)
#define PATCH_MAGIC10 0x5359523A // v10 (no chip quirks)
#define PATCH_V10_SIZE offsetof(SPatch, qGrit)
#define PATCH_MAGIC9 0x53595239  // v9 (no motion lanes)
#define PATCH_V9_SIZE offsetof(SPatch, mot)
#define PATCH_MAGIC8 0x53595238  // v8 (no drawn LFOs)
#define PATCH_V8_SIZE offsetof(SPatch, dl)
#define PATCH_MAGIC7 0x53595237  // v7 (no OUT FILTER)
#define PATCH_V7_SIZE offsetof(SPatch, ofType)
#define PATCH_MAGIC6 0x53595236  // v6 (no grain settings)
#define PATCH_V6_SIZE offsetof(SPatch, grainMode)
#define PATCH_MAGIC5 0x53595235  // v5 (no reverb type)
#define PATCH_V5_SIZE offsetof(SPatch, revType)
#define PATCH_MAGIC4 0x53595234  // v4 (no chip layer / chip echo / orbit)
#define PATCH_V4_SIZE offsetof(SPatch, chipLevel)
#define PATCH_MAGIC3 0x53595233  // v3 (no macros / master FX)
#define PATCH_V3_SIZE offsetof(SPatch, mod2)
#define PATCH_MAGIC2 0x53595232  // v2 (no LFO sync / table)
#define PATCH_V2_SIZE offsetof(SPatch, lfoSync)
#define PATCH_MAGIC1 0x53595231  // v1 (no arp fields)
#define PATCH_V1_SIZE offsetof(SPatch, arp)

static void initWire(SPatch* p);
static void patchInit(SPatch* p) {   // the INIT patch: one saw, open filter, no effects
	memset(p, 0, sizeof(*p));
	p->magic = PATCH_MAGIC;
	snprintf(p->name, sizeof(p->name), "INIT");
	for (int o = 0; o < 2; o++) {
		OscP* q = &p->osc[o];
		q->on = (o == 0); q->table = T_BASIC; q->pos = 0.66f; q->level = 0.8f; q->uni = 1;
		q->detune = 14; q->blend = 0.6f; q->width = 0.8f;
	}
	p->subShape = 0; p->subOct = -1; p->noiseType = 0;
	p->filtType = 0; p->cutoff = 0.8f; p->res = 0.1f; p->keytrack = 0.3f;
	p->env1 = (AHDSR){0.005f, 0, 0.6f, 0.7f, 0.35f};
	p->env2 = (AHDSR){0.002f, 0, 0.45f, 0.2f, 0.4f};
	p->lfoShape[0] = 0; p->lfoRate[0] = 0.6f; p->lfoShape[1] = 0; p->lfoRate[1] = 5.0f;
	p->mod[0] = (ModSlot){SRC_ENV2, DST_CUTOFF, 0.0f};
	p->mod[1] = (ModSlot){SRC_STICK, DST_APOS, 0.5f};
	p->mod[2] = (ModSlot){SRC_OFF, DST_BPOS, 0.0f};
	p->mod[3] = (ModSlot){SRC_OFF, DST_PITCH, 0.0f};
	p->bendRange = 2; p->chorus = 0.2f; p->revSize = 0.5f; p->revDamp = 0.5f; p->fxMix = 0.5f;
	p->driveAmt = 0.4f; p->master = 0.8f; p->echoT = 1;
	p->arp = 0; p->arpRate = 3; p->arpOct = 1; p->arpGate = 0.5f; p->bpm = 0; p->arpStrum = 0; p->arpStrumDir = 0; p->gatePat = 0; p->gateRate = 0; p->gateDepth = 1.0f;
	for (int i = 0; i < 2; i++) { p->lfoSync[i] = 0; p->lfoTable[i] = T_HARMONIX; p->lfoPos[i] = 0.5f; }
	for (int i = 0; i < 4; i++) { p->mod2[i] = (ModSlot){SRC_OFF, DST_PITCH, 0}; p->macro[i] = 0; }
	for (int i = 0; i < 8; i++) p->modCurve[i] = 0;
	p->modFx = 0; p->dlySync = 0; p->shimmer = 0; p->freeze = 0; p->pumpRate = 1; p->lofiBits = 0;
	p->modRate = 0.4f; p->modDepth = 0.5f; p->modMix = 0.5f; p->fxColor = 0.5f; p->eqLow = p->eqMid = p->eqHigh = 0;
	p->dlyMix = 0; p->dlyWow = 0.25f; p->dlySat = 0.3f; p->revMix = 0; p->ott = 0; p->pump = 0; p->lofiRate = 0;
	p->chipLevel = 0; p->chipDuty = 3; p->chipOct = 0; p->csEcho = 0; p->csEchoFb = 0.45f; p->csEchoT = 2; p->orbit = 0; p->orbitRate = 0.2f; p->revType = 0; p->dlyType = 0; p->grainMode = 0; p->grainSrc = 1; p->grainSize = 0.35f; p->grainScan = 0.5f; p->grainSpray = 0.2f; p->grainPitch = 0; p->grainLevel = 0.8f; p->ofType = 0; p->ofCut = 0.6f; p->ofRes = 0.2f; p->ofDrive = 0.2f; p->ofEnv = 0;
	p->qGrit = 0; p->csEchoPitch = 0; p->qScream = 0; p->qMixDrive = 0;
	for (int i = 0; i < 64; i++) { p->dl[0][i] = (s8)(sinf(TWO_PI * i / 64) * 127); p->dl[1][i] = (s8)((i < 32 ? i * 4 - 64 : 191 - i * 4) * 127 / 64); }
	p->dlRate[0] = 1.0f; p->dlRate[1] = 0.5f; p->dlSync[0] = p->dlSync[1] = 0;
	memset(p->mot, 128, sizeof(p->mot)); p->motTgt[0] = 0; p->motTgt[1] = 2; p->motTgt[2] = 4; p->motTgt[3] = 12; memset(p->motOn, 0, sizeof(p->motOn)); p->motBars = 1;
	p->hasBeat = 0; p->drums = 0; p->kit = 0; p->kickPat = 1; p->snarePat = 1; p->hatPat = 1; p->drumRec = 0; p->drumLevel = 0.8f; p->swing = 0;
	p->euHits[0] = 5; p->euHits[1] = 2; p->euHits[2] = 11; p->euRot[0] = 0; p->euRot[1] = 4; p->euRot[2] = 0; p->euLen = 16;
	p->hasLoop = 0; p->loopBars = 0; p->loopSpeed = 0; p->loopWow = 0;
	p->thru = 0; p->xyMode = 7; p->tapeKeys = 0; p->thruCut = 1.0f; p->thruRes = 0.1f; p->thruEnv = 0; p->thruLfo = 0; p->echoWow = 0;   // xyMode 7 = PAGE: the pad follows the page
	for (int i = 0; i < 8; i++) p->fxOrder[i] = i;
}

#define OA p->osc[0]
#define OB p->osc[1]
#define MOD(i, s, d, a) p->mod[i] = (ModSlot){s, d, a}
#define ENV1(A, H, D, S, R) p->env1 = (AHDSR){A, H, D, S, R}
#define ENV2(A, H, D, S, R) p->env2 = (AHDSR){A, H, D, S, R}
#define LFO(i, sh, md, rt) do { p->lfoShape[i] = sh; p->lfoMode[i] = md; p->lfoRate[i] = rt; } while (0)
#define SAW 0.66f
#define SINE 0.0f
#define TRI 0.33f
#define SQR 1.0f

// ================================================================ factory library (rebuilt from scratch)
// Every patch is built on the current engine: OUT FILTER ladders, drawn LFO 3/4, MOTION lanes, grains, the chip
// square, breath and the 2DS's sensors. The circle pad still morphs OSC A's WT POS unless a patch says otherwise.
#define MOD2(i, s, d, a) p->mod2[i] = (ModSlot){s, d, a}
#define REVB(m, t) do { p->revMix = m; p->revType = t; } while (0)
#define DLYS(m, sy, t) do { p->dlyMix = m; p->dlySync = sy; p->dlyType = t; } while (0)
#define MFX(t, r, d, m) do { p->modFx = t; p->modRate = r; p->modDepth = d; p->modMix = m; } while (0)
#define OUTF(t, c, r, dr, e) do { p->ofType = t; p->ofCut = c; p->ofRes = r; p->ofDrive = dr; p->ofEnv = e; } while (0)
#define GRN(sz, sc, sp, pi, lv) do { p->grainMode = 2; p->grainSrc = 1; p->grainSize = sz; p->grainScan = sc; p->grainSpray = sp; p->grainPitch = pi; p->grainLevel = lv; } while (0)
#define ORBIT(a, r) do { p->orbit = a; p->orbitRate = r; } while (0)
#define ARP(m, r, o, g) do { p->arp = m; p->arpRate = r; p->arpOct = o; p->arpGate = g; } while (0)
#define DRV(t, a) do { p->drive = t; p->driveAmt = a; } while (0)
#define CHIP(l, d, o) do { p->chipLevel = l; p->chipDuty = d; p->chipOct = o; } while (0)
#define NOSTICK MOD(1, SRC_OFF, DST_APOS, 0)
enum { RV_HALL, RV_ROOM, RV_PLATE, RV_SPRING, RV_GATED, RV_SWELL };
enum { DL_TAPE, DL_DIGI, DL_PING, DL_DUB, DL_LOFI, DL_REV };
enum { S16 = 1, S8, S8D, S4, S4D, S2 };   // delay sync
enum { OF_L24 = 1, OF_L12, OF_ACID, OF_DIRTY, OF_CREAMY, OF_LP, OF_HP, OF_BP, OF_NOTCH, OF_COMBP, OF_COMBM, OF_PHASER, OF_FLANGER };
enum { FX_CHORUS = 1, FX_FLANGER, FX_PHASER, FX_COMB, FX_FORMANT, FX_HYPER, FX_DIM };
enum { LY_1BAR = 3, LY_HALF, LY_4, LY_8, LY_16 };   // LFO sync
// a MOTION lane: shape kinds 0 rise, 1 fall, 2 sine, 3 triangle, 4 eight steps, 5 slow wander, 6 one swell
static void lane(SPatch* p, int l, int tgt, int kind, float lo, float hi, int bars) {
	static const float ST[8] = {0.1f, 0.7f, 0.35f, 0.9f, 0.2f, 0.55f, 0.8f, 0.4f};
	unsigned h = 2166136261u ^ (unsigned)(tgt * 31 + l);
	for (int i = 0; i < 64; i++) {
		float t = i / 64.0f, v;
		switch (kind) {
		case 0: v = t; break; case 1: v = 1 - t; break; case 2: v = 0.5f - 0.5f * cosf(TWO_PI * t); break;
		case 3: v = t < 0.5f ? t * 2 : 2 - t * 2; break; case 4: v = ST[i / 8]; break;
		case 5: { h = h * 16777619u ^ (unsigned)i; float a = (h >> 8 & 255) / 255.0f; v = i ? (p->mot[l][i - 1] / 255.0f - lo) / (hi - lo + 1e-6f) * 0.8f + a * 0.2f : a; break; }
		default: v = sinf(3.14159f * t); break;
		}
		float x = lo + (hi - lo) * v; if (x < 0) x = 0; if (x > 1) x = 1;
		p->mot[l][i] = (u8)(x * 255);
	}
	p->motTgt[l] = tgt; p->motOn[l] = 1; p->motBars = bars < 0 ? 0 : bars > 3 ? 3 : bars;   // bars: 0..3 = 1 / 2 / 4 / 8 bars, shared by all lanes
}
// drawn LFO 3 / 4 shapes: 0 sine, 1 ramp down, 2 melody steps (values are semitones/12), 3 gate, 4 rise-hold
static void dshape(SPatch* p, int k, int kind, const s8* steps8) {
	for (int i = 0; i < 64; i++) {
		float v;
		switch (kind) {
		case 0: v = sinf(TWO_PI * i / 64); break;
		case 1: v = 1 - i / 31.5f; break;
		case 2: v = steps8[i / 8] / 12.0f; break;
		case 3: v = (i % 16) < 10 ? 1 : -1; break;
		default: v = i < 24 ? -1 + i / 12.0f : 1; break;
		}
		p->dl[k][i] = (s8)(v * 127);
	}
}
static int gPackMissing = 0;
static int wtFile(const char* nm, int fallback) {   // a table from the downloadable pack, or a built-in stand-in
	char f[48]; snprintf(f, sizeof(f), "%s.wav", nm);
	for (int i = 0; i < gNFiles; i++) if (!strcasecmp(gFiles[i], f)) return T_BUILTIN + i;
	gPackMissing = 1; return fallback;
}

// ---- BASS
static void b_subfloor(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = SINE; OA.oct = -1; OB.on = 1; OB.table = T_BASIC; OB.pos = TRI; OB.level = 0.25f;
	p->voiceMode = 1; p->glide = 0.03f; ENV1(0.004f, 0, 0.4f, 0.9f, 0.12f); OUTF(OF_L24, 0.35f, 0.15f, 0.3f, 0.2f); p->eqLow = 2; NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.4f); }
static void b_303(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; p->voiceMode = 2; p->glide = 0.06f; ENV1(0.002f, 0, 0.3f, 0.7f, 0.08f);
	ENV2(0.001f, 0, 0.22f, 0.05f, 0.15f); OUTF(OF_ACID, 0.18f, 0.82f, 0.45f, 0.5f); MOD(2, SRC_VEL, DST_OFCUT, 0.2f); NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.5f); DLYS(0.15f, S8D, DL_DUB); }
static void b_reese(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; OA.uni = 4; OA.detune = 22; OA.width = 0.5f; p->voiceMode = 1;
	ENV1(0.01f, 0, 0.5f, 0.9f, 0.2f); OUTF(OF_L24, 0.42f, 0.2f, 0.35f, 0); MFX(FX_PHASER, 0.12f, 0.6f, 0.4f); p->ott = 0.35f; LFO(0, LS_SINE, 0, 0.1f); MOD(2, SRC_LFO1, DST_OFCUT, 0.15f);
	lane(p, 0, 4, 2, 0.3f, 0.7f, 2); lane(p, 1, 5, 3, 0.1f, 0.4f, 2); p->xyMode = 3; }
static void b_wobble(SPatch* p) { p->master = 0.55f; OA.table = T_GROWL; OA.oct = -1; p->voiceMode = 1; ENV1(0.003f, 0, 0.3f, 0.9f, 0.1f); LFO(0, LS_SINE, 0, 2); p->lfoSync[0] = LY_8;
	MOD(2, SRC_LFO1, DST_APOS, 0.6f); MOD(3, SRC_LFO1, DST_OFCUT, 0.35f); OUTF(OF_L12, 0.4f, 0.45f, 0.5f, 0); p->ott = 0.5f; p->subLevel = 0.35f; NOSTICK; MOD(1, SRC_STICK, DST_AWARP, 0.6f); OA.warp = W_BENDPM; }
static void b_fmknock(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SINE; OA.oct = -1; OA.warp = W_FM; OA.warpAmt = 0.25f; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 0;
	ENV2(0.0f, 0, 0.14f, 0.0f, 0.1f); MOD(2, SRC_ENV2, DST_AWARP, 0.6f); ENV1(0.002f, 0, 0.35f, 0.4f, 0.12f); p->voiceMode = 1; p->eqLow = 1.5f; }
static void b_diode(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SQR; OA.oct = -1; DRV(6, 0.55f); p->voiceMode = 1; ENV1(0.002f, 0, 0.35f, 0.6f, 0.1f);
	ENV2(0.0f, 0, 0.18f, 0.1f, 0.1f); OUTF(OF_L12, 0.3f, 0.5f, 0.3f, 0.45f); }
static void b_sp12(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = TRI; OA.oct = -1; p->subLevel = 0.5f; p->voiceMode = 1; ENV1(0.002f, 0, 0.55f, 0.0f, 0.2f);
	p->lofiBits = 2; p->lofiRate = 0.35f; OUTF(OF_L24, 0.38f, 0.1f, 0.2f, 0); p->eqLow = 2; }
static void b_cumbia(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = 0.15f; OA.oct = -1; p->subShape = 5; p->subLevel = 0.4f; ENV1(0.003f, 0, 0.35f, 0.25f, 0.15f);
	p->voiceMode = 1; OUTF(OF_CREAMY, 0.4f, 0.2f, 0.1f, 0.15f); MFX(FX_CHORUS, 0.5f, 0.3f, 0.2f); }
static void b_dnb(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; OA.uni = 3; OA.detune = 16; OB.on = 1; OB.table = T_BASIC; OB.pos = SQR; OB.oct = -1; OB.level = 0.4f;
	p->voiceMode = 1; ENV1(0.003f, 0, 0.4f, 0.9f, 0.12f); OUTF(OF_NOTCH, 0.45f, 0.5f, 0.3f, 0); MOD(2, SRC_CHAOSX, DST_OFCUT, 0.25f); p->ott = 0.45f; }
static void b_tiltgrowl(SPatch* p) { p->master = 0.55f; OA.table = T_GROWL; OA.oct = -1; OA.warp = W_ASYMPM; p->voiceMode = 1; ENV1(0.004f, 0, 0.3f, 0.9f, 0.1f);
	NOSTICK; MOD(1, SRC_STICK, DST_AWARP, 0.9f); MOD(2, SRC_XY, DST_OFCUT, 0.5f); MOD(3, SRC_TILTX, DST_AWARP, 0.15f); OUTF(OF_DIRTY, 0.45f, 0.4f, 0.4f, 0); p->ott = 0.4f; }
static void b_upright(SPatch* p) { p->master = 0.8f; OA.table = T_BASIC; OA.pos = 0.25f; OA.oct = -1; ENV1(0.002f, 0, 0.6f, 0.0f, 0.25f); MOD(2, SRC_VEL, DST_CUTOFF, 0.4f);
	p->cutoff = 0.45f; OUTF(OF_COMBP, 0.22f, 0.25f, 0, 0); REVB(0.12f, RV_ROOM); }
static void b_chip(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = TRI; OA.oct = -1; CHIP(0.4f, 1, -1); p->voiceMode = 1; ENV1(0.002f, 0, 0.3f, 0.8f, 0.05f); p->lofiBits = 1; }

// ---- LEAD
static void l_gulfsync(SPatch* p) { p->master = 0.55f; OA.table = T_SYNC; OA.warp = W_SYNC; ENV2(0.0f, 0, 0.5f, 0.2f, 0.3f); MOD(2, SRC_ENV2, DST_AWARP, 0.6f);
	p->voiceMode = 2; p->glide = 0.04f; ENV1(0.004f, 0, 0.3f, 0.85f, 0.25f); DLYS(0.22f, S8D, DL_TAPE); REVB(0.15f, RV_PLATE); }
static void l_talkbox(SPatch* p) { p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 8; MFX(FX_FORMANT, 0.1f, 0.5f, 0.5f); p->voiceMode = 2; p->glide = 0.05f;
	MOD(2, SRC_STICK, DST_FXP, 0.8f); ENV1(0.01f, 0, 0.3f, 0.9f, 0.2f); REVB(0.12f, RV_ROOM); }
static void l_bigunison(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 5; OA.detune = 26; OA.width = 1; ENV1(0.01f, 0, 0.4f, 0.85f, 0.4f);   // 5 voices a note: 4 notes before stealing
	MFX(FX_HYPER, 0.4f, 0.5f, 0.35f); DLYS(0.2f, S8D, DL_PING); REVB(0.2f, RV_HALL); OUTF(OF_LP, 0.75f, 0.1f, 0, 0); p->xyMode = 3; }
// BUSY BOY: the Korg cumbia lead (M1 / N364 / X5D), matched to a recording: its own waveform (the BUSY BOY table, the 3rd
// harmonic on top), two slightly detuned layers (the original's harmonics drift in phase), and a pitch scoop of about 25
// cents up into every note over ~60 ms. No glide and no vibrato: the original has neither.
static void l_busyboy(SPatch* p) { p->master = 0.65f; OA.table = wtFile("BUSY BOY", T_PWM); OA.pos = 5.0f / 7.0f; OA.uni = 2; OA.detune = 4; OA.width = 0.4f;
	p->voiceMode = 0; p->glide = 0; ENV1(0.004f, 0, 0.3f, 1.0f, 0.08f); ENV2(0.0f, 0, 0.06f, 0.0f, 0.05f); MOD(2, SRC_ENV2, DST_PITCH, -0.021f);
	p->cutoff = 1.0f; p->res = 0; REVB(0.12f, RV_ROOM); }
static void l_whistle(SPatch* p) { p->master = 0.6f; OA.table = T_PWM; OA.pos = 0.4f; p->voiceMode = 2; p->glide = 0.07f; LFO(0, LS_SINE, 0, 5.5f); MOD(2, SRC_LFO1, DST_PITCH, 0.012f);
	MOD(3, SRC_LFO1, DST_APOS, 0.1f); ENV1(0.02f, 0, 0.2f, 0.9f, 0.2f); DLYS(0.2f, S4, DL_TAPE); }
static void l_scream(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; DRV(8, 0.5f); MOD(2, SRC_STICK, DST_AWARP, 0); p->voiceMode = 2; p->glide = 0.03f;
	ENV1(0.003f, 0, 0.3f, 0.9f, 0.2f); OUTF(OF_L24, 0.55f, 0.55f, 0.6f, 0.25f); NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.5f); DLYS(0.18f, S8, DL_DIGI); }
static void l_moogtape(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; OB.on = 1; OB.table = T_BASIC; OB.pos = SQR; OB.oct = -1; OB.level = 0.4f; p->voiceMode = 2; p->glide = 0.08f;
	ENV2(0.0f, 0, 0.4f, 0.4f, 0.3f); OUTF(OF_L24, 0.32f, 0.45f, 0.35f, 0.45f); ENV1(0.005f, 0, 0.3f, 0.9f, 0.25f); DLYS(0.25f, S8D, DL_TAPE); p->dlyWow = 0.5f; }
static void l_chiphero(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.25f; OA.warp = W_QUANT; OA.warpAmt = 0.5f; CHIP(0.45f, 1, 0); p->voiceMode = 2;
	LFO(0, LS_TRI, 0, 6.0f); MOD(2, SRC_LFO1, DST_PITCH, 0.01f); ENV1(0.002f, 0, 0.2f, 0.8f, 0.1f); DLYS(0.2f, S8, DL_LOFI); }
static void l_glidesine(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = SINE; p->voiceMode = 2; p->glide = 0.12f; LFO(0, LS_SINE, 0, 5.0f); MOD(2, SRC_LFO1, DST_PITCH, 0.01f);
	ENV1(0.02f, 0, 0.2f, 1.0f, 0.3f); MFX(FX_CHORUS, 0.5f, 0.4f, 0.3f); DLYS(0.25f, S4D, DL_TAPE); REVB(0.18f, RV_PLATE); }
static void l_ringer(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.4f; OA.warp = W_RM; OA.warpAmt = 0.6f; OB.table = T_BASIC; OB.pos = SINE; OB.semi = 7;
	MOD(2, SRC_STICK, DST_AWARP, 0.4f); ENV1(0.005f, 0, 0.5f, 0.7f, 0.4f); DLYS(0.2f, S8D, DL_PING); REVB(0.2f, RV_PLATE); }
static void l_acidscream(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; p->voiceMode = 2; p->glide = 0.05f; ENV2(0.0f, 0, 0.3f, 0.2f, 0.2f);
	OUTF(OF_ACID, 0.3f, 0.88f, 0.7f, 0.55f); NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.5f); ENV1(0.002f, 0, 0.3f, 0.9f, 0.15f); DLYS(0.2f, S8D, DL_DUB); }
static void l_bender(SPatch* p) { p->master = 0.55f; OA.table = T_DIGITAL; OA.pos = 0.4f; OA.uni = 2; OA.detune = 9; NOSTICK; MOD(1, SRC_STICK, DST_PITCH, 0.17f); MOD(2, SRC_XY, DST_OFCUT, 0.4f);
	p->voiceMode = 2; ENV1(0.005f, 0, 0.3f, 0.9f, 0.3f); OUTF(OF_L12, 0.6f, 0.35f, 0.2f, 0); DLYS(0.2f, S8, DL_DIGI); }

// ---- PAD
static void p_felt(SPatch* p) { p->master = 0.65f; OA.table = T_PWM; OA.uni = 4; OA.detune = 16; OA.width = 1; LFO(0, LS_TRI, 0, 0.3f); MOD(2, SRC_LFO1, DST_APOS, 0.35f);
	ENV1(0.6f, 0, 1.0f, 0.9f, 1.4f); OUTF(OF_CREAMY, 0.66f, 0.15f, 0, 0); MFX(FX_DIM, 0.3f, 0.6f, 0.5f); REVB(0.3f, RV_HALL); }
static void p_glass(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.pos = 0.55f; OA.uni = 3; OA.detune = 9; OB.on = 1; OB.table = T_DIGITAL; OB.oct = 1; OB.level = 0.2f;
	LFO(0, LS_SINE, 0, 0.07f); MOD(2, SRC_LFO1, DST_APOS, 0.4f); ENV1(0.4f, 0, 1.0f, 0.9f, 2.2f); REVB(0.35f, RV_PLATE); p->shimmer = 1; p->eqHigh = 1.5f; }
static void p_vowel(SPatch* p) { p->master = 0.7f; OA.table = T_VOWELS; OA.uni = 3; OA.detune = 12; LFO(0, LS_TRI, 0, 0.06f); MOD(2, SRC_LFO1, DST_APOS, 0.8f);
	ENV1(0.7f, 0, 1.0f, 0.9f, 1.8f); MFX(FX_CHORUS, 0.25f, 0.5f, 0.35f); REVB(0.35f, RV_HALL); }
static void p_combweather(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.uni = 3; OA.detune = 10; p->noiseType = 1; p->noiseLevel = 0.08f; ENV1(0.8f, 0, 1.0f, 1.0f, 2.0f);
	OUTF(OF_COMBP, 0.45f, 0.6f, 0, 0); MOD(2, SRC_CHAOSX, DST_OFCUT, 0.35f); MOD(3, SRC_CHAOSY, DST_APOS, 0.4f); REVB(0.35f, RV_HALL); }
static void p_cloudbank(SPatch* p) { p->master = 0.75f; OA.table = T_HARMONIX; OA.level = 0.2f; LFO(0, LS_SH, 0, 0.35f); MOD(2, SRC_LFO1, DST_APOS, 0.6f); GRN(0.8f, 0.5f, 0.7f, 0, 1.0f);
	ENV1(0.9f, 0, 1.0f, 1.0f, 2.5f); REVB(0.45f, RV_HALL); }
static void p_strings77(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 4; OA.detune = 18; OA.width = 1; ENV1(0.35f, 0, 1.0f, 0.9f, 0.9f);
	OUTF(OF_L12, 0.5f, 0.1f, 0, 0); MFX(FX_CHORUS, 0.6f, 0.8f, 0.6f); REVB(0.25f, RV_HALL); p->eqHigh = -1; p->xyMode = 3; }
static void p_drawnswell(SPatch* p) { p->master = 0.65f; OA.table = T_SYNC; OA.uni = 3; OA.detune = 12; dshape(p, 0, 4, NULL); p->dlSync[0] = LY_1BAR - 1; p->dlRate[0] = 0.25f;
	MOD(2, SRC_LFO3, DST_OFCUT, 0.4f); OUTF(OF_L24, 0.35f, 0.35f, 0.2f, 0); ENV1(0.5f, 0, 1.0f, 1.0f, 1.5f); REVB(0.35f, RV_HALL); }
static void p_haze(SPatch* p) { p->master = 0.7f; OA.table = T_VOWELS; OA.uni = 3; OA.detune = 14; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 1; OB.level = 0.25f; ENV1(0.6f, 0, 1.0f, 0.9f, 1.8f);
	lane(p, 0, 0, 2, 0.1f, 0.9f, 3); lane(p, 1, 4, 3, 0.5f, 0.85f, 2); OUTF(OF_CREAMY, 0.65f, 0.25f, 0, 0); REVB(0.35f, RV_HALL);
	lane(p, 2, 9, 5, 0.1f, 0.4f, 2); lane(p, 3, 10, 2, 0.2f, 0.5f, 2); p->xyMode = 5; }
static void p_reverse(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.uni = 4; OA.detune = 14; ENV1(0.4f, 0, 1.0f, 0.9f, 1.2f); REVB(0.45f, RV_SWELL); DLYS(0.3f, S4, DL_REV); }
static void p_sonar(SPatch* p) { p->master = 0.75f; OA.table = T_HARMONIX; OA.oct = -1; OA.uni = 3; OA.detune = 8; ENV1(1.0f, 0, 1.0f, 1.0f, 2.5f); OUTF(OF_PHASER, 0.4f, 0.6f, 0, 0);
	LFO(0, LS_SINE, 0, 0.05f); MOD(2, SRC_LFO1, DST_OFCUT, 0.4f); ORBIT(0.5f, 0.06f); REVB(0.4f, RV_HALL); }
// ENGINE SHOWCASE pads: each shows one part of Syrup off (wavetable scanning, the grain engine, warp morphing, shimmer)
static void p_tabletide(SPatch* p) { p->master = 0.65f; OA.table = T_DIGITAL; OA.uni = 3; OA.detune = 14; OA.width = 1; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 1; OB.level = 0.25f;
	ENV1(0.5f, 0, 1.0f, 0.9f, 1.8f); ENV2(1.5f, 0, 2.0f, 0.4f, 1.5f); LFO(0, LS_TRI, 0, 0.11f); MOD(2, SRC_LFO1, DST_APOS, 0.9f); MOD(3, SRC_ENV2, DST_BPOS, 0.7f);
	OUTF(OF_CREAMY, 0.6f, 0.2f, 0, 0.2f); MFX(FX_CHORUS, 0.3f, 0.5f, 0.35f); REVB(0.4f, RV_HALL); p->xyMode = 5; }   // the tables wash through every frame
static void p_grainhalo(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.pos = 0.6f; OA.level = 0.35f; GRN(0.7f, 0.6f, 0.35f, 12, 0.85f); LFO(1, LS_SINE, 0, 0.07f); MOD(2, SRC_LFO2, DST_APOS, 0.6f);
	ENV1(0.9f, 0, 1.0f, 0.95f, 2.5f); p->shimmer = 1; REVB(0.5f, RV_HALL); MFX(FX_DIM, 0.3f, 0.4f, 0.4f); }   // a cloud of grains an octave up, shimmering
static void p_prismshift(SPatch* p) { p->master = 0.6f; OA.table = T_SYNC; OA.warp = W_BENDPM; OA.uni = 2; OA.detune = 10; OA.width = 1; OB.on = 1; OB.table = T_VOWELS; OB.level = 0.4f;
	ENV1(0.4f, 0, 1.0f, 0.9f, 1.6f); LFO(0, LS_TRI, 0, 0.09f); LFO(1, LS_SH, 0, 0.6f); MOD(2, SRC_LFO1, DST_AWARP, 0.7f); MOD(3, SRC_LFO2, DST_BPOS, 0.35f);
	MFX(FX_PHASER, 0.15f, 0.6f, 0.4f); OUTF(OF_CREAMY, 0.55f, 0.25f, 0, 0); REVB(0.35f, RV_PLATE); p->xyMode = 1; MOD2(0, SRC_M1, DST_AWARP, 0.6f); MOD2(1, SRC_M2, DST_BPOS, 0.6f); }   // warp bending, vowels stepping
static void p_shimmerdrift(SPatch* p) { p->master = 0.65f; OA.table = T_BASIC; OA.pos = TRI; OA.uni = 3; OA.detune = 18; OA.width = 1; OB.on = 1; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 1; OB.level = 0.3f;
	ENV1(1.2f, 0, 1.0f, 1.0f, 3.0f); p->shimmer = 1; REVB(0.6f, RV_SWELL); DLYS(0.25f, S4D, DL_TAPE); p->dlyWow = 0.6f; MFX(FX_CHORUS, 0.2f, 0.6f, 0.4f); }   // shimmer + tape echo: it blooms upward
static void p_hallchoir(SPatch* p) { p->master = 0.7f; OA.table = T_VOWELS; OA.level = 0.4f; LFO(0, LS_TRI, 0, 0.08f); MOD(2, SRC_LFO1, DST_APOS, 0.6f); GRN(0.55f, 0.5f, 0.15f, 0, 0.8f);
	ENV1(0.6f, 0, 1.0f, 0.9f, 2.0f); REVB(0.45f, RV_HALL); MFX(FX_CHORUS, 0.2f, 0.5f, 0.3f); }

// ---- KEYS
static void k_epsat(SPatch* p) { p->master = 0.65f; OA.table = T_BASIC; OA.pos = SINE; OA.warp = W_FM; OA.warpAmt = 0.2f; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 2;
	ENV2(0.0f, 0, 0.6f, 0.1f, 0.4f); MOD(2, SRC_ENV2, DST_AWARP, 0.35f); MOD(3, SRC_VEL, DST_AWARP, 0.3f); LFO(0, LS_SINE, 0, 4.5f); MOD2(0, SRC_LFO1, DST_PAN, 0.4f);
	ENV1(0.002f, 0, 1.4f, 0.3f, 0.5f); DLYS(0.15f, S8D, DL_TAPE); REVB(0.18f, RV_ROOM); }
static void k_drawbar(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.35f; ENV1(0.005f, 0, 0.1f, 1.0f, 0.06f); MFX(FX_CHORUS, 5.5f, 0.4f, 0.5f); DRV(1, 0.25f); REVB(0.18f, RV_SPRING); }
static void k_lofiup(SPatch* p) { p->master = 0.75f; OA.table = T_DIGITAL; OA.pos = 0.2f; OB.on = 1; OB.table = T_BASIC; OB.pos = TRI; OB.level = 0.4f; ENV1(0.002f, 0, 1.6f, 0.0f, 0.6f);
	MOD(2, SRC_VEL, DST_CUTOFF, 0.4f); p->cutoff = 0.55f; p->lofiBits = 1; p->lofiRate = 0.2f; DLYS(0.12f, S4, DL_TAPE); p->dlyWow = 0.7f; REVB(0.2f, RV_ROOM); }
static void k_clav(SPatch* p) { p->master = 0.6f; OA.table = T_PWM; OA.pos = 0.15f; ENV1(0.001f, 0, 0.45f, 0.0f, 0.12f); ENV2(0.0f, 0, 0.2f, 0.0f, 0.1f);
	OUTF(OF_DIRTY, 0.35f, 0.55f, 0.3f, 0.5f); MOD(2, SRC_VEL, DST_OFCUT, 0.3f); MFX(FX_PHASER, 0.4f, 0.5f, 0.3f); }
static void k_wurli(SPatch* p) { p->master = 0.65f; OA.table = T_BASIC; OA.pos = 0.2f; DRV(7, 0.3f); MOD(2, SRC_VEL, DST_AWARP, 0.4f); OA.warp = W_ASYMP; ENV1(0.002f, 0, 1.0f, 0.2f, 0.4f);
	LFO(0, LS_SINE, 0, 5.0f); MOD(3, SRC_LFO1, DST_ALEV, 0.15f); REVB(0.15f, RV_ROOM); }
static void k_vibes(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SINE; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 2; OB.level = 0.15f; ENV1(0.002f, 0, 2.0f, 0.0f, 1.2f);
	LFO(0, LS_SINE, 0, 5.5f); MOD(2, SRC_LFO1, DST_ALEV, 0.3f); REVB(0.25f, RV_PLATE); }
static void k_toy(SPatch* p) { p->master = 0.6f; OA.table = T_DIGITAL; OA.pos = 0.7f; DRV(11, 0.5f); ENV1(0.001f, 0, 0.4f, 0.0f, 0.2f); REVB(0.15f, RV_ROOM); DLYS(0.15f, S16, DL_LOFI); }
static void k_harpsi(SPatch* p) { p->master = 0.5f; OA.table = T_SYNC; OA.pos = 0.6f; OA.uni = 2; OA.detune = 6; ENV1(0.001f, 0, 0.9f, 0.0f, 0.3f); ENV2(0.0f, 0, 0.15f, 0, 0.1f);
	MOD(0, SRC_ENV2, DST_CUTOFF, 0.3f); p->cutoff = 0.65f; REVB(0.2f, RV_HALL); }
static void k_rotary(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.55f; ENV1(0.005f, 0, 0.1f, 1.0f, 0.06f); MFX(FX_CHORUS, 1.2f, 0.6f, 0.6f); NOSTICK;
	MOD(1, SRC_STICK, DST_FXP, 0.8f); DRV(5, 0.3f); REVB(0.15f, RV_SPRING); }
static void k_cumbiaorg(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.55f; OB.on = 1; OB.table = T_BASIC; OB.pos = SQR; OB.oct = 1; OB.level = 0.25f; ENV1(0.004f, 0, 0.1f, 1.0f, 0.05f);
	LFO(0, LS_SINE, 0, 6.2f); MOD(2, SRC_LFO1, DST_PITCH, 0.01f); OUTF(OF_LP, 0.7f, 0.1f, 0, 0); REVB(0.2f, RV_SPRING); DLYS(0.1f, S8, DL_TAPE); }

// ---- PLUCK
static void u_nylon(SPatch* p) { p->master = 0.7f; OA.table = wtFile("SP PLUCK DECAY", T_BASIC); OA.pos = 0.2f; ENV2(0.0f, 0, 0.3f, 0.0f, 0.2f); MOD(2, SRC_ENV2, DST_APOS, 0.6f);
	ENV1(0.001f, 0, 1.0f, 0.0f, 0.4f); MOD(3, SRC_VEL, DST_CUTOFF, 0.35f); p->cutoff = 0.55f; REVB(0.2f, RV_ROOM); }
static void u_marimba(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = SINE; OB.on = 1; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 2; OB.level = 0.2f; ENV1(0.001f, 0, 0.5f, 0.0f, 0.3f);
	OUTF(OF_COMBP, 0.5f, 0.2f, 0, 0); REVB(0.2f, RV_ROOM); }
static void u_kalimba(SPatch* p) { p->master = 0.7f; OA.table = T_DIGITAL; OA.pos = 0.45f; ENV1(0.001f, 0, 0.8f, 0.0f, 0.5f); MOD(2, SRC_RAND, DST_PITCH, 0.003f); REVB(0.25f, RV_PLATE); DLYS(0.15f, S8, DL_PING); }
static void u_digiharp(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.pos = 0.6f; OA.warp = W_FM; OA.warpAmt = 0.3f; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 1;
	ENV2(0.0f, 0, 0.3f, 0, 0.2f); MOD(2, SRC_ENV2, DST_AWARP, 0.5f); ENV1(0.001f, 0, 0.7f, 0.0f, 0.4f); DLYS(0.25f, S8D, DL_PING); REVB(0.2f, RV_HALL); }
static void u_stab99(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 3; OA.detune = 14; p->chord = 1; ENV1(0.001f, 0, 0.25f, 0.0f, 0.15f); ENV2(0.0f, 0, 0.15f, 0, 0.1f);
	OUTF(OF_L12, 0.35f, 0.4f, 0.3f, 0.5f); REVB(0.2f, RV_ROOM); DLYS(0.15f, S8D, DL_DIGI); }
static void u_koto(SPatch* p) { p->master = 0.6f; OA.table = T_SYNC; OA.pos = 0.3f; ENV2(0.0f, 0, 0.25f, 0, 0.1f); MOD(2, SRC_ENV2, DST_APOS, 0.5f); ENV1(0.001f, 0, 1.2f, 0.0f, 0.5f); REVB(0.3f, RV_SPRING); }
static void u_chipplink(SPatch* p) { p->master = 0.6f; OA.table = T_PWM; OA.pos = 0.2f; OA.warp = W_QUANT; OA.warpAmt = 0.6f; CHIP(0.3f, 0, 1); ENV1(0.001f, 0, 0.25f, 0.0f, 0.1f); DLYS(0.2f, S16, DL_LOFI); }
static void u_grain(SPatch* p) { p->master = 0.7f; OA.table = T_DIGITAL; OA.level = 0.4f; GRN(0.12f, 0.7f, 0.3f, 12, 0.7f); ENV1(0.001f, 0, 0.6f, 0.0f, 0.4f); REVB(0.25f, RV_PLATE); DLYS(0.2f, S8, DL_PING); }
static void u_steel(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SINE; OA.warp = W_FM; OA.warpAmt = 0.45f; OB.table = T_BASIC; OB.pos = SINE; OB.semi = 5; OB.oct = 1;
	ENV2(0.0f, 0, 0.5f, 0, 0.2f); MOD(2, SRC_ENV2, DST_AWARP, 0.4f); ENV1(0.001f, 0, 0.9f, 0.0f, 0.5f); REVB(0.25f, RV_PLATE); }
static void u_raintin(SPatch* p) { p->master = 0.7f; OA.table = T_DIGITAL; OA.pos = 0.5f; ENV1(0.001f, 0, 0.3f, 0.0f, 0.25f); MOD(2, SRC_RAND, DST_APOS, 0.6f); MOD(3, SRC_RAND, DST_CUTOFF, 0.4f);
	MOD2(0, SRC_RAND, DST_PAN, 0.8f); OUTF(OF_COMBM, 0.55f, 0.4f, 0, 0); REVB(0.3f, RV_PLATE); }

// ---- CHORD (made for PADS and the strum strip)
static void c_organstab(SPatch* p) { p->master = 0.5f; OA.table = T_HARMONIX; OA.pos = 0.4f; ENV1(0.002f, 0, 0.25f, 0.3f, 0.12f); p->pump = 0.5f; REVB(0.15f, RV_ROOM); }
static void c_neosoul(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = 0.1f; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 1; OB.level = 0.15f; ENV1(0.01f, 0, 1.2f, 0.4f, 0.6f);
	MFX(FX_CHORUS, 0.3f, 0.5f, 0.4f); DLYS(0.15f, S4D, DL_TAPE); p->dlyWow = 0.5f; REVB(0.2f, RV_ROOM); }
static void c_dub(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = 0.5f; ENV1(0.002f, 0, 0.18f, 0.0f, 0.1f); OUTF(OF_BP, 0.5f, 0.3f, 0.2f, 0); DLYS(0.4f, S8D, DL_DUB); REVB(0.25f, RV_SPRING); }
static void c_techno(SPatch* p) { p->master = 0.5f; OA.table = T_DIGITAL; OA.pos = 0.6f; ENV1(0.001f, 0, 0.2f, 0.0f, 0.15f); OUTF(OF_COMBP, 0.4f, 0.45f, 0.2f, 0); REVB(0.3f, RV_HALL); DLYS(0.2f, S8D, DL_DIGI); }
static void c_strum(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = 0.55f; OA.uni = 2; OA.detune = 6; ENV1(0.002f, 0, 1.4f, 0.0f, 0.8f); ENV2(0.0f, 0, 0.4f, 0, 0.3f);
	MOD(0, SRC_ENV2, DST_CUTOFF, 0.35f); p->cutoff = 0.4f; MFX(FX_CHORUS, 0.4f, 0.3f, 0.25f); REVB(0.2f, RV_HALL); }
static void c_omni(SPatch* p);
// ARP STRUM showcases: hold one key, the CHORD arp strums it
static void c_strum8(SPatch* p) { c_strum(p); p->chord = 2; ARP(6, 1, 1, 0.85f); p->arpStrum = 22; p->arpStrumDir = 2; }   // MAJ7, eighths, down-up like a rhythm guitar
static void c_harproll(SPatch* p) { c_omni(p); p->chord = 1; ARP(6, 0, 1, 0.95f); p->arpStrum = 70; p->arpStrumDir = 0; }   // slow rolled MIN7, one stroke a beat
static void c_rake(SPatch* p) { c_strum(p); OA.pos = 0.7f; p->cutoff = 0.5f; p->chord = 4; ARP(6, 3, 1, 0.6f); p->arpStrum = 12; p->arpStrumDir = 3; }   // SUS, fast raked sixteenths
// PADS + BASS: chord sounds made for the pads with a bass line under them (the bass plays the same sound an octave down,
// so each has a sub and an envelope that suits both). Loading one opens the pads with its BASS style and progression.
static void c_soulwalk(SPatch* p) { k_epsat(p); p->master = 0.6f; p->subShape = 0; p->subLevel = 0.25f; ENV1(0.003f, 0, 1.4f, 0.35f, 0.5f); MFX(FX_CHORUS, 0.3f, 0.5f, 0.35f); REVB(0.18f, RV_ROOM); }
static void c_discooct(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 3; OA.detune = 14; OA.width = 1; p->subLevel = 0.3f; ENV1(0.04f, 0, 0.5f, 0.75f, 0.35f);
	OUTF(OF_L12, 0.55f, 0.15f, 0, 0.1f); MFX(FX_CHORUS, 0.6f, 0.7f, 0.5f); DLYS(0.12f, S8D, DL_DIGI); REVB(0.22f, RV_HALL); }
static void c_housepiano(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.5f; OB.on = 1; OB.table = T_BASIC; OB.pos = TRI; OB.oct = -1; OB.level = 0.35f; p->subLevel = 0.2f;
	ENV1(0.001f, 0, 0.7f, 0.15f, 0.25f); ENV2(0.0f, 0, 0.3f, 0.0f, 0.2f); MOD(2, SRC_ENV2, DST_APOS, 0.3f); MFX(FX_CHORUS, 0.4f, 0.3f, 0.25f); REVB(0.2f, RV_ROOM); }
static void c_funkclav(SPatch* p) { k_clav(p); p->master = 0.6f; p->subShape = 1; p->subLevel = 0.3f; DRV(1, 0.2f); }
static void c_skank(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.4f; p->subLevel = 0.35f; ENV1(0.002f, 0, 0.16f, 0.0f, 0.08f); MFX(FX_CHORUS, 4.5f, 0.4f, 0.4f);
	DLYS(0.2f, S8D, DL_DUB); REVB(0.2f, RV_SPRING); }
static void c_gallop(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 2; OA.detune = 8; p->subLevel = 0.35f; DRV(14, 0.45f); ENV1(0.002f, 0, 0.5f, 0.6f, 0.2f);
	OUTF(OF_L12, 0.6f, 0.2f, 0.2f, 0.15f); REVB(0.15f, RV_ROOM); }
static void c_omni(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OB.on = 1; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 1; OB.level = 0.3f; ENV1(0.002f, 0, 2.0f, 0.0f, 1.5f);
	MFX(FX_CHORUS, 0.5f, 0.6f, 0.45f); REVB(0.35f, RV_HALL); }
static void c_pump(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 4; OA.detune = 16; ENV1(0.01f, 0, 0.6f, 0.8f, 0.3f); OUTF(OF_L12, 0.5f, 0.2f, 0, 0); p->pump = 0.7f; REVB(0.2f, RV_HALL); }
static void c_gospel(SPatch* p) { p->master = 0.5f; OA.table = T_HARMONIX; OA.pos = 0.6f; OB.on = 1; OB.table = T_HARMONIX; OB.pos = 0.3f; OB.oct = -1; OB.level = 0.3f; ENV1(0.01f, 0, 0.1f, 1.0f, 0.15f);
	MFX(FX_CHORUS, 5.0f, 0.5f, 0.5f); DRV(1, 0.3f); REVB(0.25f, RV_HALL); }
static void c_shoegaze(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 4; OA.detune = 30; OA.width = 1; DRV(14, 0.5f); ENV1(0.15f, 0, 1.0f, 0.9f, 1.5f);
	MFX(FX_HYPER, 0.3f, 0.7f, 0.5f); REVB(0.5f, RV_HALL); p->shimmer = 1; OUTF(OF_LP, 0.6f, 0.1f, 0, 0); p->xyMode = 5; }

// ---- SEQUENCE (arps and movement)
static void q_berlin(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; ARP(1, 3, 2, 0.45f); ENV1(0.002f, 0, 0.2f, 0.3f, 0.1f); ENV2(0.0f, 0, 0.15f, 0, 0.1f);
	OUTF(OF_L24, 0.35f, 0.5f, 0.3f, 0.35f); lane(p, 0, 4, 3, 0.25f, 0.6f, 3); DLYS(0.2f, S8D, DL_TAPE); }
static void q_acidcircle(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; ARP(3, 3, 2, 0.6f); p->voiceMode = 0; ENV1(0.002f, 0, 0.2f, 0.4f, 0.08f);
	ENV2(0.0f, 0, 0.18f, 0, 0.1f); OUTF(OF_ACID, 0.25f, 0.8f, 0.5f, 0.5f); lane(p, 0, 5, 2, 0.55f, 0.92f, 2); lane(p, 1, 4, 3, 0.15f, 0.45f, 1); DLYS(0.2f, S8D, DL_DUB); }
static void q_gate(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 5; OA.detune = 20; ENV1(0.01f, 0, 1.0f, 1.0f, 0.4f); dshape(p, 0, 3, NULL); p->dlSync[0] = LY_16;
	OUTF(OF_L12, 0.55f, 0.2f, 0, 0); REVB(0.25f, RV_HALL); DLYS(0.15f, S8D, DL_PING); p->gatePat = 4; p->gateDepth = 0.9f; }   // the real TRANCE GATE now
static void q_pingarp(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.5f; ARP(3, 1, 2, 0.4f); ENV1(0.001f, 0, 0.3f, 0.0f, 0.2f); DLYS(0.3f, S8D, DL_PING); REVB(0.2f, RV_PLATE); }
static void q_chaosarp(SPatch* p) { p->master = 0.55f; OA.table = T_DIGITAL; ARP(4, 3, 2, 0.5f); ENV1(0.001f, 0, 0.25f, 0.2f, 0.1f); MOD(2, SRC_CHAOSX, DST_APOS, 0.7f); MOD(3, SRC_CHAOSY, DST_OFCUT, 0.4f);
	OUTF(OF_L12, 0.5f, 0.45f, 0.2f, 0); DLYS(0.2f, S16, DL_DIGI); }
static void q_motionbells(SPatch* p) { p->master = 0.6f; OA.table = T_DIGITAL; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 1; OB.level = 0.3f; ARP(1, 1, 2, 0.5f); ENV1(0.001f, 0, 0.6f, 0.0f, 0.5f);
	lane(p, 0, 0, 4, 0.1f, 0.9f, 2); lane(p, 1, 8, 2, 0.2f, 0.8f, 3); MFX(FX_COMB, 0.3f, 0.5f, 0.3f); REVB(0.3f, RV_PLATE); }
static void q_octbounce(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.3f; ARP(3, 3, 3, 0.35f); ENV1(0.001f, 0, 0.2f, 0.3f, 0.08f); OUTF(OF_L24, 0.45f, 0.3f, 0.2f, 0); DLYS(0.15f, S16, DL_DIGI); }
static void q_drawnsteps(SPatch* p) { static const s8 M[8] = {0, 3, 7, 10, 12, 7, 5, 3}; p->master = 0.55f; OA.table = T_SYNC; OA.pos = 0.4f; dshape(p, 0, 2, M); p->dlSync[0] = LY_HALF;
	MOD(2, SRC_LFO3, DST_PITCH, 1.0f); ENV1(0.005f, 0, 0.3f, 0.9f, 0.2f); OUTF(OF_L24, 0.62f, 0.35f, 0.3f, 0); DLYS(0.2f, S8D, DL_PING); }
static void q_grainrun(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.level = 0.3f; GRN(0.2f, 0.75f, 0.25f, 0, 0.8f); ARP(1, 3, 2, 0.5f); ENV1(0.002f, 0, 0.3f, 0.2f, 0.2f);
	lane(p, 0, 0, 0, 0.0f, 1.0f, 2); REVB(0.25f, RV_PLATE); }
static void q_polyrhythm(SPatch* p) { p->master = 0.55f; OA.table = T_VOWELS; ARP(1, 3, 1, 0.5f); dshape(p, 0, 0, NULL); dshape(p, 1, 1, NULL); p->dlSync[0] = LY_HALF + 1; p->dlSync[1] = 8;
	MOD(2, SRC_LFO3, DST_APOS, 0.5f); MOD(3, SRC_LFO4, DST_OFCUT, 0.35f); OUTF(OF_L12, 0.5f, 0.4f, 0.2f, 0); ENV1(0.002f, 0, 0.25f, 0.3f, 0.1f); DLYS(0.2f, S8D, DL_TAPE); }

// ---- TEXTURE
static void x_tidepool(SPatch* p) { p->master = 0.75f; OA.table = T_HARMONIX; OA.level = 0.1f; p->noiseType = 3; p->noiseLevel = 0.12f; GRN(0.6f, 0.52f, 0.5f, 0, 0.9f);
	LFO(0, LS_SINE, 0, 0.08f); MOD(2, SRC_LFO1, DST_APOS, 0.8f); ENV1(1.2f, 0, 1.0f, 1.0f, 3.0f); ORBIT(0.6f, 0.05f); REVB(0.45f, RV_HALL); }
static void x_frozen(SPatch* p) { p->master = 0.75f; OA.table = T_VOWELS; OA.pos = 0.4f; OA.level = 0.0f; GRN(1.0f, 0.5f, 0.05f, 0, 1.0f); LFO(0, LS_SINE, 0, 0.04f); MOD(2, SRC_LFO1, DST_APOS, 0.15f);
	ENV1(0.4f, 0, 1.0f, 1.0f, 2.0f); REVB(0.45f, RV_HALL); }
static void x_netstatic(SPatch* p) { p->master = 0.6f; OA.table = T_DIGITAL; OA.uni = 2; p->noiseType = 2; p->noiseLevel = 0.15f; MOD(2, SRC_CHAOSX, DST_APOS, 0.8f); MOD(3, SRC_CHAOSY, DST_OFCUT, 0.5f);
	OUTF(OF_BP, 0.55f, 0.5f, 0.3f, 0); DRV(11, 0.6f); ENV1(0.3f, 0, 1.0f, 1.0f, 1.0f); DLYS(0.2f, S16, DL_LOFI); }
static void x_taperoom(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = 0.3f; OA.level = 0.4f; OB.on = 1; OB.oct = -1; OB.level = 0.3f; GRN(0.45f, 0.48f, 0.1f, -12, 0.6f);
	ENV1(0.3f, 0, 1.0f, 0.9f, 1.2f); OUTF(OF_CREAMY, 0.45f, 0.2f, 0.2f, 0); DLYS(0.3f, S4, DL_TAPE); p->dlyWow = 0.8f; p->lofiRate = 0.15f; REVB(0.3f, RV_ROOM); }
static void x_sea(SPatch* p) { p->master = 0.8f; OA.level = 0.0f; p->noiseType = 3; p->noiseLevel = 0.7f; OUTF(OF_L24, 0.35f, 0.3f, 0.1f, 0); LFO(0, LS_SINE, 0, 0.09f); MOD(2, SRC_LFO1, DST_OFCUT, 0.35f);
	MOD(3, SRC_LFO1, DST_PAN, 0.5f); ENV1(1.5f, 0, 1.0f, 1.0f, 3.0f); REVB(0.3f, RV_HALL); }
static void x_geiger(SPatch* p) { p->master = 0.7f; OA.table = T_DIGITAL; OA.level = 0.2f; p->noiseType = 2; p->noiseLevel = 0.5f; LFO(0, LS_SH, 0, 7.0f); MOD(2, SRC_LFO1, DST_OFCUT, 0.4f);
	OUTF(OF_HP, 0.5f, 0.4f, 0, 0); ENV1(0.2f, 0, 1.0f, 1.0f, 1.0f); REVB(0.2f, RV_ROOM); }
static void x_drone60(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.oct = -2; OA.uni = 3; OA.detune = 4; OB.on = 1; OB.table = T_VOWELS; OB.oct = -1; OB.level = 0.4f; LFO(0, LS_SINE, 0, 0.03f);
	MOD(2, SRC_LFO1, DST_APOS, 1.0f); MOD(3, SRC_CHAOSX, DST_BPOS, 0.6f); ENV1(2.0f, 0, 1.0f, 1.0f, 4.0f); OUTF(OF_CREAMY, 0.4f, 0.3f, 0.3f, 0); REVB(0.35f, RV_HALL); }
static void x_shimmercloud(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.level = 0.2f; GRN(0.4f, 0.55f, 0.45f, 12, 0.85f); LFO(0, LS_SINE, 0, 0.05f); MOD(2, SRC_LFO1, DST_APOS, 0.8f);
	ENV1(0.8f, 0, 1.0f, 1.0f, 2.5f); REVB(0.5f, RV_SWELL); p->shimmer = 1; }
static void x_sonarping(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = SINE; ENV1(0.002f, 0, 1.5f, 0.0f, 1.5f); DLYS(0.45f, S4D, DL_DIGI); REVB(0.45f, RV_HALL); ORBIT(0.4f, 0.1f); }
static void x_storm(SPatch* p) { p->master = 0.7f; OA.table = T_GROWL; OA.oct = -1; OA.level = 0.4f; p->noiseType = 1; p->noiseLevel = 0.4f; OUTF(OF_FLANGER, 0.3f, 0.7f, 0.2f, 0);
	MOD(2, SRC_CHAOSX, DST_OFCUT, 0.5f); MOD(3, SRC_CHAOSY, DST_APOS, 0.6f); ENV1(1.0f, 0, 1.0f, 1.0f, 3.0f); REVB(0.4f, RV_HALL); }

// ---- CHIP
static void h_gbwave(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OA.warp = W_QUANT; OA.warpAmt = 0.75f; ENV1(0.002f, 0, 0.3f, 0.7f, 0.08f); p->lofiBits = 4; p->lofiRate = 0.3f; }
static void h_nes(SPatch* p) { p->master = 0.55f; OA.level = 0.0f; CHIP(0.8f, 0, 0); p->voiceMode = 2; ENV1(0.002f, 0, 0.2f, 0.8f, 0.05f); LFO(0, LS_TRI, 0, 6.0f); MOD(2, SRC_LFO1, DST_PITCH, 0.008f); p->lofiBits = 2; }
static void h_c64(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; LFO(0, LS_TRI, 0, 0.6f); MOD(2, SRC_LFO1, DST_APOS, 0.5f); OUTF(OF_L12, 0.45f, 0.5f, 0.4f, 0.3f); ENV1(0.003f, 0, 0.3f, 0.7f, 0.15f); p->lofiBits = 1; }
static void h_arcade(SPatch* p) { p->master = 0.55f; OA.level = 0.3f; OA.table = T_PWM; CHIP(0.5f, 1, 0); ARP(1, 5, 2, 0.8f); ENV1(0.001f, 0, 0.2f, 0.5f, 0.05f); p->lofiBits = 2; }
// DRUM SYNTH: the DRUM HITS table played by a fast envelope sweeping WT POS through one drum, attack to tail, at
// the key's pitch. Drum g sits at WT POS 8g/63 .. (8g+7)/63: the patch rests at its tail, the envelope pulls it to the attack.
#define DRUMSYN(g) do { OA.table = T_DRUMS; OA.pos = (8 * (g) + 7) / 63.0f; MOD(2, SRC_ENV2, DST_APOS, -7 / 63.0f); } while (0)
static void q_tblkick(SPatch* p) { p->master = 0.7f; DRUMSYN(0); OA.oct = -1; ENV1(0.001f, 0, 0.45f, 0, 0.2f); ENV2(0, 0, 0.18f, 0, 0.1f); MOD(3, SRC_ENV2, DST_PITCH, 0.6f); p->cutoff = 0.7f; }
static void q_tblsnare(SPatch* p) { p->master = 0.6f; DRUMSYN(1); ENV1(0.001f, 0, 0.25f, 0, 0.12f); ENV2(0, 0, 0.12f, 0, 0.08f); MOD(3, SRC_ENV2, DST_PITCH, 0.2f); REVB(0.18f, RV_ROOM); }
static void q_tbltoms(SPatch* p) { p->master = 0.65f; DRUMSYN(2); ENV1(0.001f, 0, 0.5f, 0, 0.25f); ENV2(0, 0, 0.25f, 0, 0.1f); MOD(3, SRC_ENV2, DST_PITCH, 0.35f); REVB(0.2f, RV_PLATE); }
static void q_tblmetal(SPatch* p) { p->master = 0.5f; DRUMSYN(6); OB.on = 1; OB.table = T_DRUMS; OB.pos = 39 / 63.0f; OB.semi = 7; OB.level = 0.5f; ENV1(0.001f, 0, 0.6f, 0, 0.3f); ENV2(0, 0, 0.3f, 0, 0.1f); DLYS(0.25f, S8D, DL_PING); }
static void q_tblzap(SPatch* p) { p->master = 0.55f; DRUMSYN(7); ENV1(0.001f, 0, 0.3f, 0, 0.15f); ENV2(0, 0, 0.2f, 0, 0.1f); MOD(3, SRC_ENV2, DST_PITCH, 1.0f); p->lofiBits = 2; }
// CHIP ARPS: one key, a whole chord rolled by the arp at tracker speed, the way the C64 / NES / Game Boy faked chords
static void h_arpc64(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.3f; LFO(0, LS_TRI, 0, 0.4f); MOD(2, SRC_LFO1, DST_APOS, 0.35f); p->chord = 2; ARP(1, 5, 1, 0.95f); ENV1(0.001f, 0, 0.4f, 0.8f, 0.12f); p->lofiBits = 1; }
static void h_arpnes(SPatch* p) { p->master = 0.55f; OA.level = 0.0f; CHIP(0.8f, 1, 0); p->chord = 1; ARP(3, 5, 1, 0.9f); ENV1(0.001f, 0, 0.3f, 0.8f, 0.06f); p->lofiBits = 2; }
static void h_arpgb(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = TRI; OA.warp = W_QUANT; OA.warpAmt = 0.7f; p->chord = 7; ARP(1, 4, 2, 0.8f); ENV1(0.002f, 0, 0.25f, 0.7f, 0.08f); p->lofiBits = 4; p->lofiRate = 0.25f; }
static void h_arpdemo(SPatch* p) { p->master = 0.5f; OA.level = 0.4f; OA.table = T_PWM; OA.pos = 0.15f; CHIP(0.5f, 2, 0); p->chord = 6; ARP(1, 5, 2, 0.9f); ENV1(0.001f, 0, 0.3f, 0.7f, 0.2f); DLYS(0.3f, S8D, DL_TAPE); p->lofiBits = 2; }
static void h_arppower(SPatch* p) { p->master = 0.6f; OA.level = 0.3f; OA.table = T_PWM; OA.pos = 0.5f; OA.oct = -1; CHIP(0.6f, 3, -1); p->chord = 3; ARP(3, 3, 2, 0.6f); ENV1(0.001f, 0, 0.2f, 0.6f, 0.05f); p->lofiBits = 2; }
static void h_arptracker(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.25f; p->chord = 5; ARP(2, 5, 1, 0.85f); ENV1(0.001f, 0, 0.35f, 0.75f, 0.1f); p->lofiBits = 3; p->lofiRate = 0.15f; REVB(0.15f, RV_ROOM); }
static void h_powerup(SPatch* p) { p->master = 0.5f; OA.table = T_PWM; OA.pos = 0.3f; CHIP(0.4f, 1, 0); ENV2(0.0f, 0, 0.35f, 0, 0.1f); MOD(2, SRC_ENV2, DST_PITCH, -0.5f); MOD(0, SRC_OFF, DST_CUTOFF, 0);
	ENV1(0.001f, 0, 0.4f, 0.0f, 0.1f); p->lofiBits = 2; DLYS(0.2f, S16, DL_LOFI); }
static void h_laser(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SQR; ENV2(0.0f, 0, 0.3f, 0, 0.1f); MOD(2, SRC_ENV2, DST_PITCH, 1.0f); MOD(0, SRC_OFF, DST_CUTOFF, 0);
	ENV1(0.001f, 0, 0.35f, 0.0f, 0.1f); p->lofiBits = 3; DLYS(0.2f, S16, DL_LOFI); }
static void h_bass8(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OA.oct = -1; OA.warp = W_QUANT; OA.warpAmt = 0.8f; CHIP(0.3f, 3, -1); p->voiceMode = 1; ENV1(0.001f, 0, 0.2f, 0.8f, 0.04f); }
static void h_bitorgan(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.3f; DRV(11, 0.6f); ENV1(0.004f, 0, 0.1f, 1.0f, 0.05f); LFO(0, LS_SQR, 0, 7.0f); MOD(2, SRC_LFO1, DST_ALEV, 0.25f); p->lofiBits = 2; }
static void h_squarechoir(SPatch* p) { p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 8; CHIP(0.35f, 3, 1); LFO(0, LS_TRI, 0, 0.2f); MOD(2, SRC_LFO1, DST_APOS, 0.6f);
	ENV1(0.2f, 0, 1.0f, 0.9f, 0.8f); p->lofiBits = 1; REVB(0.25f, RV_GATED); }

// ---- performance leads: core control is the circle pad and touch; sensors only ever add a little on top
static void l_flute(SPatch* p) { p->master = 0.8f; OA.table = T_BASIC; OA.pos = 0.15f; p->noiseLevel = 0.08f; p->cutoff = 0.5f; LFO(0, LS_SINE, 0, 5.0f); MOD(2, SRC_LFO1, DST_PITCH, 0.006f);
	MOD(3, SRC_MIC, DST_CUTOFF, 0.2f); p->voiceMode = 2; p->glide = 0.05f; ENV1(0.06f, 0, 0.2f, 0.9f, 0.2f); REVB(0.25f, RV_PLATE); }   // breath into the mic brightens it (optional)
static void l_theremin(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = SINE; OB.on = 1; OB.table = T_BASIC; OB.pos = TRI; OB.level = 0.2f; p->voiceMode = 2; p->glide = 0.25f;
	NOSTICK; MOD(1, SRC_STICK, DST_PITCH, 0.25f); LFO(0, LS_SINE, 0, 6.0f); MOD(2, SRC_LFO1, DST_PITCH, 0.01f); MOD(3, SRC_TWIST, DST_PITCH, 0.05f); ENV1(0.08f, 0, 0.2f, 1.0f, 0.5f); REVB(0.3f, RV_HALL); }
static void l_xysculpt(SPatch* p) { p->master = 0.6f; OA.table = T_SYNC; OA.uni = 3; OA.detune = 10; NOSTICK; MOD(1, SRC_XY, DST_OFCUT, 0.6f); MOD(2, SRC_STICK, DST_AWARP, 0.7f); OA.warp = W_BENDPM;
	OUTF(OF_L24, 0.55f, 0.5f, 0.4f, 0); ENV1(0.01f, 0, 0.4f, 0.9f, 0.4f); DLYS(0.2f, S8D, DL_TAPE); }

// ---- SPACE: patches that move around you (loading one turns OUTPUT to SURROUND - headphones best)
static void s_orbitpad(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.uni = 4; OA.detune = 16; OA.width = 1; LFO(0, LS_SINE, 0, 0.09f); MOD(2, SRC_LFO1, DST_APOS, 0.5f);
	ENV1(0.5f, 0, 1.0f, 0.9f, 2.0f); ORBIT(0.9f, 0.12f); REVB(0.35f, RV_HALL); }
static void s_heli(SPatch* p) { p->master = 0.55f; OA.table = T_GROWL; OA.oct = -1; LFO(0, LS_SQR, 0, 9.0f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.5f); p->noiseLevel = 0.15f;
	ENV1(0.3f, 0, 1.0f, 1.0f, 1.0f); p->cutoff = 0.5f; ORBIT(1.0f, 1.6f); REVB(0.15f, RV_ROOM); }
static void s_circlearp(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = 0.7f; ARP(1, 3, 2, 0.4f); ENV1(0.002f, 0, 0.25f, 0.0f, 0.2f); ORBIT(1.0f, 0.5f); DLYS(0.25f, S8D, DL_PING); }
static void s_aroundyou(SPatch* p) { p->master = 0.8f; OA.table = T_HARMONIX; OA.oct = -1; OA.uni = 3; OA.detune = 6; LFO(0, LS_SINE, 0, 0.03f); MOD(2, SRC_LFO1, DST_APOS, 1.0f);
	OB.on = 1; OB.table = T_VOWELS; OB.level = 0.4f; LFO(1, LS_TRI, 0, 0.05f); MOD(3, SRC_LFO2, DST_BPOS, 1.0f); ENV1(1.0f, 0, 1.0f, 1.0f, 4.0f); ORBIT(1.0f, 0.05f); REVB(0.5f, RV_SWELL); }
static void s_bells(SPatch* p) { p->master = 0.65f; OA.table = T_DIGITAL; OA.pos = 0.6f; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 2; OB.level = 0.2f; ENV1(0.002f, 0, 2.0f, 0.0f, 1.5f); ORBIT(0.85f, 0.25f); REVB(0.35f, RV_PLATE); }
static void s_spiral(SPatch* p) { p->master = 0.6f; OA.table = T_SYNC; ENV2(0, 0, 0.2f, 0, 0.1f); MOD(2, SRC_ENV2, DST_APOS, 0.7f); ENV1(0.002f, 0, 0.5f, 0.0f, 0.4f); ORBIT(0.9f, 0.35f); DLYS(0.3f, S8, DL_PING); REVB(0.2f, RV_HALL); }
static void s_passing(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 2; OA.detune = 8; LFO(0, LS_SAW, 0, 0.2f); MOD(2, SRC_LFO1, DST_PITCH, -0.04f); MOD(3, SRC_LFO1, DST_OFCUT, 0.3f);
	OUTF(OF_L12, 0.5f, 0.2f, 0, 0); ENV1(0.3f, 0, 1.0f, 1.0f, 1.0f); ORBIT(1.0f, 0.2f); REVB(0.2f, RV_HALL); }
static void s_tilt(SPatch* p) { p->master = 0.65f; OA.table = T_DIGITAL; OA.uni = 2; OA.detune = 10; MOD(2, SRC_XY, DST_CUTOFF, 0.5f); MOD(3, SRC_STICK, DST_PAN, 0.6f); ENV1(0.2f, 0, 1.0f, 0.9f, 1.2f); ORBIT(0.8f, 0.3f); REVB(0.3f, RV_HALL); }

// ---- CHIPSET: the 2DS's own sound hardware doing the work. CHIP SQUARE is the second sound chip's PSG square
// channels, CHIP ECHO loops that chip's capture units back into it, LO-FI is the bit / rate crush, ORBIT the 3D pan.
static void c_psgstack(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OA.level = 0.35f; CHIP(0.8f, 1, 0); p->voiceMode = 2; p->glide = 0.04f;
	ENV1(0.002f, 0, 0.3f, 0.8f, 0.12f); OUTF(OF_L12, 0.55f, 0.35f, 0.3f, 0); p->csEcho = 0.3f; p->csEchoFb = 0.4f; p->csEchoT = 1; }
static void c_canyon(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.5f; OA.level = 0.3f; CHIP(0.7f, 3, 0); ENV1(0.003f, 0, 0.5f, 0.4f, 0.4f);
	p->csEcho = 0.65f; p->csEchoFb = 0.75f; p->csEchoT = 2; REVB(0.25f, RV_HALL); }
static void c_crushbell(SPatch* p) { p->master = 0.55f; OA.table = T_HARMONIX; OA.pos = 0.7f; CHIP(0.35f, 0, 1); ENV1(0.001f, 0, 0.9f, 0.0f, 0.9f); p->lofiBits = 3; p->lofiRate = 0.45f;
	p->csEcho = 0.4f; p->csEchoFb = 0.55f; p->csEchoT = 3; REVB(0.2f, RV_PLATE); }
static void c_choir8(SPatch* p) { p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 10; OA.level = 0.5f; CHIP(0.55f, 2, 0); ENV1(0.25f, 0, 1.0f, 0.9f, 0.8f);
	LFO(0, LS_TRI, 0, 0.15f); MOD(2, SRC_LFO1, DST_APOS, 0.5f); ORBIT(0.9f, 0.12f); p->lofiBits = 5; REVB(0.35f, RV_HALL); }
static void c_capturedub(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OA.level = 0.4f; CHIP(0.6f, 3, -1); ENV1(0.002f, 0, 0.25f, 0.0f, 0.2f);
	p->csEcho = 0.75f; p->csEchoFb = 0.8f; p->csEchoT = 1; DLYS(0.3f, S4D, DL_DUB); OUTF(OF_L24, 0.5f, 0.3f, 0.2f, 0.3f); }
static void c_twochips(SPatch* p) { p->master = 0.55f; OA.table = T_SYNC; OA.pos = 0.3f; OA.level = 0.45f; CHIP(0.6f, 1, -1); ARP(1, 3, 2, 0.55f); ENV1(0.001f, 0, 0.2f, 0.3f, 0.1f);
	LFO(0, LS_SINE, 0, 0.2f); MOD(2, SRC_LFO1, DST_APOS, 0.5f); p->csEcho = 0.35f; p->csEchoFb = 0.5f; p->csEchoT = 2; }
static void c_quantgrit(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.warp = W_QUANT; OA.warpAmt = 0.6f; CHIP(0.4f, 4, 0); p->lofiBits = 2; p->lofiRate = 0.25f;
	LFO(0, LS_SH, 0, 4.0f); MOD(2, SRC_LFO1, DST_AWARP, 0.4f); ENV1(0.002f, 0, 0.35f, 0.5f, 0.15f); OUTF(OF_DIRTY, 0.6f, 0.3f, 0.5f, 0); }
static void c_gborbit(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = TRI; OA.warp = W_QUANT; OA.warpAmt = 0.75f; CHIP(0.5f, 0, 1); ARP(2, 3, 2, 0.6f);
	ENV1(0.001f, 0, 0.25f, 0.4f, 0.1f); ORBIT(1.0f, 0.5f); p->csEcho = 0.3f; p->csEchoFb = 0.45f; p->csEchoT = 1; p->lofiBits = 4; }
static void c_doubleecho(SPatch* p) { p->master = 0.55f; OA.table = T_DIGITAL; OA.pos = 0.4f; CHIP(0.45f, 2, 0); ENV1(0.002f, 0, 0.4f, 0.2f, 0.3f);
	p->csEcho = 0.5f; p->csEchoFb = 0.6f; p->csEchoT = 3; DLYS(0.3f, S8D, DL_PING); REVB(0.2f, RV_ROOM); }
static void c_subchip(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; OA.level = 0.5f; CHIP(0.8f, 3, -2); p->voiceMode = 1; ENV1(0.002f, 0, 0.4f, 0.7f, 0.1f);
	OUTF(OF_ACID, 0.4f, 0.55f, 0.4f, 0.5f); ENV2(0.0f, 0, 0.25f, 0.0f, 0.1f); }

// chip quirks: FILTER SCREAM, DSP GRIT, MIX OVERDRIVE, CORRUPT noise, ECHO PITCH, CHIP OCTAVE past the output rate
static void c_screamlead(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; p->voiceMode = 2; p->glide = 0.05f; p->cutoff = 0.45f; p->res = 0.3f; p->qScream = 0.6f;
	ENV1(0.005f, 0, 0.4f, 0.9f, 0.2f); ENV2(0.0f, 0, 0.3f, 0.2f, 0.2f); MOD(2, SRC_ENV2, DST_CUTOFF, 0.3f); LFO(0, LS_SINE, 0, 5.0f); MOD(3, SRC_LFO1, DST_PITCH, 0.006f); DLYS(0.15f, S8D, DL_TAPE); }
static void c_rawsampler(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.pos = 0.8f; OA.oct = 1; p->qGrit = 2; ENV1(0.001f, 0, 0.5f, 0.3f, 0.3f); p->lofiRate = 0.2f; REVB(0.15f, RV_ROOM); }
static void c_hotorgan(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SINE; OB.on = 1; OB.table = T_BASIC; OB.pos = SQR; OB.oct = 1; OB.level = 0.45f; p->subLevel = 0.3f;
	p->qMixDrive = 0.65f; ENV1(0.003f, 0, 0.1f, 1.0f, 0.06f); MFX(FX_CHORUS, 0.7f, 0.4f, 0.4f); REVB(0.15f, RV_ROOM); }
static void c_glitchstorm(SPatch* p) { p->master = 0.55f; OA.table = T_DIGITAL; OA.level = 0.4f; p->noiseType = 5; p->noiseLevel = 0.5f; CHIP(0.35f, 1, 4); p->qScream = 0.35f; p->cutoff = 0.5f;
	LFO(0, LS_SH, 0, 7.0f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.4f); ENV1(0.002f, 0, 0.3f, 0.6f, 0.2f); DLYS(0.2f, S16, DL_LOFI); }
static void c_pitchcanyon(SPatch* p) { p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.4f; OA.level = 0.4f; CHIP(0.6f, 2, 0); ENV1(0.002f, 0, 0.3f, 0.0f, 0.25f);
	p->csEcho = 0.6f; p->csEchoFb = 0.7f; p->csEchoT = 1; p->csEchoPitch = 7; REVB(0.2f, RV_HALL); }

// ---- MOTION: patches that move by themselves, to show the engine off: MOTION lanes (bar-long automation),
// drawn LFOs, chaos, the grain engine, 3D orbit, and the second sound chip, all at once in BUSY BOY.
static void m_busyboy(SPatch* p) { static const s8 M[8] = {0, 12, 7, 3, 10, 7, 15, 5};
	p->master = 0.5f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 9; OB.on = 1; OB.table = T_SYNC; OB.oct = 1; OB.level = 0.3f; OB.warp = W_SYNC;
	ARP(3, 3, 2, 0.5f); ENV1(0.001f, 0, 0.22f, 0.35f, 0.12f); ENV2(0.0f, 0, 0.18f, 0.0f, 0.1f);
	lane(p, 0, 0, 2, 0.05f, 0.95f, 2); lane(p, 1, 4, 4, 0.35f, 0.9f, 1); lane(p, 2, 7, 3, 0.0f, 0.8f, 2); lane(p, 3, 11, 5, 0.2f, 0.8f, 3);
	dshape(p, 0, 2, M); p->dlSync[0] = LY_4; MOD(2, SRC_LFO3, DST_BLEV, 0.3f); dshape(p, 1, 3, NULL); p->dlSync[1] = LY_HALF; MOD(3, SRC_LFO4, DST_PAN, 0.6f);
	MOD2(0, SRC_CHAOSX, DST_FXP, 0.4f); MOD2(1, SRC_ENV2, DST_OFCUT, 0.3f);
	OUTF(OF_L12, 0.6f, 0.35f, 0.35f, 0); CHIP(0.3f, 1, 1); p->csEcho = 0.3f; p->csEchoFb = 0.5f; p->csEchoT = 1;
	GRN(0.25f, 0.5f, 0.4f, 12, 0.35f); ORBIT(0.6f, 0.4f); MFX(FX_PHASER, 0.3f, 0.6f, 0.35f); DLYS(0.22f, S8D, DL_PING); REVB(0.2f, RV_PLATE); p->ott = 0.35f; }
static void m_sink2(SPatch* p) {   // KITCHEN SINK with every master effect on at once, playing 4-note chords: needs the core-2 chain
	m_busyboy(p); p->master = 0.42f; p->chord = 1; p->shimmer = 1; REVB(0.3f, RV_HALL); DLYS(0.25f, S8D, DL_TAPE); p->dlyWow = 0.5f; p->pump = 0.5f; p->ott = 0.5f;
}
static void m_tidal(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.uni = 3; OA.detune = 12; OB.on = 1; OB.table = T_VOWELS; OB.level = 0.4f; ENV1(0.4f, 0, 1.0f, 0.9f, 1.4f);
	lane(p, 3, 8, 2, 0.2f, 0.6f, 2); lane(p, 0, 0, 2, 0.0f, 1.0f, 2); lane(p, 1, 1, 3, 1.0f, 0.0f, 3); lane(p, 2, 4, 5, 0.45f, 0.85f, 2); OUTF(OF_CREAMY, 0.7f, 0.2f, 0, 0); MFX(FX_DIM, 0.3f, 0.6f, 0.5f); REVB(0.35f, RV_HALL);
	p->xyMode = 1; MOD2(0, SRC_M1, DST_APOS, 0.5f); MOD2(1, SRC_M2, DST_OFCUT, 0.5f); }
static void m_stepsculpt(SPatch* p) { p->master = 0.6f; OA.table = T_GROWL; OA.pos = 0.3f; OA.warp = W_BENDPM; ENV1(0.002f, 0, 0.4f, 0.7f, 0.2f); p->voiceMode = 2; p->glide = 0.05f;
	lane(p, 0, 2, 4, 0.25f, 0.9f, 0); lane(p, 1, 6, 4, 0.0f, 0.9f, 1); lane(p, 2, 0, 3, 0.1f, 0.8f, 1); OUTF(OF_L24, 0.6f, 0.45f, 0.4f, 0); DLYS(0.2f, S8, DL_DIGI); }
static void m_breathglass(SPatch* p) { p->master = 0.65f; OA.table = T_DIGITAL; OA.pos = 0.6f; OA.uni = 3; OA.detune = 8; ENV1(0.6f, 0, 1.0f, 0.9f, 2.0f);
	GRN(0.7f, 0.5f, 0.5f, 12, 0.5f); lane(p, 0, 10, 6, 0.1f, 0.7f, 2); lane(p, 1, 11, 2, 0.1f, 0.9f, 3); lane(p, 2, 0, 3, 0.3f, 0.9f, 2); REVB(0.3f, RV_SWELL); }
static void m_chaosgarden(SPatch* p) { p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 10; OB.on = 1; OB.table = T_HARMONIX; OB.level = 0.35f; ENV1(0.15f, 0, 0.8f, 0.6f, 0.8f);
	MOD(2, SRC_CHAOSX, DST_APOS, 0.6f); MOD(3, SRC_CHAOSY, DST_PAN, 0.7f); MOD2(0, SRC_RAND, DST_BPOS, 0.5f); MOD2(1, SRC_CHAOSY, DST_OFCUT, 0.25f);
	OUTF(OF_LP, 0.6f, 0.3f, 0.1f, 0); DLYS(0.25f, S8D, DL_TAPE); REVB(0.3f, RV_HALL); }
static void m_acidmorph(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; ARP(1, 3, 1, 0.45f); p->voiceMode = 1; ENV1(0.001f, 0, 0.2f, 0.2f, 0.08f); ENV2(0.0f, 0, 0.2f, 0.0f, 0.1f);
	lane(p, 0, 4, 0, 0.2f, 0.85f, 3); lane(p, 1, 5, 3, 0.3f, 0.8f, 1); OUTF(OF_ACID, 0.4f, 0.6f, 0.5f, 0.45f); DRV(3, 0.3f); DLYS(0.2f, S8D, DL_DUB); }
static void m_vowelwalk(SPatch* p) { p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 6; ENV1(0.05f, 0, 0.6f, 0.8f, 0.4f); p->voiceMode = 2; p->glide = 0.08f;
	lane(p, 0, 0, 4, 0.0f, 1.0f, 1); lane(p, 1, 3, 2, 0.2f, 0.6f, 2); MFX(FX_FORMANT, 0.4f, 0.5f, 0.4f); DLYS(0.2f, S8, DL_TAPE); REVB(0.2f, RV_ROOM); }
static void m_liquidsync(SPatch* p) { p->master = 0.55f; OA.table = T_SYNC; OA.warp = W_SYNC; OA.uni = 2; OA.detune = 7; ENV1(0.01f, 0, 0.5f, 0.8f, 0.4f);
	lane(p, 0, 6, 2, 0.0f, 1.0f, 1); lane(p, 1, 0, 5, 0.2f, 0.8f, 2); lane(p, 2, 4, 3, 0.4f, 0.9f, 2); OUTF(OF_L12, 0.6f, 0.3f, 0.3f, 0); MFX(FX_CHORUS, 0.3f, 0.5f, 0.3f); DLYS(0.2f, S8D, DL_PING); }
static void m_drawndance(SPatch* p) { static const s8 M[8] = {0, 7, 12, 7, 3, 7, 10, 15}; p->master = 0.55f; OA.table = T_PWM; OA.pos = 0.4f; ENV1(0.002f, 0, 0.3f, 0.8f, 0.15f);
	dshape(p, 0, 2, M); p->dlSync[0] = LY_HALF; MOD(2, SRC_LFO3, DST_PITCH, 1.0f); dshape(p, 1, 3, NULL); p->dlSync[1] = LY_HALF + 1; MOD(3, SRC_LFO4, DST_AMP, 0.5f);
	lane(p, 0, 0, 2, 0.1f, 0.9f, 2); OUTF(OF_L24, 0.65f, 0.3f, 0.3f, 0); DLYS(0.25f, S8D, DL_PING); REVB(0.2f, RV_PLATE); }
static void s_grainscan(SPatch* p) {   // the GRAINSCAN oscillator: grains of OSC A's table, pitched to the key, the read point
	// walking forward (SCAN) while LFO 1 sweeps it (matrix -> GRAIN POS). OSC A itself is silent: only grains.
	p->master = 0.7f; OA.table = T_VOWELS; OA.level = 0.0f; GRN(0.3f, 0.62f, 0.15f, 0, 1.0f); ENV1(0.05f, 0, 1.0f, 0.9f, 0.9f);
	LFO(0, LS_SINE, 0, 0.15f); MOD(2, SRC_LFO1, DST_GRPOS, 0.6f); REVB(0.3f, RV_HALL); DLYS(0.18f, S8D, DL_PING); }
static void m_graintide(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.level = 0.25f; GRN(0.45f, 0.5f, 0.35f, 0, 0.9f); ENV1(0.3f, 0, 1.0f, 0.9f, 1.5f);
	lane(p, 0, 11, 2, 0.0f, 1.0f, 3); lane(p, 1, 0, 3, 0.2f, 0.9f, 2); lane(p, 2, 10, 6, 0.15f, 0.5f, 3); ORBIT(0.5f, 0.1f); REVB(0.3f, RV_HALL); }


// sensor showcase: BREATH + MIC on (HARDWARE page) for TALK BOX, CAMERA on for HAND WAVE
static void m_talkbox(SPatch* p) {   // hold a chord and talk: vowels open and close the filter, "s" sounds crack it open
	p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OB.on = 1; OB.table = T_BASIC; OB.pos = SQR; OB.semi = -12; OB.level = 0.6f;
	p->cutoff = 0.15f; p->res = 0.55f; ENV1(0.005f, 0, 1.0f, 1.0f, 0.2f);
	MOD(2, SRC_MIC, DST_CUTOFF, 0.45f); MOD(3, SRC_MICBRT, DST_CUTOFF, 0.35f); MOD2(0, SRC_MICBRT, DST_APOS, 0.3f); REVB(0.15f, RV_ROOM); }
static void m_gyrovibe(SPatch* p) {   // rock it forward and back for vibrato, roll it to sweep: rotation, not position
	p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 6; p->voiceMode = 2; p->glide = 0.05f;
	p->cutoff = 0.5f; p->res = 0.3f; ENV1(0.02f, 0, 0.6f, 0.9f, 0.4f);
	MOD(2, SRC_SPINX, DST_PITCH, 0.03f); MOD(3, SRC_SPINY, DST_CUTOFF, 0.5f); MOD2(0, SRC_TWIST, DST_APOS, 0.5f); DLYS(0.2f, S8D, DL_TAPE); REVB(0.3f, RV_HALL); }
static void m_handwave(SPatch* p) {   // wave a hand in front of the camera: movement opens the filter and pushes the table
	p->master = 0.6f; OA.table = T_HARMONIX; OA.pos = 0.2f; OA.uni = 2; OA.detune = 5;
	p->cutoff = 0.25f; p->res = 0.4f; ENV1(0.3f, 0, 1.0f, 0.9f, 1.0f);
	MOD(2, SRC_CAMMOVE, DST_CUTOFF, 0.55f); MOD(3, SRC_CAMMOVE, DST_APOS, 0.5f); MOD2(0, SRC_CAML, DST_AMP, 0.2f); REVB(0.35f, RV_HALL); }

// QUIRKS: one patch per hardware trick, in listening-test order. GLASS PLUCK is the clean reference.
static void q_glasspluck(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.pos = 0.85f; p->cutoff = 0.35f; p->res = 0.2f; p->keytrack = 0.6f;
	ENV1(0.001f, 0, 0.35f, 0.0f, 0.3f); ENV2(0.0f, 0, 0.15f, 0.0f, 0.1f); MOD(2, SRC_ENV2, DST_CUTOFF, 0.45f); MOD(3, SRC_VEL, DST_CUTOFF, 0.15f); REVB(0.2f, RV_PLATE); }
static void q_screamsolo(SPatch* p) {   // the ring is the note: cutoff tracks the key so it holds pitch across a run
	p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 2; OA.detune = 5; p->voiceMode = 2; p->glide = 0.04f;
	p->cutoff = 0.55f; p->res = 0.6f; p->keytrack = 1.0f; p->qScream = 0.75f; ENV1(0.015f, 0, 0.5f, 0.85f, 0.25f);
	ENV2(0.0f, 0, 0.12f, 0.0f, 0.1f); MOD(2, SRC_ENV2, DST_CUTOFF, 0.25f); MOD(3, SRC_M1, DST_RES, 0.4f); DLYS(0.12f, S8D, DL_TAPE); }
static void q_acidscream(SPatch* p) {   // mid-register 303 line; press RIFF on the SEQ page for a pattern
	p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; p->voiceMode = 1; ARP(1, 3, 1, 0.45f);
	p->cutoff = 0.3f; p->res = 0.5f; p->keytrack = 0.6f; p->qScream = 0.45f; ENV1(0.002f, 0, 0.2f, 0.3f, 0.08f);
	ENV2(0.0f, 0, 0.18f, 0.0f, 0.1f); MOD(2, SRC_ENV2, DST_CUTOFF, 0.5f); MOD(3, SRC_VEL, DST_RES, 0.3f); DLYS(0.2f, S8D, DL_DUB); }
static void q_gritkeys(SPatch* p) {   // narrow pulse, raw interpolation: play it high, the aliasing is the sound
	p->master = 0.6f; OA.table = T_PWM; OA.pos = 0.85f; OA.oct = 1; p->qGrit = 2; p->cutoff = 0.9f; p->res = 0.1f;
	ENV1(0.001f, 0, 0.3f, 0.2f, 0.2f); MOD(2, SRC_VEL, DST_APOS, 0.12f); REVB(0.12f, RV_ROOM); }
static void q_corruptperc(SPatch* p) {   // the decoder's garbage is the drum: hold a note, the arp plays it
	p->master = 0.6f; OA.level = 0; p->noiseType = 5; p->noiseLevel = 0.9f; p->cutoff = 1.0f; p->res = 0;
	ENV1(0.001f, 0, 0.08f, 0.0f, 0.05f); ARP(1, 3, 1, 0.25f); MOD(2, SRC_RAND, DST_AMP, 0.3f); }
static void q_subscream(SPatch* p) {   // clean CSND sub under the low end, Scream growls above it
	p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; p->subShape = 0; p->subOct = -1; p->subLevel = 0.75f;
	p->cutoff = 0.5f; p->res = 0.5f; p->keytrack = 1.0f; p->qScream = 0.6f; ENV1(0.003f, 0, 0.4f, 0.7f, 0.15f);
	ENV2(0.0f, 0, 0.2f, 0.1f, 0.1f); MOD(2, SRC_ENV2, DST_CUTOFF, 0.3f); }
static void q_drivemono(SPatch* p) {   // one voice into the mixer's ceiling; MACRO 1 pushes it harder
	p->master = 0.5f; OA.table = T_BASIC; OA.pos = SQR; OB.on = 1; OB.table = T_BASIC; OB.pos = SAW; OB.semi = 7; OB.level = 0.5f; p->voiceMode = 2; p->glide = 0.03f;
	p->qMixDrive = 0.5f; p->macro[0] = 0.3f; MOD(2, SRC_M1, DST_AMP, 0.6f); MOD(3, SRC_VEL, DST_AMP, 0.3f); ENV1(0.003f, 0, 0.3f, 0.9f, 0.1f); p->cutoff = 0.7f; }
static void q_tapedub(SPatch* p) {   // chip echo replayed slow: each repeat drops; few repeats so the tail stays put
	p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OA.level = 0.4f; CHIP(0.55f, 2, 0); ENV1(0.002f, 0, 0.2f, 0.0f, 0.15f);
	p->csEcho = 0.65f; p->csEchoFb = 0.35f; p->csEchoT = 3; p->csEchoPitch = -2; p->cutoff = 0.6f; }
static void q_vowelpad(SPatch* p) {   // no quirks: slow vowel morph, breath bends it, tilt opens it
	p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 8; OA.warp = W_BENDP; OA.warpAmt = 0.1f;
	ENV1(0.6f, 0, 1.0f, 0.9f, 1.5f); LFO(0, LS_SINE, 0, 0.08f); MOD(2, SRC_LFO1, DST_APOS, 0.6f); MOD(3, SRC_MIC, DST_AWARP, 0.3f);
	MOD2(0, SRC_TILTY, DST_CUTOFF, 0.2f); p->cutoff = 0.55f; REVB(0.4f, RV_HALL); }

typedef struct { const char* name; void (*fn)(SPatch*); } PresetDef;
#define MAXPRE 20
#define NSTYLES 17   // last style = USER
// the browser lists these (DRUMS and GROOVE stay in the code, for old saves, but are out of the menus: the drum grid does that now)
#define NVSTY 15
static const int VSTY[NVSTY] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16};
static int stySlot(int st) { for (int i = 0; i < NVSTY; i++) if (VSTY[i] == st) return i; return st == 16 ? NVSTY - 1 : NVSTY - 2; }   /* DRUMS / GROOVE (hidden): as if on RAVE, so > is USER */
static int styHidden(int st) { return st == 14 || st == 15; }
static const char* STYLE_NAMES[NSTYLES] = {"BASS", "LEAD", "PAD", "KEYS", "PLUCK", "CHORD", "SEQUENCE", "TEXTURE", "CHIP", "SPACE", "CHIPSET", "MOTION", "QUIRKS", "RAVE", "DRUMS", "GROOVE", "USER"};
#define STYLE_SPACE 9
// ---- RAVE (euro house and rave: pads, stabs, piano, organ bass, hoover)
static void r_ravestrings(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 4; OA.detune = 20; OA.width = 1; OB.on = 1; OB.table = T_BASIC; OB.pos = SAW; OB.oct = 1; OB.uni = 2; OB.detune = 12; OB.level = 0.3f;
	ENV1(0.3f, 0, 1.0f, 0.95f, 1.1f); OUTF(OF_L12, 0.58f, 0.12f, 0, 0); LFO(0, LS_SINE, 0, 0.12f); MOD(2, SRC_LFO1, DST_OFCUT, 0.08f);
	MFX(FX_CHORUS, 0.5f, 0.8f, 0.55f); REVB(0.32f, RV_HALL); p->eqHigh = 1; }
static void r_dreampad(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 5; OA.detune = 28; OA.width = 1; OB.on = 1; OB.table = T_PWM; OB.pos = 0.5f; OB.oct = -1; OB.level = 0.35f;
	ENV1(0.5f, 0, 1.0f, 1.0f, 1.8f); OUTF(OF_L24, 0.38f, 0.3f, 0.1f, 0); LFO(0, LS_TRI, 0, 0.5f); p->lfoSync[0] = LY_4; MOD(2, SRC_LFO1, DST_OFCUT, 0.3f);
	MFX(FX_HYPER, 0.3f, 0.6f, 0.4f); p->ott = 0.25f; REVB(0.4f, RV_HALL); p->shimmer = 1; DLYS(0.15f, S4D, DL_PING); }
static void r_hooverpad(SPatch* p) { p->master = 0.45f; OA.table = T_PWM; OA.pos = 0.3f; OA.uni = 4; OA.detune = 30; OA.width = 1; OB.on = 1; OB.table = T_BASIC; OB.pos = SAW; OB.oct = -1; OB.uni = 2; OB.detune = 24; OB.level = 0.55f;
	ENV1(0.25f, 0, 1.0f, 0.9f, 1.2f); ENV2(0.25f, 0, 0.5f, 0, 0.4f); MOD(2, SRC_ENV2, DST_PITCH, -0.06f); LFO(0, LS_SINE, 0, 0.3f); MOD(3, SRC_LFO1, DST_APOS, 0.25f);
	OUTF(OF_L12, 0.55f, 0.2f, 0.25f, 0); MFX(FX_CHORUS, 0.8f, 1.0f, 0.65f); REVB(0.3f, RV_HALL); }
static void r_supersaw(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 4; OA.detune = 32; OA.width = 1; OB.on = 1; OB.table = T_BASIC; OB.pos = SAW; OB.oct = 1; OB.detune = 18; OB.level = 0.25f;
	ENV1(0.02f, 0, 1.0f, 1.0f, 0.5f); OUTF(OF_L12, 0.7f, 0.15f, 0, 0); p->pump = 0.75f; p->ott = 0.35f; REVB(0.25f, RV_HALL); DLYS(0.15f, S8D, DL_PING);
	lane(p, 0, 4, 0, 0.45f, 0.9f, 3); p->xyMode = 3; }   // OUT CUT opens over 8 bars
static void r_eurochoir(SPatch* p) { p->master = 0.6f; OA.table = T_VOWELS; OA.pos = 0.15f; OA.uni = 4; OA.detune = 14; OA.width = 1; OB.on = 1; OB.table = T_VOWELS; OB.pos = 0.4f; OB.oct = 1; OB.level = 0.2f;
	ENV1(0.35f, 0, 1.0f, 0.9f, 1.4f); LFO(0, LS_SINE, 0, 0.1f); MOD(2, SRC_LFO1, DST_APOS, 0.12f); LFO(1, LS_SINE, 0, 5.0f); MOD(3, SRC_LFO2, DST_PITCH, 0.004f);
	MFX(FX_CHORUS, 0.4f, 0.7f, 0.5f); REVB(0.4f, RV_HALL); }
static void r_flangepad(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 5; OA.detune = 18; OA.width = 1; p->noiseType = 0; p->noiseLevel = 0.04f;
	ENV1(0.6f, 0, 1.0f, 1.0f, 1.6f); OUTF(OF_FLANGER, 0.5f, 0.65f, 0, 0); LFO(0, LS_TRI, 0, 0.5f); p->lfoSync[0] = LY_8; MOD(2, SRC_LFO1, DST_OFCUT, 0.4f);
	REVB(0.35f, RV_HALL); }
static void r_housepiano(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SINE; OA.warp = W_FM; OA.warpAmt = 0.45f; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 1;
	ENV2(0.0f, 0, 0.35f, 0.15f, 0.3f); MOD(2, SRC_ENV2, DST_AWARP, 0.4f); MOD(3, SRC_VEL, DST_AWARP, 0.25f); OA.uni = 2; OA.detune = 5;
	ENV1(0.001f, 0, 1.1f, 0.15f, 0.35f); p->eqHigh = 2; MFX(FX_CHORUS, 0.6f, 0.3f, 0.25f); REVB(0.2f, RV_ROOM); }
static void r_organbass(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.pos = 0.3f; OA.oct = -1; OB.on = 1; OB.table = T_BASIC; OB.pos = SINE; OB.oct = -1; OB.level = 0.6f;
	p->voiceMode = 1; ENV1(0.002f, 0, 0.28f, 0.25f, 0.08f); p->subLevel = 0.3f; OUTF(OF_L12, 0.5f, 0.15f, 0.2f, 0); p->eqLow = 1.5f; NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.4f); }
static void r_ravestab(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 3; OA.detune = 18; OA.width = 1; OB.on = 1; OB.table = T_PWM; OB.pos = 0.4f; OB.oct = 1; OB.level = 0.3f; p->chord = 1;
	ENV1(0.001f, 0, 0.3f, 0.0f, 0.2f); ENV2(0.0f, 0, 0.2f, 0, 0.1f); OUTF(OF_L12, 0.42f, 0.35f, 0.3f, 0.45f); REVB(0.3f, RV_HALL); DLYS(0.2f, S8D, DL_PING); }
static void r_deepchord(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = 0.4f; OA.uni = 2; OA.detune = 8; p->chord = 1; ENV1(0.002f, 0, 0.35f, 0.0f, 0.25f); ENV2(0.0f, 0, 0.25f, 0, 0.2f);
	OUTF(OF_L24, 0.32f, 0.35f, 0.2f, 0.35f); NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.4f); DLYS(0.35f, S8D, DL_DUB); REVB(0.2f, RV_ROOM); }
static void r_eurobass(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; OA.uni = 2; OA.detune = 10; p->voiceMode = 1; ENV1(0.002f, 0, 0.2f, 0.0f, 0.08f);
	ENV2(0.0f, 0, 0.15f, 0.0f, 0.1f); OUTF(OF_L24, 0.3f, 0.3f, 0.3f, 0.4f); p->subLevel = 0.35f; NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.4f); }
static void r_eurolead(SPatch* p) { p->master = 0.5f; OA.table = T_PWM; OA.pos = 0.25f; OA.uni = 3; OA.detune = 12; OB.on = 1; OB.table = T_BASIC; OB.pos = SAW; OB.oct = 1; OB.level = 0.35f;
	p->voiceMode = 2; p->glide = 0.03f; ENV1(0.004f, 0, 0.4f, 0.8f, 0.25f); LFO(0, LS_SINE, 0, 5.5f); MOD(2, SRC_LFO1, DST_PITCH, 0.006f);
	OUTF(OF_L12, 0.65f, 0.2f, 0.1f, 0); MFX(FX_CHORUS, 0.5f, 0.5f, 0.35f); DLYS(0.25f, S8D, DL_PING); REVB(0.25f, RV_HALL); }
static void r_hoover(SPatch* p) { p->master = 0.45f; OA.table = T_PWM; OA.pos = 0.35f; OA.uni = 6; OA.detune = 32; OA.width = 1; OB.on = 1; OB.table = T_BASIC; OB.pos = SAW; OB.oct = -1; OB.uni = 3; OB.detune = 26; OB.level = 0.6f;
	p->voiceMode = 2; p->glide = 0.12f; ENV1(0.01f, 0, 0.5f, 0.9f, 0.3f); ENV2(0.0f, 0, 0.18f, 0, 0.2f); MOD(2, SRC_ENV2, DST_PITCH, -0.12f);
	LFO(0, LS_SINE, 0, 0.8f); MOD(3, SRC_LFO1, DST_APOS, 0.3f); OUTF(OF_L12, 0.6f, 0.2f, 0.35f, 0); MFX(FX_CHORUS, 0.9f, 1.0f, 0.7f); REVB(0.25f, RV_HALL); }

// ---- added from the patch-ideas review: the controls no patch used yet (FREEZE, NOTE, SHAKE, wide bend) and
// combinations the library hadn't tried
static void x_freezeglass(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.uni = 3; OA.detune = 10; ENV1(0.8f, 0, 1.0f, 1.0f, 2.5f);
	p->freeze = 1; p->shimmer = 1; REVB(0.5f, RV_HALL); p->revSize = 0.9f; LFO(0, LS_SINE, 0, 0.05f); MOD(2, SRC_LFO1, DST_APOS, 0.6f); p->ott = 0.25f; }
static void x_jetcomb(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 3; OA.detune = 14; OA.width = 1; ENV1(0.6f, 0, 1.0f, 1.0f, 1.8f);
	OUTF(OF_FLANGER, 0.5f, 0.6f, 0, 0); LFO(0, LS_TRI, 0, 0.15f); MOD(2, SRC_LFO1, DST_OFCUT, 0.35f); MFX(FX_FLANGER, 0.07f, 0.8f, 0.5f); REVB(0.25f, RV_HALL); }
static void x_wiredrone(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 2; OA.detune = 6; ENV1(1.0f, 0, 1.0f, 1.0f, 2.5f);
	ENV2(1.5f, 0, 4.0f, 0.3f, 2.0f); OUTF(OF_COMBM, 0.4f, 0.85f, 0.2f, 0); MOD(2, SRC_ENV2, DST_OFCUT, 0.5f); NOSTICK; MOD(1, SRC_STICK, DST_OFCUT, 0.4f); REVB(0.35f, RV_HALL); }
static void k_reel(SPatch* p) { p->master = 0.65f; OA.table = T_DIGITAL; OA.pos = 0.25f; OB.on = 1; OB.table = T_BASIC; OB.pos = TRI; OB.level = 0.35f; ENV1(0.002f, 0, 1.4f, 0.2f, 0.5f);
	OUTF(OF_CREAMY, 0.5f, 0.15f, 0, 0); MOD(2, SRC_NOTE, DST_OFCUT, -0.45f); MOD(3, SRC_VEL, DST_OFCUT, 0.2f); DLYS(0.12f, S4, DL_TAPE); p->dlyWow = 0.6f; REVB(0.15f, RV_ROOM); }   // high notes darker, like an old sampler
static void k_glasstop(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SINE; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 1; OB.level = 0.3f; ENV1(0.002f, 0, 1.6f, 0.15f, 0.7f);
	OUTF(OF_L12, 0.35f, 0.2f, 0, 0); MOD(2, SRC_NOTE, DST_OFCUT, 0.45f); REVB(0.25f, RV_PLATE); }   // the opposite: glassy top, soft low end
static void q_turntable(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; p->voiceMode = 1; p->bendRange = 12; p->lofiBits = 2; p->lofiRate = 0.4f;
	OUTF(OF_L24, 0.45f, 0.15f, 0.2f, 0); ENV1(0.002f, 0, 0.5f, 0.0f, 0.1f); ENV2(0.0f, 0, 0.4f, 0, 0.2f); MOD(2, SRC_ENV2, DST_PITCH, -0.6f);
	DLYS(0.2f, S8D, DL_REV); p->qMixDrive = 0.3f; }   // a record stop on every note; the circle pad bends a full octave
static void q_screamdub(SPatch* p) {   // the screaming filter into the chip echo replayed slow: each repeat falls
	p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 2; OA.detune = 5; p->voiceMode = 2; p->glide = 0.04f;
	p->cutoff = 0.55f; p->res = 0.6f; p->keytrack = 1.0f; p->qScream = 0.7f; ENV1(0.01f, 0, 0.5f, 0.7f, 0.3f);
	p->csEcho = 0.6f; p->csEchoFb = 0.4f; p->csEchoT = 3; p->csEchoPitch = -2; }
static void m_shakegrit(SPatch* p) { p->master = 0.55f; OA.table = T_GROWL; OA.warp = W_ASYMPM; OA.warpAmt = 0.1f; p->voiceMode = 1; ENV1(0.004f, 0, 0.3f, 0.9f, 0.2f);
	MOD(2, SRC_SHAKE, DST_AWARP, 0.7f); MOD(3, SRC_TWIST, DST_OFCUT, 0.4f); OUTF(OF_DIRTY, 0.5f, 0.3f, 0.3f, 0); REVB(0.12f, RV_ROOM); }   // shake it for grit, turn it for brightness
static void m_singit(SPatch* p) { p->master = 0.6f; OA.table = T_VOWELS; OA.uni = 2; OA.detune = 8; MOD(2, SRC_MICBRT, DST_APOS, 0.8f); MFX(FX_FORMANT, 0.1f, 0.5f, 0.4f);
	MOD(3, SRC_MICBRT, DST_FXP, 0.6f); ENV1(0.02f, 0, 0.3f, 0.9f, 0.3f); REVB(0.2f, RV_PLATE); }   // the table follows your voice: "ss" bright, "oo" dark (mic on)
static void p_gate85(SPatch* p) { p->master = 0.55f; OA.table = T_VOWELS; OA.pos = 0.3f; OA.uni = 4; OA.detune = 14; OA.width = 1; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 1; OB.level = 0.25f;
	ENV1(0.01f, 0, 0.8f, 0.8f, 0.3f); REVB(0.5f, RV_GATED); p->pump = 0.4f; MFX(FX_CHORUS, 0.4f, 0.5f, 0.35f); }
static void p_spinpad(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.uni = 3; OA.detune = 12; OA.width = 1; ENV1(0.5f, 0, 1.0f, 1.0f, 1.6f);
	MOD(2, SRC_SPINX, DST_PAN, 0.8f); MOD(3, SRC_SPINY, DST_APOS, 0.5f); OUTF(OF_CREAMY, 0.6f, 0.2f, 0, 0); REVB(0.35f, RV_HALL); }   // rotate the console to swing it around
static void h_gb4(SPatch* p) { p->master = 0.6f; OA.table = T_PWM; OA.pos = 0.5f; p->lofiBits = 5; p->qGrit = 2; CHIP(0.5f, 0, -1); ENV1(0.002f, 0, 0.3f, 0.7f, 0.1f); p->voiceMode = 2; }   // both crushers at once
// ---- v12 patches: a beat, a loop setup, CHIP > FX or the XY pad mode come with the sound
// GROOVE
static void g_boombap(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = TRI; OA.oct = -1; p->subLevel = 0.45f; p->voiceMode = 1; p->glide = 0.03f;
	ENV1(0.004f, 0, 0.5f, 0.3f, 0.15f); OUTF(OF_L24, 0.32f, 0.2f, 0.25f, 0); p->eqLow = 1.5f;
	p->hasBeat = 1; p->drums = 1; p->kit = 1; p->kickPat = 6; p->snarePat = 1; p->hatPat = 5; p->swing = 0.14f; p->bpm = 90; }   // 909: BOOM BAP, BACKBEAT, SHUFFLE
static void g_trap(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SINE; OA.oct = -1; p->voiceMode = 2; p->glide = 0.08f; p->lofiBits = 2; p->lofiRate = 0.35f;
	ENV1(0.003f, 0, 1.2f, 0.6f, 0.3f); DRV(1, 0.35f); p->eqLow = 2;
	p->hasBeat = 1; p->drums = 1; p->kit = 0; p->kickPat = 3; p->snarePat = 3; p->hatPat = 4; p->bpm = 140; }   // 808 through 12 bits, gliding
static void g_euclid(SPatch* p) { p->master = 0.6f; OA.table = T_PWM; OA.pos = 0.25f; CHIP(0.45f, 1, 0); ENV1(0.002f, 0, 0.25f, 0.0f, 0.12f); DLYS(0.2f, S8D, DL_LOFI);
	p->hasBeat = 1; p->drums = 1; p->kit = 3; p->kickPat = 8; p->snarePat = 7; p->hatPat = 7; p->euLen = 12;
	p->euHits[0] = 3; p->euHits[1] = 2; p->euHits[2] = 7; p->euRot[0] = 0; p->euRot[1] = 3; p->euRot[2] = 1; p->bpm = 120; }   // 12-step euclid rows on the chip kit
static void g_beatbox(SPatch* p) { p->master = 0.65f; OA.table = T_BASIC; OA.pos = 0.2f; OB.on = 1; OB.table = T_BASIC; OB.pos = TRI; OB.level = 0.4f; ENV1(0.003f, 0, 1.0f, 0.3f, 0.4f); REVB(0.15f, RV_ROOM);
	p->hasBeat = 1; p->drums = 1; p->kit = 0; p->kickPat = 7; p->snarePat = 6; p->hatPat = 6; p->drumRec = 1; p->bpm = 95; }   // MIC TAP armed: knock the beat in
static void g_loopcomp(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.pos = 0.45f; ENV1(0.002f, 0, 0.4f, 0.2f, 0.2f); OUTF(OF_BP, 0.5f, 0.25f, 0, 0); p->eqLow = -2;
	GRN(0.4f, 0.5f, 0.2f, 0, 0.5f); p->grainSrc = 0; p->hasLoop = 1; p->loopBars = 1; }   // dry and mid-focused to sit over its own 2-bar loop; grains read the loop
static void g_tapeloop(SPatch* p) { p->master = 0.65f; OA.table = T_BASIC; OA.pos = SINE; OA.warp = W_FM; OA.warpAmt = 0.2f; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 2;
	ENV2(0.0f, 0, 0.6f, 0.1f, 0.4f); MOD(2, SRC_ENV2, DST_AWARP, 0.35f); ENV1(0.002f, 0, 1.4f, 0.3f, 0.5f); DLYS(0.15f, S8D, DL_TAPE); p->dlyWow = 0.6f; REVB(0.15f, RV_ROOM);
	p->hasLoop = 1; p->loopBars = 1; p->loopWow = 0.6f; }   // record it, and the loop wobbles like an old cassette
static void g_variecho(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OA.level = 0.3f; CHIP(0.6f, 2, 0); ENV1(0.002f, 0, 0.3f, 0.0f, 0.15f);
	p->csEcho = 0.55f; p->csEchoFb = 0.5f; p->csEchoT = 1; p->echoWow = 0.8f; }   // the chip echo's repeats bend
static void g_slowdown(SPatch* p) { p->master = 0.6f; OA.table = T_HARMONIX; OA.uni = 3; OA.detune = 12; ENV1(0.3f, 0, 1.0f, 0.9f, 1.2f); REVB(0.3f, RV_HALL);
	p->hasLoop = 1; p->loopBars = 1; p->tapeKeys = 1; }   // the loop runs while you hold notes and winds down when you let go
// CHIP
static void h_chipsweep(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OA.level = 0.2f; CHIP(0.8f, 2, 0); p->voiceMode = 2; p->glide = 0.03f;
	ENV1(0.002f, 0, 0.5f, 0.6f, 0.2f); ENV2(0.0f, 0, 0.35f, 0.1f, 0.2f); p->thru = 1; p->thruCut = 0.2f; p->thruRes = 0.7f; p->thruEnv = 0.7f; }   // the chip square with a real filter envelope
static void h_filtdrums(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SINE; OA.oct = -1; OA.level = 0.7f; p->voiceMode = 1; ENV1(0.003f, 0, 0.4f, 0.6f, 0.15f);
	LFO(0, LS_SINE, 0, 0.5f); p->lfoSync[0] = LY_1BAR; p->thru = 1; p->thruCut = 0.45f; p->thruRes = 0.5f; p->thruLfo = 0.3f; p->ott = 0.3f;
	p->hasBeat = 1; p->drums = 1; p->kit = 1; p->kickPat = 1; p->snarePat = 1; p->hatPat = 2; p->drumLevel = 0.55f; p->bpm = 124; }   // the drums open and close once a bar
static void h_ottbass(SPatch* p) { p->master = 0.65f; OA.table = T_BASIC; OA.pos = SQR; OA.oct = -1; OA.level = 0.3f; CHIP(0.8f, 3, -1); p->subLevel = 0.5f; p->voiceMode = 1;
	ENV1(0.002f, 0, 0.4f, 0.8f, 0.1f); ENV2(0.0f, 0, 0.2f, 0.0f, 0.1f); p->thru = 1; p->thruCut = 0.45f; p->thruEnv = 0.3f; p->ott = 0.6f; }   // chip square + sub through OTT
// PAD
static void p_xymorph(SPatch* p) { p->master = 0.6f; OA.table = T_GROWL; OA.warp = W_BENDPM; OA.uni = 3; OA.detune = 12; OA.width = 1; ENV1(0.4f, 0, 1.0f, 1.0f, 1.4f);
	p->xyMode = 1; MOD2(0, SRC_M1, DST_APOS, 0.8f); MOD2(1, SRC_M2, DST_AWARP, 0.8f); MOD2(2, SRC_M1, DST_FXP, 0.5f); MOD2(3, SRC_M2, DST_OFCUT, 0.5f);
	MFX(FX_FORMANT, 0.1f, 0.5f, 0.35f); OUTF(OF_CREAMY, 0.5f, 0.2f, 0, 0); REVB(0.3f, RV_HALL); }   // the XY pad plays MACRO 1 + 2: four moves at once
// ---- DRUMS: the keys and pads play a drum kit (voice MODE ... KIT); arp, sequencer and MIDI play it too
static void d_808(SPatch* p) { p->voiceMode = 3; p->master = 0.8f; }
static void d_909(SPatch* p) { p->voiceMode = 4; p->master = 0.8f; }
static void d_lofi(SPatch* p) { p->voiceMode = 5; p->master = 0.8f; }
static void d_chip(SPatch* p) { p->voiceMode = 6; p->master = 0.8f; }
static void d_perc(SPatch* p) { p->voiceMode = 7; p->master = 0.8f; }   // shake the console: it plays the shaker
static void d_wave(SPatch* p) { p->voiceMode = 8; p->master = 1.0f; OA.table = T_DIGITAL; OA.pos = 0.35f; }   // the drums are made of OSC A's wavetable: change the table or WT POS and they follow
// more WAVE KITs: the same drums cut from other tables (sweep WT POS to morph the whole kit)
static void d_wvowel(SPatch* p) { d_wave(p); OA.table = T_VOWELS; OA.pos = 0.4f; }      // talking drums: formant kick, vowel toms
static void d_wgrowl(SPatch* p) { d_wave(p); OA.table = T_GROWL; OA.pos = 0.65f; }      // FM: clangy metal hats, a gritty kick
static void d_wpluck(SPatch* p) { d_wave(p); OA.table = T_PLUCK; OA.pos = 0.2f; }       // KARPLUS: woody, string-like toms and bell
static void d_wsync(SPatch* p) { d_wave(p); OA.table = T_SYNC; OA.pos = 0.75f; }        // sync saw: buzzy, aggressive
static void d_wpwm(SPatch* p) { d_wave(p); OA.table = T_PWM; OA.pos = 0.55f; }          // pulse: hollow, chiptune-adjacent
static void d_wharm(SPatch* p) { d_wave(p); OA.table = T_HARMONIX; OA.pos = 0.15f; }    // few harmonics: soft, round, sub-heavy
// ---- added patches (with XY PAD modes set for playing)
static void b_dark808(SPatch* p) { p->master = 0.85f; OA.table = T_BASIC; OA.pos = SINE; OA.oct = -1; OA.level = 0.9f; p->subShape = 0; p->subOct = -1; p->subLevel = 0.8f;
	ENV1(0.002f, 0, 0.45f, 0.0f, 0.35f); ENV2(0.0f, 0, 0.08f, 0.0f, 0.05f); MOD(0, SRC_ENV2, DST_PITCH, 0.2f);   // starts a little over 2 semitones high, drops in
	DRV(5, 0.25f); p->voiceMode = 1; p->glide = 0.04f; NOSTICK; p->xyMode = 3; }
static void b_303scream(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; OA.level = 0.8f; p->noiseType = 0; p->noiseLevel = 0.15f;
	ENV1(0.002f, 0, 0.2f, 0.3f, 0.1f); ENV2(0.0f, 0, 0.15f, 0.0f, 0.1f); MOD(0, SRC_ENV2, DST_OFCUT, 0.6f); OUTF(OF_ACID, 0.35f, 0.75f, 0.5f, 0.4f); p->qScream = 0.55f;
	p->ott = 0.45f; p->lofiBits = 2; p->lofiRate = 0.15f; p->voiceMode = 1; p->glide = 0.06f; NOSTICK; lane(p, 0, 4, 4, 0.2f, 0.9f, 1); p->xyMode = 3; }
static void r_neonpwm(SPatch* p) { p->master = 0.65f; OA.table = T_PWM; OA.pos = 0.5f; OA.uni = 3; OA.detune = 12; OA.width = 0.6f; OB.on = 1; OB.table = T_BASIC; OB.pos = SAW; OB.oct = 1; OB.level = 0.3f;
	ENV1(0.002f, 0, 0.1f, 1.0f, 0.4f); LFO(0, LS_TRI, 0, 0.25f); MOD(0, SRC_LFO1, DST_APOS, 0.25f);   // slow pulse-width sweep
	OUTF(OF_CREAMY, 0.65f, 0.2f, 0.1f, 0); MFX(FX_DIM, 0.4f, 0.5f, 0.45f); DLYS(0.25f, S8, DL_PING); p->voiceMode = 2; p->glide = 0.05f; NOSTICK; p->xyMode = 5; }
static void p_cineswell(SPatch* p) { p->master = 0.75f; OA.table = T_HARMONIX; OA.pos = 0.3f; OA.uni = 2; OA.detune = 8; GRN(0.65f, 0.4f, 0.3f, 12, 0.7f); ENV1(0.8f, 0, 1.0f, 0.9f, 2.5f);
	LFO(0, LS_SH, 0, 0.15f); MOD(0, SRC_LFO1, DST_APOS, 0.5f); OUTF(OF_CREAMY, 0.6f, 0.15f, 0, 0); REVB(0.55f, RV_SWELL); MFX(FX_DIM, 0.3f, 0.4f, 0.4f); p->xyMode = 5; }
static void k_taperhodes(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SINE; OA.level = 0.7f; OB.on = 1; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 1; OB.level = 0.4f;
	OB.warp = W_FM; OB.warpAmt = 0.35f; ENV1(0.002f, 0, 0.5f, 0.0f, 0.6f); MOD(0, SRC_VEL, DST_BWARP, 0.4f); p->noiseType = 2; p->noiseLevel = 0.12f;
	OUTF(OF_LP, 0.55f, 0.1f, 0, 0); DLYS(0.15f, S4, DL_TAPE); p->dlyWow = 0.65f; p->dlySat = 0.4f; p->lofiBits = 2; p->lofiRate = 0.25f; NOSTICK; p->xyMode = 3; }
// ---- cinematic set: slow movement over static complexity, restraint over obvious modulation
static void s_breathpad(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.pos = 0.3f; OA.uni = 3; OA.detune = 12; OA.width = 1;
	OB.on = 1; OB.table = T_VOWELS; OB.level = 0.4f; OB.oct = 1; ENV1(1.2f, 0, 2.0f, 0.9f, 2.5f);
	lane(p, 0, 0, 2, 0.2f, 0.8f, 3); lane(p, 1, 4, 3, 0.4f, 0.75f, 3);   // WT POS and OUT CUT breathe over 8 bars, out of step with each other
	OUTF(OF_CREAMY, 0.6f, 0.15f, 0, 0); MFX(FX_DIM, 0.3f, 0.4f, 0.3f); REVB(0.45f, RV_HALL); p->xyMode = 5; }   // XY: FX MIX + COLOR = width and space
static void x_tensiondrone(SPatch* p) { p->master = 0.7f; OA.table = T_BASIC; OA.pos = SINE; OA.oct = -2; OA.level = 0.6f; p->noiseType = 1; p->noiseLevel = 0.25f;
	ENV1(1.5f, 0, 2.0f, 1.0f, 3.0f);
	LFO(0, LS_SINE, 0, 0.07f); LFO(1, LS_TRI, 0, 0.113f); MOD(2, SRC_LFO1, DST_PITCH, 0.008f); MOD(3, SRC_LFO2, DST_PITCH, 0.006f);   // two slow LFOs at unrelated rates: a drift that doesn't repeat
	lane(p, 0, 4, 5, 0.3f, 0.6f, 3); OUTF(OF_L24, 0.35f, 0.4f, 0.2f, 0); DLYS(0.2f, S4, DL_TAPE); p->dlyWow = 0.7f; p->dlySat = 0.3f; REVB(0.5f, RV_SWELL); p->xyMode = 3; }
static void u_softpluck(SPatch* p) { p->master = 0.75f; OA.table = T_BASIC; OA.pos = TRI; OA.uni = 2; OA.detune = 8; ENV1(0.005f, 0, 0.4f, 0.0f, 0.8f); ENV2(0.005f, 0, 0.2f, 0.0f, 0.5f);
	MOD(0, SRC_ENV2, DST_OFCUT, 0.4f); DRV(5, 0.15f); OUTF(OF_LP, 0.5f, 0.1f, 0, 0); REVB(0.25f, RV_PLATE); DLYS(0.15f, S8, DL_PING); p->xyMode = 3; }
// INIT PATCH starts with the macros wired (at 0 they do nothing): M1 opens the filter, M2 moves A's wavetable,
// M3 adds resonance, M4 is the FX MIX (0 = dry, the reverb comes in as it rises); XY PAD = MACRO 1+2, so the pad plays filter and wavetable from the first note
static void initWire(SPatch* p) {
	MOD2(0, SRC_M1, DST_CUTOFF, 0.5f); MOD2(1, SRC_M2, DST_APOS, 0.5f); MOD2(2, SRC_M3, DST_RES, 0.5f); MOD2(3, SRC_M4, DST_FXMIX, 1.0f); p->xyMode = 1;
	p->fx = 3; p->fxMix = 0;   // REVERB at 0: M4 brings it in (at 0 the patch is dry)
}
// ---- KARPLUS: plucked strings from the KARPLUS table. ENV2 sweeps WT POS from the bright burst (0) to the bare string
// (1): base WT POS 1, ENV2 -> A WT POS -1. Harder hits start brighter.
static void u_ksguitar(SPatch* p) { p->master = 0.75f; OA.table = T_PLUCK; OA.pos = 1.0f; ENV2(0.0f, 0, 0.9f, 0.0f, 0.4f); MOD(2, SRC_ENV2, DST_APOS, -1.0f); MOD(3, SRC_VEL, DST_APOS, -0.25f);
	ENV1(0.001f, 0, 2.2f, 0.0f, 0.35f); OUTF(OF_LP, 0.75f, 0.1f, 0, 0); REVB(0.18f, RV_ROOM); NOSTICK; p->xyMode = 7; }
static void u_ksbass(SPatch* p) { p->master = 0.8f; OA.table = T_PLUCK; OA.pos = 1.0f; OA.oct = -1; ENV2(0.0f, 0, 0.45f, 0.0f, 0.2f); MOD(2, SRC_ENV2, DST_APOS, -1.0f); MOD(3, SRC_VEL, DST_APOS, -0.2f);
	ENV1(0.001f, 0, 1.4f, 0.0f, 0.15f); p->subLevel = 0.3f; p->voiceMode = 1; NOSTICK; p->xyMode = 7; }
static void u_ksharp(SPatch* p) { p->master = 0.7f; OA.table = T_PLUCK; OA.pos = 1.0f; OA.oct = 1; ENV2(0.0f, 0, 1.6f, 0.0f, 0.8f); MOD(2, SRC_ENV2, DST_APOS, -1.0f); MOD(3, SRC_VEL, DST_APOS, -0.3f);
	ENV1(0.001f, 0, 3.5f, 0.0f, 1.2f); REVB(0.35f, RV_HALL); DLYS(0.15f, S8D, DL_PING); NOSTICK; p->xyMode = 7; }
static const PresetDef PRESETS[NSTYLES - 1][MAXPRE] = {
	{{"SUB FLOOR", b_subfloor}, {"303 ON 3RD AVE", b_303}, {"REESE CRATE", b_reese}, {"WOBBLE TIDE", b_wobble}, {"FM KNOCK", b_fmknock}, {"DIODE BUZZ", b_diode},
	 {"SP-12 DROP", b_sp12}, {"CUMBIA BAJO", b_cumbia}, {"ROLLING NOTCH", b_dnb}, {"STICK GROWL", b_tiltgrowl}, {"UPRIGHT THUMB", b_upright}, {"CHIP BASS", b_chip}, {"DARK 808", b_dark808}, {"303 SCREAM", b_303scream}},
	{{"GULF SYNC", l_gulfsync}, {"TALK BOX", l_talkbox}, {"WIDE UNISON", l_bigunison}, {"WHISTLE", l_whistle}, {"FOLD SCREAM", l_scream}, {"LADDER TAPE", l_moogtape},
	 {"CHIP HERO", l_chiphero}, {"G-FUNK SINE", l_glidesine}, {"RINGER", l_ringer}, {"ACID SCREAM", l_acidscream}, {"PAD BENDER", l_bender},
	 {"FLUTE", l_flute}, {"STICK THEREMIN", l_theremin}, {"XY SCULPT", l_xysculpt}, {"BUSY BOY", l_busyboy}},
	{{"WARM FELT", p_felt}, {"GLASS HOUR", p_glass}, {"VOWEL DUSK", p_vowel}, {"COMB WEATHER", p_combweather}, {"CLOUD BANK", p_cloudbank}, {"STRINGS 77", p_strings77},
	 {"DRAWN SWELL", p_drawnswell}, {"MOTION HAZE", p_haze}, {"SWELL BACKWARD", p_reverse}, {"DEEP SONAR", p_sonar}, {"GRAIN CHOIR", p_hallchoir}, {"GATE 85", p_gate85}, {"SPIN PAD", p_spinpad}, {"XY MORPH", p_xymorph}, {"CINE SWELL", p_cineswell},
	 {"TABLE TIDE", p_tabletide}, {"GRAIN HALO", p_grainhalo}, {"PRISM SHIFT", p_prismshift}, {"SHIMMER DRIFT", p_shimmerdrift}},
	{{"EP SATURDAY", k_epsat}, {"DRAWBAR", k_drawbar}, {"LOFI UPRIGHT", k_lofiup}, {"CLAV FUNK", k_clav}, {"WURLI BARK", k_wurli}, {"VIBES", k_vibes},
	 {"TOY KEYS", k_toy}, {"HARPSI", k_harpsi}, {"ROTARY", k_rotary}, {"CUMBIA ORGANO", k_cumbiaorg}, {"REEL PIANO", k_reel}, {"GLASS TOP", k_glasstop}, {"TAPE RHODES", k_taperhodes}},
	{{"NYLON", u_nylon}, {"MARIMBA WOOD", u_marimba}, {"KALIMBA", u_kalimba}, {"DIGI HARP", u_digiharp}, {"STAB 1999", u_stab99}, {"KOTO SPRING", u_koto},
	 {"CHIP PLINK", u_chipplink}, {"GRAIN PLUCK", u_grain}, {"STEEL PAN", u_steel}, {"RAIN ON TIN", u_raintin}, {"SOFT PLUCK", u_softpluck}, {"KS GUITAR", u_ksguitar}, {"KS BASS", u_ksbass}, {"KS HARP", u_ksharp}},
	{{"ORGAN STAB", c_organstab}, {"NEO SOUL 9", c_neosoul}, {"DUB CHORD", c_dub}, {"TECHNO STAB", c_techno}, {"STRUM GUITAR", c_strum}, {"OMNI HARP", c_omni},
	 {"PUMP CHORDS", c_pump}, {"GOSPEL ORGAN", c_gospel}, {"SHOEGAZE WALL", c_shoegaze},
	 {"STRUM 8THS", c_strum8}, {"HARP ROLL", c_harproll}, {"RAKE 16THS", c_rake},
	 {"SOUL WALK", c_soulwalk}, {"DISCO OCTAVES", c_discooct}, {"HOUSE PIANO", c_housepiano}, {"FUNK CLAV", c_funkclav}, {"SKANK ORGAN", c_skank}, {"GALLOP DRIVE", c_gallop}},
	{{"BERLIN 16", q_berlin}, {"ACID CIRCLE", q_acidcircle}, {"TRANCE GATE", q_gate}, {"PING ARP", q_pingarp}, {"CHAOS ARP", q_chaosarp}, {"MOTION BELLS", q_motionbells},
	 {"OCTAVE BOUNCE", q_octbounce}, {"DRAWN STEPS", q_drawnsteps}, {"GRAIN RUN", q_grainrun}, {"POLYRHYTHM", q_polyrhythm},
	 {"TABLE KICK", q_tblkick}, {"TABLE SNARE", q_tblsnare}, {"TABLE TOMS", q_tbltoms}, {"TABLE METAL", q_tblmetal}, {"TABLE ZAP", q_tblzap}},
	{{"TIDE POOL", x_tidepool}, {"FROZEN TABLE", x_frozen}, {"NET STATIC", x_netstatic}, {"TAPE ROOM", x_taperoom}, {"BROWN SEA", x_sea}, {"GEIGER", x_geiger},
	 {"DRONE 60", x_drone60}, {"SHIMMER CLOUD", x_shimmercloud}, {"SONAR PING", x_sonarping}, {"STORM FRONT", x_storm}, {"FREEZE GLASS", x_freezeglass}, {"JET COMB", x_jetcomb}, {"WIRE DRONE", x_wiredrone}, {"KITCHEN SINK2", m_sink2}, {"TENSION DRONE", x_tensiondrone}},
	{{"GB WAVE", h_gbwave}, {"NES PULSE", h_nes}, {"C64 PWM", h_c64}, {"ARCADE ARP", h_arcade}, {"POWER UP", h_powerup}, {"LASER", h_laser},
	 {"8-BIT BASS", h_bass8}, {"BIT ORGAN", h_bitorgan}, {"SQUARE CHOIR", h_squarechoir}, {"4-BIT GB", h_gb4}, {"CHIP SWEEP", h_chipsweep}, {"FILTER DRUMS", h_filtdrums}, {"OTT CHIP BASS", h_ottbass},
	 {"C64 ARP", h_arpc64}, {"NES ARP", h_arpnes}, {"GB ARP", h_arpgb}, {"DEMO ARP", h_arpdemo}, {"POWER ARP", h_arppower}, {"TRACKER ARP", h_arptracker}},
	{{"ORBIT PAD", s_orbitpad}, {"HELICOPTER", s_heli}, {"CIRCLE ARP", s_circlearp}, {"AROUND YOU", s_aroundyou}, {"3D BELLS", s_bells}, {"SPIRAL PLUCK", s_spiral},
	 {"PASSING CAR", s_passing}, {"STEER ORBIT", s_tilt}, {"BREATH PAD", s_breathpad}, {"GRAINSCAN", s_grainscan}},
	{{"PSG STACK", c_psgstack}, {"ECHO CANYON", c_canyon}, {"CRUSH BELL", c_crushbell}, {"8-BIT CHOIR", c_choir8}, {"CAPTURE DUB", c_capturedub},
	 {"TWO CHIPS", c_twochips}, {"QUANT GRIT", c_quantgrit}, {"GB ORBIT", c_gborbit}, {"DOUBLE ECHO", c_doubleecho}, {"SUB CHIP", c_subchip},
	 {"SCREAM LEAD", c_screamlead}, {"RAW SAMPLER", c_rawsampler}, {"HOT ORGAN", c_hotorgan}, {"GLITCH STORM", c_glitchstorm}, {"PITCH CANYON", c_pitchcanyon}},
	{{"KITCHEN SINK", m_busyboy}, {"TIDAL TABLE", m_tidal}, {"STEP SCULPT", m_stepsculpt}, {"GLASS BREATH", m_breathglass}, {"CHAOS GARDEN", m_chaosgarden},
	 {"ACID MORPH", m_acidmorph}, {"VOWEL WALK", m_vowelwalk}, {"LIQUID SYNC", m_liquidsync}, {"DRAWN DANCE", m_drawndance}, {"GRAIN TIDE", m_graintide},
	 {"TALK BOX", m_talkbox}, {"GYRO VIBE", m_gyrovibe}, {"HAND WAVE", m_handwave}, {"SHAKE GRIT", m_shakegrit}, {"SING IT", m_singit}},
	{{"GLASS PLUCK", q_glasspluck}, {"SCREAM SOLO", q_screamsolo}, {"ACID SCREAM", q_acidscream}, {"GRIT KEYS", q_gritkeys}, {"CORRUPT PERC", q_corruptperc},
	 {"SUB SCREAM", q_subscream}, {"DRIVE MONO", q_drivemono}, {"TAPE DUB", q_tapedub}, {"VOWEL PAD", q_vowelpad}, {"TURNTABLE", q_turntable}, {"SCREAM DUB", q_screamdub}},
	{{"RAVE STRINGS", r_ravestrings}, {"DREAM PAD", r_dreampad}, {"HOOVER PAD", r_hooverpad}, {"SUPERSAW PUMP", r_supersaw}, {"EURO CHOIR", r_eurochoir},
	 {"FLANGE PAD", r_flangepad}, {"HOUSE PIANO", r_housepiano}, {"ORGAN BASS", r_organbass}, {"RAVE STAB", r_ravestab}, {"DEEP CHORD", r_deepchord},
	 {"EURO BASS", r_eurobass}, {"EURO LEAD", r_eurolead}, {"HOOVER", r_hoover}, {"NEON PWM", r_neonpwm}},
	{{"808 KIT", d_808}, {"909 KIT", d_909}, {"LOFI KIT", d_lofi}, {"CHIP KIT", d_chip}, {"PERC KIT", d_perc}, {"WAVE KIT", d_wave},
	 {"VOWEL KIT", d_wvowel}, {"FM KIT", d_wgrowl}, {"PLUCK KIT", d_wpluck}, {"SYNC KIT", d_wsync}, {"PULSE KIT", d_wpwm}, {"SOFT KIT", d_wharm}},
	{{"BOOM BAP BASS", g_boombap}, {"TRAP SP-12", g_trap}, {"EUCLID CHIP", g_euclid}, {"BEATBOX TAP", g_beatbox}, {"LOOP PARTNER", g_loopcomp},
	 {"TAPE LOOP", g_tapeloop}, {"VARIECHO", g_variecho}, {"SLOWDOWN", g_slowdown}},
};
#define NUSER 8
static SPatch gUser[NUSER];
static int gStyle = 2, gPreset = 0;      // style + patch index shown in the browser
static char gPatchName[24] = "INIT";
static int gSaveSlot = 0;
static const char* USER_PATH = "sdmc:/3ds/syrup/user_presets.bin";

static void userLoad(void) {
	for (int i = 0; i < NUSER; i++) { patchInit(&gUser[i]); snprintf(gUser[i].name, sizeof(gUser[i].name), "EMPTY %d", i + 1); gUser[i].magic = 0; }
	FILE* f = safeRead(USER_PATH);
	if (!f) return;
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	if (len == (long)(sizeof(SPatch) * NUSER)) {
		SPatch tmp[NUSER];
		if (fread(tmp, sizeof(SPatch), NUSER, f) == NUSER) for (int i = 0; i < NUSER; i++) if (tmp[i].magic == PATCH_MAGIC) gUser[i] = tmp[i];
	} else if (len == (long)(PATCH_V1_SIZE * NUSER) || len == (long)(PATCH_V2_SIZE * NUSER) || len == (long)(PATCH_V3_SIZE * NUSER) || len == (long)(PATCH_V4_SIZE * NUSER) || len == (long)(PATCH_V5_SIZE * NUSER) || len == (long)(PATCH_V6_SIZE * NUSER) || len == (long)(PATCH_V7_SIZE * NUSER) || len == (long)(PATCH_V8_SIZE * NUSER) || len == (long)(PATCH_V9_SIZE * NUSER) || len == (long)(PATCH_V10_SIZE * NUSER) || len == (long)(PATCH_V11_SIZE * NUSER) || len == (long)(PATCH_V12_SIZE * NUSER) || len == (long)(PATCH_V13_SIZE * NUSER) || len == (long)(PATCH_V14_SIZE * NUSER)) {   // older save files: newer fields get defaults
		long rs = len / NUSER;
		for (int i = 0; i < NUSER; i++) {
			SPatch t; patchInit(&t);
			SPatch d; patchInit(&d);
			if (fread(&t, rs, 1, f) == 1 && (t.magic == PATCH_MAGIC1 || t.magic == PATCH_MAGIC2 || t.magic == PATCH_MAGIC3 || t.magic == PATCH_MAGIC4 || t.magic == PATCH_MAGIC5 || t.magic == PATCH_MAGIC6 || t.magic == PATCH_MAGIC7 || t.magic == PATCH_MAGIC8 || t.magic == PATCH_MAGIC9 || t.magic == PATCH_MAGIC10 || t.magic == PATCH_MAGIC11 || t.magic == PATCH_MAGIC12 || t.magic == PATCH_MAGIC13 || t.magic == PATCH_MAGIC14)) {
				memcpy((char*)&t + rs, (char*)&d + rs, sizeof(SPatch) - rs);   // fields this save didn't have: defaults
				t.magic = PATCH_MAGIC; gUser[i] = t;
			}
		}
	}
	fclose(f);
}
static int userSave(void) {
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777);
	FILE* f = safeOpen(USER_PATH);
	if (!f) return 0;
	int ok = fwrite(gUser, sizeof(SPatch), NUSER, f) == NUSER;
	return safeClose(f, USER_PATH, ok);
}

static void patchCapture(SPatch* p) {
	p->magic = PATCH_MAGIC;
	snprintf(p->name, sizeof(p->name), "%.19s", gPatchName);
	for (int o = 0; o < 2; o++) {
		Osc* s = &gOsc[o]; OscP* q = &p->osc[o];
		q->on = s->on; q->table = s->table < T_BUILTIN ? s->table : T_BASIC; q->warp = s->warp; q->oct = s->oct; q->semi = s->semi; q->uni = s->uni;
		q->pos = s->pos; q->warpAmt = s->warpAmt; q->level = s->level; q->fine = s->fine; q->detune = s->detune; q->blend = s->blend; q->width = s->width;
	}
	p->subShape = gSubShape; p->subOct = gSubOct; p->noiseType = gNoiseType; p->filtType = gFiltType;
	p->subLevel = gSubLevel; p->noiseLevel = gNoiseLevel; p->cutoff = gCutoff; p->res = gRes; p->keytrack = gKeytrack;
	p->env1 = gEnv1; p->env2 = gEnv2;
	for (int i = 0; i < 2; i++) { p->lfoShape[i] = gLfo[i].shape; p->lfoMode[i] = gLfo[i].mode; p->lfoRate[i] = gLfo[i].rate; p->lfoSync[i] = gLfo[i].sync; p->lfoTable[i] = gLfo[i].table; p->lfoPos[i] = gLfo[i].pos; }
	for (int i = 0; i < 4; i++) { p->mod[i] = gMod[i]; p->mod2[i] = gMod[i + 4]; p->macro[i] = gMacro[i]; }
	for (int i = 0; i < 8; i++) p->modCurve[i] = gModCurve[i];
	p->modFx = gModFx; p->dlySync = gDlySync; p->shimmer = gShimmer; p->freeze = gFreeze; p->pumpRate = gPumpRate; p->lofiBits = gLofiBits;
	p->modRate = gModRate; p->modDepth = gModDepth; p->modMix = gModMix; p->fxColor = gFxColor; p->eqLow = gEqLow; p->eqMid = gEqMid; p->eqHigh = gEqHigh;
	p->dlyMix = gDlyMix; p->dlyWow = gDlyWow; p->dlySat = gDlySat; p->revMix = gRevMix; p->ott = gOtt; p->pump = gPump; p->lofiRate = gLofiRate;
	p->chipLevel = gChipLevel; p->chipDuty = gChipDuty; p->chipOct = gChipOct; p->csEcho = gCsEcho; p->csEchoFb = gCsEchoFb; p->csEchoT = gCsEchoT; p->orbit = gOrbit; p->orbitRate = gOrbitRate; p->revType = gRevType; p->dlyType = gDlyType; p->grainMode = gGrains; p->grainSrc = gGrainSrc; p->grainSize = gGrainSize; p->grainScan = gGrainScan; p->grainSpray = gGrainSpray; p->grainPitch = gGrainPitch; p->grainLevel = gGrainLevel; p->ofType = gOfType; p->ofCut = gOfCut; p->ofRes = gOfRes; p->ofDrive = gOfDrive; p->ofEnv = gOfEnv;
	p->qGrit = gQGrit; p->csEchoPitch = gCsEchoPitch; p->qScream = gQScream; p->qMixDrive = gQMixDrive;
	p->hasBeat = gDrums; p->drums = gDrums; p->kit = gKit; p->kickPat = gKickPat == EU_IDX_(0) ? gKickPat : gDrumSel[0]; p->snarePat = gSnarePat == EU_IDX_(1) ? gSnarePat : gDrumSel[1]; p->hatPat = gHatPat == EU_IDX_(2) ? gHatPat : gDrumSel[2];   // the pattern last pasted (MINE once edited) p->drumRec = 0; p->drumLevel = gDrumLevel; p->swing = gSwing;
	for (int r = 0; r < 3; r++) { p->euHits[r] = gEuHits[r]; p->euRot[r] = gEuRot[r]; } p->euLen = gEuLen;
	p->hasLoop = 0;   // (the looper is gone: tape does it)
	p->thru = gThru; p->xyMode = gXyMode; p->tapeKeys = gTapeKeys; p->thruCut = gThruCut; p->thruRes = gThruRes; p->thruEnv = gThruEnv; p->thruLfo = gThruLfo; p->echoWow = gEchoWow;
	for (int i = 0; i < 8; i++) p->fxOrder[i] = i < NFXB ? gFxOrder[i] : i;
	for (int k = 0; k < 2; k++) { for (int i = 0; i < 64; i++) p->dl[k][i] = (s8)(gDl[k][i] * 127); p->dlRate[k] = gDlRate[k]; p->dlSync[k] = gDlSync[k]; }
	memcpy(p->mot, gMot, sizeof(gMot)); memcpy(p->motTgt, gMotTgt, sizeof(gMotTgt)); memcpy(p->motOn, gMotOn, sizeof(gMotOn)); p->motBars = gMotBars;
	p->voiceMode = gVoiceMode; p->chord = gChord; p->fx = gFx; p->echoT = gEchoT; p->drive = gDrive; p->bendRange = gBendRange;
	p->glide = gGlide; p->chorus = gChorus; p->revSize = gRevSize; p->revDamp = gRevDamp; p->fxMix = gFxMix; p->driveAmt = gDriveAmt; p->master = gMaster;
	p->arp = gArp; p->arpRate = gArpRate; p->arpOct = gArpOct; p->arpGate = gArpGate; p->bpm = gBPM; p->arpStrum = gArpStrum; p->arpStrumDir = gArpStrumDir; p->gatePat = gGatePat; p->gateRate = gGateRate; p->gateDepth = gGateDepth;
}

static int gForceBuild = 3;
static void patchApplyRaw(const SPatch* p);
// a held or latched chord carries on into the new patch (it used to be cut): the same notes again, on the new sound
static void midiRestrike(void);
static void patchApply(const SPatch* p) {
	int keep[8], n = 0; float v = gHeldVel;
	if (gHeldN) for (int i = 0; i < gHeldN && n < 8; i++) keep[n++] = gHeld[i];   // keys / pads / LATCH
	else if (gArpN && !gMidiHeldN) { for (int i = 0; i < gArpN && n < 8; i++) keep[n++] = gArpPool[i]; v = gArpVel; }
	gHeldN = 0; if (!gMidiHeldN) gArpN = 0;
	patchApplyRaw(p);
	if (gVoiceMode >= 3) return;
	if (n) playSet(keep, n, v);
	if (gMidiHeldN) midiRestrike();   // keys held on a MIDI controller: they sound on in the new patch, and their note-offs still end them
}
static void patchApplyRaw(const SPatch* p) {
	allOff();
	LightLock_Lock(&gMipsLock);
	for (int o = 0; o < 2; o++) {
		Osc* s = &gOsc[o]; const OscP* q = &p->osc[o];
		int newTable = q->table != s->table;
		s->on = q->on; s->table = q->table; s->warp = q->warp; s->oct = q->oct; s->semi = q->semi; s->uni = q->uni < 1 ? 1 : (q->uni > 3 ? 3 : q->uni);
		s->pos = q->pos; s->warpAmt = q->warpAmt; s->level = q->level; s->fine = q->fine; s->detune = q->detune; s->blend = q->blend; s->width = q->width;
		if (newTable || !s->tb.nFrames) tableSelect(&s->tb, s->table);
	}
	int subChg = p->subShape != gSubShape, noiseChg = p->noiseType != gNoiseType;
	gSubShape = p->subShape; gSubOct = p->subOct; gNoiseType = p->noiseType % 6; gFiltType = p->filtType;
	gSubLevel = p->subLevel; gNoiseLevel = p->noiseLevel; gCutoff = p->cutoff; gRes = p->res; gKeytrack = p->keytrack;
	gEnv1 = p->env1; gEnv2 = p->env2;
	for (int i = 0; i < 2; i++) { gLfo[i].shape = p->lfoShape[i] < NLFOSH ? p->lfoShape[i] : 0; gLfo[i].mode = p->lfoMode[i]; gLfo[i].rate = p->lfoRate[i]; gLfo[i].sync = p->lfoSync[i] < NSYNC ? p->lfoSync[i] : 0; gLfo[i].table = p->lfoTable[i] < T_BUILTIN ? p->lfoTable[i] : 0; gLfo[i].pos = p->lfoPos[i]; gLfo[i].tabKey = -1; }
	for (int i = 0; i < 4; i++) { gMod[i] = p->mod[i]; gMod[i + 4] = p->mod2[i]; gMacro[i] = p->macro[i]; }
	for (int i = 0; i < 8; i++) gModCurve[i] = p->modCurve[i] & 3;
	gModFx = p->modFx % 8; gDlySync = p->dlySync % 7; gShimmer = p->shimmer & 1; gFreeze = p->freeze & 1; gPumpRate = p->pumpRate % 4; gLofiBits = p->lofiBits % 6;
	gModRate = p->modRate; gModDepth = p->modDepth; gModMix = p->modMix; gFxColor = p->fxColor; gEqLow = p->eqLow; gEqMid = p->eqMid; gEqHigh = p->eqHigh;
	gDlyMix = p->dlyMix; gDlyWow = p->dlyWow; gDlySat = p->dlySat; gRevMix = p->revMix; gOtt = p->ott; gPump = p->pump; gLofiRate = p->lofiRate;
	chipSet(p->chipLevel); gChipDuty = p->chipDuty; gChipOct = p->chipOct; gCsEcho = p->csEcho; gCsEchoFb = p->csEchoFb; gCsEchoT = p->csEchoT; gOrbit = p->orbit; gOrbitRate = p->orbitRate; gRevType = p->revType % NREVT; gDlyType = p->dlyType % NDLYT; dlyTypeChanged();
	gQGrit = p->qGrit % 3; gCsEchoPitch = p->csEchoPitch; gQScream = p->qScream; gQMixDrive = p->qMixDrive;
	// v12. Sound settings always follow the patch; a beat or a loop setup only when the patch carries one
	gThru = p->thru & 1; gThruCut = p->thruCut; gThruRes = p->thruRes; gThruEnv = p->thruEnv; gThruLfo = p->thruLfo; gEchoWow = p->echoWow;
	fxOrderSet(p->fxOrder);
	gXyMode = p->xyMode >= 0 && p->xyMode < 8 ? p->xyMode : 0;   // NXY
	if (gXyMode == 7) for (int m = 0; m < 4; m++) if (p->mod[m].src == SRC_XY || p->mod2[m].src == SRC_XY) gXyMode = 0;   // a patch that plays the XY source keeps WT + CUTOFF
	gTapeKeys = p->tapeKeys & 1;
	if (!p->hasBeat && gBeatFromPatch) {   // leaving a patch that brought its own beat: it goes with the patch
		gBeatFromPatch = 0; gDrums = 0; if (gCsndOK) drumsChanged();
		if (gDrumRec) { gDrumRec = 0; if (gCsndOK) drumRecChanged(); }
	}
	if (p->hasBeat) {
		gBeatFromPatch = p->drums & 1;
		gDrums = p->drums & 1; gKit = p->kit >= 0 && p->kit < 6 ? p->kit : 0; gKickPat = p->kickPat % 9; gSnarePat = p->snarePat % 8; gHatPat = p->hatPat % 8;
		gDrumLevel = p->drumLevel; gSwing = p->swing;
		for (int r = 0; r < 3; r++) { gEuHits[r] = p->euHits[r] % 17; gEuRot[r] = p->euRot[r] % 16; } gEuLen = p->euLen >= 2 && p->euLen <= 16 ? p->euLen : 16;
		{ int k = gKickPat, sn = gSnarePat, h = gHatPat; drumPaste(0, k, 0); drumPaste(1, sn, 0); drumPaste(2, h, 0); }   // the patch's patterns land in the grid, editable like your own
		if (gCsndOK) drumsChanged();
		if (p->drumRec != gDrumRec) { gDrumRec = p->drumRec % 5; if (gCsndOK) drumRecChanged(); }
	}
	if (p->ofType % NOFT != gOfType) { gOfType = p->ofType % NOFT; ofReset(); } gOfCut = p->ofCut; gOfRes = p->ofRes; gOfDrive = p->ofDrive; gOfEnv = p->ofEnv;
	for (int k = 0; k < 2; k++) { for (int i = 0; i < 64; i++) gDl[k][i] = p->dl[k][i] / 127.0f; gDlRate[k] = p->dlRate[k]; gDlSync[k] = p->dlSync[k] % NSYNC; }
	memcpy(gMot, p->mot, sizeof(gMot)); for (int l = 0; l < 4; l++) { gMotTgt[l] = p->motTgt[l] % 16; gMotOn[l] = p->motOn[l] & 1; } gMotBars = p->motBars & 3; gMotRec = -1; gMotPh = 0;
	gVoiceMode = (p->voiceMode >= 0 && p->voiceMode < 10) ? p->voiceMode : 0; gChord = p->chord >= 0 && p->chord < NCHORD ? p->chord : 0; gFx = p->fx; gEchoT = p->echoT; gDrive = p->drive; gBendRange = p->bendRange;
	gGlide = p->glide; gChorus = p->chorus; gRevSize = p->revSize; gRevDamp = p->revDamp; gFxMix = p->fxMix; gDriveAmt = p->driveAmt; gMaster = p->master;
	gGatePat = p->gatePat >= 0 && p->gatePat < NGATE ? p->gatePat : 0; gGateRate = p->gateRate >= 0 && p->gateRate < 3 ? p->gateRate : 0; gGateDepth = p->gateDepth;
	gArp = p->arp; gArpRate = p->arpRate; gArpOct = p->arpOct < 1 ? 1 : p->arpOct; gArpGate = p->arpGate; gArpStrum = p->arpStrum; gArpStrumDir = p->arpStrumDir >= 0 && p->arpStrumDir < NSTRD ? p->arpStrumDir : 0;   // the tempo is yours: loading a patch never changes it
	gArpN = 0; gArpOnN = 0; gHeldN = 0;
	if (subChg) buildSub();
	LightLock_Unlock(&gMipsLock);
	// grains after the table lock is released (building the TABLE source takes that lock)
	{ int gm = p->grainMode % 3, gs = p->grainSrc % 3; gGrainSize = p->grainSize; gGrainScan = p->grainScan; gGrainSpray = p->grainSpray; gGrainPitch = p->grainPitch; gGrainLevel = p->grainLevel;
	  if (!(gm == 0 && gGrains == 1) && (gm != gGrains || gs != gGrainSrc || gm)) { gGrains = gm; gGrainSrc = gs; grainsChanged(); } }
	if (noiseChg) buildNoise();
	snprintf(gPatchName, sizeof(gPatchName), "%s", p->name);
	gForceBuild = 3;
}


static const float PGAIN[NSTYLES - 1][MAXPRE] = {   // measured: brings every factory patch to a similar loudness
	{0.49f, 0.79f, 0.91f, 0.81f, 0.59f, 0.60f, 0.49f, 0.90f, 1.42f, 1.32f, 1.28f, 0.68f, 0.97f, 1.19f, 1.00f},
	{2.57f, 1.93f, 3.31f, 0.67f, 0.86f, 0.65f, 0.68f, 0.59f, 1.03f, 0.80f, 2.27f, 0.60f, 0.47f, 6.00f, 1.00f},
	{6.00f, 1.00f, 3.07f, 3.77f, 4.25f, 3.46f, 1.76f, 6.00f, 0.84f, 3.48f, 3.53f, 1.64f, 1.35f, 2.50f, 1.31f, 2.00f, 1.35f, 2.00f, 1.30f},
	{0.59f, 0.60f, 0.68f, 0.91f, 0.72f, 0.61f, 1.77f, 2.05f, 0.75f, 0.75f, 1.11f, 0.95f, 1.80f, 1.00f, 1.00f},
	{0.79f, 1.50f, 0.64f, 1.21f, 2.44f, 0.93f, 1.07f, 2.21f, 0.59f, 1.63f, 1.58f, 1.56f, 0.62f, 1.07f, 1.00f},
	{2.33f, 1.34f, 4.24f, 2.18f, 2.20f, 1.12f, 2.30f, 0.67f, 2.08f, 1.00f, 1.00f, 1.00f, 0.68f, 1.13f, 1.40f, 0.75f, 1.50f, 0.80f},
	{1.32f, 1.05f, 4.46f, 1.10f, 3.54f, 1.82f, 2.40f, 6.00f, 1.34f, 2.41f, 0.90f, 2.80f, 0.90f, 6.00f, 2.50f},
	{6.00f, 1.51f, 3.91f, 1.19f, 3.50f, 4.82f, 6.00f, 4.32f, 0.62f, 1.36f, 1.42f, 1.93f, 1.42f, 3.55f, 2.18f},
	{0.90f, 1.24f, 1.14f, 1.50f, 1.37f, 1.28f, 0.91f, 0.63f, 1.18f, 0.76f, 0.76f, 0.70f, 0.80f, 0.52f, 0.84f, 0.93f, 0.79f, 1.15f, 0.63f},
	{1.30f, 0.81f, 1.34f, 2.99f, 0.90f, 1.37f, 3.46f, 1.10f, 6.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{0.89f, 1.73f, 0.84f, 1.09f, 1.20f, 1.20f, 1.20f, 1.00f, 1.20f, 0.89f, 1.00f, 1.00f, 0.60f, 1.20f, 1.20f},
	{2.66f, 2.19f, 0.62f, 4.29f, 2.26f, 0.85f, 2.35f, 6.00f, 1.43f, 1.13f, 1.00f, 1.00f, 1.00f, 2.00f, 1.60f},   // MOTION
	{1.73f, 3.00f, 2.80f, 2.50f, 1.60f, 0.70f, 0.69f, 2.50f, 1.86f, 0.80f, 2.00f, 1.00f, 1.00f, 1.00f, 1.00f},   // QUIRKS
	{1.39f, 2.04f, 1.36f, 1.81f, 1.69f, 1.50f, 1.17f, 1.24f, 2.90f, 3.10f, 2.20f, 1.65f, 1.26f, 1.52f, 1.00f},   // RAVE
	{1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.60f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},   // DRUMS
	{0.94f, 0.36f, 1.00f, 0.60f, 2.50f, 0.54f, 1.80f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},   // GROOVE
};
static int presetCount(int style) {
	if (style == NSTYLES - 1) return NUSER;
	int n = 0; while (n < MAXPRE && PRESETS[style][n].name) n++;
	return n;
}
static void say(const char* m);
// edit guard: after you change a patch, the first step to another patch only warns (the second one goes)
static int gEdited = 0, gGuard = 0, gStyleCur = 0, gPresetCur = 0;
static char gCallTxt[24], gCallCtx[40]; static float gCallT = 0; static const void* gCallP = NULL;   // big pop-up of the value being changed (Shrub-style); gCallP: its Param, for the bar
static void padPreset(int style, int idx);
static void presetLoad(int style, int idx) {
	gEdited = 0; gGuard = 0; gStyleCur = style; gPresetCur = idx;
	if (style == NSTYLES - 1) { if (gUser[idx].magic == PATCH_MAGIC) patchApply(&gUser[idx]); return; }
	SPatch p; patchInit(&p);
	gPackMissing = 0;
	PRESETS[style][idx].fn(&p);
	p.master *= PGAIN[style][idx];
	snprintf(p.name, sizeof(p.name), "%s", PRESETS[style][idx].name);
	patchApply(&p);
	padPreset(style, idx);   // PADS + BASS presets open the pads with their bass line
	if (style == STYLE_SPACE && gOutMode == 0) { gOutMode = 1; outputApply(); say("3D PATCH: OUTPUT SET TO SURROUND (HEADPHONES BEST)"); }
	else if (gPackMissing) say("GET WAVETABLES (HW PAGE) FOR THE FULL SOUND");
}
// AUTOSAVE: the sound you were playing (edits included) comes back next time, even after a power-off.
// last.bin = magic, style, preset, edited, the patch. Written every few seconds when it changed, and on exit.
static const char* LAST_PATH = "sdmc:/3ds/syrup/last.bin";
#define LAST_MAGIC 0x53594C31   // "SYL1"
static SPatch gLastPatch; static int gLastHdr[4];
static int lastSnap(void) {   // 1 = something changed since the last write
	static SPatch p; patchCapture(&p);
	int h[4] = {LAST_MAGIC, gStyleCur, gPresetCur, gEdited};
	if (!memcmp(&p, &gLastPatch, sizeof(p)) && !memcmp(h, gLastHdr, sizeof(h))) return 0;
	gLastPatch = p; memcpy(gLastHdr, h, sizeof(h)); return 1;
}
static void lastWrite(void) {
	FILE* f = safeOpen(LAST_PATH); if (!f) return;
	safeClose(f, LAST_PATH, fwrite(gLastHdr, sizeof(gLastHdr), 1, f) == 1 && fwrite(&gLastPatch, sizeof(gLastPatch), 1, f) == 1);
}
static int lastLoad(void) {
	FILE* f = safeRead(LAST_PATH); if (!f) return 0;
	int h[4] = {0}; static SPatch p; int ok = fread(h, sizeof(h), 1, f) == 1 && h[0] == LAST_MAGIC && fread(&p, sizeof(p), 1, f) == 1 && p.magic == PATCH_MAGIC;
	fclose(f); if (!ok) return 0;
	if (h[1] >= 0 && h[1] < NSTYLES) { gStyle = gStyleCur = h[1]; gPreset = gPresetCur = h[2] >= 0 ? h[2] : 0; }
	patchApply(&p); gEdited = h[3] ? 1 : 0; gGuard = 0;
	gLastPatch = p; memcpy(gLastHdr, h, sizeof(h));
	return 1;
}
static const char* presetName(int style, int idx) { return style == NSTYLES - 1 ? gUser[idx].name : PRESETS[style][idx].name; }
// ---- preset browser (PATCH page): < > style, ^ v patch, A saves to a USER slot
// USER lists the 8 slots plus INIT PATCH; an empty slot keeps the current sound so you can save into it.
static int gSaveArm = -1; static u64 gSaveArmT = 0;
static int askName(const char* cur, char* out, int len) {   // the system keyboard; 1 = a new name was typed
	allOff();
	SwkbdState kb; char buf[40] = "";
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 13);
	swkbdSetHintText(&kb, "NAME THIS PATCH");
	swkbdSetInitialText(&kb, cur);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_CONFIRM) return 0;
	int n = 0;
	for (const unsigned char* c = (const unsigned char*)buf; *c && n < len - 1 && n < 13; c++) {
		if (*c >= 0x80) { while ((c[1] & 0xC0) == 0x80) c++; out[n++] = '?'; continue; }   // one ? per non-ASCII character
		out[n++] = (char)toupper(*c);
	}
	out[n] = 0; return n > 0;
}
static int gBrowseDefer = 0, gBrowsePend = 0;
static void patchMark(const void* ctl);
static int gBrFocus = 1;   // preset browser: 0 = the STYLES list has up / down, 1 = the PATCHES list
static int browseStep(int ds, int dp, int fresh) {   // returns 1 when the sound changed
	(void)fresh;
	if (gEdited) patchMark(&gEdited);   // leaving an edited patch: SELECT + L brings it back
	int u = NSTYLES - 1;
	if (ds) {
		gStyle = VSTY[(stySlot(gStyle) + NVSTY + ds) % NVSTY]; gPreset = 0;
	}
	else { int n = presetCount(gStyle) + (gStyle == u); gPreset = (gPreset + n + dp) % n; }
	gSaveArm = -1;
	if (gStyle == u && gPreset == NUSER) { SPatch ip; patchInit(&ip); initWire(&ip); patchApply(&ip); gEdited = 0; gGuard = 0; say("INIT PATCH: ONE PLAIN SAW"); return 1; }
	if (gStyle == u && gUser[gPreset].magic != PATCH_MAGIC) {   // empty slot: the sound stays, but a patch's own beat and drum kit don't follow you here
		int ch = 0;
		if (gBeatFromPatch) { gBeatFromPatch = 0; gDrums = 0; if (gCsndOK) drumsChanged(); if (gDrumRec) { gDrumRec = 0; if (gCsndOK) drumRecChanged(); } ch = 1; }
		if (gVoiceMode >= 3) { allOff(); gVoiceMode = 0; ch = 1; }
		say("EMPTY SLOT: SAVE (TWICE) TO KEEP THIS SOUND HERE"); return ch;
	}
	if (gBrowseDefer) { gBrowsePend = 1; return 0; }   // scrolling held: load where the list stops, not every patch on the way
	gBrowsePend = 0; presetLoad(gStyle, gPreset); return 1;
}
// START: a quick menu over the top screen (no more quitting by accident)
static int gStartMenu = 0, gStartSel = 0;
#define NSTART 7
static const char* START_NAMES[NSTART] = {"SAVE PATCH", "BROWSER", "PANIC", "HELP", "THEME", "MIDI LEARN", "QUIT"};
static void browseSave(void) {
	int slot = (gStyle == NSTYLES - 1 && gPreset < NUSER) ? gPreset : -1;
	if (slot < 0) { for (int i = 0; i < NUSER; i++) if (gUser[i].magic != PATCH_MAGIC) { slot = i; break; } if (slot < 0) slot = gSaveSlot; }
	u64 t = osGetTime();
	if (gSaveArm != slot || t - gSaveArmT > 3000) {
		gSaveArm = slot; gSaveArmT = t; char m[52];
		snprintf(m, sizeof(m), "AGAIN: SAVE TO USER %d%s", slot + 1, gUser[slot].magic == PATCH_MAGIC ? " (REPLACES IT)" : "");
		say(m); return;
	}
	gSaveArm = -1; gSaveSlot = slot;
	char nm[24]; if (askName(gPatchName, nm, sizeof(nm))) snprintf(gPatchName, sizeof(gPatchName), "%s", nm);
	patchCapture(&gUser[slot]); userSave();
	gStyle = NSTYLES - 1; gPreset = slot; gStyleCur = gStyle; gPresetCur = slot; gEdited = 0; gGuard = 0;
	char m[40]; snprintf(m, sizeof(m), "SAVED TO USER %d", slot + 1); say(m);
}

// ---------------------------------------------------------------- parameter pages
enum { PT_F, PT_I, PT_E };
enum { D_CHIP = 1 << 26, D_GRAIN = 1 << 27, D_DLYT = 1 << 28, D_CAMV = 1 << 29, D_NETMIDI = 1 << 30 };
#define D_ADDED D_CAMV   // shares the flag; told apart by the param
#define D_OFT D_DLYT   // shares the flag; told apart by the param
#define D_DREC D_DRUMS   // shares the drums flag; told apart by the param
enum { D_A = 1, D_B = 2, D_SUB = 4, D_NOISE = 8, D_TBLA = 16, D_TBLB = 32, D_STYLE = 64, D_PRESET = 128, D_SAVE = 256, D_INIT = 512, D_REST = 1024, D_CLEAR = 2048, D_SEQ = 4096, D_SCALE = 8192, D_OUT = 16384, D_TILT = 32768, D_THEME = 65536, D_LOOP = 1 << 17, D_LOOPTAB = 1 << 18, D_MICTAB = 1 << 19, D_CAMTAB = 1 << 20, D_CAM = 1 << 21, D_LINK = 1 << 22, D_DLWT = 1 << 23, D_DRUMS = 1 << 24, D_UPDATE = 1 << 25, D_EUCLID = 1 << 26 };
typedef struct { const char* name; int type; void* p; float mn, mx, step; const char** names; const char* unit; int dirty; } Param;

#define PF(n, ptr, mn, mx, st, unit, d) {n, PT_F, ptr, mn, mx, st, NULL, unit, d}
#define PI_(n, ptr, mn, mx, d) {n, PT_I, ptr, mn, mx, 1, NULL, "", d}
#define PE(n, ptr, cnt, names, d) {n, PT_E, ptr, 0, (cnt) - 1, 1, names, "", d}
#define PEND {NULL, 0, NULL, 0, 0, 0, NULL, NULL, 0}

static const char* ONOFF[2] = {"OFF", "ON"};
static int gTableCount = T_BUILTIN;
static const char* TABLE_NAME_DUMMY[1] = {""};
static const char* PRESET_DUMMY[1] = {""};
static const char* PRESS_RIGHT[1] = {"PRESS >"};
static const char* SLOT_NAMES[NUSER] = {"USER 1", "USER 2", "USER 3", "USER 4", "USER 5", "USER 6", "USER 7", "USER 8"};

#define OSC_PAGE(o, D, DT) { \
	PE("ON", &gOsc[o].on, 2, ONOFF, D), \
	{"TABLE", PT_E, &gOsc[o].table, 0, 0, 1, TABLE_NAME_DUMMY, "", DT}, \
	PF("WT POS", &gOsc[o].pos, 0, 1, 0.01f, "%", D), \
	PE("WARP", &gOsc[o].warp, W_COUNT, WARP_NAMES, D), \
	PF("WARP AMT", &gOsc[o].warpAmt, 0, 1, 0.01f, "%", D), \
	PF("LEVEL", &gOsc[o].level, 0, 1, 0.01f, "%", 0), \
	PI_("OCTAVE", &gOsc[o].oct, -3, 3, D_A | D_B), \
	PI_("SEMI", &gOsc[o].semi, -12, 12, 0), \
	PF("FINE", &gOsc[o].fine, -100, 100, 1, "CT", 0), \
	PI_("UNISON", &gOsc[o].uni, 1, 3, 0), \
	PF("DETUNE", &gOsc[o].detune, 0, 60, 1, "CT", 0), \
	PF("BLEND", &gOsc[o].blend, 0, 1, 0.01f, "%", 0), \
	PF("WIDTH", &gOsc[o].width, 0, 1, 0.01f, "%", 0), \
	PEND }

static Param P_OSCA[] = OSC_PAGE(0, D_A, D_TBLA);
static Param P_OSCB[] = OSC_PAGE(1, D_B, D_TBLB);
static Param P_SUBN[] = {
	PE("SUB SHAPE", &gSubShape, 6, SUB_SHAPES, D_SUB), PI_("SUB OCTAVE", &gSubOct, -2, 0, 0),
	PF("SUB LEVEL", &gSubLevel, 0, 1, 0.01f, "%", 0),
	PE("NOISE TYPE", &gNoiseType, 6, NOISE_TYPES, D_NOISE), PF("NOISE LEVEL", &gNoiseLevel, 0, 1, 0.01f, "%", 0),
	PF("CHIP SQUARE", &gChipLevel, 0, 1, 0.02f, "%", D_CHIP), PE("CHIP WIDTH", &gChipDuty, 7, DUTY_NAMES, 0), PI_("CHIP OCTAVE", &gChipOct, -2, 5, 0),
	PEND};
static Param P_FILT[] = {
	PE("FILTER", &gFiltType, 5, FILT_TYPES, 0), PF("CUTOFF", &gCutoff, 0, 1, 0.01f, "HZ", 0),
	PE("FILTER 2", &gFilt2, 3, FILT2_NAMES, 0), PE("LOW CUT", &gLowCut, 6, LOWCUT_NAMES, 0),
	PF("RES", &gRes, 0, 1, 0.01f, "%", 0), PF("KEYTRACK", &gKeytrack, 0, 1, 0.05f, "%", 0),
	PF("ENV1 ATK", &gEnv1.a, 0, 4, 0.005f, "S", 0), PF("ENV1 HOLD", &gEnv1.h, 0, 2, 0.01f, "S", 0),
	PF("ENV1 DEC", &gEnv1.d, 0.01f, 4, 0.02f, "S", 0), PF("ENV1 SUS", &gEnv1.s, 0, 1, 0.01f, "%", 0),
	PF("ENV1 REL", &gEnv1.r, 0.01f, 5, 0.02f, "S", 0),
	PF("ENV2 ATK", &gEnv2.a, 0, 4, 0.005f, "S", 0), PF("ENV2 DEC", &gEnv2.d, 0.01f, 4, 0.02f, "S", 0),
	PF("ENV2 SUS", &gEnv2.s, 0, 1, 0.01f, "%", 0), PF("ENV2 REL", &gEnv2.r, 0.01f, 5, 0.02f, "S", 0),
	PEND};
static int gXyMode = 0;
#define NXY 8
static const char* XY_NAMES[NXY] = {"WT + CUTOFF", "MACRO 1+2", "MACRO 3+4", "CUTOFF + RES", "WT A + WT B", "FX MIX + COLOR", "PERF", "PAGE"};   // PAGE: two values of the page you're on
static int gStickMode = 0;
enum { SM_DATA, SM_BEND, SM_PAGE, SM_MACROS };
static const char* STICK_NAMES[4] = {"DATA", "BEND + MOD", "PAGE", "MACROS"};
static float gStickDX = 0;   // DATA: circle pad left / right, edits the selected value
static Param P_MOD[] = {
	PF("MACRO 1", &gMacro[0], 0, 1, 0.02f, "%", 0), PF("MACRO 2", &gMacro[1], 0, 1, 0.02f, "%", 0), PF("MACRO 3", &gMacro[2], 0, 1, 0.02f, "%", 0), PF("MACRO 4", &gMacro[3], 0, 1, 0.02f, "%", 0),
	PE("XY PAD", &gXyMode, NXY, XY_NAMES, D_THEME), PF("CHAOS RATE", &gChaosRate, 0, 1, 0.02f, "%", 0),
	PF("LFO3 RATE", &gDlRate[0], 0.05f, 20, 0.05f, "HZ", 0), PE("LFO3 SYNC", &gDlSync[0], NSYNC, SYNC_NAMES, 0), PF("LFO4 RATE", &gDlRate[1], 0.05f, 20, 0.05f, "HZ", 0), PE("LFO4 SYNC", &gDlSync[1], NSYNC, SYNC_NAMES, 0),
	PI_("LFO1 CC OUT", &gLfoCC[0], 0, 127, 0), PI_("LFO2 CC OUT", &gLfoCC[1], 0, 127, 0),
	PE("LFO1 SHAPE", &gLfo[0].shape, NLFOSH, LFO_SHAPES, 0), PF("LFO1 RATE", &gLfo[0].rate, 0.05f, 20, 0.05f, "HZ", 0), PE("LFO1 SYNC", &gLfo[0].sync, NSYNC, SYNC_NAMES, 0),
	PE("LFO1 MODE", &gLfo[0].mode, 2, LFO_MODES, 0), PE("LFO1 TABLE", &gLfo[0].table, T_BUILTIN, BUILTIN_NAMES, 0), PF("LFO1 WT POS", &gLfo[0].pos, 0, 1, 0.02f, "%", 0),
	PE("LFO2 SHAPE", &gLfo[1].shape, NLFOSH, LFO_SHAPES, 0), PF("LFO2 RATE", &gLfo[1].rate, 0.05f, 20, 0.05f, "HZ", 0), PE("LFO2 SYNC", &gLfo[1].sync, NSYNC, SYNC_NAMES, 0),
	PE("LFO2 MODE", &gLfo[1].mode, 2, LFO_MODES, 0), PE("LFO2 TABLE", &gLfo[1].table, T_BUILTIN, BUILTIN_NAMES, 0), PF("LFO2 WT POS", &gLfo[1].pos, 0, 1, 0.02f, "%", 0),
	PE("1 SOURCE", &gMod[0].src, SRC_COUNT, SRC_NAMES, 0), PE("1 DEST", &gMod[0].dst, DST_COUNT, DST_NAMES, 0), PF("1 AMOUNT", &gMod[0].amt, -1, 1, 0.02f, "%", 0), PE("1 CURVE", &gModCurve[0], 4, CURVE_NAMES, 0),
	PE("2 SOURCE", &gMod[1].src, SRC_COUNT, SRC_NAMES, 0), PE("2 DEST", &gMod[1].dst, DST_COUNT, DST_NAMES, 0), PF("2 AMOUNT", &gMod[1].amt, -1, 1, 0.02f, "%", 0), PE("2 CURVE", &gModCurve[1], 4, CURVE_NAMES, 0),
	PE("3 SOURCE", &gMod[2].src, SRC_COUNT, SRC_NAMES, 0), PE("3 DEST", &gMod[2].dst, DST_COUNT, DST_NAMES, 0), PF("3 AMOUNT", &gMod[2].amt, -1, 1, 0.02f, "%", 0), PE("3 CURVE", &gModCurve[2], 4, CURVE_NAMES, 0),
	PE("4 SOURCE", &gMod[3].src, SRC_COUNT, SRC_NAMES, 0), PE("4 DEST", &gMod[3].dst, DST_COUNT, DST_NAMES, 0), PF("4 AMOUNT", &gMod[3].amt, -1, 1, 0.02f, "%", 0), PE("4 CURVE", &gModCurve[3], 4, CURVE_NAMES, 0),
	PE("5 SOURCE", &gMod[4].src, SRC_COUNT, SRC_NAMES, 0), PE("5 DEST", &gMod[4].dst, DST_COUNT, DST_NAMES, 0), PF("5 AMOUNT", &gMod[4].amt, -1, 1, 0.02f, "%", 0), PE("5 CURVE", &gModCurve[4], 4, CURVE_NAMES, 0),
	PE("6 SOURCE", &gMod[5].src, SRC_COUNT, SRC_NAMES, 0), PE("6 DEST", &gMod[5].dst, DST_COUNT, DST_NAMES, 0), PF("6 AMOUNT", &gMod[5].amt, -1, 1, 0.02f, "%", 0), PE("6 CURVE", &gModCurve[5], 4, CURVE_NAMES, 0),
	PE("7 SOURCE", &gMod[6].src, SRC_COUNT, SRC_NAMES, 0), PE("7 DEST", &gMod[6].dst, DST_COUNT, DST_NAMES, 0), PF("7 AMOUNT", &gMod[6].amt, -1, 1, 0.02f, "%", 0), PE("7 CURVE", &gModCurve[6], 4, CURVE_NAMES, 0),
	PE("8 SOURCE", &gMod[7].src, SRC_COUNT, SRC_NAMES, 0), PE("8 DEST", &gMod[7].dst, DST_COUNT, DST_NAMES, 0), PF("8 AMOUNT", &gMod[7].amt, -1, 1, 0.02f, "%", 0), PE("8 CURVE", &gModCurve[7], 4, CURVE_NAMES, 0),
	PEND};
static const char* BEND_NAMES[13] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
static Param P_VOICE[] = {
	PE("MODE", &gVoiceMode, 3, VOICE_MODES, 0), PF("GLIDE", &gGlide, 0, 1, 0.01f, "S", 0), PE("MPE", &gMpe, 2, ONOFF, 0),
	PE("BEND RANGE", &gBendRange, 13, BEND_NAMES, 0), PE("CHORD", &gChord, NCHORD, CHORD_NAMES, 0),
	PE("GATE", &gGatePat, NGATE, GATE_NAMES, 0), PE("GATE RATE", &gGateRate, 3, GRATE_NAMES, 0), PF("GATE DEPTH", &gGateDepth, 0, 1, 0.05f, "%", 0),
	PF("CHORUS", &gChorus, 0, 1, 0.02f, "%", 0), PE("GPU VISUALS", &gGpuFx, 2, ONOFF, 0), PE("FX", &gFx, 4, FX_NAMES, 0), PE("ECHO TIME", &gEchoT, 3, ECHO_T_NAMES, 0),
	PF("ECHO FDBK", &gEchoFb, 0, 0.9f, 0.03f, "%", 0), PE("OUTPUT", &gOutMode, 4, OUT_NAMES, D_OUT), PE("SURROUND", &gSurDepth, 4, DEPTH_NAMES, D_OUT),
	{"TILT CENTER", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_TILT},
	PF("REV SIZE", &gRevSize, 0, 1, 0.02f, "%", 0), PF("REV DAMP", &gRevDamp, 0, 1, 0.02f, "%", 0), PF("FX MIX", &gFxMix, 0, 1, 0.02f, "%", 0),
	PE("DRIVE", &gDrive, 15, DRIVE_TYPES, D_A | D_B), PF("DRIVE AMT", &gDriveAmt, 0, 1, 0.02f, "%", D_A | D_B),
	PF("MASTER", &gMaster, 0, 1.5f, 0.02f, "%", 0),
	PEND};

static Param P_PRESET[] = {
	PE("STYLE", &gStyle, NSTYLES, STYLE_NAMES, D_STYLE),
	{"PATCH", PT_E, &gPreset, 0, 0, 1, PRESET_DUMMY, "", D_PRESET},
	PE("SAVE SLOT", &gSaveSlot, NUSER, SLOT_NAMES, 0),
	{"SAVE NOW", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_SAVE},
	{"INIT PATCH", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_INIT},
	PEND};
static Param P_PLAY[] = {
	// most-reached first: tempo and the transport, then the arp, the key, then the details
	PI_("BPM", &gBPM, 40, 240, 0), PF("SWING", &gSwing, 0, 0.5f, 0.02f, "%", 0), PE("ARP", &gArp, NARPM, ARP_NAMES, D_SEQ),
	PE("ARP RATE", &gArpRate, 6, RATE_NAMES, 0), PE("SEQ RATE", &gSeqRate, 3, SEQRATE_NAMES, 0), PE("SEQ KEYS", &gSeqKeys, 2, SEQKEYS_NAMES, 0), PF("PAD MORPH", &gPadMorph, 0, 1, 0.05f, "S", 0), PE("ROLL SCALE", &gRollScale, 3, RSC_NAMES, 0), PE("STRUM NOTES", &gStrumScale, 2, STRN_NAMES, 0), PE("ROOT", &gRoot, 12, NOTE_NAMES, D_SCALE), PE("SCALE", &gScale, NSCALES, SCALE_NAMES, D_SCALE),
	PF("SEQ GATE", &gSeqGate, 0.1f, 1, 0.05f, "%", 0), PI_("ARP OCTAVES", &gArpOct, 1, 4, 0), PF("ARP GATE", &gArpGate, 0.1f, 1, 0.05f, "%", 0), PF("ARP STRUM", &gArpStrum, 0, 150, 5, "MS", 0),
	PE("STRUM DIR", &gArpStrumDir, NSTRD, STRUMDIR_NAMES, 0), PE("REC QUANT", &gRecQ, 4, RQ_NAMES, 0), PE("PREROLL", &gPreroll, 3, PRE_NAMES, 0), PE("CLICK", &gClick, 4, CLICK_NAMES, 0), PF("CLICK LEVEL", &gClickLevel, 0, 1, 0.05f, "%", 0), PI_("TAPE ALIGN", &gTapeAlign, -50, 50, 0),
	PE("SHAKE QUANT", &gShakeQ, 3, SHQ_NAMES, 0), PE("TAP REC", &gTapRec, 3, TAPR_NAMES, 0),
	PEND};
static Param P_VIS[] = {
	PE("VIS MODE", &gVisMode, NVISM, VISM_NAMES, 0), PE("VIS OSC", &gVisOsc, 2, AB_NAMES, 0), PE("OVERLAY", &gVisOverlay, 2, ONOFF, 0),
	PE("TRAILS", &gVisTrails, 2, ONOFF, 0), PE("PARTICLES", &gVisParts, 2, ONOFF, 0),
	PE("THEME", &gTheme, NTHEMES, THEME_NAMES, D_THEME), PE("SCANLINES", &gScan, 2, ONOFF, D_THEME), PE("BRIGHTNESS", &gBright, 6, BRIGHT_NAMES, D_THEME),
	PEND};
static const char* LINK_NAMES_P[6] = {"OFF", "WIFI HOST", "WIFI JOIN", "IR SEND", "IR RECEIVE", "IR MIDI IN"};
static int gCamOn, gLink, gBeatLight;
static float gNetLat = 20;   // ms: FM-1 LINK's timing buffer (steady delay instead of wobble)
static float gThruCut, gThruRes, gEchoWow; static int gThru;   // defined with the looper / CHIP > FX
static Param P_HW[] = {
	PE("GRAINS", &gGrains, 3, GRAIN_MODES, D_GRAIN), PE("GRAIN SOURCE", &gGrainSrc, 3, GRAIN_SRCS, D_GRAIN), PF("GRAIN SIZE", &gGrainSize, 0, 1, 0.02f, "%", 0), PF("GRAIN SCAN", &gGrainScan, 0, 1, 0.02f, "%", 0),
	PF("GRAIN SPRAY", &gGrainSpray, 0, 1, 0.02f, "%", 0), PF("GRAIN PITCH", &gGrainPitch, -24, 24, 1, "ST", 0), PF("GRAIN LEVEL", &gGrainLevel, 0, 1, 0.05f, "%", 0),
	{"TAPE > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_LOOPTAB},
	{"KIT > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_DLWT},
	{"MIC > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_MICTAB},
	PE("CAMERA", &gCamOn, 2, ONOFF, D_CAM),
	{"CAM > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_CAMTAB},
	PE("ADD EDITOR", &gAddEdit, 2, ONOFF, D_ADDED), {"RULE > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_DLWT},
	PE("CAM WAVE", &gCamVid, 3, VID_NAMES, D_CAMV), PF("WT SCAN", &gWtScan, 0, 4, 0.05f, "X", 0),
	PE("WI-FI MIDI", &gMidiNet, 2, ONOFF, D_NETMIDI), {"WEB UPLOAD", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_DLWT}, PF("NET BUFFER", &gNetLat, 0, 60, 1, "MS", 0),
	{"UPDATE SYRUP", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_UPDATE},
	{"GET WAVETABLES", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_DLWT},
	{"NET > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_DLWT},
	PE("LINK", &gLink, 6, LINK_NAMES_P, D_LINK), PE("BEAT LIGHT", &gBeatLight, 2, ONOFF, 0),
	PEND};
static const char* ONOFF2[2] = {"OFF", "ON"};
static Param P_MASTER[] = {
	PE("OUT FILTER", &gOfType, NOFT, OFT_NAMES, D_OFT), PF("OUT CUTOFF", &gOfCut, 0, 1, 0.01f, "%", 0), PF("OUT RES", &gOfRes, 0, 1, 0.02f, "%", 0),
	PF("OUT DRIVE", &gOfDrive, 0, 1, 0.02f, "%", 0), PF("OUT ENV2", &gOfEnv, -1, 1, 0.02f, "%", 0),
	PE("MASTER FX", &gMasterFX, 2, ONOFF2, 0),
	PE("MOD FX", &gModFx, 8, MODFX_NAMES, 0), PF("MOD RATE", &gModRate, 0.02f, 4, 0.02f, "HZ", 0), PF("MOD DEPTH", &gModDepth, 0, 1, 0.02f, "%", 0),
	PF("MOD MIX", &gModMix, 0, 1, 0.02f, "%", 0), PF("FX COLOR", &gFxColor, 0, 1, 0.02f, "%", 0),
	PF("EQ LOW", &gEqLow, -12, 12, 0.5f, "DB", 0), PF("EQ MID", &gEqMid, -12, 12, 0.5f, "DB", 0), PF("EQ HIGH", &gEqHigh, -12, 12, 0.5f, "DB", 0),
	PE("DELAY TYPE", &gDlyType, NDLYT, DLYT_NAMES, D_DLYT), PF("DELAY MIX", &gDlyMix, 0, 1, 0.02f, "%", 0), PE("DELAY SYNC", &gDlySync, 7, DSYNC_NAMES, 0), PF("TAPE WOW", &gDlyWow, 0, 1, 0.02f, "%", 0), PF("TAPE SAT", &gDlySat, 0, 1, 0.02f, "%", 0),
	PF("REVERB MIX", &gRevMix, 0, 1, 0.02f, "%", 0), PE("REVERB TYPE", &gRevType, NREVT, REVT_NAMES, 0), PE("SHIMMER", &gShimmer, 2, ONOFF2, 0), PE("FREEZE", &gFreeze, 2, ONOFF2, 0),
	PF("OTT", &gOtt, 0, 1, 0.02f, "%", 0), PF("PUMP", &gPump, 0, 1, 0.02f, "%", 0), PE("PUMP RATE", &gPumpRate, 4, PUMP_NAMES, 0),
	PE("LOFI BITS", &gLofiBits, 6, BITS_NAMES, 0), PF("LOFI RATE", &gLofiRate, 0, 1, 0.02f, "%", 0),
	PF("3D ORBIT", &gOrbit, 0, 1, 0.02f, "%", 0), PF("ORBIT RATE", &gOrbitRate, 0, 2, 0.02f, "HZ", 0),
	PE("CIRCLE PAD", &gStickMode, 4, STICK_NAMES, 0), PE("NEW 3DS CONTROLS", &gN3, 2, ONOFF, 0), PE("XY GLIDE", &gXyGlide, 4, XYG_NAMES, 0), PE("SPEAKER EQ", &gSpkEq, 3, SPK_NAMES, 0), PE("FX ON CORE 2", &gFxCore2, 2, ONOFF2, 0), PE("DSP GRIT", &gQGrit, 3, GRIT_NAMES, 0), PF("FILTER SCREAM", &gQScream, 0, 1, 0.02f, "%", 0), PF("MIX OVERDRIVE", &gQMixDrive, 0, 1, 0.02f, "%", 0),
	PEND};
static Param P_DRUMS[] = {
	// (patterns, BEAT, MIC TAP, tune, chance and euclid live on the DRUMS grid now: its buttons, hold a step, hold a name)
	PE("DRUMS", &gDrums, 2, ONOFF2, D_DRUMS), PE("KIT", &gKit, NKIT, KIT_NAMES, D_DRUMS), PE("KIT FOLDER", &gKitDir, 1, gKitDirP, 0), {"MIC > PAD", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_DLWT}, PF("DRUM LEVEL", &gDrumLevel, 0, 1, 0.05f, "%", 0), PF("ACCENT", &gDrAccAmt, 0, 1, 0.05f, "%", 0),
	PI_("BPM", &gBPM, 40, 240, 0), PF("SWING", &gSwing, 0, 0.5f, 0.02f, "%", 0), PE("MUTE", &gDrumMute, 8, DMUTE_NAMES, 0), PE("DRUM POLY", &gDrumPoly, 2, ONOFF, 0),
	PF("CHIP ECHO", &gCsEcho, 0, 1, 0.02f, "%", 0), PE("ECHO TIME", &gCsEchoT, 4, CSECHO_T_NAMES, 0), PF("ECHO REPEATS", &gCsEchoFb, 0, 0.85f, 0.03f, "%", 0), PI_("ECHO PITCH", &gCsEchoPitch, -12, 12, 0), PF("ECHO WOW", &gEchoWow, 0, 1, 0.05f, "%", 0),
	PE("CHIP > FX", &gThru, 2, ONOFF2, 0), PF("CHIP CUTOFF", &gThruCut, 0, 1, 0.02f, "%", 0), PF("CHIP RES", &gThruRes, 0, 1, 0.02f, "%", 0),
	PEND};
#define NPAGES 12
static Param* PAGES[NPAGES] = {P_PRESET, P_OSCA, P_OSCB, P_SUBN, P_FILT, P_MOD, P_VOICE, P_PLAY, P_VIS, P_HW, P_MASTER, P_DRUMS};
static const int NAV[NPAGES] = {0, 1, 2, 3, 4, 5, 6, 10, 7, 11, 8, 9};   // X / Y order: tabs left to right
static int navStep(int page, int dir) { int i = 0; while (NAV[i] != page) i++; return NAV[(i + dir + NPAGES) % NPAGES]; }
// L / R: PATCH, OSC A, FILT+ENV, LFO+MOD, VOICE+FX, MASTER FX, SEQ. OSC B, SUB+NOISE, DRUMS, VIS and HW are a chip
// tap or X / Y away. From one of those, L / R go to the main page next to it.
#define NCORE 7
static const int CORE[NCORE] = {0, 1, 4, 5, 6, 10, 7};
static int coreStep(int page, int dir) {
	for (int i = 0; i < NCORE; i++) if (CORE[i] == page) return CORE[(i + dir + NCORE) % NCORE];
	int p = page; for (int k = 0; k < NPAGES; k++) { p = navStep(p, dir); for (int i = 0; i < NCORE; i++) if (CORE[i] == p) return p; }
	return 0;
}
#define NCHIP 7
static int gCurPage = 0;   // the page on the top screen (the bottom chips light up for it)
static int chipOfPage(int pg) { switch (pg) { case 1: case 2: case 3: return 0; case 4: return 1; case 5: return 2; case 6: return 3; case 10: case 11: return 4; case 7: return 5; default: return -1; } }
static const char* PAGE_NAMES[NPAGES] = {"PRESETS", "OSC A", "OSC B", "SUB+NOISE", "FILT+ENV", "LFO+MOD", "VOICE+FX", "ARP+SEQ", "VISUALIZER", "HARDWARE", "MASTER FX", "DRUMS"};
static int pageLen(int pg) { int n = 0; while (PAGES[pg][n].name) n++; return n; }

static void paramFmt(const Param* p, char* out, int len) {
	if (p->type == PT_E) {
		int v = *(int*)p->p;
		if (p->names == TABLE_NAME_DUMMY) {   // name it from the index just picked: tb.name only catches up once the table loads (it read one behind)
			const Osc* o = p->p == &gOsc[0].table ? &gOsc[0] : &gOsc[1];
			if (v >= 0 && v < T_BUILTIN) snprintf(out, len, "%.16s", BUILTIN_NAMES[v]);
			else if (v >= T_BUILTIN && v < gTableCount) {
				char nm[64]; snprintf(nm, sizeof(nm), "%s", gFiles[v - T_BUILTIN]); size_t n = strlen(nm); if (n > 4) nm[n - 4] = 0;
				for (char* c = nm; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
				snprintf(out, len, "%.16s", nm);
			} else snprintf(out, len, "%.16s", o->tb.name);   // live / additive tables keep their own name
		}
		else if (p->names == PRESET_DUMMY) snprintf(out, len, "%.16s", presetName(gStyle, gPreset));
		else if (p->names == PRESS_RIGHT) snprintf(out, len, "PRESS >");
		else if (!p->names || v < (int)p->mn || v > (int)p->mx) snprintf(out, len, "%d", v);   // out of range: show the number, never read past the list
		else snprintf(out, len, "%s", p->names[v]);
		return;
	}
	if (p->type == PT_I) { if (!strcmp(p->name, "UNISON")) snprintf(out, len, "%d", *(int*)p->p); else snprintf(out, len, p->mn < 0 ? "%+d" : "%d", *(int*)p->p); return; }
	float v = *(float*)p->p;
	if (!strcmp(p->unit, "%")) snprintf(out, len, "%d%%", (int)lrintf(v * 100));
	else if (!strcmp(p->unit, "HZ") && p->p == &gCutoff) snprintf(out, len, "%dHZ", (int)(60.0f * powf(15000.0f / 60.0f, v)));
	else if (!strcmp(p->unit, "HZ")) snprintf(out, len, "%.2fHZ", v);
	else if (!strcmp(p->unit, "S")) snprintf(out, len, "%.3fS", v);
	else snprintf(out, len, "%+d%s", (int)lrintf(v), p->unit);
}

static int paramChange(Param* p, int dir, int fast) {
	if (p->type == PT_F) {
		float* v = (float*)p->p; float st = p->step * (fast ? 4 : 1);
		float nv = *v + dir * st;
		if (nv < p->mn) nv = p->mn;
		if (nv > p->mx) nv = p->mx;
		if (nv == *v) return 0;
		*v = nv; return 1;
	}
	int* v = (int*)p->p;
	int mx = (int)p->mx;
	if (p->names == TABLE_NAME_DUMMY) mx = gTableCount - 1;
	if (p->names == PRESET_DUMMY) mx = presetCount(gStyle) - 1;
	if (p->names == PRESS_RIGHT) return 0;
	int nv = *v + dir * ((fast && p->type == PT_I && p->mx > 32) ? 5 : 1);
	if (p->type == PT_E) { if (nv < 0) nv = mx; if (nv > mx) nv = 0; }
	else { if (nv < (int)p->mn) nv = (int)p->mn; if (nv > mx) nv = mx; }
	if (nv == *v) return 0;
	*v = nv; return 1;
}

// ---------------------------------------------------------------- drawing
static u8* fb; static int fbW; static u8* fontGfx;
static inline void px(int x, int y, u8 r, u8 g, u8 b) {
	if (x < 0 || x >= fbW || y < 0 || y >= 240) return;
	u8* p = fb + (x * 240 + (239 - y)) * 3; p[0] = b; p[1] = g; p[2] = r;
}
static void rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b) { for (int x = x0; x < x0 + w; x++) for (int y = y0; y < y0 + h; y++) px(x, y, r, g, b); }
static void line(int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b) {
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
	for (;;) { px(x0, y0, r, g, b); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}
static void drawChar(int x, int y, char c, int sc, u8 r, u8 g, u8 b) {
	const u8* gl = fontGfx + ((u8)c) * 8;
	for (int row = 0; row < 8; row++) for (int col = 0; col < 8; col++) if (gl[row] & (0x80 >> col)) rect(x + col * sc, y + row * sc, sc, sc, r, g, b);
}
static void text(int x, int y, const char* s, int sc, u8 r, u8 g, u8 b) { for (; *s; s++, x += 8 * sc) drawChar(x, y, *s, sc, r, g, b); }

// splash screen: the ANSI logo on the top screen while Syrup loads
#include "splash_img.h"
static int gSplash = 1; static u64 gSplashT0;
static void drawTip(int y);
static void splashDraw(void) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	memset(fb, 0, 400 * 240 * 3);
	int x0 = (400 - SPLASH_W) / 2, y0 = 14, n = 0;
	for (int i = 0; i < (int)sizeof(SPLASH_RLE); i += 2) {
		const u8* c = SPLASH_PAL + SPLASH_RLE[i + 1] * 3;
		for (int k = 0; k < SPLASH_RLE[i]; k++, n++) px(x0 + n % SPLASH_W, y0 + n / SPLASH_W, c[0], c[1], c[2]);
	}
	static const char* T = "WAVETABLE SYNTH";
	text(200 - (int)strlen(T) * 4, y0 + SPLASH_H + 22, T, 1, 255, 255, 85);
	text(200 - (int)strlen("V" SYRUP_VERSION "  BUILD " BUILD_STR) * 4, y0 + SPLASH_H + 36, "V" SYRUP_VERSION "  BUILD " BUILD_STR, 1, 170, 85, 0);
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	memset(fb, 0, 320 * 240 * 3);
	text(160 - 10 * 4, 40, "LOADING...", 1, 170, 85, 0);
	drawTip(110);
}
// TIPS on the bottom screen while Syrup loads (and on the start screen): one at random each time
static const char* TIPS[] = {
	"HOLD SELECT AND PUSH THE CIRCLE PAD: EVERY PAGE, THE TAPE AND THE TRANSPORT ARE ONE FLICK AWAY.",
	"TAP SELECT TO PLAY / STOP. SELECT + L UNDOES, SELECT + R REDOES. SELECT + DOWN IS PANIC.",
	"HOLD A DRUM STEP FOR RATCHETS AND CONDITIONS LIKE 1:2 OR FILL.",
	"HOLD A DRUM'S NAME TO SHAPE ITS SOUND: TUNE, PITCH ENVELOPE, DECAY, TONE, DRIVE, PAN.",
	"BEAT IN THE DRUM GRID PASTES A WHOLE RHYTHM: CUMBIA, BOSSA, BOOM BAP, WALTZ AND 26 MORE.",
	"EVERY DRUM ROW CAN HAVE ITS OWN LENGTH AND SPEED: HOLD ITS NAME, SET RATE AND LEN.",
	"CLR, THEN TAP A DRUM'S NAME, CLEARS JUST THAT ROW.",
	"THE PIANO ROLL: TAP A NOTE IN, DRAG FOR LENGTH, HOLD IT FOR ITS STEP EDITOR.",
	"PUSH THE CIRCLE PAD ON THE PIANO ROLL OR DRUM GRID FOR A CURSOR: A PUTS A NOTE, HOLD A EDITS IT.",
	"SEQ RATE 1/32 RUNS THE STEPS TWICE AS FAST. REC QUANT 32ND KEEPS YOUR FAST FILLS.",
	"TAPE RECORDS FOUR TRACKS IN TIME WITH THE SEQUENCE. EACH TRACK CAN PLAY AT 1/2, X2 AND MORE.",
	"CLICK ALWAYS GIVES YOU A METRONOME THAT NEVER ENDS UP ON THE TAPE.",
	"YOUR SOUND IS SAVED EVERY FEW SECONDS: SWITCH OFF AND IT'S STILL THERE.",
	"ON THE PATCH PAGE, TAP A STYLE, THEN A PATCH. SAVE TWICE KEEPS YOUR EDIT.",
	"WI-FI MIDI ON: PLAY SYRUP FROM YOUR PHONE'S BROWSER, A DAW, OR AN FM-1.",
	"THE MOD MATRIX CAN SEND AN LFO TO GRAIN POS, CUTOFF, WT POS AND MORE.",
	"PADS PLAYS CHORDS: LOW = TRIADS, HIGHER = 7THS AND 9THS, THE EDGES INVERT.",
	"CHIP > FX SENDS THE DRUMS AND SUB THROUGH THE MASTER EFFECTS.",
	"L AND R MOVE THE OCTAVE. X AND Y STEP THROUGH THE PAGES.",
	"TAPE > TABLE TURNS YOUR LAST TAPE TRACK INTO A WAVETABLE.",
};
#define NTIPS ((int)(sizeof(TIPS) / sizeof(TIPS[0])))
static int gTip = -1;
static void drawTip(int y) {   // word-wrapped, 38 characters a line
	if (gTip < 0) gTip = (int)(osGetTime() % NTIPS);
	text(160 - 3 * 4, y, "TIP", 1, 255, 255, 85);
	const char* t = TIPS[gTip]; int line = 0;
	while (*t && line < 5) {
		int n = (int)strlen(t), cut = n;
		if (n > 38) { cut = 38; while (cut > 0 && t[cut] != ' ') cut--; if (!cut) cut = 38; }
		char b[40]; memcpy(b, t, cut); b[cut] = 0;
		text(160 - cut * 4, y + 16 + line * 12, b, 1, 170, 170, 170);
		t += cut; while (*t == ' ') t++; line++;
	}
}
static void splashBottomReady(void) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	memset(fb, 0, 320 * 240 * 3);
	text(160 - 21 * 4, 40, "TAP OR PRESS TO START", 1, 255, 255, 85);
	drawTip(110);
}

// ---------------------------------------------------------------- look: Pip-Boy layout, EGA 16-colour palette
static const u8 EGA[16][3] = {
	{0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170}, {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
	{85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255}, {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255}};
typedef struct {
	const char* name;
	u8 bg[3], line[3], dim[3], txt[3], hi[3], hiTxt[3], val[3], warn[3];
	u8 keyW[3], keyB[3], keyOn[3], keyOnB[3], keyLine[3], kbBg[3], tab[3], tabTxt[3], pad[3], grid[3];
	u8 acc[4];        // EGA visual ramp (unused when smooth)
	int ega;          // 1: EGA-style (meters, dotted grids, outlined tabs, quantised visual colours)
	int rampOn; u8 ramp[3][3];   // visual colour ramp (GPU visuals) for non-EGA themes; ORIGINAL uses amber > magenta > cyan
	int boxed;        // 1: panels are whole double-line boxes (BBS menu style), not corner marks
	u8 ink[3];        // (worked out) text on a lit button: the theme's own dark or light, whichever reads
} Theme;
static const Theme THEMES[NTHEMES] = {
	{"ORIGINAL", {14, 9, 4}, {200, 110, 20}, {150, 100, 40}, {255, 225, 170}, {70, 44, 14}, {255, 220, 160}, {255, 225, 170}, {255, 80, 60},
	 {240, 228, 205}, {26, 16, 8}, {255, 170, 40}, {200, 110, 20}, {30, 20, 10}, {30, 20, 10}, {40, 26, 9}, {200, 150, 90}, {28, 18, 8}, {44, 28, 12}, {0, 0, 0, 0}, 0},
	{"PIP GREEN", {0, 0, 0}, {0, 170, 0}, {0, 170, 0}, {85, 255, 85}, {85, 255, 85}, {0, 0, 0}, {85, 255, 85}, {255, 85, 85},
	 {0, 0, 0}, {0, 170, 0}, {85, 255, 85}, {85, 255, 85}, {85, 255, 85}, {0, 0, 0}, {0, 0, 0}, {85, 255, 85}, {0, 0, 0}, {0, 170, 0}, {2, 10, 11, 15}, 1},
	{"DOS BLUE", {0, 0, 170}, {0, 170, 170}, {170, 170, 170}, {255, 255, 255}, {0, 170, 170}, {0, 0, 0}, {255, 255, 85}, {255, 85, 85},
	 {255, 255, 255}, {0, 0, 0}, {255, 255, 85}, {255, 255, 85}, {85, 85, 85}, {0, 0, 170}, {0, 0, 170}, {255, 255, 255}, {0, 0, 170}, {170, 170, 170}, {11, 14, 13, 15}, 1},
	{"AMBER EGA", {0, 0, 0}, {170, 85, 0}, {170, 85, 0}, {255, 255, 85}, {255, 255, 85}, {0, 0, 0}, {255, 255, 255}, {255, 85, 85},
	 {0, 0, 0}, {170, 85, 0}, {255, 255, 85}, {255, 255, 85}, {255, 255, 85}, {0, 0, 0}, {0, 0, 0}, {255, 255, 85}, {0, 0, 0}, {170, 85, 0}, {6, 14, 12, 15}, 1},
	{"MAGENTA", {0, 0, 0}, {170, 0, 170}, {170, 0, 170}, {255, 85, 255}, {255, 85, 255}, {0, 0, 0}, {85, 255, 255}, {255, 85, 85},
	 {0, 0, 0}, {170, 0, 170}, {85, 255, 255}, {85, 255, 255}, {255, 85, 255}, {0, 0, 0}, {0, 0, 0}, {255, 85, 255}, {0, 0, 0}, {170, 0, 170}, {5, 13, 11, 15}, 1},
	// STICKER: orange backing, sky-blue fur, hot-pink bow, cream, navy outlines
	{"STICKER", {26, 24, 50}, {232, 100, 52}, {110, 165, 200}, {200, 232, 245}, {226, 66, 136}, {255, 246, 238}, {255, 150, 90}, {255, 90, 90},
	 {246, 238, 226}, {34, 38, 68}, {226, 66, 136}, {232, 100, 52}, {34, 38, 68}, {34, 38, 68}, {58, 48, 92}, {200, 232, 245}, {36, 34, 66}, {62, 60, 104}, {0, 0, 0, 0}, 0,
	 1, {{232, 100, 52}, {226, 66, 136}, {110, 185, 225}}},
	// IMPULSE: Impulse Tracker style - black fields, tracker-grey panels and tabs, deep blue selection bar
	// IMPULSE: Impulse Tracker's screen - warm khaki-grey panels everywhere with black text, lighter raised buttons,
	// darker insets, the dark blue selection bar, values in deep blue
	{"IMPULSE", {168, 164, 152}, {236, 232, 220}, {84, 80, 72}, {0, 0, 0}, {44, 44, 132}, {255, 255, 255}, {20, 20, 110}, {150, 30, 20},
	 {236, 232, 220}, {28, 28, 28}, {72, 72, 196}, {100, 100, 216}, {100, 96, 88}, {132, 128, 120}, {200, 196, 184}, {0, 0, 0}, {136, 132, 122}, {112, 108, 100}, {0, 0, 0, 0}, 0,
	 1, {{60, 56, 50}, {72, 72, 210}, {236, 232, 220}}, 1},
	// CYBER: neon cyan and magenta at full strength, glows on the dim bottom screen
	{"CYBER", {10, 10, 20}, {0, 255, 255}, {100, 100, 150}, {220, 220, 255}, {255, 0, 128}, {255, 255, 255}, {0, 255, 128}, {255, 50, 50},
	 {190, 195, 235}, {10, 10, 20}, {255, 0, 128}, {200, 0, 100}, {0, 255, 255}, {15, 15, 30}, {20, 20, 40}, {0, 255, 255}, {25, 25, 45}, {40, 40, 70}, {0, 0, 0, 0}, 0,
	 1, {{0, 255, 255}, {255, 0, 128}, {0, 255, 128}}},
	// SOLARIZED (dark): muted, low eye strain, yellow values
	{"SOLARIZED", {0, 43, 54}, {88, 110, 117}, {101, 123, 131}, {131, 148, 150}, {38, 139, 210}, {253, 246, 227}, {181, 137, 0}, {220, 50, 47},
	 {238, 232, 213}, {0, 43, 54}, {133, 153, 0}, {100, 115, 0}, {147, 161, 161}, {0, 43, 54}, {7, 54, 66}, {131, 148, 150}, {0, 43, 54}, {88, 110, 117}, {0, 0, 0, 0}, 0,
	 1, {{133, 153, 0}, {38, 139, 210}, {181, 137, 0}}},
	// GB POCKET: four greens, dark on light
	{"GB POCKET", {155, 188, 15}, {48, 98, 48}, {70, 120, 48}, {15, 56, 15}, {15, 56, 15}, {155, 188, 15}, {15, 56, 15}, {160, 40, 30},
	 {155, 188, 15}, {48, 98, 48}, {15, 56, 15}, {15, 56, 15}, {15, 56, 15}, {48, 98, 48}, {139, 172, 15}, {15, 56, 15}, {139, 172, 15}, {48, 98, 48}, {0, 0, 0, 0}, 0,
	 1, {{15, 56, 15}, {48, 98, 48}, {139, 172, 15}}},
	// VAPORWAVE: pastel pink, purple and teal, bright text on deep purple
	{"VAPORWAVE", {30, 10, 40}, {255, 105, 180}, {150, 100, 180}, {240, 220, 255}, {0, 255, 255}, {30, 10, 40}, {255, 215, 0}, {255, 50, 50},
	 {235, 205, 245}, {30, 10, 40}, {255, 105, 180}, {200, 50, 130}, {0, 255, 255}, {40, 15, 55}, {50, 20, 70}, {255, 105, 180}, {60, 25, 80}, {100, 50, 130}, {0, 0, 0, 0}, 0,
	 1, {{255, 105, 180}, {0, 255, 255}, {255, 215, 0}}},
	// CGA: IBM mode 4 - black, cyan, magenta, white only
	{"CGA", {0, 0, 0}, {85, 255, 255}, {0, 170, 170}, {255, 255, 255}, {255, 0, 255}, {0, 0, 0}, {85, 255, 255}, {255, 85, 85},
	 {255, 255, 255}, {0, 0, 0}, {85, 255, 255}, {255, 85, 255}, {0, 170, 170}, {0, 0, 0}, {0, 0, 0}, {85, 255, 255}, {0, 24, 24}, {0, 85, 85}, {0, 3, 5, 15}, 1},
	// SINCLAIR: ZX Spectrum brights on black, blue tab bars
	{"SINCLAIR", {0, 0, 0}, {85, 255, 255}, {85, 85, 255}, {255, 255, 255}, {255, 85, 255}, {0, 0, 0}, {85, 255, 85}, {255, 85, 85},
	 {205, 205, 205}, {0, 0, 85}, {85, 255, 255}, {85, 255, 85}, {0, 170, 170}, {0, 0, 0}, {85, 85, 255}, {0, 0, 0}, {0, 0, 40}, {0, 170, 170}, {12, 14, 11, 10}, 1},
	// X11: Motif grey workstation - the bright one: black text, navy selection, dark red values
	{"X11", {176, 176, 176}, {0, 0, 0}, {96, 96, 96}, {0, 0, 0}, {0, 0, 128}, {255, 255, 255}, {128, 0, 0}, {255, 0, 0},
	 {220, 220, 220}, {96, 96, 96}, {0, 0, 128}, {128, 128, 255}, {0, 0, 0}, {128, 128, 128}, {176, 176, 176}, {0, 0, 0}, {196, 196, 196}, {128, 128, 128}, {0, 9, 4, 7}, 1},
	// NORD: snow text on polar-night navy, frost accents
	{"NORD", {46, 52, 64}, {136, 192, 208}, {104, 118, 135}, {216, 222, 233}, {71, 97, 138}, {216, 222, 233}, {143, 188, 187}, {191, 97, 106},
	 {216, 222, 233}, {59, 66, 82}, {143, 188, 187}, {136, 192, 208}, {104, 118, 135}, {59, 66, 82}, {76, 86, 106}, {216, 222, 233}, {67, 76, 94}, {94, 104, 124}, {0, 0, 0, 0}, 0,
	 1, {{191, 97, 106}, {235, 203, 139}, {136, 192, 208}}},
	// ACID: rave neon - acid green, yellow, magenta on near-black
	{"ACID", {6, 8, 6}, {85, 255, 85}, {40, 120, 40}, {230, 255, 230}, {255, 255, 0}, {0, 0, 0}, {0, 255, 255}, {255, 0, 128},
	 {205, 235, 205}, {4, 6, 4}, {85, 255, 85}, {255, 255, 0}, {40, 120, 40}, {4, 6, 4}, {24, 40, 24}, {85, 255, 85}, {12, 16, 12}, {30, 50, 30}, {0, 0, 0, 0}, 0,
	 1, {{255, 255, 0}, {85, 255, 85}, {255, 0, 255}}},
	// BBS: a dial-up board's ANSI main menu - black, double-line cyan boxes, light grey labels, bright cyan values,
	// a dark teal selection bar; readable first (grey on black, not dim teal)
	{"BBS", {0, 0, 0}, {0, 170, 170}, {170, 170, 170}, {230, 230, 230}, {0, 92, 92}, {255, 255, 255}, {85, 255, 255}, {255, 85, 85},
	 {200, 200, 200}, {0, 0, 0}, {85, 255, 255}, {0, 170, 170}, {0, 120, 120}, {0, 0, 0}, {0, 34, 34}, {85, 255, 255}, {0, 16, 16}, {0, 60, 60}, {3, 11, 7, 15}, 1,
	 0, {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, 1},
	// HERCULES: the monochrome card - grey phosphor on black, "intense" white for values, inverse video for the bar,
	// nothing in colour anywhere (warnings are just intense too)
	{"HERCULES", {0, 0, 0}, {170, 170, 170}, {125, 125, 125}, {205, 205, 205}, {205, 205, 205}, {0, 0, 0}, {255, 255, 255}, {255, 255, 255},
	 {205, 205, 205}, {0, 0, 0}, {255, 255, 255}, {170, 170, 170}, {70, 70, 70}, {0, 0, 0}, {45, 45, 45}, {225, 225, 225}, {30, 30, 30}, {70, 70, 70}, {0, 8, 7, 15}, 1,
	 1, {{90, 90, 90}, {175, 175, 175}, {255, 255, 255}}, 1},
	// HERC AMBER / HERC GREEN: the same card on the brown-amber and the green phosphor monitors
	{"HERC AMBER", {0, 0, 0}, {200, 120, 10}, {140, 88, 10}, {232, 150, 22}, {232, 150, 22}, {0, 0, 0}, {255, 200, 60}, {255, 210, 90},
	 {232, 150, 22}, {0, 0, 0}, {255, 200, 60}, {180, 110, 10}, {70, 45, 5}, {0, 0, 0}, {52, 33, 4}, {242, 172, 42}, {30, 19, 2}, {72, 46, 6}, {0, 6, 14, 14}, 1,
	 1, {{90, 55, 5}, {200, 120, 10}, {255, 200, 60}}, 1},
	{"HERC GREEN", {0, 0, 0}, {40, 170, 40}, {32, 125, 32}, {64, 222, 64}, {64, 222, 64}, {0, 0, 0}, {130, 255, 130}, {160, 255, 160},
	 {64, 222, 64}, {0, 0, 0}, {130, 255, 130}, {40, 150, 40}, {10, 60, 10}, {0, 0, 0}, {10, 46, 10}, {92, 240, 92}, {4, 26, 4}, {14, 62, 14}, {0, 2, 10, 10}, 1,
	 1, {{15, 70, 15}, {40, 170, 40}, {130, 255, 130}}, 1}
};
// Every theme is checked when first used: text must stand out from what it sits on (WCAG-style contrast; a colour
// that's too close is pushed lighter or darker, keeping its hue), and a one-colour theme (GB POCKET, PIP GREEN...)
// keeps to its colour: its warning red becomes its own brightest shade.
static Theme gThFix[NTHEMES]; static int gThBuilt = 0;
static float thLum(const u8* c) { float r[3]; for (int i = 0; i < 3; i++) { float v = c[i] / 255.0f; r[i] = v <= 0.03928f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); } return 0.2126f * r[0] + 0.7152f * r[1] + 0.0722f * r[2]; }
static float thCon(const u8* a, const u8* b) { float x = thLum(a), y = thLum(b); if (x < y) { float t = x; x = y; y = t; } return (x + 0.05f) / (y + 0.05f); }
static void thPush(u8* fg, const u8* bg, float want) {   // move fg away from bg (lighter or darker) until it reads
	int up = thLum(bg) < 0.18f;
	for (int k = 0; k < 40 && thCon(fg, bg) < want; k++)
		for (int i = 0; i < 3; i++) { int v = up ? fg[i] + (255 - fg[i]) / 6 + 2 : fg[i] - fg[i] / 6 - 2; fg[i] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }
}
static float thHue(const u8* c, float* sat) {
	float r = c[0] / 255.0f, g = c[1] / 255.0f, b = c[2] / 255.0f, mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
	*sat = mx > 0 ? d / mx : 0; if (d < 1e-4f) return 0;
	float h = mx == r ? fmodf((g - b) / d, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4; h *= 60; return h < 0 ? h + 360 : h;
}
static void themeBuild(void) {
	for (int n = 0; n < NTHEMES; n++) {
		Theme* t = &gThFix[n]; *t = THEMES[n];
		// one colour? the hues of its main colours sit together: then nothing from outside (no red warning)
		const u8* M[5] = {t->txt, t->val, t->line, t->keyOn, t->hi}; float h0 = -1, spread = 0; int cnt = 0;
		for (int i = 0; i < 5; i++) { float sa, h = thHue(M[i], &sa); if (sa < 0.2f) continue; if (h0 < 0) h0 = h; float d = fabsf(h - h0); if (d > 180) d = 360 - d; if (d > spread) spread = d; cnt++; }
		if (cnt >= 3 && spread < 35) {
			float sa, wh = thHue(t->warn, &sa), d = fabsf(wh - h0); if (d > 180) d = 360 - d;
			if (sa > 0.2f && d > 45) { const u8* br = thLum(t->txt) > thLum(t->val) ? t->txt : t->val; memcpy(t->warn, br, 3); }
		}
		thPush(t->txt, t->bg, 7.0f); thPush(t->val, t->bg, 4.5f); thPush(t->dim, t->bg, 3.2f); thPush(t->warn, t->bg, 3.0f);
		thPush(t->tabTxt, t->tab, 4.5f); thPush(t->hiTxt, t->hi, 4.5f);
		// ink: text on the lit things (keyOn, val, warn as a background): the theme's bg or txt, whichever reads best
		const u8* a = t->bg; const u8* b = t->txt; float ca = fminf(thCon(a, t->keyOn), thCon(a, t->val)), cb = fminf(thCon(b, t->keyOn), thCon(b, t->val));
		memcpy(t->ink, ca >= cb ? a : b, 3);
		if (thCon(t->ink, t->keyOn) < 4.5f) { u8 k[3]; memcpy(k, t->ink, 3); thPush(k, t->keyOn, 4.5f); memcpy(t->ink, k, 3); }
	}
	gThBuilt = 1;
}
static inline const Theme* thCur(void) { if (!gThBuilt) themeBuild(); return &gThFix[gTheme]; }
#define TH (thCur())
#define RGB3(a) (a)[0], (a)[1], (a)[2]
#define EC(i) EGA[i][0], EGA[i][1], EGA[i][2]
#define BG    RGB3(TH->bg)
#define PANEL RGB3(TH->bg)
#define AMB   RGB3(TH->txt)
#define AMB2  RGB3(TH->line)
#define DIM   RGB3(TH->dim)
#define TXT   RGB3(TH->txt)
#define LINE  RGB3(TH->line)
#define VAL   RGB3(TH->val)
#define HI    RGB3(TH->hi)
#define HITXT RGB3(TH->hiTxt)
#define WARN  RGB3(TH->warn)

// Pip-Boy style corner brackets around a panel
static void outline(int x, int y, int w, int h, u8 r, u8 g, u8 b);
static void frame(int x, int y, int w, int h) {
	if (TH->boxed) { outline(x, y, w, h, LINE); outline(x + 2, y + 2, w - 4, h - 4, LINE); return; }   // BBS: a double-line box
	int L = 8;
	rect(x, y, L, 1, LINE); rect(x, y, 1, L, LINE);
	rect(x + w - L, y, L, 1, LINE); rect(x + w - 1, y, 1, L, LINE);
	rect(x, y + h - 1, L, 1, LINE); rect(x, y + h - L, 1, L, LINE);
	rect(x + w - L, y + h - 1, L, 1, LINE); rect(x + w - 1, y + h - L, 1, L, LINE);
}
// CRT scanlines on the top screen: every other row a step darker
static void scanlines(void) {
	if (!gScan) return;
	for (int x = 0; x < 400; x++) {
		u8* p = fb + x * 240 * 3;
		for (int r = 0; r < 240; r += 2) { p[r * 3] = p[r * 3] * 5 >> 3; p[r * 3 + 1] = p[r * 3 + 1] * 5 >> 3; p[r * 3 + 2] = p[r * 3 + 2] * 5 >> 3; }
	}
}

static void drawWave(const s16* lv, int N, int x0, int y0, int w, int h, int bright) {
	if (TH->ega) { rect(x0, y0, w, h, BG); for (int x = x0; x < x0 + w; x += 2) px(x, y0 + h / 2, LINE); }
	else { rect(x0, y0, w, h, RGB3(TH->pad)); rect(x0, y0 + h / 2, w, 1, RGB3(TH->grid)); }
	int px0 = 0, py0 = 0;
	for (int i = 0; i <= w - 6; i++) {
		int n = i * N / (w - 6); if (n >= N) n = N - 1;
		int x = x0 + 3 + i, y = y0 + h / 2 - (int)(lv[n] / 32768.0f * (h / 2 - 4));
		if (i) { if (bright) { line(px0, py0, x, y, TXT); line(px0, py0 + 1, x, y + 1, TXT); } else line(px0, py0, x, y, DIM); }
		px0 = x; py0 = y;
	}
	frame(x0, y0, w, h);
}

static void drawStack(const Osc* o, float pos, int ox, int oy, int W, int H) {
	const Table* tb = &o->tb; int steps = 18;
	float ds = 1.0f / tb->peak;
	int curK = (int)(pos * (steps - 1) + 0.5f);
	for (int k = steps - 1; k >= 0; k--) {
		int fi = (int)((float)k / (steps - 1) * (tb->nFrames - 1));
		const float* fr = tb->frames + fi * CYC;
		int bx = ox + k * 3, by = oy - k * 4, cur = k == curK;
		int px0 = bx, py0 = by - (int)(fr[0] * ds * H * 0.5f);
		for (int n = 1; n <= 40; n++) {
			int x = bx + n * W / 40, y = by - (int)(fr[(n * CYC / 40) & (CYC - 1)] * ds * H * 0.5f);
			if (cur) line(px0, py0, x, y, TXT); else line(px0, py0, x, y, DIM);
			px0 = x; py0 = y;
		}
	}
}

// ---- envelope shapes (FILTER+ENV page): A, H, D, S, R drawn to scale (square-root time so short stages stay visible)
static const char* gSelName = NULL;   // selected control, so its stage can be highlighted
typedef struct { float x[6], y[6]; } EnvPts;   // start, peak, hold end, decay end, sustain end, release end
static float envW(float t) { return sqrtf(t + 0.002f); }
static void envShape(const AHDSR* E, float X0, float Y0, float W, float H, EnvPts* P) {
	float wa = envW(E->a), wh = E->h > 0 ? envW(E->h) : 0, wd = envW(E->d), wr = envW(E->r);
	float ws = (wa + wh + wd + wr) * 0.3f + 0.12f;   // sustain: a plateau of fixed share
	float k = W / (wa + wh + wd + ws + wr), x = X0, yb = Y0 + H, ys = Y0 + H * (1 - E->s);
	P->x[0] = x; P->y[0] = yb;
	x += wa * k; P->x[1] = x; P->y[1] = Y0;
	x += wh * k; P->x[2] = x; P->y[2] = Y0;
	x += wd * k; P->x[3] = x; P->y[3] = ys;
	x += ws * k; P->x[4] = x; P->y[4] = ys;
	x += wr * k; P->x[5] = x; P->y[5] = yb;
}
static int envSelSeg(int which) {   // 0 A, 1 H, 2 D, 3 S, 4 R for the selected control, else -1
	if (!gSelName || strncmp(gSelName, which ? "ENV2 " : "ENV1 ", 5)) return -1;
	const char* t = gSelName + 5;
	return !strcmp(t, "ATK") ? 0 : !strcmp(t, "HOLD") ? 1 : !strcmp(t, "DEC") ? 2 : !strcmp(t, "SUS") ? 3 : !strcmp(t, "REL") ? 4 : -1;
}
static const SVoice* newestVoice(void) {
	const SVoice* nv = NULL;
	for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started && !gV[i].isEcho && (!nv || gV[i].age > nv->age)) nv = &gV[i];
	return nv;
}
static int envDot(int which, const EnvPts* P, float* dx, float* dy) {   // where the newest note is on the shape
	const SVoice* v = newestVoice(); if (!v) return 0;
	const AHDSR* E = which ? &gEnv2 : &gEnv1;
	int st = which ? v->stage2 : v->stage; float e = which ? v->env2 : v->env, hd = which ? v->hold2 : v->hold1;
	switch (st) {
	case 1: *dx = P->x[0] + (P->x[1] - P->x[0]) * e; break;
	case 5: *dx = P->x[1] + (P->x[2] - P->x[1]) * (E->h > 0 ? hd / E->h : 0); break;
	case 2: *dx = P->x[2] + (P->x[3] - P->x[2]) * ((1 - e) / (1 - E->s + 1e-4f)); break;
	case 3: *dx = (P->x[3] + P->x[4]) * 0.5f; break;
	case 4: { float s0 = E->s > 0.02f ? E->s : 1, u = 1 - e / s0; if (u < 0) u = 0; if (u > 1) u = 1; *dx = P->x[4] + (P->x[5] - P->x[4]) * u; } break;
	default: return 0;
	}
	*dy = P->y[0] - (P->y[0] - P->y[1]) * e;
	return 1;
}
// labels drawn by the CPU (also over the GPU panel)
static void envLabels(void) {
	static const char* SEG = "AHDSR";
	for (int w = 0; w < 2; w++) {
		float Y0 = 30 + w * 64; EnvPts P; envShape(w ? &gEnv2 : &gEnv1, 14, Y0 + 13, 176, 33, &P);
		text(8, Y0 + 3, w ? "MOD ENV" : "AMP ENV", 1, DIM);
		int sel = envSelSeg(w);
		for (int k = 0; k < 5; k++) {
			if (k == 1 && (w ? gEnv2.h : gEnv1.h) <= 0) continue;
			int x = (int)((P.x[k] + P.x[k + 1]) * 0.5f) - 4;
			char c[2] = {SEG[k], 0};
			if (k == sel) text(x, (int)Y0 + 50, c, 1, TXT); else text(x, (int)Y0 + 50, c, 1, DIM);
		}
	}
}
// CPU version
// FILTER RESPONSE (FILT+ENV page): the main filter and FILTER 2 evaluated at 188 log-spaced frequencies, with the
// harmonics of OSC A's live cycle at the newest note before (dim) and after (bright) the filter
static float bqMag(float b0, float b1, float b2, float a0, float a1, float a2, float w) {
	float c1 = cosf(w), s1 = sinf(w), c2 = cosf(2 * w), s2 = sinf(2 * w);
	float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
	return sqrtf((nr * nr + ni * ni) / (dr * dr + di * di + 1e-12f));
}
static float filterGain(float f) {   // |H| of the voice filter at f Hz, as the DSP is set up
	float hz = baseCutoff(); if (hz < 40) hz = 40; if (hz > 15000) hz = 15000;
	float w0 = TWO_PI * hz / DSPRATE, w = TWO_PI * f / DSPRATE, cw = cosf(w0), sw = sinf(w0), g = 1;
	float q = 0.707f + gRes * 9.0f, qb = 0.5f + gRes * 6.0f, al = sw / (2 * (gFiltType >= 2 ? (gFiltType == 4 ? 0.7f + gRes * 6.0f : qb) : q));
	switch (gFiltType) {
	case 0: g = bqMag((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al, w); break;
	case 1: g = bqMag((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + al, -2 * cw, 1 - al, w); break;
	case 2: g = bqMag(al, 0, -al, 1 + al, -2 * cw, 1 - al, w); break;
	case 3: g = bqMag(1, -2 * cw, 1, 1 + al, -2 * cw, 1 - al, w); break;
	default: { float A = 2.0f; g = bqMag(1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A, w); }
	}
	if (gFilt2 == 1) { float k = expf(-TWO_PI * hz / DSPRATE); g *= (1 - k) / sqrtf(1 + k * k - 2 * k * cosf(w)); }   // +6 dB slope: one pole at the cutoff
	else if (gFilt2 == 2) { float fc = LOWCUT_HZ[gLowCut]; g *= f / sqrtf(f * f + fc * fc); }   // LOW CUT
	return g;
}
static void drawFilterPlot(int x0, int y0, int w, int h) {
	frame(x0, y0, w, h);
	int ix = x0 + 4, iw = w - 8, iy = y0 + 3, ih = h - 6;
	static const float FLO = 30, FHI = 15000;
	#define FX(f) (ix + (int)(logf((f) / FLO) / logf(FHI / FLO) * (iw - 1)))
	#define DBY(d) (iy + (int)((12 - (d)) / 60.0f * (ih - 1)))
	for (int d = 0; d >= -36; d -= 12) for (int x = ix; x < ix + iw; x += 3) px(x, DBY(d), RGB3(TH->grid));   // 0, -12, -24, -36 dB
	static const float DEC[3] = {100, 1000, 10000}; for (int k = 0; k < 3; k++) for (int y = iy; y < iy + ih; y += 3) px(FX(DEC[k]), y, RGB3(TH->grid));
	// harmonics of OSC A's live cycle at the newest note (or A3), before / after the filter
	const SVoice* v = newestVoice(); float f0 = v ? v->cur * oscTune(0) : 220.0f; const Mips* m = &gOsc[0].mips;
	float top = 1e-6f; for (int k = 1; k < 128; k++) { float a = sqrtf(m->spec[k].re * m->spec[k].re + m->spec[k].im * m->spec[k].im); if (a > top) top = a; }
	for (int k = 1; k < 128 && f0 * k < FHI; k++) {
		float f = f0 * k; if (f < FLO) continue;
		float a = sqrtf(m->spec[k].re * m->spec[k].re + m->spec[k].im * m->spec[k].im) / top; if (a < 1e-4f) continue;
		float db = 20 * log10f(a), dbf = db + 20 * log10f(filterGain(f) + 1e-6f); int x = FX(f), yb = iy + ih - 1;
		int y1 = DBY(db < -48 ? -48 : db); if (y1 < yb) rect(x, y1, 1, yb - y1, RGB3(TH->dim));
		int y2 = DBY(dbf < -48 ? -48 : dbf > 12 ? 12 : dbf); if (y2 < yb) rect(x, y2, 1, yb - y2, RGB3(TH->txt));
	}
	// the response curve, bright
	int py = -1;
	for (int x = ix; x < ix + iw; x++) {
		float f = FLO * powf(FHI / FLO, (float)(x - ix) / (iw - 1)), db = 20 * log10f(filterGain(f) + 1e-6f);
		if (db > 12) db = 12; if (db < -48) db = -48;
		int y = DBY(db); if (y > iy + ih - 1) y = iy + ih - 1;
		if (py >= 0) { int a = py < y ? py : y, b = py < y ? y : py; rect(x, a, 1, b - a + 1, RGB3(TH->val)); }
		py = y;
	}
	float hz = baseCutoff(); if (hz >= FLO && hz <= FHI) { int cx = FX(hz); for (int y = iy; y < iy + ih; y += 2) px(cx, y, RGB3(TH->line)); }
	char t[32]; snprintf(t, sizeof(t), hz >= 1000 ? "%.1fK" : "%.0f", hz >= 1000 ? hz / 1000 : hz); text(x0 + w - 6 - (int)strlen(t) * 8, y0 + 4, t, 1, RGB3(TH->val));
	static const char* FT[5] = {"LP", "HP", "BP", "NOTCH", "PEAK"}; text(x0 + 6, y0 + 4, FT[gFiltType % 5], 1, RGB3(TH->dim));
	#undef FX
	#undef DBY
}
static void drawEnvelopesCPU(void) {
	for (int w = 0; w < 2; w++) {
		int Y0 = 30 + w * 64, H = 60;
		rect(4, Y0, 196, H, RGB3(TH->pad));
		EnvPts P; envShape(w ? &gEnv2 : &gEnv1, 14, Y0 + 13, 176, 33, &P);
		for (int x = (int)P.x[0]; x < (int)P.x[5]; x++) {   // fill under the curve
			int k = 0; while (k < 4 && x > P.x[k + 1]) k++;
			float u = (x - P.x[k]) / (P.x[k + 1] - P.x[k] + 1e-4f), y = P.y[k] + (P.y[k + 1] - P.y[k]) * u;
			rect(x, (int)y, 1, (int)(P.y[0] - y), RGB3(TH->grid));
		}
		for (int x = 8; x < 196; x += 4) px(x, (int)P.y[3], DIM);   // sustain level
		int sel = envSelSeg(w);
		for (int k = 0; k < 5; k++) {
			line((int)P.x[k], (int)P.y[k], (int)P.x[k + 1], (int)P.y[k + 1], k == sel ? TH->txt[0] : TH->line[0], k == sel ? TH->txt[1] : TH->line[1], k == sel ? TH->txt[2] : TH->line[2]);
			if (k == sel) line((int)P.x[k], (int)P.y[k] - 1, (int)P.x[k + 1], (int)P.y[k + 1] - 1, TXT);
		}
		for (int k = 1; k < 5; k++) rect((int)P.x[k] - 1, (int)P.y[k] - 1, 3, 3, TXT);
		float dx, dy; if (envDot(w, &P, &dx, &dy)) rect((int)dx - 2, (int)dy - 2, 5, 5, VAL);
		frame(4, Y0, 196, H);
	}
	envLabels();
}

// ---- LFO pictures (LFO+MOD page): one cycle of each LFO's shape, a playhead and the current value
static float lfoDrawAt(const LFO* l, float p) {   // S&H drawn as random steps instead of its held value
	if (l->shape == LS_SH) return hash01((int)(p * 8), 7) * 2 - 1;
	return lfoShapeAt(l, p);
}
static int lfoSelected(int i) { return gSelName && !strncmp(gSelName, i ? "LFO2" : "LFO1", 4); }
static void lfoLabels(void) {
	for (int i = 0; i < 2; i++) {
		const LFO* l = &gLfo[i]; char t[40], r[12];
		if (l->sync) snprintf(r, sizeof(r), "%s", SYNC_NAMES[l->sync]); else snprintf(r, sizeof(r), "%.2fHZ", l->rate);
		snprintf(t, sizeof(t), "LFO%d %s", i + 1, LFO_SHAPES[l->shape]);
		if (lfoSelected(i)) text(8, 33 + i * 64, t, 1, TXT); else text(8, 33 + i * 64, t, 1, DIM);
		text(196 - (int)strlen(r) * 8, 33 + i * 64, r, 1, DIM);
	}
}
static void drawLfosCPU(void) {
	for (int i = 0; i < 2; i++) {
		const LFO* l = &gLfo[i]; int Y0 = 30 + i * 64, H = 60, mid = Y0 + 36, amp = 18;
		rect(4, Y0, 196, H, RGB3(TH->pad));
		rect(10, mid, 184, 1, RGB3(TH->grid));
		int pyv = 0;
		for (int x = 0; x <= 184; x++) {
			int y = mid - (int)(lfoDrawAt(l, x / 184.0f) * amp);
			if (x) { if (lfoSelected(i)) line(9 + x, pyv, 10 + x, y, TXT); else line(9 + x, pyv, 10 + x, y, LINE); }
			pyv = y;
		}
		int hx = 10 + (int)(l->ph * 184), hy = mid - (int)(l->val * amp);
		for (int y = Y0 + 12; y < Y0 + H - 2; y += 2) px(hx, y, DIM);
		rect(hx - 2, hy - 2, 5, 5, VAL);
		frame(4, Y0, 196, H);
	}
	lfoLabels();
}

// ---- what each control does (shown in the footer for the selected row)
static const char* paramHelp(const char* n) {
	static const char* H[][2] = {
		{"STYLE", "PICK A STYLE OF PATCH"}, {"PATCH", "PICK A PATCH - IT LOADS AS YOU SCROLL"}, {"SAVE SLOT", "WHICH USER SLOT TO SAVE INTO"},
		{"SAVE NOW", "PRESS > TO SAVE THIS SOUND"}, {"INIT PATCH", "PRESS > FOR A PLAIN SAW TO START FROM"},
		{"ON", "OSCILLATOR ON / OFF"}, {"TABLE", "WHICH WAVETABLE"}, {"WT POS", "POSITION IN THE WAVETABLE"}, {"WARP", "HOW THE WAVE IS BENT"},
		{"WARP AMT", "HOW MUCH WARP"}, {"LEVEL", "VOLUME OF THIS OSCILLATOR"}, {"OCTAVE", "OCTAVE UP / DOWN"}, {"SEMI", "SEMITONES UP / DOWN"},
		{"FINE", "FINE TUNE IN CENTS"}, {"UNISON", "STACKED VOICES PER NOTE (1-3)"}, {"DETUNE", "SPREAD BETWEEN UNISON VOICES"},
		{"BLEND", "LEVEL OF THE OUTER UNISON VOICES"}, {"WIDTH", "STEREO SPREAD OF UNISON"},
		{"SUB SHAPE", "SUB OSCILLATOR WAVE"}, {"SUB OCTAVE", "SUB OSCILLATOR OCTAVE"}, {"SUB LEVEL", "SUB OSCILLATOR VOLUME"},
		{"NOISE TYPE", "WHITE, PINK OR VINYL CRACKLE"}, {"NOISE LEVEL", "NOISE VOLUME"},
		{"FILTER", "FILTER TYPE"}, {"CUTOFF", "FILTER BRIGHTNESS"}, {"FILTER 2", "SECOND HARDWARE FILTER STAGE"}, {"LOW CUT", "REMOVES LOWS UNDER THIS"},
		{"RES", "FILTER RESONANCE"}, {"KEYTRACK", "HIGHER NOTES OPEN THE FILTER"},
		{"ENV1 ATK", "VOLUME FADE-IN TIME"}, {"ENV1 HOLD", "TIME AT FULL BEFORE DECAY"}, {"ENV1 DEC", "TIME TO FALL TO SUSTAIN"},
		{"ENV1 SUS", "LEVEL WHILE THE KEY IS HELD"}, {"ENV1 REL", "FADE-OUT AFTER RELEASE"},
		{"ENV2 ATK", "MOD ENVELOPE ATTACK"}, {"ENV2 DEC", "MOD ENVELOPE DECAY"}, {"ENV2 SUS", "MOD ENVELOPE SUSTAIN"}, {"ENV2 REL", "MOD ENVELOPE RELEASE"},
		{"LFO1 SHAPE", "LFO 1 WAVE - WT = WAVETABLE SHAPES"}, {"LFO1 RATE", "LFO 1 SPEED"}, {"LFO1 MODE", "FREE RUNNING OR RESTART PER NOTE"},
		{"LFO1 SYNC", "LOCK LFO 1 TO THE TEMPO"}, {"LFO1 TABLE", "WAVETABLE FOR SHAPE: WAVETABLE"}, {"LFO1 WT POS", "WHICH FRAME OF THAT WAVETABLE"},
		{"LFO2 SYNC", "LOCK LFO 2 TO THE TEMPO"}, {"LFO2 TABLE", "WAVETABLE FOR SHAPE: WAVETABLE"}, {"LFO2 WT POS", "WHICH FRAME OF THAT WAVETABLE"},
		{"LFO2 SHAPE", "LFO 2 WAVE"}, {"LFO2 RATE", "LFO 2 SPEED"}, {"LFO2 MODE", "FREE RUNNING OR RESTART PER NOTE"},
		{"1 SOURCE", "MOD SLOT 1: WHAT MOVES IT"}, {"1 DEST", "MOD SLOT 1: WHAT IT MOVES"}, {"1 AMOUNT", "MOD SLOT 1: HOW MUCH"},
		{"2 SOURCE", "MOD SLOT 2: WHAT MOVES IT"}, {"2 DEST", "MOD SLOT 2: WHAT IT MOVES"}, {"2 AMOUNT", "MOD SLOT 2: HOW MUCH"},
		{"3 SOURCE", "MOD SLOT 3: WHAT MOVES IT"}, {"3 DEST", "MOD SLOT 3: WHAT IT MOVES"}, {"3 AMOUNT", "MOD SLOT 3: HOW MUCH"},
		{"4 SOURCE", "MOD SLOT 4: WHAT MOVES IT"}, {"4 DEST", "MOD SLOT 4: WHAT IT MOVES"}, {"4 AMOUNT", "MOD SLOT 4: HOW MUCH"},
		{"MODE", "POLY, MONO OR LEGATO"}, {"GLIDE", "SLIDE TIME BETWEEN NOTES"}, {"BEND RANGE", "CIRCLE PAD BEND IN SEMITONES"},
		{"CHORD", "CHORD PLAYED BY ONE KEY (ALSO A)"}, {"GATE", "TRANCE GATE: CHOPS THE SOUND TO THE BEAT"}, {"MPE", "EACH NOTE ITS OWN BEND, PRESSURE, SLIDE (CH 2-16)"}, {"LFO1 CC OUT", "SEND LFO 1 AS THIS CC OVER NETWORK MIDI (0 = OFF)"}, {"LFO2 CC OUT", "SEND LFO 2 AS THIS CC OVER NETWORK MIDI (0 = OFF)"}, {"GATE RATE", "STEP LENGTH OF THE GATE PATTERN"}, {"GATE DEPTH", "HOW FAR THE GATE CLOSES"}, {"CHORUS", "STEREO CHORUS AMOUNT"}, {"GPU VISUALS", "GRAPHICS CHIP VISUALS ON THE LEFT"},
		{"FX", "ECHO, SPACE OR REVERB"}, {"ECHO TIME", "DELAY TIME"}, {"ECHO FDBK", "NUMBER OF REPEATS"}, {"OUTPUT", "STEREO OR VIRTUAL SURROUND"},
		{"SURROUND", "SURROUND DEPTH"}, {"TILT CENTER", "PRESS > TO RE-CENTRE THE TILT"}, {"REV SIZE", "REVERB ROOM SIZE"},
		{"REV DAMP", "DARKER REVERB TAIL"}, {"FX MIX", "HOW MUCH EFFECT"}, {"DRIVE", "DISTORTION TYPE"}, {"DRIVE AMT", "HOW MUCH DISTORTION"},
		{"MASTER", "OUTPUT VOLUME"}, {"ROOT", "KEY OF THE SCALE"}, {"SCALE", "LOCK THE KEYS TO A SCALE"}, {"ARP", "ARPEGGIATOR PATTERN"},
		{"ARP RATE", "ARPEGGIATOR SPEED"}, {"ARP OCTAVES", "OCTAVES THE ARP CLIMBS"}, {"ARP GATE", "ARP NOTE LENGTH"}, {"ARP STRUM", "CHORD ARP: TIME BETWEEN STRINGS"}, {"STRUM DIR", "DOWN, UP, OR BOTH LIKE A GUITAR"}, {"BPM", "TEMPO"},
		{"SEQ GATE", "STEP NOTE LENGTH"}, {"SEQ KEYS", "KEYS PLAY ALONG OR TRANSPOSE"}, {"PAD MORPH", "PADS: SLIDE GLIDES TO THE NEXT CHORD (0 = RETRIGGER)"}, {"ROLL SCALE", "PIANO ROLL: SHOW THE SCALE, OR ONLY ITS NOTES"}, {"STRUM NOTES", "PADS STRUM STRIP: CHORD TONES OR THE SCALE"},
		{"SWING", "SHUFFLE FEEL"},
		{"VIS MODE", "VISUALIZER STYLE"}, {"VIS OSC", "OSCILLATOR SHOWN"}, {"OVERLAY", "SHOW TEXT OVER THE VISUALIZER"},
		{"THEME", "COLOUR SCHEME"},
		{"TAPE > TABLE", "PRESS >: THE LAST TAPE TRACK BECOMES A WAVETABLE"}, {"KIT > TABLE", "PRESS >: THE DRUM KIT BECOMES A WAVETABLE (8 DRUMS)"}, {"ECHO WOW", "CHIP ECHO SPEED DRIFTS: REPEATS BEND"}, {"CHIP > FX", "SUB, CHIP, DRUMS THROUGH THE FILTER + MASTER FX"}, {"CHIP CUTOFF", "LOW PASS ON THE CHIP PARTS (WITH CHIP > FX)"}, {"CHIP RES", "RESONANCE OF THAT FILTER"},
		{"MIC > TABLE", "PRESS >, THEN SING 1 SECOND INTO THE MIC"},
		{"CAMERA", "CAMERA AS MOD SOURCES (CAM LIGHT / COLOR)"}, {"CAM > TABLE", "PRESS > TO TURN A PHOTO INTO A WAVETABLE"},
		{"LINK", "SYNC TEMPO WITH ANOTHER SYRUP"},
		{"MACRO 1", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"}, {"MACRO 2", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"},
		{"MACRO 3", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"}, {"MACRO 4", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"},
		{"XY PAD", "WHAT THE TOUCH PAD CONTROLS"}, {"CIRCLE PAD", "DATA: LEFT/RIGHT EDITS THE SELECTED VALUE"}, {"SPEAKER EQ", "AUTO: ON WHEN THE HEADPHONES ARE OUT"},
		{"5 SOURCE", "MOD SLOT 5: WHAT MOVES IT"}, {"5 DEST", "MOD SLOT 5: WHAT IT MOVES"}, {"5 AMOUNT", "MOD SLOT 5: HOW MUCH"}, {"6 SOURCE", "MOD SLOT 6: WHAT MOVES IT"}, {"6 DEST", "MOD SLOT 6: WHAT IT MOVES"}, {"6 AMOUNT", "MOD SLOT 6: HOW MUCH"}, {"7 SOURCE", "MOD SLOT 7: WHAT MOVES IT"}, {"7 DEST", "MOD SLOT 7: WHAT IT MOVES"}, {"7 AMOUNT", "MOD SLOT 7: HOW MUCH"}, {"8 SOURCE", "MOD SLOT 8: WHAT MOVES IT"}, {"8 DEST", "MOD SLOT 8: WHAT IT MOVES"}, {"8 AMOUNT", "MOD SLOT 8: HOW MUCH"}, {"1 CURVE", "MOD SLOT 1: RESPONSE CURVE"}, {"2 CURVE", "MOD SLOT 2: RESPONSE CURVE"}, {"3 CURVE", "MOD SLOT 3: RESPONSE CURVE"}, {"4 CURVE", "MOD SLOT 4: RESPONSE CURVE"}, {"5 CURVE", "MOD SLOT 5: RESPONSE CURVE"}, {"6 CURVE", "MOD SLOT 6: RESPONSE CURVE"}, {"7 CURVE", "MOD SLOT 7: RESPONSE CURVE"}, {"8 CURVE", "MOD SLOT 8: RESPONSE CURVE"}, 
		{"MASTER FX", "WHOLE SOUND THROUGH THE EFFECT CHAIN"}, {"MOD FX", "CHORUS, FLANGER, PHASER, COMB, FORMANT"}, {"MOD RATE", "MOD FX SPEED"},
		{"MOD DEPTH", "MOD FX DEPTH / FEEDBACK"}, {"MOD MIX", "MOD FX AMOUNT"}, {"FX COLOR", "COMB PITCH / FORMANT VOWEL (MOD DEST)"},
		{"EQ LOW", "BASS BOOST / CUT"}, {"EQ MID", "MIDS BOOST / CUT"}, {"EQ HIGH", "TREBLE BOOST / CUT"},
		{"DELAY MIX", "TAPE DELAY AMOUNT (ADDS TO FX ECHO)"}, {"DELAY SYNC", "DELAY TIME LOCKED TO THE BPM"}, {"TAPE WOW", "TAPE PITCH WOBBLE"},
		{"TAPE SAT", "TAPE SATURATION ON REPEATS"}, {"REVERB MIX", "REVERB AMOUNT (ADDS TO FX REVERB)"}, {"SHIMMER", "OCTAVE-UP REVERB TAIL"},
		{"FREEZE", "HOLD THE REVERB FOREVER"}, {"OTT", "MULTIBAND UP+DOWN COMPRESSION"}, {"PUMP", "SIDECHAIN DUCKING ON THE BEAT"},
		{"PUMP RATE", "HOW OFTEN IT PUMPS; KICK = SIDECHAIN FROM THE DRUMS"}, {"LOFI BITS", "BIT CRUSH THE WHOLE SOUND"}, {"LOFI RATE", "SAMPLE RATE REDUCTION"},
		{"DRUMS", "DRUM MACHINE ON / OFF"}, {"KIT", "DRUM SOUNDS"}, {"ACCENT", "HOW MUCH LOUDER ACC STEPS HIT"}, {"KICK", "KICK PATTERN"}, {"SNARE", "SNARE PATTERN"}, {"HAT", "HI-HAT PATTERN"},
		{"DRUM LEVEL", "DRUM VOLUME"}, {"KICK TUNE", "KICK PITCH, LIVE, IN SEMITONES"}, {"SNARE TUNE", "SNARE, CLAP, RIM PITCH"}, {"HAT TUNE", "HATS + CRASH PITCH"}, {"KICK CHANCE", "HOW OFTEN EACH KICK PLAYS"}, {"SNARE CHANCE", "HOW OFTEN EACH SNARE PLAYS"}, {"HAT CHANCE", "HOW OFTEN EACH HAT PLAYS: LOWER = LOOSER"}, {"MUTE", "SILENCE ROWS, KEEP THE PATTERN"}, {"DRUM POLY", "REPEATED HITS RING OVER EACH OTHER"}, {"GET WAVETABLES", "PRESS > TO DOWNLOAD THE SYRUP PACK (WI-FI)"}, {"UPDATE SYRUP", "PRESS > TO INSTALL THE NEWEST BUILD (WI-FI)"}, {"NET > TABLE", "PRESS >: RANDOM WEB PAGE BYTES -> WAVETABLE"}, {"GRAINS", "LOOP: PLAY THE SOURCE  KEYS: GRAIN OSCILLATOR"}, {"GRAIN SOURCE", "LOOP RECORDING, OSC A TABLE OR LAST SECOND OF MIC"}, {"CAM WAVE", "LIVE CAMERA TABLE, OR RECORD AN 8 S VIDEO TABLE"}, {"WT SCAN", "SWEEPS OSC A WT POS (1X = TABLE IN 8 S)"}, {"WI-FI MIDI", "PHONE REMOTE, NETWORK MIDI 5004, FM-1 LINK 5008"}, {"NET BUFFER", "FM-1 LINK TIMING: HIGHER = STEADIER, LOWER = SOONER"}, {"BEAT LIGHT", "SCREENS FLASH ON ARP / SEQ STEPS"}, {"TAPE ALIGN", "MS: + PULLS NEW TAKES EARLIER, - LATER"}, {"CLICK", "METRONOME: COUNT-IN / RECORDING / ALWAYS (NEVER ON TAPE)"}, {"CLICK LEVEL", "METRONOME VOLUME"}, {"SEQ RATE", "A STEP IS A 16TH, A 32ND OR AN 8TH"}, {"REC QUANT", "16TH, 8TH, 32ND, OR OFF (KEEPS YOUR TIMING)"}, {"FX ON CORE 2", "MASTER EFFECTS ON THE SECOND CPU CORE (+5 MS)"}, {"DSP GRIT", "DSP INTERPOLATION: LINEAR = DULLER, RAW = ALIASED CRUNCH"}, {"FILTER SCREAM", "LOW PASS RINGS BY ITSELF (NEEDS MASTER FX ON)"}, {"MIX OVERDRIVE", "DSP CHANNELS PAST FULL, CLIPPED AT 16 BITS"}, {"ECHO PITCH", "CHIP ECHO REPLAYED FASTER / SLOWER: REPEATS SHIFT"}, {"EUCLID STEPS", "LENGTH OF EUCLID ROWS: UNDER 16 = POLYRHYTHM"}, {"KICK HITS", "HITS SPREAD EVENLY - SETS KICK TO EUCLID"}, {"KICK ROTATE", "SHIFTS THE KICK EUCLID PATTERN"}, {"SNARE HITS", "HITS SPREAD EVENLY - SETS SNARE TO EUCLID"}, {"SNARE ROTATE", "SHIFTS THE SNARE EUCLID PATTERN"}, {"HAT HITS", "HITS SPREAD EVENLY - SETS HAT TO EUCLID"}, {"HAT ROTATE", "SHIFTS THE HAT EUCLID PATTERN"},
		{"TRAILS", "VISUALIZER LEAVES TRAILS"}, {"PARTICLES", "SPARKS ON NOTES AND BEATS"}, {"SCANLINES", "CRT SCANLINES ON THE TOP SCREEN"},
	};
	for (unsigned i = 0; i < sizeof(H) / sizeof(H[0]); i++) if (!strcmp(H[i][0], n)) return H[i][1];
	return "";
}

// ---- Pip-Boy tabs: main sections with sub-pages
static const char* SECT_NAMES[7] = {"PATCH", "OSC", "SHAPE", "FX", "SEQ", "VIS", "HW"};
static const int PAGE_SECT[NPAGES] = {0, 1, 1, 1, 2, 2, 3, 4, 5, 6, 3, 4};
static const char* SUB_NAMES[NPAGES] = {"PRESETS", "OSC A", "OSC B", "SUB+NOISE", "FILTER+ENV", "LFO+MOD", "VOICE+FX", "ARP+SEQ", "VISUALIZER", "HARDWARE", "MASTER FX", "DRUMS"};

static const SVoice* newestVoice(void);
static float gPosEff[2];
static int visActive(int page);
static void drawDrumsPanel(void);
static void drawMasterPanel(void);
static void drawHardwarePanel(void);
static char gMsg[48]; static float gMsgT;
static int cpuPct(void) {   // second core's load over the last second
	static u64 lb = 0, lt = 0; static int pct = 0; u64 nt = svcGetSystemTick();
	if (nt - lt > SYSCLOCK_ARM11) { u64 b = gCore1Busy; pct = lt ? (int)((b - lb) * 100 / (nt - lt)) : 0; lb = b; lt = nt; }
	return pct;
}
static int paramDst(const void* p) {   // the matrix destination that moves this param, or -1
	if (p == &gCutoff) return DST_CUTOFF; if (p == &gRes) return DST_RES;
	if (p == &gOsc[0].pos) return DST_APOS; if (p == &gOsc[1].pos) return DST_BPOS;
	if (p == &gOsc[0].warpAmt) return DST_AWARP; if (p == &gOsc[1].warpAmt) return DST_BWARP;
	if (p == &gOsc[0].level) return DST_ALEV; if (p == &gOsc[1].level) return DST_BLEV;
	if (p == &gFxMix) return DST_FXMIX;
	return -1;
}
static const char* SET_PATH = "sdmc:/3ds/syrup/settings.bin";
static void settingsSave(int page) { FILE* f = safeOpen(SET_PATH); if (!f) return; int d[9] = {0x53455432, page, gStickMode, gSpkEq, gBright, gSeqRate, gTheme, gXyGlide, gN3}; safeClose(f, SET_PATH, fwrite(d, sizeof(d), 1, f) == 1); }
static int settingsLoad(void) {   // returns the page you were on last time
	FILE* f = safeRead(SET_PATH); int d[4] = {0}, pg = 0; if (!f) return pg;   // first run: the browser
	if (fread(d, sizeof(d), 1, f) == 1 && (d[0] == 0x53455431 || d[0] == 0x53455432)) {
		if (d[1] >= 0 && d[1] < NPAGES && d[1] != 8) pg = d[1]; gSpkEq = d[3] >= 0 && d[3] < 3 ? d[3] : 2;
		gStickMode = d[0] == 0x53455432 && d[2] >= 0 && d[2] < 4 ? d[2] : SM_DATA;   // older settings: the new default, DATA
		int b = 0; if (fread(&b, sizeof(b), 1, f) == 1 && b >= 0 && b < 6) gBright = b;   // older settings: no brightness, SYSTEM
		if (fread(&b, sizeof(b), 1, f) == 1 && b >= 0 && b < 3) gSeqRate = b;
		if (fread(&b, sizeof(b), 1, f) == 1 && b >= 0 && b < NTHEMES) gTheme = b;   // the theme you picked stays picked
		if (fread(&b, sizeof(b), 1, f) == 1 && b >= 0 && b < 4) gXyGlide = b;
		if (fread(&b, sizeof(b), 1, f) == 1 && b >= 0 && b < 2) gN3 = b;
	}
	fclose(f); return pg;
}
static int skipRecent(void) {   // a clock skip in the last 3 s (the count itself only ever grows)
	static int last = 0; static u64 t = 0; u64 now = svcGetSystemTick();
	if (gClockSkips != last) { last = gClockSkips; t = now ? now : 1; }
	return t && now - t < SYSCLOCK_ARM11 * 3;
}
static int gPageMarkDue = 0, gSeqSaveDue = 0;   // SD writes the main loop makes once it has let go of the voice lock
static const Param* gStickP = NULL;   // the param stickNudge last moved (for the value pop-up)
static void pagePair(int page, float** px_, float** py_) {   // the two values a stick moves on each page
	switch (page) {
		case 1: case 2: *px_ = &gOsc[page - 1].pos; *py_ = &gCutoff; break;
		case 5: case 0: *px_ = &gMacro[0]; *py_ = &gMacro[1]; break;
		case 6: *px_ = &gFxMix; *py_ = &gChorus; break;
		case 7: *px_ = &gSwing; *py_ = &gSeqGate; break;
		case 10: *px_ = &gDlyMix; *py_ = &gRevMix; break;
		default: *px_ = &gCutoff; *py_ = &gRes; break;
	}
}
static int stickNudge(float* v, float d) {   // move a float param by d of its range, clamped; 1 = changed
	if (!v || !d) return 0;
	for (int pg = 0; pg < NPAGES; pg++) for (const Param* p = PAGES[pg]; p->name; p++) if (p->p == v && p->type == PT_F) {
		float r = p->mx - p->mn, n = *v + d * r; n = n < p->mn ? p->mn : n > p->mx ? p->mx : n;
		gStickP = p;
		if (n != *v) { *v = n; gEdited = 1; return 1; }
		return 0;
	}
	gStickP = NULL;
	float n = *v + d; n = n < 0 ? 0 : n > 1 ? 1 : n; if (n != *v) { *v = n; return 1; } return 0;
}
static int gLatchShow = 0;
static void drawHeader(int page) {
	text(4, 3, "SYRUP", 1, TXT);
	rect(0, 13, 400, 1, LINE);
	int x = 58, sect = PAGE_SECT[page];
	for (int i = 0; i < 7; i++) {
		int w = strlen(SECT_NAMES[i]) * 8;
		if (i == sect) {   // the active tab breaks the line, Pip-Boy style
			rect(x - 4, 13, w + 8, 1, BG);
			rect(x - 4, 7, 1, 7, LINE); rect(x + w + 3, 7, 1, 7, LINE);
			rect(x - 4, 7, 3, 1, LINE); rect(x + w + 1, 7, 3, 1, LINE);
			text(x, 3, SECT_NAMES[i], 1, TXT);
		} else text(x, 3, SECT_NAMES[i], 1, DIM);
		x += w + 11;
	}
	char pn[16]; snprintf(pn, sizeof(pn), "%.9s", gPatchName);
	text(396 - (int)strlen(pn) * 8, 3, pn, 1, VAL);
	// sub-pages of this section
	int sx = 8;
	for (int q = 0; q < NPAGES; q++) { int p = NAV[q]; if (PAGE_SECT[p] != sect) continue;
		int w = strlen(SUB_NAMES[p]) * 8;
		if (p == page) { rect(sx - 3, 16, w + 6, 11, HI); text(sx, 18, SUB_NAMES[p], 1, HITXT); }
		else text(sx, 18, SUB_NAMES[p], 1, DIM);
		sx += w + 16;
	}
	// status, right side of the sub-tab row: tempo, notes playing, CPU, transport
	char st[40]; int voices = 0;
	for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started && !gV[i].isEcho) voices++;
	snprintf(st, sizeof(st), "%s%s%d BPM %dV %d%%", gDry ? "DRY " : "", gLatchShow ? "LATCH " : "", gBPM, voices, cpuPct());
	if (gSeqPlaying) strcat(st, " >");
	int stx = 396 - (int)strlen(st) * 8;
	if (stx > sx) text(stx, 18, st, 1, (skipRecent() || gLatchShow || gDry) ? WARN : DIM);
	if (gEdited) { char pn[16]; snprintf(pn, sizeof(pn), "%.9s", gPatchName); int nx = 396 - (int)strlen(pn) * 8; rect(nx - 2, 1, (int)strlen(pn) * 8 + 4, 11, WARN); text(nx, 3, pn, 1, BG); }   // edited: the name turns to the warning colour
}
static void drawFooter(int page, int sel) {
	rect(0, 226, 400, 1, LINE);
	if (!gAudioOK) {   // no DSP firmware dump yet: say exactly how to make one (Luma's Rosalina menu works right here)
		static const char* L[] = {"NO SOUND YET: ONE-TIME SETUP", "", "THE 3DS SOUND CHIP NEEDS ITS FIRMWARE", "COPIED TO THE SD CARD (NINTENDO'S, SO", "SYRUP CAN'T INCLUDE IT). TO MAKE IT:", "",
			"1. PRESS L + DOWN + SELECT (ROSALINA)", "2. MISCELLANEOUS OPTIONS >", "   DUMP DSP FIRMWARE", "3. PRESS B, THEN QUIT AND REOPEN SYRUP", "", "(SYRUP TRIED TO DO THIS ITSELF AND COULDN'T)"};
		int n = sizeof(L) / sizeof(L[0]) - (gDspAuto == -1 ? 0 : 2), h = n * 12 + 16, y0 = 113 - h / 2;   // the last line only if Syrup tried
		rect(40, y0, 320, h, BG); rect(40, y0, 320, 1, WARN); rect(40, y0 + h - 1, 320, 1, WARN); rect(40, y0, 1, h, WARN); rect(359, y0, 1, h, WARN);
		for (int i = 0; i < n; i++) text(200 - (int)strlen(L[i]) * 4, y0 + 8 + i * 12, L[i], 1, i == 0 ? WARN : TXT);
		text(4, 230, "NO SOUND - DSP FIRMWARE NOT DUMPED (SEE ABOVE)", 1, WARN); return; }
	if (gSafeMode) { char m[60]; snprintf(m, sizeof(m), "SAFE MODE (THIS START ONLY): LAST RUN STOPPED AT %.14s", gCrashStage); text(4, 230, m, 1, WARN); return; }
	if (gMsgT > 0) { text(4, 230, gMsg, 1, VAL); return; }
	if (page == 0) { text(4, 230, "TAP A PATCH BELOW   D-PAD WORKS TOO", 1, DIM); return; }
	const char* h = paramHelp(PAGES[page][sel].name);
	char m[52]; snprintf(m, sizeof(m), "%.49s", *h ? h : "D-PAD: PICK / CHANGE   X/Y: PAGES");
	text(4, 230, m, 1, DIM);
}

static void drawTop(int page, int sel, int oct, int latch) {
	gLatchShow = latch;
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	if (page == 8 && visActive(8)) return;   // the GPU frame covers the whole screen; overlay is drawn after the copy
	rect(0, 0, 400, 240, BG);
	drawHeader(page);
	int focus = (page == 2) ? 1 : 0;
	if (page == 8) {
		text(96, 100, gGpuHung ? "GPU HUNG - NO VISUALIZER" : "VISUALIZER NEEDS THE GPU", 1, WARN);
		drawWave(gOsc[0].mips.lev[2], LEVN[2], 4, 120, 392, 100, gOsc[0].on);
		drawFooter(page, sel);
		scanlines();
		return;
	}
	if (page == 9) drawHardwarePanel();
	else if (page == 10) drawMasterPanel();
	else if (page == 11) drawDrumsPanel();
	else if (page == 0) {   // preset browser: styles | patches
		frame(4, 30, 196, 194);
		for (int k = 0; k < NVSTY; k++) {
			int y = 34 + k * 10, i = VSTY[k];
			if (i == gStyle && !gBrFocus) { rect(7, y - 2, 74, 10, HI); text(10, y, STYLE_NAMES[i], 1, HITXT); }
			else if (i == gStyle) { outline(7, y - 2, 74, 10, HI); text(10, y, STYLE_NAMES[i], 1, VAL); }   // (the other list has the D-pad)
			else text(10, y, STYLE_NAMES[i], 1, DIM);
		}
		rect(84, 33, 1, 186, LINE);
		int user = gStyle == NSTYLES - 1;
		for (int i = 0; i < presetCount(gStyle) + user; i++) {
			int y = 34 + i * 11, cur = i == gPreset;
			char nm[16];
			if (user && i == NUSER) snprintf(nm, sizeof(nm), "INIT PATCH");
			else if (user && gUser[i].magic != PATCH_MAGIC) snprintf(nm, sizeof(nm), "%d  - EMPTY -", i + 1);
			else snprintf(nm, sizeof(nm), "%.13s", presetName(gStyle, i));
			if (cur && gBrFocus) { rect(87, y - 2, 110, 11, HI); text(90, y, nm, 1, HITXT); }
			else if (cur) { outline(87, y - 2, 110, 11, HI); text(90, y, nm, 1, VAL); }
			else text(90, y, nm, 1, TXT);
		}
		char cnt[40]; snprintf(cnt, sizeof(cnt), "%d PATCHES%s", presetCount(gStyle), gEdited ? " *" : ""); text(10, 212, cnt, 1, gEdited ? VAL : DIM);   /* (the build number is on the start screen and in HELP) */
	} else if (page == 7) {   // scale / arp / sequencer
		char s[40];
		frame(4, 30, 196, 194);
		snprintf(s, sizeof(s), "%s %s", NOTE_NAMES[gRoot], SCALE_NAMES[gScale]); text(10, 36, s, 1, VAL);
		snprintf(s, sizeof(s), "ARP %s %s X%d", ARP_NAMES[gArp], RATE_NAMES[gArpRate], gArpOct); text(10, 48, s, 1, TXT);
		snprintf(s, sizeof(s), "%d BPM  SWING %d%%", gBPM, (int)(gSwing * 100 + 0.5f)); text(10, 60, s, 1, TXT);
		snprintf(s, sizeof(s), "PAT %c  %s", 'A' + gPatCur, gSeqPlaying ? "PLAYING" : "STOPPED"); text(10, 72, s, 1, gSeqPlaying ? VAL : DIM);
		if (gSongOn && gSongLen) { snprintf(s, sizeof(s), "SONG %d/%d", gSongPos + 1, gSongLen); text(10, 84, s, 1, VAL); }
		text(10, 98, "ROLL: BOTTOM SCREEN", 1, RGB3(TH->dim));
		gestureDemo(14, 114, 1);
	} else if (page == 1 || page == 2) {
		Osc* o = &gOsc[focus];
		if (!visActive(page)) {
			drawWave(o->mips.lev[2], LEVN[2], 4, 30, 196, 100, o->on);
			drawStack(o, gPosEff[focus], 8, 208, 150, 20);
		}
		text(8, 34, o->on ? (focus ? "OSC B" : "OSC A") : (focus ? "OSC B OFF" : "OSC A OFF"), 1, DIM);
		char tn[26]; snprintf(tn, sizeof(tn), "%.24s", o->tb.name); text(6, 214, tn, 1, VAL);
	} else {
		if (page == 4) { if (!visActive(page)) drawEnvelopesCPU(); }
		else if (page == 5) { if (!visActive(page)) drawLfosCPU(); }
		else {
			if (!visActive(page)) {
				drawWave(gOsc[0].mips.lev[2], LEVN[2], 4, 30, 196, 60, gOsc[0].on);
				drawWave(gOsc[1].mips.lev[2], LEVN[2], 4, 94, 196, 60, gOsc[1].on);
			}
			text(8, 34, "A", 1, DIM); text(8, 98, "B", 1, DIM);
		}
		if (page == 4) drawFilterPlot(4, 158, 196, 66);   // FILT+ENV: the filter response instead of the status lines
		else {
		char s[40];
		snprintf(s, sizeof(s), "%s  OCT %+d%s", VOICE_MODES[gVoiceMode], oct, latch ? "  LATCH" : ""); text(6, 160, s, 1, TXT);
		snprintf(s, sizeof(s), "CHORD %s  FX %s", CHORD_NAMES[gChord], FX_NAMES[gFx]); text(6, 172, s, 1, TXT);
		int voices = 0, dsp = 0, cs = 0, csTot = 0;
		for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started && !gV[i].isEcho) voices++;
		for (int c = 0; c < NCH; c++) if (gDspOwner[c] >= 0 || (gNoiseOn && c == NOISE_CH)) dsp++;
		for (int c = 0; c < NCS; c++) if (csUsable(c)) { csTot++; if (gCsOwner[c] >= 0) cs++; }
		snprintf(s, sizeof(s), "NOTES %d  FX BUS %s", voices, busActive() ? (gBusSeen ? "OK" : "ON") : (gBusFail ? "FALLBK" : "OFF")); text(6, 186, s, 1, DIM);
		{ u32 dr = gAudioOK ? ndspGetDroppedFrames() : 0;
			snprintf(s, sizeof(s), "DSP %d/24  CSND %d/%d%s", dsp, cs, csTot, dr ? " DROP" : skipRecent() ? " SKIP" : ""); text(6, 198, s, 1, (dr || skipRecent()) ? WARN : DIM); }
		if (gGpuHung) { snprintf(s, sizeof(s), "GPU HUNG AFTER %d FRAMES", gGpuFrames); text(6, 210, s, 1, WARN); }
		else {
			int pct = cpuPct();
			if (gWorker) snprintf(s, sizeof(s), "CORE 2 %d%%  FX %s", pct, (!gFxCore2 || (gCore2Lim && gCore2Lim < 50)) ? "CORE 1" : (!(gFxThread || gFxOnWorker) || gFxC2Fail) ? "FELL BACK" : gFxThread ? "CORE 3" : "ON CORE 2");
			if (gWorker) text(6, 210, s, 1, DIM);   // (no second core: nothing to say, everything runs on the first)
		}
		}
	}
	if (page == 0) {   // browser info: what's loaded and how to drive the list
		frame(204, 30, 194, 194);
		text(212, 38, "NOW PLAYING", 1, DIM);
		int sc = strlen(gPatchName) <= 11 ? 2 : 1;
		text(212, 52, gPatchName, sc, VAL);
		char s2[40]; snprintf(s2, sizeof(s2), "%s  %d / %d", STYLE_NAMES[gStyleCur], gPresetCur + 1, presetCount(gStyleCur));
		text(212, 76, s2, 1, TXT);
		if (gEdited) text(212, 88, "* EDITED (SAVE IT?)", 1, WARN);
		rect(208, 104, 186, 1, LINE);
		text(212, 112, "TAP A PATCH BELOW", 1, TXT);
		text(212, 126, "TAP A STYLE TO SEE IT", 1, TXT);
		text(212, 140, "SAVE TWICE: KEEP IT", 1, TXT);
		text(212, 162, "UNDO A PATCH CHANGE:", 1, DIM);
		text(212, 176, "SELECT + L", 1, DIM);
		text(212, 192, "D-PAD WORKS TOO", 1, DIM);
		drawFooter(page, sel);
		if (!visActive(page)) scanlines();
		return;
	}
	// parameter list: inverse bar on the selected row, meters for continuous values, < > on choices
	Param* P = PAGES[page]; int n = pageLen(page);
	int rows = 15, first = sel - rows + 1; if (first < 0) first = 0;
	int y = 33;
	frame(204, 30, 194, 194);
	for (int i = first; i < n && i < first + rows; i++, y += 12) {
		char v[24]; paramFmt(&P[i], v, sizeof(v));
		int cur = i == sel, w = strlen(v) * 8;
		if (cur) rect(206, y - 2, 190, 11, HI);
		if (cur) text(208, y, P[i].name, 1, HITXT); else text(208, y, P[i].name, 1, DIM);
		if (P[i].type == PT_F) {   // 8-block meter
			float t = (*(float*)P[i].p - P[i].mn) / (P[i].mx - P[i].mn + 1e-9f);
			int blocks = (int)(t * 8 + 0.5f);
			for (int b = 0; b < 8; b++) {
				int bx = 300 + b * 4;
				if (b < blocks) { if (cur) rect(bx, y, 3, 7, HITXT); else rect(bx, y, 3, 7, DIM); }   // unselected rows dimmer: the selected one stands out
				else { if (cur) rect(bx, y + 6, 3, 1, HITXT); else rect(bx, y + 6, 3, 1, DIM); }
			}
			if (w > 54) w = 54;
		}
		if (cur && TH->ega && P[i].type == PT_E && P[i].names != PRESS_RIGHT) { char b2[28]; snprintf(b2, sizeof(b2), "<%s>", v); int bx = 394 - (int)strlen(b2) * 8; text(bx, y, b2, 1, HITXT); text(bx - 1, y, b2, 1, HITXT); }
		else if (cur && TH->ega) { text(394 - w, y, v, 1, HITXT); text(393 - w, y, v, 1, HITXT); }   // the selected value, bold
		else if (cur) { text(394 - w, y, v, 1, VAL); text(393 - w, y, v, 1, VAL); }
		else text(394 - w, y, v, 1, VAL);
	}
	if (first > 0) text(388, 30, "^", 1, TXT);
	if (first + rows < n) text(388, 216, "v", 1, TXT);
	drawFooter(page, sel);
	if (!visActive(page)) scanlines();
}

static int gHelpSlide = 0;
#define NHELP 4
static void drawHelp(void) {   // L / R flip the slides; any other button closes
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 0, 400, 240, BG);
	static const char* T[NHELP] = {"BUTTONS", "TOUCH", "SEQUENCER", "SOUND"};
	char h[40]; snprintf(h, sizeof(h), "HELP %d/%d: %s", gHelpSlide + 1, NHELP, T[gHelpSlide]);
	text(4, 3, h, 1, TXT); rect(0, 13, 400, 1, LINE);
	text(400 - 8 * 21, 3, "L/R: MORE  ANY: CLOSE", 1, DIM);
	static const char* S[NHELP][18] = {
		{"PAGES",
		 "  X / Y ........ NEXT / PREVIOUS PAGE",
		 "  D-PAD U/D .... PICK A CONTROL",
		 "  D-PAD L/R .... CHANGE IT (HOLD = FAST)",
		 "BUTTONS",
		 "  A ............ CHORD MODE",
		 "  B ............ LATCH (NOTES KEEP RINGING)",
		 "  L / R ........ OCTAVE DOWN / UP",
		 "  CIRCLE PAD ... BEND + THE STICK SOURCE",
		 "  START ........ MENU (SAVE, PRESETS, QUIT)",
		 "SELECT",
		 "  TAP .......... PLAY / STOP (SEQ + DRUMS)",
		 "  + L / + R .... UNDO / REDO",
		 "  + DOWN ....... PANIC: EVERYTHING STOPS", NULL},
		{"BOTTOM SCREEN",
		 "  TABS ......... OSC FILT MOD VOICE FX SEQ PADS",
		 "                 (TAP AGAIN FOR THE NEXT VIEW)",
		 "  XY PAD ....... WHAT IT MOVES IS WRITTEN ON IT",
		 "  ROW .......... OCT- OCT+ SCALE LATCH CHORD",
		 "                 PLAY PANIC",
		 "  KEYS ......... PLAY; LOWER ON A KEY = LOUDER",
		 "PADS",
		 "  LOW = TRIAD, MIDDLE = 7TH, TOP = 9TH",
		 "  LEFT EDGE = INVERSION, RIGHT EDGE = SUS4",
		 "  STRUM STRIP: SLIDE TO STRUM THE CHORD", NULL},
		{"PIANO ROLL (SEQ TAB)",
		 "  TAP .......... A NOTE IN / OUT",
		 "  DRAG ......... A LONGER NOTE",
		 "  HOLD ......... STEP LOCKS + RHYTHM ROWS",
		 "  KEYS (LEFT) .. DRAG TO SCROLL; L/R = OCTAVE",
		 "  RIFF ......... A NEW LINE IN THIS OCTAVE",
		 "  REC .......... KEYS WRITE STEPS",
		 "  PAT .......... PATTERNS, SONG, TOOLS, UNDO",
		 "MENU",
		 "  CLEAR ........ PRESS > TWICE",
		 "  WHILE IT PLAYS: KEYS TRANSPOSE", NULL},
		{"SOUND",
		 "  OWN WAVETABLES: SERUM .WAV IN /3DS/SYRUP/",
		 "  MOD SOURCES: TILT, SPIN, BREATH (MIC), CAMERA",
		 "  DRUMS + TAPE PLAY ON THE SECOND SOUND CHIP",
		 "  TAPE REC TURNS CHIP > FX ON SO DRUMS RECORD",
		 "NO SOUND?",
		 "  THE DSP FIRMWARE: HOLD L + DOWN + SELECT,",
		 "  MISCELLANEOUS OPTIONS, DUMP DSP FIRMWARE",
		 "STUCK NOTES",
		 "  SELECT + DOWN, OR PANIC ON THE BOTTOM ROW",
		 "MIDI LEARN",
		 "  PICK A CONTROL, START > MIDI LEARN, TURN A KNOB", NULL},
	};
	int y = 22;
	for (int i = 0; S[gHelpSlide][i]; i++, y += 11) {
		const char* l = S[gHelpSlide][i];
		if (l[0] != ' ') text(8, y, l, 1, VAL); else text(8, y, l, 1, TXT);
	}
	scanlines();
}

#define TAB_H 20
#define XY_Y  22
#define XY_H  94
#define PAD_ROW ((240 - KB_Y) / 2)   // drum pad row height
#define BLK_H 58                     // black key height
#define BLK_GRACE 3                  // extra touch width each side of a black key (more and D / G / A lose their top)
#define KEY_HOLD 3                   // a held key keeps the stylus until it is this far into another key
#define WORK_B 118   // bottom of the work area; the PERF row sits under it, then the keyboard
#define KB_Y  136
#define KB_X  36    // the keys start here: OCT+ / OCT- / CHORD / LATCH stand in a column to their left
#define KB_WW ((320.0f - KB_X) / 14)
#define SIDE_H ((240 - KB_Y) / 4)
#define NDRUM 10
static volatile float gPadFlash[NDRUM];   // drum pads light when they're hit: taps, MIDI and the drum machine
static const char* kitPadName(int d); static int kitPadNote(int d); static int kitPadOrder(int i);
static int keyAt(int x, int y, int base) {
	x -= KB_X;
	if (gVoiceMode >= 3) {   // drum pads: 2 rows of 5
		int c = x / 57, r = y < KB_Y + PAD_ROW ? 0 : 1; if (c > 4) c = 4; if (c < 0) c = 0;
		return kitPadNote(kitPadOrder(r * 5 + c));
	}
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};
	float ww = KB_WW;
	if (gScale) { int i = (int)(x / ww); if (i > 13) i = 13; if (i < 0) i = 0; gKeyDeg = i; return scaleNote(base + gRoot, i); }
	if (y < KB_Y + BLK_H + 2)
		for (int i = 0; i < 14; i++) {
			if (!BL[i % 7]) continue;
			int bx = (int)((i + 1) * ww - ww * 0.3f);
			// the touch area is wider than the drawn key (resistive screen + stylus): BLK_GRACE px into the white keys each side
			if (x >= bx - BLK_GRACE && x < bx + (int)(ww * 0.6f) + BLK_GRACE) return base + (i / 7) * 12 + WN[i % 7] + 1;
		}
	int i = (int)(x / ww); if (i > 13) i = 13; if (i < 0) i = 0;
	return base + (i / 7) * 12 + WN[i % 7];
}

static void outline(int x, int y, int w, int h, u8 r, u8 g, u8 b);
static void drawAdd(void);
static void addTouch(int x, int y);
static void addApply(void);
static void say(const char* m);
// ---- step grid on the bottom screen (PLAY page): 16 steps x 8 notes of the current scale, tap to place
static int gGridOn = 0, gGridOct = 0;
static float gAuditionT = 0;
#define GRID_TOP (XY_Y + 16)
#define GRID_ROWH 10
static int gridNote(int row) {   // row 0 = bottom; follows the scale (major steps when the scale is off)
	static const int MAJ[8] = {0, 2, 4, 5, 7, 9, 11, 12};
	int root = 48 + gRoot + 12 * gGridOct;
	return gScale ? scaleNote(root, row) : root + MAJ[row];
}
static void rectP(int x, int y, int w, int h, const u8* c) { rect(x, y, w, h, c[0], c[1], c[2]); }
#define GRID_BTNS 8
static int gPatView = 0, gStepEd = -1, gStepEdRow = 0;
static int gUndoOk = 0, gUndoIdx = 0;   // the UNDO button on PAT: the newest history step is a pattern edit
// HISTORY: SELECT+L undo, SELECT+R redo. Pattern edits (notes, RIFF, CLEAR, the tools) and patch edits
// (a run of turns on one control is one step) go on one list, newest last.
#define HIST 12
typedef struct { int kind, idx; union { SeqPat pat; SPatch patch; u8 drums[2048]; } u; } HistE;   /* drums: the whole grid, 2016 bytes */   // kind 0: pattern idx, 1: the patch, 2: the drum grid
static void drumSnapGet(u8* o); static void drumSnapSet(const u8* in);
static HistE gUndoH[HIST], gRedoH[HIST], gHistTmp; static int gUndoN = 0, gRedoN = 0;
static void histPush(HistE* st, int* n, const HistE* e) { if (*n == HIST) { memmove(st, st + 1, sizeof(HistE) * (HIST - 1)); (*n)--; } st[(*n)++] = *e; }
static void histSnap(HistE* e, int kind, int idx) { e->kind = kind; e->idx = idx; if (!kind) { patStore(); e->u.pat = gPat[idx]; } else if (kind == 2) drumSnapGet(e->u.drums); else patchCapture(&e->u.patch); }
static void histFlags(void) { gUndoOk = gUndoN > 0 && gUndoH[gUndoN - 1].kind == 0; if (gUndoOk) gUndoIdx = gUndoH[gUndoN - 1].idx; }
static void undoMark(void) { static HistE e; histSnap(&e, 0, gPatCur); histPush(gUndoH, &gUndoN, &e); gRedoN = 0; histFlags(); }
static void patchMark(const void* ctl) {   // before a patch edit; turns of the same control within 1.5 s are one step
	static const void* lastCtl = NULL; static u64 lastT = 0; u64 t = osGetTime();
	if (ctl != lastCtl || t - lastT > 1500) { static HistE e; histSnap(&e, 1, 0); histPush(gUndoH, &gUndoN, &e); gRedoN = 0; histFlags(); }
	lastCtl = ctl; lastT = t;
}
static void patApplyWorking(int i) {   // pattern i becomes the working pattern (no store first)
	gPatCur = i; SeqPat* p = &gPat[i]; memcpy(gSeq, p->st, sizeof(gSeq)); gSeqLen = p->len < 1 ? 16 : p->len > NSTEP ? NSTEP : p->len; gSeqBase = p->base;
	memcpy(gRowLen, p->rowLen, 8); memcpy(gRowHits, p->rowHits, 8); memcpy(gRowRot, p->rowRot, 8); memcpy(gRowNote, p->rowNote, 8);
	if (gSeqPos >= gSeqLen) gSeqPos = 0;
}
static void patchApply(const SPatch* p);
static int histStep(int undo) {   // 0 nothing to do, 1 pattern, 2 patch, 3 drums
	HistE *from = undo ? gUndoH : gRedoH, *to = undo ? gRedoH : gUndoH; int *fn = undo ? &gUndoN : &gRedoN, *tn = undo ? &gRedoN : &gUndoN;
	if (!*fn) return 0;
	static HistE e; e = from[--*fn];
	histSnap(&gHistTmp, e.kind, e.idx); histPush(to, tn, &gHistTmp);
	if (!e.kind) { gPat[e.idx] = e.u.pat; patApplyWorking(e.idx); } else if (e.kind == 2) drumSnapSet(e.u.drums); else patchApply(&e.u.patch);
	histFlags(); return 1 + e.kind;
}
static int gLvN = 0, gLvNote[4], gLvStep = 0; static float gLvT0 = 0;   // the notes being held while recording live
static void liveRecToggle(void); static void liveRecStop(void); static void liveDown(const int* notes, int n, float vel); static void liveUp(void); static void liveAdd(const int* notes, int n, float vel);
static int gDgEd = 0, gDgShift = 0; static void drawDgEd(void); static void drawDrumGrid(void); static int drumGridTouch(int x, int y, int press); static int drumGridRelease(void);
static void drawPatView(void); static void drawTapeView(void); static int tapeTouch(int x, int y, int press, int release); static void tapeSeqStarted(void);
static void drawStepEd(void);
// PIANO ROLL (SEQ page, the whole bottom screen): pitch up the side with a keyboard, the 16 steps across.
// Tap an empty cell: a note. Drag from it: a longer note. Tap a note: gone. Drag a note's tail: its length.
// Hold a cell: the step editor. Drag the keys up / down to scroll; OCT- / OCT+ jump an octave.
#define ROLL_KW (gVoiceMode >= 3 ? 58 : 24)   // a kit patch: wider, the drum names down the side
#define ROLL_TOP (XY_Y + 16)
#define ROLL_RH 13
#define VEL_Y (ROLL_TOP + ROLL_ROWS * ROLL_RH + 3)   // the velocity lane under the roll: a bar per step, drag to set
#define VEL_H (238 - VEL_Y - 13)   // (under it: the PAGE strip)
#define PG_Y (VEL_Y + VEL_H + 2)
#define ROLL_ROWS 12   // one octave on screen, taller rows (the keys drag to scroll; L / R jump an octave)
#define ROLL_VIEW 16   // steps on screen: one page
#define ROLL_CW ((320 - ROLL_KW) / (float)ROLL_VIEW)
static int gRollLow = 45;   // lowest pitch on screen
static int gRollPg = 0, gRollFollow = 1;   // the page shown (16 steps each); FOLLOW: it turns with the playhead
#define RP0 (gRollPg * ROLL_VIEW)
static int rollX(int st) { return ROLL_KW + (int)((st - RP0) * ROLL_CW); }
static int rollIn(int st) { return st >= RP0 && st < RP0 + ROLL_VIEW; }
// KIT patches: one row per drum (kick at the bottom), named; other patches: chromatic from gRollLow
// ROLL SCALE: SHOW lights the scale's rows (the rest dark, the root marked); FOLD shows only the scale's notes
static int rollFold(void) { return gRollScale == 2 && gScale && gVoiceMode < 3; }
static int rollNote(int row) {
	if (gVoiceMode >= 3) return row >= 0 && row < 10 ? kitPadNote(row) : -1;
	if (!rollFold()) return gRollLow + row;
	if (row < 0) return -1;
	int n = gRollLow; while (!inScale(n)) n++;
	for (int r = 0; r < row; r++) { n++; while (!inScale(n)) n++; }
	return n;
}
static int rollRow(int nt) {
	if (gVoiceMode >= 3) { for (int d = 0; d < 10; d++) if (kitPadNote(d) == nt) return d; return -1; }
	if (!rollFold()) return nt >= gRollLow && nt < gRollLow + ROLL_ROWS ? nt - gRollLow : -1;
	if (nt < gRollLow || !inScale(nt)) return -1;
	for (int r = 0; r < ROLL_ROWS; r++) if (rollNote(r) == nt) return r;
	return -1;
}
static int rollY(int nt) { return ROLL_TOP + (ROLL_ROWS - 1 - rollRow(nt)) * ROLL_RH; }
static int isBlack(int nt) { static const u8 B[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0}; return B[nt % 12]; }
static int noteCover(int st, int nt, int* k0) {   // the step whose note nt covers step st, or -1
	for (int s0 = st; s0 >= 0; s0--) { const Step* p = &gSeq[s0]; for (int k = 0; k < p->n; k++) if (p->note[k] == nt && s0 + noteLen(p, k) > st) { if (k0) *k0 = k; return s0; } }
	return -1;
}
// PAGES: a pattern runs up to 64 steps, 4 pages of 16. The strip under the roll: tap a page to see it (one past the
// end makes the pattern that long), tap the page you're on to turn FOLLOW (the view turns with the playhead) on / off,
// COPY> puts this page on the next one (and makes room for it).
#define PG_W ((320 - ROLL_KW - 52 - 34) / 4)
static u64 gRollClrT = 0;   // CLR: the first tap of a double tap
static int rollClrArmed(void) { return gRollClrT && osGetTime() - gRollClrT < 450; }   // a double tap
static void drawPageStrip(void) {
	rectP(0, PG_Y, 320, 240 - PG_Y, TH->bg);
	text(2, PG_Y + 2, gRollFollow ? "F" : "PG", 1, RGB3(gRollFollow ? TH->val : TH->dim));
	int pp = gSeqPlaying && gSeqPos >= 0 ? gSeqPos / ROLL_VIEW : -1, used = (gSeqLen + ROLL_VIEW - 1) / ROLL_VIEW;
	for (int i = 0; i < NSTEP / ROLL_VIEW; i++) {
		int x = ROLL_KW + i * PG_W, cur = i == gRollPg, has = i < used;
		rectP(x + 1, PG_Y, PG_W - 2, 11, cur ? TH->keyOn : has ? TH->tab : TH->bg); if (!has && !cur) outlineP(x + 1, PG_Y, PG_W - 2, 11, TH->grid);
		char t[4]; snprintf(t, sizeof(t), "%d", i + 1); const u8* c = cur ? TH->ink : has ? TH->tabTxt : TH->dim; text(x + PG_W / 2 - 4, PG_Y + 2, t, 1, c[0], c[1], c[2]);
		if (i == pp) rectP(x + 3, PG_Y + 4, 3, 3, cur ? TH->ink : TH->keyOn);   // the playhead is on this page
	}
	int cx = ROLL_KW + 4 * PG_W; rectP(cx + 1, PG_Y, 50, 11, gRollPg < NSTEP / ROLL_VIEW - 1 ? TH->tab : TH->bg); text(cx + 6, PG_Y + 2, "COPY>", 1, RGB3(TH->tabTxt));
	{ int a = rollClrArmed(), x = cx + 53; rectP(x, PG_Y, 320 - x - 1, 11, a ? TH->warn : TH->tab); text(x + (320 - x) / 2 - 12, PG_Y + 2, a ? "ALL" : "CLR", 1, RGB3(a ? TH->ink : TH->tabTxt)); }
}
static void undoMark(void);
static int pageStripTouch(int x) {   // press on the strip
	if (x < ROLL_KW) { gRollFollow = !gRollFollow; say(gRollFollow ? "FOLLOW: THE PAGE TURNS WITH THE PLAYHEAD" : "FOLLOW OFF"); return 1; }
	int i = (x - ROLL_KW) / PG_W;
	if (x >= ROLL_KW + 4 * PG_W + 52) {   // CLR: a double tap clears the pattern
		if (!rollClrArmed()) { gRollClrT = osGetTime(); say("CLR: DOUBLE-TAP TO CLEAR THE PATTERN"); return 1; }
		undoMark(); memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; gRollPg = 0; gRollClrT = 0; say("PATTERN CLEARED (SELECT+L UNDOES)"); return 1;
	}
	if (i >= 4) {   // COPY>
		if (gRollPg >= NSTEP / ROLL_VIEW - 1) { say("THE LAST PAGE"); return 0; }
		undoMark(); int a = RP0, b = a + ROLL_VIEW; memcpy(&gSeq[b], &gSeq[a], sizeof(Step) * ROLL_VIEW);
		if (gSeqLen < b + ROLL_VIEW) gSeqLen = b + ROLL_VIEW; gRollPg++; gRollFollow = 0;
		char m[40]; snprintf(m, sizeof(m), "PAGE %d COPIED TO PAGE %d", gRollPg, gRollPg + 1); say(m); return 1;
	}
	if (i == gRollPg) { gRollFollow = !gRollFollow; say(gRollFollow ? "FOLLOW ON" : "FOLLOW OFF: THIS PAGE STAYS"); return 1; }
	gRollPg = i; if (gSeqPlaying) gRollFollow = 0;
	if (gSeqLen < i * ROLL_VIEW + 1) { gSeqLen = (i + 1) * ROLL_VIEW; char m[32]; snprintf(m, sizeof(m), "PATTERN: %d STEPS", gSeqLen); say(m); }
	return 1;
}
static int gLane = 0;
static const char* LANE_NAMES[5] = {"VEL", "GATE", "PROB", "ACC", "SLD"};
static void outlineP(int x, int y, int w, int h, const u8* c) { outline(x, y, w, h, c[0], c[1], c[2]); }
static int laneGet(const Step* sp, int lane) {   // 0..255
	if (lane == 0) return sp->vel;
	if (lane == 1) { int g = sp->gate ? sp->gate : (int)(gSeqGate * 100); return g * 255 / 100; }
	return (100 - sp->skip) * 255 / 100;
}
static void laneSet(Step* sp, int lane, int v) {   // v 0..255
	if (lane == 0) sp->vel = (u8)(v < 10 ? 10 : v);
	else if (lane == 1) { int g = v * 100 / 255; sp->gate = (u8)(g < 5 ? 5 : g > 100 ? 100 : g); }
	else { int pr = v * 100 / 255; if (pr < 1) pr = 1; sp->skip = (u8)(100 - pr); }
}
static void drawGrid(void) {
	rectP(0, XY_Y, 320, 240 - XY_Y, TH->bg);
	char pb[8]; snprintf(pb, sizeof(pb), "PAT%c", 'A' + gPatCur);
	char rb[8]; if (gPreFor == 2) snprintf(rb, sizeof(rb), "IN %d", (int)(gPreBeats - gPreDone) + 1); else snprintf(rb, sizeof(rb), "REC");
	const char* B[GRID_BTNS] = {gSeqMode == 1 ? "STOP" : "PLAY", pb, "<LEN", "LEN>", gPatView == 3 ? "NOTE" : "DRUM", "TAPE", "RIFF", rb};   // (octaves: L / R, or drag the keys)
	for (int i = 0; i < GRID_BTNS; i++) {
		int x = i * 320 / GRID_BTNS, lit = (i == 0 && gSeqMode == 1) || (i == 1 && gPatView == 1) || (i == 4 && gPatView == 3) || (i == 5 && gPatView == 2) || i == 6 || (i == 7 && (gLiveRec || gPreFor == 2));
		rectP(x + 2, XY_Y, 320 / GRID_BTNS - 4, 14, i == 7 && (gLiveRec || gPreFor == 2) ? TH->warn : lit ? TH->keyOn : TH->tab);
		const u8* tc = lit ? TH->ink : TH->tabTxt;
		text(x + 160 / GRID_BTNS - (int)strlen(B[i]) * 4, XY_Y + 3, B[i], 1, tc[0], tc[1], tc[2]);
	}
	if (gPatView == 3) { gDgShift = 16; drawDrumGrid(); if (gDgEd) drawDgEd(); gDgShift = 0; return; }   // DRUMS, right in the sequencer
	if (gStepEd >= 0) { drawStepEd(); return; }
	if (gPatView == 2) { drawTapeView(); return; }
	if (gPatView) { drawPatView(); return; }
	int below = 0, above = 0;
	for (int r = 0; r < ROLL_ROWS; r++) {   // lanes and the keyboard down the side
		int nt = rollNote(r), y = ROLL_TOP + (ROLL_ROWS - 1 - r) * ROLL_RH, kit = gVoiceMode >= 3;
		if (nt < 0) { rectP(0, y, 320, ROLL_RH - 1, TH->bg); continue; }   // a kit has 10 drums: the top rows stay empty
		int bk = kit ? (r & 1) : isBlack(nt), cr = rowOfNote(nt), out = !kit && gScale && gRollScale && !inScale(nt);
		rectP(ROLL_KW, y, 320 - ROLL_KW, ROLL_RH - 1, out ? TH->bg : !kit && gScale && gRollScale ? TH->pad : bk ? TH->bg : TH->pad);   // SHOW: the scale lit, the rest dark
		if (!kit && gScale && gRollScale && (nt - gRoot) % 12 == 0) rectP(ROLL_KW, y + ROLL_RH - 2, 320 - ROLL_KW, 1, TH->line);   // the root
		for (int st = RP0; st < RP0 + ROLL_VIEW; st++) { int x = rollX(st); if (st >= gSeqLen) rectP(x, y, (int)ROLL_CW + 1, ROLL_RH - 1, TH->bg); rectP(x, y, 1, ROLL_RH - 1, (st & 3) == 0 ? TH->line : TH->grid); }
		int snd = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4 && gV[i].note == nt) { snd = 1; break; }
		rectP(0, y, ROLL_KW - 1, ROLL_RH - 1, snd ? TH->keyOn : bk ? TH->keyB : TH->keyW);   // the side keys play along
		if (kit) { const u8* c = snd ? TH->ink : bk ? TH->keyW : TH->keyB; text(2, y + 3, kitPadName(r), 1, c[0], c[1], c[2]); }
		else if (nt % 12 == 0) { char t[6]; snprintf(t, sizeof(t), "C%d", nt / 12 - 1); text(2, y + 4, t, 1, RGB3(TH->keyB)); }
		if (cr >= 0) rectP(ROLL_KW - 4, y, 3, ROLL_RH - 1, TH->keyOn);   // this pitch runs its own rhythm (step editor > ROW)
	}
	if (curShown() && rollRow(gRcN) >= 0 && rollIn(gRcS)) { int x = rollX(gRcS), y = rollY(gRcN); outline(x, y, (int)ROLL_CW, ROLL_RH - 1, RGB3(TH->keyOn)); }   // the circle pad's cursor
	if (gSeqPlaying && gSeqPos >= 0 && rollIn(gSeqPos)) { int x = rollX(gSeqPos); rectP(x, ROLL_TOP, (int)ROLL_CW, 1, TH->keyOn); rectP(x, ROLL_TOP + ROLL_ROWS * ROLL_RH - 1, (int)ROLL_CW, 1, TH->keyOn); }
	for (int st = 0; st < NSTEP; st++) {   // the notes, as bars as long as they last
		const Step* p = &gSeq[st];
		for (int k = 0; k < p->n; k++) {
			int nt = p->note[k];
			if (rollRow(nt) < 0) { if (gVoiceMode >= 3 || nt < gRollLow) below = 1; else above = 1; continue; }
			if (rowOfNote(nt) >= 0) continue;
			int L = noteLen(p, k), e = st + L > NSTEP ? NSTEP : st + L;
			int a0 = st < RP0 ? RP0 : st, e0 = e > RP0 + ROLL_VIEW ? RP0 + ROLL_VIEW : e; if (a0 >= e0) continue;   // this page's part of it
			int x = rollX(a0) + 1, w = rollX(e0) - x - 1, y = rollY(nt) + 1;
			int play = gSeqPlaying && gSeqPos >= st && gSeqPos < e;
			int vh = 3 + (ROLL_RH - 6) * p->vel / 255;   // velocity: how much of the bar is lit
			rectP(x, y, w, ROLL_RH - 3, TH->grid); rectP(x, y + ROLL_RH - 3 - vh, w, vh, play ? TH->keyOn : st < gSeqLen ? TH->val : TH->dim);
			if (a0 == st) rectP(x, y, 2, ROLL_RH - 3, TH->txt);   // the note's start (on this page)
			if (p->fl & 1) rectP(x + 3, y + 2, 3, ROLL_RH - 7, TH->bg);   // accent
			if (p->fl & 2) rectP(x + w, y + ROLL_RH / 2 - 2, 3, 2, TH->keyOn);   // slide into the next note
			{   // the step's locks as letters inside the note, tracker-style: C cutoff, R res, P pitch, G gate, N nudge, % chance
				char lk[8]; int n = 0;
				if (p->cond) lk[n++] = '?'; if (p->cut) lk[n++] = 'C'; if (p->res) lk[n++] = 'R'; if (p->pitch) lk[n++] = 'P'; if (p->gate) lk[n++] = 'G'; if (p->nudge) lk[n++] = 'N'; if (p->skip) lk[n++] = '%';
				int lx = x + (p->fl & 1 ? 7 : 3), fit = (x + w - lx) / 8; if (n > fit) n = fit; lk[n] = 0;
				if (n > 0) { rectP(lx, y, n * 8, ROLL_RH - 3, TH->txt); text(lx, y + 1, lk, 1, RGB3(TH->bg)); }
			}
		}
		if (!rollIn(st)) continue;
		if (p->gate || p->skip || p->cut || p->res || p->pitch || p->nudge || p->rat > 1 || p->cond) rectP(rollX(st) + (int)ROLL_CW / 2 - 1, ROLL_TOP - 2, 3, 2, TH->keyOn);   // the step has locks
		if (p->rat > 1) for (int k = 0; k < p->rat; k++) rectP(rollX(st) + 2 + k * 4, ROLL_TOP - 5, 2, 2, TH->warn);   // ratchet dots
	}
	for (int r = 0; r < 8; r++) if (rowCustom(r) && rollRow(gRowNote[r]) >= 0) {   // rhythm rows: their own length and hits
		int L = rowLenOf(r), y = rollY(gRowNote[r]) + 1;
		for (int i = RP0; i < RP0 + ROLL_VIEW; i++) {
			int x = rollX(i) + 1, w = (int)ROLL_CW - 2, head = gSeqPlaying && (int)(gSeqClock % L) == i;
			if (i >= L) continue;
			if (rowCellOn(r, i)) rectP(x, y, w, ROLL_RH - 3, head ? TH->keyOn : TH->val); else if (head) outline(x, y, w, ROLL_RH - 3, RGB3(TH->keyOn));
		}
	}
	if (above) { int x = 312; rectP(x, ROLL_TOP + 1, 5, 2, TH->warn); rectP(x + 1, ROLL_TOP, 3, 1, TH->warn); }   // more notes up there
	if (below) { int x = 312, y = ROLL_TOP + ROLL_ROWS * ROLL_RH - 4; rectP(x, y, 5, 2, TH->warn); rectP(x + 1, y + 2, 3, 1, TH->warn); }
	{   // THE LANE under the notes: VEL / GATE / PROB as bars (drag up / down, or across), ACCENT / SLIDE as switches.
		// Tap its name to step through them.
		rectP(0, VEL_Y - 2, 320, 240 - VEL_Y + 2, TH->bg);
		rectP(0, VEL_Y, ROLL_KW - 1, VEL_H, TH->tab); text(ROLL_KW / 2 - (int)strlen(LANE_NAMES[gLane]) * 4, VEL_Y + VEL_H / 2 - 4, LANE_NAMES[gLane], 1, RGB3(TH->tabTxt));
		for (int st = RP0; st < RP0 + ROLL_VIEW; st++) {
			const Step* sp = &gSeq[st]; int x = rollX(st) + 1, w = (int)ROLL_CW - 2, has = sp->n > 0, at = gSeqPlaying && gSeqPos == st;
			rectP(x, VEL_Y, w, VEL_H, st < gSeqLen ? TH->pad : TH->bg);
			if (!has) continue;
			if (gLane >= 3) {   // switches
				int on = gLane == 3 ? (sp->fl & 1) : (sp->fl & 2) != 0;
				if (on) rectP(x + 2, VEL_Y + 6, w - 4, VEL_H - 12, at ? TH->keyOn : TH->val); else outlineP(x + 2, VEL_Y + 6, w - 4, VEL_H - 12, TH->grid);
			} else {
				int v = laneGet(sp, gLane), h = (VEL_H - 2) * v / 255;
				if (gLane == 1 && !sp->gate) outlineP(x, VEL_Y + VEL_H - h, w, h, TH->grid);   // GATE unset: the pattern's SEQ GATE, hollow
				else rectP(x, VEL_Y + VEL_H - h, w, h, at ? TH->keyOn : TH->val);
			}
		}
	}
	drawPageStrip();
}
// pattern + song view (PAT button): 16 patterns, COPY / CLEAR, and a song chain of up to 32 patterns
#define PV_Y (XY_Y + 18)
#define PV_SONG_Y (PV_Y + 45)    // 4 rows of 16 song slots
#define PV_SONG_H 11
#define PV_TOOLS_Y (PV_SONG_Y + 4 * PV_SONG_H + 4)   // REV ROT HUM / UNDO
#define PV_TAPE_Y (PV_TOOLS_Y + 20)
static void drawPatView(void) {
	text(4, PV_Y, "PATTERN", 1, RGB3(TH->dim));
	for (int i = 0; i < NPAT; i++) {
		int x = 4 + i * 19, y = PV_Y + 10, cur = i == gPatCur, q = i == gPatNext;
		rectP(x, y, 17, 14, cur ? TH->keyOn : TH->tab); if (q) outline(x, y, 17, 14, RGB3(TH->val));
		char c[2] = {(char)('A' + i), 0}; const u8* tc = cur ? TH->ink : patUsed(i) ? TH->val : TH->tabTxt; text(x + 5, y + 3, c, 1, tc[0], tc[1], tc[2]);
	}
	static const char* PB[5] = {"COPY>", "CLEAR", "SONG", "+ADD", "<DEL"};
	for (int i = 0; i < 5; i++) {
		int x = 4 + i * 63, y = PV_Y + 28, lit = i == 2 && gSongOn;
		rectP(x, y, 59, 14, lit ? TH->keyOn : TH->tab); const u8* tc = lit ? TH->ink : TH->tabTxt;
		text(x + 30 - (int)strlen(PB[i]) * 4, y + 3, PB[i], 1, tc[0], tc[1], tc[2]);
	}
	{ static const char* PTB[3] = {"REV", "ROT", "HUM"};
		for (int i = 0; i < 3; i++) { int x = 130 + i * 62; rectP(x, PV_TOOLS_Y, 58, 14, TH->tab); text(x + 29 - (int)strlen(PTB[i]) * 4, PV_TOOLS_Y + 3, PTB[i], 1, RGB3(TH->tabTxt)); } }
	rectP(4, PV_TAPE_Y, 312, 18, TH->keyOn); text(160 - 9 * 4, PV_TAPE_Y + 5, "TAPE  >>>", 1, RGB3(TH->ink));   // bounce to tape
	if (gUndoOk) { rectP(4, PV_TOOLS_Y, 122, 14, TH->tab); char u[20]; snprintf(u, sizeof(u), "UNDO (PAT %c)", 'A' + gUndoIdx); text(65 - (int)strlen(u) * 4, PV_TOOLS_Y + 3, u, 1, RGB3(TH->tabTxt)); }   // takes back the last ACID / CLEAR
	for (int i = 0; i < NSONG; i++) {
		int x = 4 + (i % 16) * 19, y = PV_SONG_Y + (i / 16) * PV_SONG_H, has = i < gSongLen, cur = has && gSongOn && gSeqPlaying && i == gSongPos;
		rectP(x, y, 17, PV_SONG_H - 1, cur ? TH->keyOn : has ? TH->pad : TH->bg); outline(x, y, 17, PV_SONG_H - 1, RGB3(TH->grid));
		if (has) { char c[2] = {(char)('A' + gSong[i]), 0}; const u8* tc = cur ? TH->ink : TH->txt; text(x + 5, y + 1, c, 1, tc[0], tc[1], tc[2]); }
	}
}
// pattern tools: REV (back to front, long notes end where they started), ROT (one step later, wraps), HUM (velocity +-20 %, nudge +-10 %)
static void patReverse(void) {
	int L = gSeqLen < 1 ? 1 : gSeqLen; static Step o[NSTEP]; memcpy(o, gSeq, sizeof(o));
	for (int i = 0; i < L; i++) { Step* d = &gSeq[i]; const Step* sr = &o[L - 1 - i]; *d = *sr; d->n = 0; memset(d->note, 0, 4); memset(d->len, 0, 4); }   // locks follow the step
	for (int i = 0; i < L; i++) for (int k = 0; k < o[i].n; k++) {
		int ln = o[i].len[k] ? o[i].len[k] : 1; if (i + ln > L) ln = L - i;
		int t = L - i - ln; Step* d = &gSeq[t];
		if (d->n >= 4) continue;
		if (!d->n && !d->vel) d->vel = o[i].vel ? o[i].vel : 190;
		d->note[d->n] = o[i].note[k]; d->len[d->n++] = (u8)ln;
	}
	for (int i = 0; i < L; i++) if (!gSeq[i].n) gSeq[i].fl = 0;
}
static void patRotate(void) {
	int L = gSeqLen < 1 ? 1 : gSeqLen; Step last = gSeq[L - 1];
	memmove(&gSeq[1], &gSeq[0], sizeof(Step) * (L - 1)); gSeq[0] = last;
	for (int i = 0; i < L; i++) for (int k = 0; k < gSeq[i].n; k++) { int ln = gSeq[i].len[k]; if (ln > L - i) gSeq[i].len[k] = (u8)(L - i); }   // a note can't run past the end
}
static void patHumanize(void) {
	int L = gSeqLen < 1 ? 1 : gSeqLen;
	for (int i = 0; i < L; i++) {
		Step* s = &gSeq[i]; if (!s->n) continue;
		int v = s->vel ? s->vel : 190; v = (int)lroundf(v * (1.0f + (rand() % 41 - 20) * 0.01f)); s->vel = (u8)(v < 30 ? 30 : v > 255 ? 255 : v);
		int nd = s->nudge + rand() % 21 - 10; s->nudge = (s8)(nd < -50 ? -50 : nd > 50 ? 50 : nd);
	}
}
static void patViewTouch(int x, int y) {   // on press
	if (y >= PV_TAPE_Y && y < PV_TAPE_Y + 18) { gPatView = 2; return; }
	if (y >= PV_Y + 10 && y < PV_Y + 24) {
		int i = (x - 4) / 19; if (i < 0 || i >= NPAT) return;
		if (gSeqPlaying && !gSongOn && i != gPatCur) { gPatNext = i; char m[40]; snprintf(m, sizeof(m), "PATTERN %c NEXT (AT THE END OF THIS ONE)", 'A' + i); say(m); }
		else if (!gSongOn) { patSwitch(i); gPatNext = -1; }
		else say("SONG IS ON: IT PICKS THE PATTERNS");
		return;
	}
	if (y >= PV_TOOLS_Y && y < PV_TOOLS_Y + 14 && x >= 130) {
		int b = (x - 130) / 62; if (b > 2) b = 2;
		undoMark();
		if (b == 0) { patReverse(); say("REVERSED (UNDO TAKES IT BACK)"); }
		if (b == 1) { patRotate(); say("ROTATED ONE STEP LATER"); }
		if (b == 2) { patHumanize(); say("HUMANIZED: VELOCITY AND TIMING LOOSENED"); }
		patStore(); return;
	}
	if (gUndoOk && y >= PV_TOOLS_Y && y < PV_TOOLS_Y + 14 && x < 126) {
		int c = gPatCur, ui = gUndoIdx; histStep(1);
		if (c != ui) say("UNDO: BACK ON THAT PATTERN"); else say("UNDO (SELECT+R REDOES)");
		return;
	}
	if (y >= PV_Y + 28 && y < PV_Y + 42) {
		int b = (x - 4) / 63;
		if (b == 0) {   // copy into the next pattern and go there
			int to = (gPatCur + 1) % NPAT; patStore(); gPat[to] = gPat[gPatCur]; int from = gPatCur; patSwitch(to); gPatNext = -1;
			char m[40]; snprintf(m, sizeof(m), "PATTERN %c COPIED TO %c", 'A' + from, 'A' + to); say(m);
		}
		if (b == 1) { undoMark(); seqClear(); gSeqLen = 16; gSeqBase = gridNote(0); say("PATTERN CLEARED"); }
		if (b == 2) { gSongOn = !gSongOn; if (gSongOn && !gSongLen) { gSong[0] = gPatCur; gSongLen = 1; } say(gSongOn ? "SONG ON: PLAYS THE CHAIN BELOW" : "SONG OFF: PLAYS ONE PATTERN"); }
		if (b == 3 && gSongLen < NSONG) gSong[gSongLen++] = gPatCur;
		if (b == 4 && gSongLen > 0) gSongLen--;
		return;
	}
	if (y >= PV_SONG_Y && y < PV_SONG_Y + 4 * PV_SONG_H) {   // a song slot: play from there
		int i = (x - 4) / 19 + ((y - PV_SONG_Y) / PV_SONG_H) * 16; if (i < 0 || i >= gSongLen) return;
		gSongPos = i; if (gSong[i] != gPatCur) patSwitch(gSong[i]);
	}
}
// step editor (hold a step): the step's locks, and the rhythm of the row you held
#define SE_N 15
static const char* SE_NAMES[SE_N] = {"VEL", "GATE", "PROB", "CUTOFF", "RES", "PITCH", "NUDGE", "ACCENT", "ROW LEN", "ROW EUCL", "ROW ROT", "SLIDE", "RATCHET", "LENGTH", "IF"};
static const int SE_MIN[SE_N] = {1, 0, 0, -100, -100, -24, -50, 0, 0, 0, 0, 0, 1, 1, 0}, SE_MAX[SE_N] = {127, 100, 100, 100, 100, 24, 50, 1, 16, 16, 15, 1, 4, 16, NCOND - 1};
static const char* COND_NAMES[NCOND];
static int seGet(int k) {
	Step* s = &gSeq[gStepEd]; int r = rowOfNote(gStepEdRow);   // gStepEdRow: the pitch you held
	switch (k) { case 0: return (s->vel + 1) / 2; case 1: return s->gate; case 2: return 100 - s->skip; case 3: return s->cut; case 4: return s->res;
	case 5: return s->pitch; case 6: return s->nudge; case 7: return s->fl & 1; case 11: return (s->fl >> 1) & 1; case 12: return s->rat ? s->rat : 1;
	case 13: { for (int i = 0; i < s->n; i++) if (s->note[i] == gStepEdRow) return noteLen(s, i); return 1; }
	case 14: return s->cond;
	case 8: return r < 0 ? 0 : gRowLen[r]; case 9: return r < 0 ? 0 : gRowHits[r]; default: return r < 0 ? 0 : gRowRot[r]; }
}
static void stepSetLen(int st, int nt, int L);
static void seSet(int k, int v) {
	Step* s = &gSeq[gStepEd];
	v = v < SE_MIN[k] ? SE_MIN[k] : v > SE_MAX[k] ? SE_MAX[k] : v;
	switch (k) { case 0: s->vel = (u8)(v * 2 > 255 ? 255 : v * 2); return; case 1: s->gate = (u8)(v && v < 5 ? 5 : v); return; case 2: s->skip = (u8)(100 - v); return;
	case 3: s->cut = (s8)v; return; case 4: s->res = (s8)v; return; case 5: s->pitch = (s8)v; return; case 6: s->nudge = (s8)v; return;
	case 7: s->fl = (u8)((s->fl & ~1) | v); return; case 11: s->fl = (u8)((s->fl & ~2) | (v << 1)); return; case 12: s->rat = (u8)v; return;
	case 13: stepSetLen(gStepEd, gStepEdRow, v); return;
	case 14: if (v == 19 || v == 20) v = v > s->cond ? 21 : 18; s->cond = (u8)v; return; }   // (NEI is for the drum rows: skipped here)
	int r = v ? rowSlotFor(gStepEdRow) : rowOfNote(gStepEdRow);   // a rhythm row is made for this pitch when you give it one
	if (r < 0) { if (v) say("ALL 8 RHYTHM ROWS ARE IN USE"); return; }
	if (k == 8) gRowLen[r] = (u8)v; else if (k == 9) gRowHits[r] = (u8)v; else gRowRot[r] = (u8)v;
}
static void seFmt(int k, int v, char* o, int n) {
	if (k == 14) snprintf(o, n, "%s", COND_NAMES[v < 0 || v >= NCOND ? 0 : v]); else if (k == 7 || k == 11) snprintf(o, n, v ? "ON" : "OFF"); else if (k == 1 && !v) snprintf(o, n, "SEQ"); else if (k == 8 && !v) snprintf(o, n, "PAT"); else if (k == 9 && !v) snprintf(o, n, "OFF");
	else if (k == 1 || k == 2) snprintf(o, n, "%d%%", v); else if (k >= 3 && k <= 6 && v) snprintf(o, n, "%+d", v); else snprintf(o, n, "%d", v);
}
#define SE_Y (XY_Y + 16)
// two tabs of full-width, 18 px rows: NOTE (how the step plays) and LOCKS (sound locks and the rhythm row)
#define SE_TAB_Y (SE_Y + 12)
#define SE_RY (SE_TAB_Y + 17)          // first row
#define SE_RH 18
#define SE_TX 88                       // slider track: x and width
#define SE_TW 172
static const int SE_PAGE[2][8] = {{0, 1, 2, 6, 7, 11, 12, 13}, {3, 4, 5, 8, 9, 10, 14, -1}};   // IF: the trig condition, on the second tab
static const char* SE_TABN[2] = {"NOTE", "LOCKS + ROW + IF"};
static int gSeTab = 0, gSeSel = 0;     // gSeSel: the selected setting (SE index), the circle pad moves it
static int gClipOk = 0; static Step gClip;
#define SE_CP_Y (SE_RY + 8 * SE_RH + 4)
static void drawStepEd(void) {
	char t[40]; int nt = gStepEdRow;
	if (kitDrumOf(nt) >= 0) snprintf(t, sizeof(t), "STEP %d %s", gStepEd + 1, kitPadName(kitDrumOf(nt)));
	else snprintf(t, sizeof(t), "STEP %d %s%d", gStepEd + 1, NOTE_NAMES[nt % 12], nt / 12 - 1); text(4, SE_Y, t, 1, RGB3(TH->val));
	rectP(262, SE_Y - 2, 54, 12, TH->keyOn); text(273, SE_Y, "DONE", 1, RGB3(TH->ink));
	rectP(126, SE_Y - 2, 40, 12, TH->tab); text(134, SE_Y, "RND", 1, RGB3(TH->tabTxt));
	rectP(170, SE_Y - 2, 40, 12, TH->tab); text(178, SE_Y, "<ST", 1, RGB3(TH->tabTxt)); rectP(214, SE_Y - 2, 40, 12, TH->tab); text(222, SE_Y, "ST>", 1, RGB3(TH->tabTxt));
	for (int i = 0; i < 2; i++) {
		int x = 4 + i * 156, on = i == gSeTab; rectP(x, SE_TAB_Y, 152, 14, on ? TH->keyOn : TH->tab);
		const u8* c = on ? TH->ink : TH->tabTxt; text(x + 76 - (int)strlen(SE_TABN[i]) * 4, SE_TAB_Y + 3, SE_TABN[i], 1, c[0], c[1], c[2]);
	}
	for (int r = 0; r < 8; r++) {
		int k = SE_PAGE[gSeTab][r]; if (k < 0) continue;
		int y = SE_RY + r * SE_RH, v = seGet(k), sel = k == gSeSel;
		if (sel) { rectP(2, y, 316, SE_RH - 2, TH->pad); outline(2, y, 316, SE_RH - 2, RGB3(TH->line)); }
		char vs[12]; seFmt(k, v, vs, sizeof(vs));
		const u8* lc = sel ? TH->val : (k >= 8 && k <= 10 ? TH->keyOn : TH->txt);
		text(8, y + 5, SE_NAMES[k], 1, lc[0], lc[1], lc[2]); text(312 - (int)strlen(vs) * 8, y + 5, vs, 1, RGB3(TH->val));
		rectP(SE_TX, y + 7, SE_TW, 3, sel ? TH->grid : TH->tab);
		float f = (v - SE_MIN[k]) / (float)(SE_MAX[k] - SE_MIN[k]); int z = SE_MIN[k] < 0 ? SE_TW / 2 : 0, p = (int)(f * (SE_TW - 1));
		if (SE_MIN[k] < 0) { int a = p < z ? p : z, b = p < z ? z : p; rectP(SE_TX + a, y + 6, b - a + 1, 5, TH->val); rectP(SE_TX + z, y + 4, 1, 9, TH->dim); }
		else rectP(SE_TX, y + 6, p + 1, 5, TH->val);
		rectP(SE_TX + p - 1, y + 3, 3, 11, sel ? TH->keyOn : TH->val);   // the knob
	}
	rectP(4, SE_CP_Y, 152, 16, TH->tab); text(64, SE_CP_Y + 4, "COPY", 1, RGB3(TH->tabTxt));
	rectP(162, SE_CP_Y, 152, 16, gClipOk ? TH->tab : TH->pad); text(218, SE_CP_Y + 4, "PASTE", 1, RGB3(gClipOk ? TH->tabTxt : TH->grid));
}
static int gSeDrag = -1;
static int stepEdTouch(int x, int y, int press, int release) {
	if (release) { gSeDrag = -1; return 1; }
	if (press && y < SE_Y + 10 && x >= 262) { gStepEd = -1; return 1; }
	if (press && y < SE_Y + 10 && x >= 126 && x < 166) {   // RND: a looser take on this step (velocity, timing, chance)
		Step* s = &gSeq[gStepEd]; int v = s->vel ? s->vel : 190;
		v += rand() % 81 - 40; s->vel = (u8)(v < 60 ? 60 : v > 255 ? 255 : v);
		s->nudge = (s8)(rand() % 21 - 10); s->skip = (u8)(rand() % 3 ? 0 : rand() % 31);
		say("STEP LOOSENED: VEL, NUDGE, CHANCE"); return 1;
	}
	if (press && y < SE_Y + 10 && x >= 170 && x < 254) {   // the step before / after (pitch follows the step's first note)
		int L = gSeqLen < 1 ? 1 : gSeqLen; gStepEd = (gStepEd + (x >= 214 ? 1 : L - 1)) % L;
		if (gSeq[gStepEd].n) gStepEdRow = gSeq[gStepEd].note[0];
		return 1;
	}
	if (press && y >= SE_CP_Y && y < SE_CP_Y + 16) {   // COPY / PASTE the whole step (notes, lengths, locks)
		if (x < 160) { gClip = gSeq[gStepEd]; gClipOk = 1; say(gClip.n ? "STEP COPIED" : "EMPTY STEP COPIED"); }
		else if (gClipOk) {
			undoMark(); Step c = gClip; int L = NSTEP - gStepEd;
			for (int k = 0; k < c.n; k++) if (c.len[k] > L) c.len[k] = (u8)L;
			gSeq[gStepEd] = c; for (int k = 0; k < c.n; k++) if (gStepEd + (c.len[k] ? c.len[k] : 1) > gSeqLen) gSeqLen = gStepEd + (c.len[k] ? c.len[k] : 1);
			if (c.n) { gStepEdRow = c.note[0]; if (gSeqBase < 0) gSeqBase = gridNote(0); if (gStepEd >= gSeqLen) gSeqLen = gStepEd + 1; }
			patStore(); say("STEP PASTED (UNDO IN PAT)");
		} else say("COPY A STEP FIRST");
		return 1;
	}
	if (press && y >= SE_TAB_Y && y < SE_TAB_Y + 15) { int t = x >= 160; if (t != gSeTab) { gSeTab = t; gSeSel = SE_PAGE[t][0]; } gSeDrag = -1; return 1; }
	if (press) {   // the row is picked on the press and kept: sliding up or down while dragging never changes it
		int r = (y - SE_RY) / SE_RH; gSeDrag = -1;
		if (y >= SE_RY && r >= 0 && r < 8 && SE_PAGE[gSeTab][r] >= 0) {
			int k = SE_PAGE[gSeTab][r]; gSeSel = k;
			if (SE_MAX[k] - SE_MIN[k] == 1) { seSet(k, !seGet(k)); return 1; }   // on / off: a tap flips it
			if (x < SE_TX - 6) return 1;   // tapping the name only selects it; the circle pad fine-tunes it
			gSeDrag = k;
		}
	}
	if (gSeDrag < 0) return press;
	float f = (x - SE_TX) / (float)(SE_TW - 1); f = f < 0 ? 0 : f > 1 ? 1 : f;
	seSet(gSeDrag, SE_MIN[gSeDrag] + (int)lroundf(f * (SE_MAX[gSeDrag] - SE_MIN[gSeDrag])));
	return 1;
}
// 303-style line: 16ths, root-heavy, octave jumps, rests, accents, slides; notes from the grid's rows
// RIFF works in the sequencer's octave: the one the piano roll is showing (its root at or near the bottom row),
// not the keyboard's octave. The roll lines up on that root so the whole riff is on screen.
static int riffBase(void) { int b = gRollLow + ((gRoot - gRollLow) % 12 + 12) % 12; if (b - gRollLow > 6) b -= 12; if (b < 12) b += 12; return b; }
static int riffNote(int base, int row) { static const int MAJ[8] = {0, 2, 4, 5, 7, 9, 11, 12}; return gScale ? scaleNote(base, row) : base + MAJ[row]; }
static void acidGen(void) {
	int base = riffBase();
	static const int W[8] = {38, 7, 14, 9, 12, 8, 12, 0};   // weights per grid row: RIFF stays inside the octave you're on (row 7, the octave up, never)
	memset(gSeq, 0, sizeof(gSeq));
	int prev = 0;
	for (int st = 0; st < 16; st++) {
		int restP = (st % 4 == 0) ? 8 : 26;
		if (rand() % 100 < restP) continue;
		int r, t = rand() % 100;
		if (st % 4 == 0 && rand() % 100 < 55) r = 0;
		else if (prev && t < 18) r = prev;
		else { int sum = 0, k = rand() % 100; for (r = 0; r < 8; r++) { sum += W[r]; if (k < sum) break; } if (r > 7) r = 0; }
		Step* sp = &gSeq[st]; sp->n = 1; sp->note[0] = (u8)riffNote(base, r); sp->vel = 190;
		if (rand() % 100 < (st % 4 == 2 ? 45 : 25)) sp->fl |= 1;
		if (rand() % 100 < 22) sp->fl |= 2;
		prev = r;
	}
	for (int st = 0; st < 16; st++) if ((gSeq[st].fl & 2) && !gSeq[(st + 1) % 16].n) gSeq[st].fl &= ~2;   // no slide into a rest
	if (!gSeq[0].n) { gSeq[0].n = 1; gSeq[0].note[0] = (u8)base; gSeq[0].vel = 190; gSeq[0].fl = 1; }
	gSeqLen = 16; gSeqBase = base; gSeqTr = 0;
}
static void gridButtons(int x) {
	int b = x * GRID_BTNS / 320;
	if (b == 0) { gSeqMode = gSeqMode == 1 ? 0 : 1; if (gSeqMode == 1 && gSeqBase < 0) gSeqBase = gridNote(0); if (gSeqMode == 0) { seqStop(); if (gLiveRec || gPreFor == 2) liveRecStop(); } }
	if (b == 7) liveRecToggle();
	if (b == 1) { gPatView = gPatView == 1 ? 0 : 1; gStepEd = -1; gDgEd = 0; }
	if (b == 2 && gSeqLen > 1) gSeqLen--;
	if (b == 3 && gSeqLen < NSTEP) gSeqLen++;
	if (b == 4) { gPatView = gPatView == 3 ? 0 : 3; gStepEd = -1; gDgEd = 0; }   // DRUMS <-> NOTES
	if (b == 5) { gPatView = gPatView == 2 ? 0 : 2; gStepEd = -1; gDgEd = 0; }   // TAPE
	if (b == 6) { undoMark(); int base = riffBase(); acidGen(); gPatView = 0; gStepEd = -1; gRollLow = base; say("RIFF: IN THE ROLL'S OCTAVE - TAP AGAIN FOR A NEW ONE"); }
}
static void stepSetNote(int st, int nt, int on) {
	Step* sp = &gSeq[st];
	int k = 0; for (; k < sp->n; k++) if (sp->note[k] == nt) break;
	if (on && k == sp->n && sp->n < 4) { sp->len[sp->n] = 1; sp->note[sp->n++] = (u8)nt; if (!sp->vel) sp->vel = 190; if (gSeqBase < 0) gSeqBase = gridNote(0); if (st >= gSeqLen) { gSeqLen = (st / 4 + 1) * 4; if (gSeqLen > NSTEP) gSeqLen = NSTEP; } }
	if (!on && k < sp->n) { for (int j = k; j + 1 < sp->n; j++) { sp->note[j] = sp->note[j + 1]; sp->len[j] = sp->len[j + 1]; } sp->n--; sp->len[sp->n] = 0; if (!sp->n) sp->fl = 0; }
}
static void stepSetLen(int st, int nt, int L) {
	Step* sp = &gSeq[st]; L = L < 1 ? 1 : L > NSTEP - st ? NSTEP - st : L;
	for (int k = 0; k < sp->n; k++) if (sp->note[k] == nt) { sp->len[k] = (u8)L; if (st + L > gSeqLen) gSeqLen = st + L; }
}
static void audition(int nt) { if (!gSeqPlaying) { playSet(&nt, 1, 0.8f); gAuditionT = 0.18f; } }
// tap empty = a note, tap a note = gone, drag a new note or a note's tail = length, drag a note's head (or up / down) = move it,
// hold = step editor; the keys scroll
static int gGtSt = -1, gGtNote = 0, gGtHit = -1, gGtDrag = 0, gGtLong = 0, gGtKeys = 0, gGtY0 = 0, gGtLow0 = 0; static u64 gGtT0;
static int gGtX0 = 0, gGtVel = 0, gGtV0 = 0;   // drag up / down on a note: its VELOCITY
static int gGtMove = 0, gMvSt = -1, gMvNt = 0, gMvLen = 1, gMvSt0 = 0, gMvNt0 = 0;   // dragging a note to a new step / pitch
static int gGtErase = 0;
static int gridTouch(int x, int y, int press, int release) {   // 1 = redraw
	if (gStepEd >= 0 && gPatView != 3 && !(press && y < XY_Y + 15)) return stepEdTouch(x, y, press, release);
	if (press && y < XY_Y + 15) { gridButtons(x); return 1; }
	if (gPatView == 3) { gDgShift = 16; int r = release ? drumGridRelease() : drumGridTouch(x, y, press); gDgShift = 0; return r; }
	if (gPatView == 2) return tapeTouch(x, y, press, release);
	if (gPatView) { if (press) patViewTouch(x, y); return press; }
	if (y >= PG_Y) return press ? pageStripTouch(x) : 0;   // the PAGE strip
	if (press && y >= VEL_Y - 2 && x < ROLL_KW) { gLane = (gLane + 1) % 5; return 1; }   // the lane's name: the next lane
	if (y >= VEL_Y - 2 && x >= ROLL_KW && !gGtKeys && (press || gGtSt == -2)) {   // the lane
		static int paint = -1;
		int st = RP0 + (int)((x - ROLL_KW) / ROLL_CW); if (st < RP0 || st >= RP0 + ROLL_VIEW) return 0;
		if (press) { undoMark(); gGtSt = -2; paint = -1; }
		if (release) { gGtSt = -1; return 0; }
		Step* sp = &gSeq[st]; if (!sp->n) return 0;
		if (gLane >= 3) {   // ACCENT / SLIDE: a tap flips it, a drag paints the same
			u8 bit = gLane == 3 ? 1 : 2; int on = (sp->fl & bit) != 0;
			if (paint < 0) paint = !on;
			if (on != paint) { if (paint) sp->fl |= bit; else sp->fl &= (u8)~bit; return 1; }
			return 0;
		}
		int v = (VEL_Y + VEL_H - y) * 255 / (VEL_H - 2); v = v < 0 ? 0 : v > 255 ? 255 : v;
		if (laneGet(sp, gLane) != v) { laneSet(sp, gLane, v); return 1; }
		return 0;
	}
	if (gGtSt == -2) { if (release) gGtSt = -1; return 0; }
	int st = RP0 + (int)((x - ROLL_KW) / ROLL_CW), row = ROLL_ROWS - 1 - (y - ROLL_TOP) / ROLL_RH, nt = rollNote(row);
	int ok = x >= ROLL_KW && st >= RP0 && st < RP0 + ROLL_VIEW && st < NSTEP && y >= ROLL_TOP && row >= 0 && row < ROLL_ROWS && nt >= 0;
	if (press) {
		gGtKeys = x < ROLL_KW && y >= ROLL_TOP; gGtY0 = y; gGtX0 = x; gGtVel = 0; gGtLow0 = gRollLow; gGtDrag = 0; gGtLong = 0; gGtT0 = svcGetSystemTick();
		if (gGtKeys) { if (row >= 0 && row < ROLL_ROWS && nt >= 0) audition(nt); gGtSt = -1; return 0; }
		gGtSt = ok ? st : -1; gGtNote = nt; gGtHit = ok ? noteCover(st, nt, NULL) : -1; gGtMove = 0; gGtErase = 0;
		return 0;
	}
	if (gGtKeys) {   // drag the keyboard: scroll
		if (release) { gGtKeys = 0; return 0; }
		if (gVoiceMode >= 3) return 0;   // a kit's rows are fixed
		int nl = gGtLow0 + (y - gGtY0) / ROLL_RH; nl = nl < 12 ? 12 : nl > 108 ? 108 : nl;
		if (nl != gRollLow) { gRollLow = nl; return 1; }
		return 0;
	}
	if (gGtSt < 0) return 0;
	if (release) {
		int s0 = gGtSt; gGtSt = -1;
		if (gGtDrag || gGtLong) return gGtDrag;
		if (rowOfNote(gGtNote) >= 0) { say("RHYTHM ROW: HOLD A STEP TO CHANGE IT (ROW LEN / EUCL)"); return 0; }
		if (gGtHit >= 0) { undoMark(); stepSetNote(gGtHit, gGtNote, 0); return 1; }   // tap a note: gone
		if (gSeq[s0].n >= 4) { say("4 NOTES PER STEP"); return 0; }
		undoMark(); stepSetNote(s0, gGtNote, 1); gTapNote = gGtNote; audition(gGtNote); return 1;
	}
	{   // ERASER: from an empty cell go up or down first, then scribble: every note you cross goes (sideways first = a long note)
		int er = gGtErase;
		if (!er && gGtHit < 0 && !gGtLong && !gGtDrag && abs(y - gGtY0) > 8 && abs(y - gGtY0) > abs(x - gGtX0) * 2) { er = gGtErase = 1; gGtDrag = 1; undoMark(); say("ERASER: SCRIBBLE OVER NOTES"); }
		if (er) { int h = ok ? noteCover(st, nt, NULL) : -1; if (h >= 0) { stepSetNote(h, nt, 0); return 1; } return 0; }
	}
	if (!gGtLong && gGtHit >= 0 && !gGtDrag) {   // first move on a note straight up or down: its VELOCITY (down = softer)
		int dx = x - gGtX0, dy = y - gGtY0;
		if (abs(dy) > 8 && abs(dy) > abs(dx) * 2) { gGtDrag = 1; gGtVel = 1; undoMark(); gGtV0 = gSeq[gGtHit].vel ? gSeq[gGtHit].vel : 190; }
	}
	if (gGtVel && gGtHit >= 0) {
		int v = gGtV0 - (y - gGtY0) * 2; v = v < 8 ? 8 : v > 255 ? 255 : v;
		if (gSeq[gGtHit].vel != v) { gSeq[gGtHit].vel = (u8)v; char m[24]; snprintf(m, sizeof(m), "VELOCITY %d", v * 127 / 255); say(m); return 1; }
		return 0;
	}
	if (!gGtLong && ok && gGtHit >= 0 && !gGtDrag && (st != gGtSt || nt != gGtNote)) {   // first move on a note: move it, or stretch it?
		int k0 = 0; noteCover(gGtSt, gGtNote, &k0); int L = noteLen(&gSeq[gGtHit], k0);
		gGtMove = nt != gGtNote || gGtSt != gGtHit + L - 1;   // off its pitch, or from its head / body = MOVE; along the row from its tail = length
	}
	if (!gGtLong && ok && gGtHit >= 0 && gGtMove) {
		// a drag that starts on a note's head, or moves off its pitch: MOVE it (its tail still sets the length)
		if (!gGtDrag) { int k0 = 0; noteCover(gGtSt, gGtNote, &k0); gGtDrag = 1; undoMark(); gMvSt0 = gMvSt = gGtHit; gMvNt0 = gMvNt = gGtNote; gMvLen = noteLen(&gSeq[gGtHit], k0); }
		int ds = gMvSt0 + (st - gGtSt), dn = gVoiceMode >= 3 ? rollNote(rollRow(gMvNt0) + row - rollRow(gGtNote)) : gMvNt0 + (nt - gGtNote); if (gVoiceMode >= 3 && dn < 0) return 0; if (ds < 0) ds = 0; if (ds > NSTEP - 1) ds = NSTEP - 1; if (dn < 0) dn = 0; if (dn > 127) dn = 127;
		if (ds != gMvSt || dn != gMvNt) {
			if (gSeq[ds].n >= 4 && !(ds == gMvSt)) return 0;   // that step is full
			stepSetNote(gMvSt, gMvNt, 0); stepSetNote(ds, dn, 1); stepSetLen(ds, dn, gMvLen); gMvSt = ds; gMvNt = dn; if (dn != gMvNt0 || ds != gMvSt0) audition(dn);
			return 1;
		}
		return 0;
	}
	if (!gGtLong && ok && st != gGtSt) {   // a drag along the row: length
		if (!gGtDrag) { gGtDrag = 1; gGtMove = 0; undoMark(); if (gGtHit < 0) { if (gSeq[gGtSt].n >= 4 || rowOfNote(gGtNote) >= 0) { gGtSt = -1; return 0; } stepSetNote(gGtSt, gGtNote, 1); gGtHit = gGtSt; } }
		stepSetLen(gGtHit, gGtNote, st - gGtHit + 1); return 1;
	}
	if (!gGtDrag && !gGtLong && svcGetSystemTick() - gGtT0 > SYSCLOCK_ARM11 * 2 / 5) {
		gGtLong = 1; gStepEd = gGtHit >= 0 ? gGtHit : gGtSt; gStepEdRow = gGtNote; gSeDrag = -1;
		say("STEP: LOCKS FOR THIS STEP, RHYTHM FOR THIS PITCH");
		return 1;
	}
	return 0;
}

static void outline(int x, int y, int w, int h, u8 r, u8 g, u8 b) { rect(x, y, w, 1, r, g, b); rect(x, y + h - 1, w, 1, r, g, b); rect(x, y, 1, h, r, g, b); rect(x + w - 1, y, 1, h, r, g, b); }

// ---- PADS: a touch chord instrument on the bottom screen (in place of the keys and XY pad). One pad per chord of
// the key; where you touch a pad picks the chord: low = triad, middle = 7th, high = 9th; left edge = inversion,
// right edge = sus4. Slide across the STRUM strip to strum the last chord like an autoharp.
static int gPads = 0, gPadSel = -1, gPadN = 0, gPadNotes[4], gStrumZone = -1;
static struct { int note; float t; } gStrumOff[24]; static int gStrumOffN = 0;
#define STRUM_MAX 6
static void voiceFade(int note, float sec) {   // let a note go fast but not dead: a short release, no click
	mirNote('F', note, sec);
	for (int i = 0; i < NVS; i++) { SVoice* v = &gV[i]; if (v->active && !v->isEcho && v->note == note && v->stage != 4) { v->stage = v->stage2 = 4; v->relMul = sec / (gEnv1.r + 0.0005f); } }
}
#define PAD_Y (TAB_H + 2)
#define PAD_H 120
#define PROG_Y (PAD_Y + PAD_H + 4)   // the PROGRESSION row between the pads and the strum strip
#define PROG_H 22
#define STRUM_Y (PROG_Y + PROG_H + 2)
static const int MAJ7[7] = {0, 2, 4, 5, 7, 9, 11};
static int padDeg(int base, int d) {   // scale degree -> note, using the scale when it has 7 notes, else major
	if (gScale && scaleLen() == 7) return scaleNote(base + gRoot, d);
	int o = d / 7; return base + gRoot + 12 * o + MAJ7[d % 7];
}
static int strumScaleNote(int base, int q) {   // STRUM NOTES SCALE: string q = the scale (all of it, any length) up from the pad's root, an octave down to start
	if (!gScale) return padDeg(base, gPadSel + q) - 12;
	int r = padDeg(base, gPadSel), k = -40; while (scaleNote(base + gRoot, k) < r) k++;
	return scaleNote(base + gRoot, k + q) - 12;
}
static int padChord(int base, int pad, int band, int side, int* out) {
	int r = padDeg(base, pad), t3 = padDeg(base, pad + 2), t5 = padDeg(base, pad + 4), t7 = padDeg(base, pad + 6), t9 = padDeg(base, pad + 8);
	if (side == 2) t3 = padDeg(base, pad + 3);   // sus4
	int n = 0;
	if (band == 2) { out[n++] = r; out[n++] = t3; out[n++] = t7; out[n++] = t9; }   // 9th: drop the 5th, keep four notes
	else { out[n++] = r; out[n++] = t3; out[n++] = t5; if (band == 1) out[n++] = t7; }
	if (side == 0) out[0] += 12;   // first inversion
	return n;
}
static void padName(int base, int pad, char* o, int len) {
	static const char* RN[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
	int r = padDeg(base, pad), a = padDeg(base, pad + 2) - r, b = padDeg(base, pad + 4) - r;
	const char* q = a == 4 && b == 7 ? "" : a == 3 && b == 7 ? "m" : a == 3 && b == 6 ? "dim" : a == 4 && b == 8 ? "+" : "?";
	char rn[8]; snprintf(rn, sizeof(rn), "%s", RN[pad]);
	if (a == 3) for (char* c = rn; *c; c++) *c = (char)(*c - 'A' + 'a');   // minor chords in lower case, the usual way
	snprintf(o, len, "%s|%s%s", rn, NOTE_NAMES[r % 12], q);
}
#define NPROG 20
static int gPadBase = 48;
static int gProg = 0, gProgAt = -1, gProgAuto = 0, gProgDirty = 0; static float gProgBar = 0;
static const char* PROG_NAMES[NPROG] = {"PROG: OFF", "I V vi IV", "vi IV I V", "I vi IV V", "ii V I I", "I IV V IV", "i VI III VII", "i iv v i",
	"I IV vi V", "vi V IV V", "I iii IV V", "ii V I vi", "vi ii V I", "IV V iii vi", "I V IV I", "IV I V vi", "i VII VI VII", "i VI VII i", "i iv VII III", "i v VI iv"};
static const int PROGS[NPROG][4] = {{0, 0, 0, 0}, {0, 4, 5, 3}, {5, 3, 0, 4}, {0, 5, 3, 4}, {1, 4, 0, 0}, {0, 3, 4, 3}, {0, 5, 2, 6}, {0, 3, 4, 0},
	{0, 3, 5, 4}, {5, 4, 3, 4}, {0, 2, 3, 4}, {1, 4, 0, 5}, {5, 1, 4, 0}, {3, 4, 2, 5}, {0, 4, 3, 0}, {3, 0, 4, 5}, {0, 6, 5, 6}, {0, 5, 6, 0}, {0, 3, 6, 2}, {0, 4, 5, 3}};   // scale degrees (pads)
static int gPadBass = 0;   // BASS: 0 off, 1 HOLD (the root an octave under the chord), 2.. a bass LINE playing a rhythm
static void bassStop(void);
static void padPreset(int style, int idx) {   // CHORD 13-18 (PADS + BASS): their bass style and progression, on the pads
	static const s8 PB[6][2] = {{6, 11}, {5, 3}, {3, 2}, {7, 5}, {9, 1}, {11, 17}};   // WALK ii V I vi, OCTV I vi IV V, OFFB vi IV I V, FUNK I IV V IV, REGG I V vi IV, GALP i VI VII i
	if (style != 5 || idx < 12 || idx > 17) return;
	bassStop(); gPadBass = PB[idx - 12][0]; gProg = PB[idx - 12][1]; gProgAt = -1; gPads = 1;
}
// BASS LINES: 16 steps a bar. R root, 3 / 5 / 7 the chord's third, fifth, seventh, O the octave, - rest, _ the note goes on
#define NBASS 12
static const char* BASS_NAMES[NBASS] = {"BASS", "HOLD", "PULS", "OFFB", "R+5", "OCTV", "WALK", "FUNK", "TRES", "REGG", "ARP", "GALP"};
static const char* BASS_LONG[NBASS] = {"BASS OFF", "BASS: HOLD THE ROOT", "BASS: 8TH PULSE", "BASS: OFFBEATS", "BASS: ROOT + FIFTH", "BASS: OCTAVES (DISCO)", "BASS: WALKING",
	"BASS: FUNK", "BASS: TRESILLO 3-3-2", "BASS: ONE DROP (REGGAE)", "BASS: ARPEGGIO", "BASS: GALLOP"};
static const char* BASS_PAT[NBASS] = {"", "", "R-R-R-R-R-R-R-R-", "--R---R---R---R-", "R___5___R___5___", "R-O-R-O-R-O-R-O-", "R___3___5___O___",
	"R--R--O-R-5--R7-", "R__R__R_R__R__R_", "------R_---R-5--", "R-3-5-O-R-3-5-O-", "R-RRR-RRR-RRR-RR"};
static int gBassNote = -1; static float gBassPh = 0; static int gBassStep = -1; static void bassStop(void);
// MORPH (pads slide): the held chord turns into the new one by gliding - voice i of the old chord (low to high) bends
// to note i of the new one over PAD MORPH seconds; extra notes start, spare ones let go. No retrigger.
static void chordMorph(const int* notes, int n, float vel) {
	if (gArp || !gHeldN) { playSet(notes, n, vel); return; }
	int o[8], nn[8], on = gHeldN < 8 ? gHeldN : 8, nw = n < 8 ? n : 8;
	for (int i = 0; i < on; i++) o[i] = gHeld[i]; for (int i = 0; i < nw; i++) nn[i] = notes[i];
	for (int i = 0; i < on; i++) for (int j = i + 1; j < on; j++) if (o[j] < o[i]) { int t = o[i]; o[i] = o[j]; o[j] = t; }
	for (int i = 0; i < nw; i++) for (int j = i + 1; j < nw; j++) if (nn[j] < nn[i]) { int t = nn[i]; nn[i] = nn[j]; nn[j] = t; }
	int m = on < nw ? on : nw;
	for (int i = 0; i < m; i++) for (int k = 0; k < NVS; k++) { SVoice* v = &gV[k]; if (v->active && !v->isEcho && v->note == o[i] && v->stage < 4 && v->slide != 3) { v->note = nn[i]; v->freq = mtof((float)nn[i]); v->slide = 3; } }
	for (int i = 0; i < m; i++) mirRetune(o[i], nn[i], 2);
	for (int k = 0; k < NVS; k++) if (gV[k].slide == 3) gV[k].slide = 2;   // (3: moved this time, so two old notes can't land on one voice)
	for (int i = m; i < on; i++) voiceOff(o[i]);
	for (int i = m; i < nw; i++) voiceOn(nn[i], vel);
	gHeldN = 0; for (int i = 0; i < nw; i++) gHeld[gHeldN++] = nn[i]; gHeldVel = vel;
}
// a pad chord sounds (and, while recording, goes into the sequencer: live REC onto the grid, step REC one step each)
static void padPlay(int base, const int* notes, int n, float vel, int sound) {
	int all[6], k = 0;
	if (gPadBass == 1 && gPadSel >= 0) all[k++] = padDeg(base, gPadSel) - 12;
	for (int i = 0; i < n && k < 6; i++) all[k++] = notes[i];
	int morph = sound == 2 && !gArp && gHeldN > 0;   // gliding from the held chord: the step before slides into this one
	if (gLiveRec) { int prev = gLvN ? gLvStep : -1; liveDown(all, k, vel); if (morph && prev >= 0 && gSeqPlaying && prev != gLvStep) gSeq[prev].fl |= 2; }
	else if (gSeqMode == 2) { int prev = gRecPos > 0 ? gRecPos - 1 : -1; if (morph && prev >= 0 && gSeq[prev].n) gSeq[prev].fl |= 2; seqRecord(all, k, vel); }
	if (sound == 2) chordMorph(all, k, vel); else if (sound) playSet(all, k, vel);
}
static void drawPads(int base) {
	gPadBase = base;
	rect(0, TAB_H, 320, 240 - TAB_H, RGB3(TH->bg));
	for (int i = 0; i < 7; i++) {
		int x = 2 + i * 45, w = 43, lit = i == gPadSel;
		rect(x, PAD_Y, w, PAD_H, lit ? TH->keyOn[0] : TH->pad[0], lit ? TH->keyOn[1] : TH->pad[1], lit ? TH->keyOn[2] : TH->pad[2]);
		outline(x, PAD_Y, w, PAD_H, RGB3(TH->line));
		for (int k = 1; k < 3; k++) rect(x + 3, PAD_Y + k * PAD_H / 3, w - 6, 1, RGB3(TH->grid));
		char nm[24]; padName(base, i, nm, sizeof(nm)); char* bar = strchr(nm, '|'); *bar = 0;
		const u8* tc = lit ? TH->ink : TH->val;
		text(x + w / 2 - (int)strlen(nm) * 4, PAD_Y + 4, nm, 1, tc[0], tc[1], tc[2]);
		text(x + w / 2 - (int)strlen(bar + 1) * 4, PAD_Y + PAD_H - 14, bar + 1, 1, tc[0], tc[1], tc[2]);
		text(x + 4, PAD_Y + PAD_H / 6 - 4, "9", 1, RGB3(TH->dim)); text(x + 4, PAD_Y + PAD_H / 2 - 4, "7", 1, RGB3(TH->dim));
	}
	{   // PROGRESSION: pick one, tap its chords in order, or AUTO plays a chord a bar with the clock
		rect(2, PROG_Y, 316, PROG_H, RGB3(TH->kbBg));
		char t[24]; snprintf(t, sizeof(t), "%s", PROG_NAMES[gProg]);
		rect(4, PROG_Y + 2, 88, PROG_H - 4, RGB3(TH->tab)); text(48 - (int)strlen(t) * 4, PROG_Y + 7, t, 1, RGB3(TH->tabTxt));
		{ const u8* bc = gPadBass ? TH->keyOn : TH->tab; rect(94, PROG_Y + 2, 32, PROG_H - 4, RGB3(bc)); const u8* btc = gPadBass ? TH->ink : TH->tabTxt; text(94 + 16 - (int)strlen(BASS_NAMES[gPadBass]) * 4, PROG_Y + 7, BASS_NAMES[gPadBass], 1, btc[0], btc[1], btc[2]); }
		if (gProg) {
			for (int k = 0; k < 4; k++) {
				int x = 128 + k * 38, lit = gProgAt == k && (gProgAuto || gPadSel == PROGS[gProg][k]);
				char nm[24]; padName(base, PROGS[gProg][k], nm, sizeof(nm)); char* bar = strchr(nm, '|'); *bar = 0;
				rect(x, PROG_Y + 2, 36, PROG_H - 4, lit ? TH->keyOn[0] : TH->pad[0], lit ? TH->keyOn[1] : TH->pad[1], lit ? TH->keyOn[2] : TH->pad[2]);
				const u8* c = lit ? TH->ink : TH->val; text(x + 18 - (int)strlen(nm) * 4, PROG_Y + 7, nm, 1, c[0], c[1], c[2]);
			}
			const u8* ac = gProgAuto ? TH->keyOn : TH->tab; rect(281, PROG_Y + 2, 37, PROG_H - 4, RGB3(ac));
			const u8* atc = gProgAuto ? TH->ink : TH->tabTxt; text(284, PROG_Y + 7, "AUTO", 1, atc[0], atc[1], atc[2]);
		} else text(132, PROG_Y + 7, "< TAP: PROGRESSION", 1, RGB3(TH->dim));
	}
	int sh = 240 - STRUM_Y - 2;
	rect(2, STRUM_Y, 316, sh, RGB3(TH->kbBg));
	for (int z = 0; z < 24; z++) { int x = 2 + z * 316 / 24; rect(x + 6, STRUM_Y + 2, 1, sh - 4, z == gStrumZone ? TH->keyOn[0] : TH->line[0], z == gStrumZone ? TH->keyOn[1] : TH->line[1], z == gStrumZone ? TH->keyOn[2] : TH->line[2]); }
	text(6, STRUM_Y + 3, "STRUM", 1, RGB3(TH->dim));
}
static int padsTouch(int base, int x, int y, int press, float* velOut) {   // 1 = screen changed
	if (y < PROG_Y - 2) {
		int i = (x - 2) / 45; if (i < 0 || i > 6) return 0;
		int ly = y - PAD_Y, band = ly < PAD_H / 3 ? 2 : ly < 2 * PAD_H / 3 ? 1 : 0, lx = x - 2 - i * 45, side = lx < 10 ? 0 : lx > 33 ? 2 : 1;
		static int lastI = -1, lastB = -1, lastS = -1;
		if (!press) {   // SLIDE: drag across the pads and the chord follows the finger (a new pad, or a new band / edge of this one)
			if (gPadSel < 0 || (i == lastI && band == lastB && side == lastS)) return 0;
			if (i == lastI && (abs(lx - (side == 0 ? 5 : side == 2 ? 38 : 21)) > 6)) return 0;   // (an edge only counts once the finger is well into it)
		}
		lastI = i; lastB = band; lastS = side;
		gPadSel = i; gPadN = padChord(base, i, band, side, gPadNotes);
		*velOut = 0.55f + 0.45f * (1 - ly / (float)PAD_H) * 0.5f + 0.2f;
		padPlay(base, gPadNotes, gPadN, *velOut, !press && gPadMorph > 0.001f ? 2 : 1);   // sliding: the chord MORPHS (each voice glides to its new note)
		return 1;
	}
	if (y < STRUM_Y - 1) {   // the PROGRESSION row
		if (!press) return 0;
		if (x >= 94 && x < 128) { gPadBass = (gPadBass + 1) % NBASS; bassStop(); say(BASS_LONG[gPadBass]); return 1; }
		if (x < 94) { gProg = (gProg + 1) % NPROG; gProgAt = -1; if (!gProg && gProgAuto) { gProgAuto = 0; playSet(NULL, 0, 0.8f); } return 1; }
		if (!gProg) return 0;
		if (x >= 280) { gProgAuto = !gProgAuto; gProgAt = -1; gProgBar = 1; if (!gProgAuto) playSet(NULL, 0, 0.8f); return 1; }
		int k = (x - 128) / 38; if (x < 128 || k > 3) return 0;
		gProgAt = k; gPadSel = PROGS[gProg][k]; gPadN = padChord(base, gPadSel, 0, 1, gPadNotes); padPlay(base, gPadNotes, gPadN, 0.8f, 1); *velOut = 0.8f;
		return 1;
	}
	if (gPadSel < 0) { gPadSel = 0; gPadN = padChord(base, 0, 0, 1, gPadNotes); }
	int z = (x - 2) * 24 / 316; if (z < 0) z = 0; if (z > 23) z = 23;
	if (z == gStrumZone) return 0;
	if (gStrumZone < 0 && (gLiveRec || gSeqMode == 2)) padPlay(base, gPadNotes, gPadN, 0.4f + 0.6f * (y - STRUM_Y) / (float)(240 - STRUM_Y), 0);   // recording: a stroke writes its chord
	// every string the stylus crossed since the last frame sounds, in order (a fast swipe used to drop most of them:
	// the touch is read 60 times a second), and a string struck again lets its last note go first rather than
	// stacking voices until they had to be stolen - which is where the clicks came from
	int from = gStrumZone < 0 ? z : gStrumZone + (z > gStrumZone ? 1 : -1), d = z >= from ? 1 : -1;
	float v = 0.4f + 0.6f * (y - STRUM_Y) / (float)(240 - STRUM_Y);
	for (int q = from; ; q += d) {
		int nt = gStrumScale ? strumScaleNote(base, q) : gPadNotes[q % gPadN] + 12 * (q / gPadN) - 12;   // walk up the chord tones (or the SCALE from the chord's root), octave by octave
		int k = 0; for (; k < gStrumOffN; k++) if (gStrumOff[k].note == nt) break;
		if (k < gStrumOffN) { voiceFade(nt, 0.02f); for (int j = k; j < gStrumOffN - 1; j++) gStrumOff[j] = gStrumOff[j + 1]; gStrumOffN--; }   // struck again: the old one fades in 20 ms
		while (gStrumOffN >= STRUM_MAX) { voiceFade(gStrumOff[0].note, 0.03f); for (int j = 0; j < gStrumOffN - 1; j++) gStrumOff[j] = gStrumOff[j + 1]; gStrumOffN--; }   // only so many strings ring: the oldest fades (stolen voices were cut dead: the clicks)
		gStrumOff[gStrumOffN].note = nt; gStrumOff[gStrumOffN].t = 0.35f; gStrumOffN++;
		voiceOn(nt, v);
		if (q == z) break;
	}
	gStrumZone = z;
	return 1;
}
static void bassStop(void) { if (gBassNote >= 0) { voiceFade(gBassNote, 0.04f); gBassNote = -1; } gBassStep = -1; }
static int bassNoteOf(char c) {   // a line's letter -> a note, from the pad that's held, an octave under the chord
	int b = gPadBase, p = gPadSel, r = padDeg(b, p) - 12;
	switch (c) { case '3': return padDeg(b, p + 2) - 12; case '5': return padDeg(b, p + 4) - 12; case '7': return padDeg(b, p + 6) - 12; case 'O': return r + 12; default: return r; }
}
static void bassTick(float dt) {   // BASS LINES: while a pad chord sounds, the line plays on the 16ths (the sequencer's, or its own at BPM)
	int on = gPads && gPadBass >= 2 && gPadSel >= 0 && gHeldN > 0;
	if (!on) { if (gBassNote >= 0 || gBassStep >= 0) bassStop(); return; }
	int st;
	if (gSeqPlaying) st = gSeqPos & 15;
	else { if (gBassStep < 0) gBassPh = 0; else gBassPh += dt * gBPM / 60.0f * 4.0f; st = ((int)gBassPh) & 15; }
	if (st == gBassStep) return;
	gBassStep = st;
	char c = BASS_PAT[gPadBass][st];
	if (c == '_') return;   // held
	if (gBassNote >= 0) { voiceFade(gBassNote, 0.03f); gBassNote = -1; }
	if (c == '-') return;
	gBassNote = bassNoteOf(c); voiceOn(gBassNote, st % 4 == 0 ? 0.9f : 0.75f);
}
static void padsTick(float dt) {   // strummed notes ring for a moment, then let go
	if (gProg && gProgAuto) {   // AUTO: the next chord on every bar (on the sequencer's bar line when it plays)
		static int lastPos = -1; int next = 0;
		if (gSeqPlaying) { if (gSeqPos == 0 && lastPos != 0) next = 1; lastPos = gSeqPos; }
		else { gProgBar += dt * gBPM / 60.0f / 4.0f; if (gProgBar >= 1) { gProgBar -= 1; next = 1; } }
		if (next) { gProgAt = (gProgAt + 1) % 4; gPadSel = PROGS[gProg][gProgAt]; gPadN = padChord(gPadBase, gPadSel, 0, 1, gPadNotes); padPlay(gPadBase, gPadNotes, gPadN, 0.8f, 1); gProgDirty = 1; }
	}
	for (int i = 0; i < gStrumOffN; i++) if ((gStrumOff[i].t -= dt) <= 0) { voiceOff(gStrumOff[i].note); gStrumOff[i] = gStrumOff[--gStrumOffN]; i--; }
}

// ---- MOD page, bottom screen: ROUTE (destination first) and LFO 3 / LFO 4 (paint the drawn LFOs).

// ---- MOTION: four lanes of movement, drawn with the stylus or recorded from the circle pad, looping over 1-8 bars
// in time with BPM. A lane that's ON sets its setting directly as it plays, like a knob being turned for you.
#define NMOT 4
#define NMTGT 16
static const char* MTGT_NAMES[NMTGT] = {"A WT POS", "B WT POS", "CUTOFF", "RES", "OUT CUT", "OUT RES", "A WARP", "B WARP", "FX COLOR", "DLY MIX", "REV MIX", "GRAIN SCAN", "MACRO 1", "MACRO 2", "MACRO 3", "MACRO 4"};
static u8 gMot[NMOT][64]; static int gMotTgt[NMOT] = {0, 2, 4, 12}, gMotOn[NMOT], gMotRec = -1, gMotRecArmed = 0, gMotBars = 1;
static float gMotPh = 0; static const int MOT_BARS[4] = {1, 2, 4, 8};
static float* motPtr(int t) {
	switch (t) {
	case 0: return &gOsc[0].pos; case 1: return &gOsc[1].pos; case 2: return &gCutoff; case 3: return &gRes;
	case 4: return &gOfCut; case 5: return &gOfRes; case 6: return &gOsc[0].warpAmt; case 7: return &gOsc[1].warpAmt;
	case 8: return &gFxColor; case 9: return &gDlyMix; case 10: return &gRevMix; case 11: return &gGrainScan;
	default: return &gMacro[t - 12];
	}
}
static void motTick(float dt) {   // main thread, inside the voice lock
	int any = gMotRec >= 0; for (int l = 0; l < NMOT; l++) any |= gMotOn[l];
	if (!any) return;
	float prev = gMotPh;
	gMotPh += dt * gBPM / 60.0f / (4.0f * MOT_BARS[gMotBars]);
	int wrapped = gMotPh >= 1; if (wrapped) gMotPh -= floorf(gMotPh);
	if (wrapped && gMotRec >= 0) {
		if (gMotRecArmed) { gMotRecArmed = 0; say("MOTION: RECORDING - MOVE THE CIRCLE PAD"); }
		else { gMotOn[gMotRec] = 1; gMotRec = -1; say("MOTION: LANE RECORDED"); }
	}
	int i = (int)(gMotPh * 64) & 63;
	if (gMotRec >= 0 && !gMotRecArmed) {   // write the circle pad's height into the armed lane
		circlePosition cp; hidCircleRead(&cp); float v = (cp.dy + 150) / 300.0f; if (v < 0) v = 0; if (v > 1) v = 1;
		int i0 = (int)(prev * 64) & 63; for (int j = i0; j != ((i + 1) & 63); j = (j + 1) & 63) gMot[gMotRec][j] = (u8)(v * 255);
	}
	float u = gMotPh * 64 - floorf(gMotPh * 64);
	for (int l = 0; l < NMOT; l++) {
		if (!gMotOn[l] || l == gMotRec) continue;
		float v = (gMot[l][i] * (1 - u) + gMot[l][(i + 1) & 63] * u) / 255.0f;
		*motPtr(gMotTgt[l]) = v;
	}
}
#define MOT_Y 58
#define MOT_LH 43
static void drawMotion(void) {
	char b[24]; snprintf(b, sizeof(b), "LOOP %d BAR%s", MOT_BARS[gMotBars], gMotBars ? "S" : "");
	rect(4, 41, 92, 14, RGB3(TH->tab)); text(8, 44, b, 1, RGB3(TH->tabTxt));
	text(104, 44, gMotRec >= 0 ? (gMotRecArmed ? "REC STARTS NEXT LOOP" : "RECORDING: CIRCLE PAD") : "DRAW, OR REC = CIRCLE PAD", 1, gMotRec >= 0 ? TH->warn[0] : TH->dim[0], gMotRec >= 0 ? TH->warn[1] : TH->dim[1], gMotRec >= 0 ? TH->warn[2] : TH->dim[2]);
	for (int l = 0; l < NMOT; l++) {
		int y = MOT_Y + l * MOT_LH, h = MOT_LH - 4;
		rect(4, y, 70, h, RGB3(TH->pad));
		text(8, y + 3, MTGT_NAMES[gMotTgt[l]], 1, RGB3(TH->val));
		const u8* oc = gMotOn[l] ? TH->keyOn : TH->tab; rect(8, y + 18, 28, 16, oc[0], oc[1], oc[2]); text(12, y + 22, "ON", 1, gMotOn[l] ? 0 : TH->tabTxt[0], gMotOn[l] ? 0 : TH->tabTxt[1], gMotOn[l] ? 0 : TH->tabTxt[2]);
		const u8* rc = gMotRec == l ? TH->warn : TH->tab; rect(40, y + 18, 30, 16, rc[0], rc[1], rc[2]); text(44, y + 22, "REC", 1, RGB3(TH->tabTxt));
		rect(78, y, 238, h, RGB3(TH->pad));
		for (int i = 0; i < 64; i++) {
			int x = 78 + i * 238 / 64, w = 238 / 64, v = gMot[l][i] * (h - 2) / 255;
			rect(x, y + h - 1 - v, w, v + 1, gMotOn[l] ? TH->line[0] : TH->dim[0], gMotOn[l] ? TH->line[1] : TH->dim[1], gMotOn[l] ? TH->line[2] : TH->dim[2]);
		}
		for (int q = 1; q < 4; q++) rect(78 + q * 238 / 4, y, 1, h, RGB3(TH->grid));
		rect(78 + (int)(gMotPh * 238), y, 1, h, RGB3(TH->keyOn));
	}
}
static int gMotDrawL = -1, gMotPrevI = -1;
static int motionTouch(int x, int y, int press, int release) {
	if (release) { gMotDrawL = -1; gMotPrevI = -1; return 0; }
	if (press && y >= 40 && y < 56 && x < 96) { gMotBars = (gMotBars + 1) % 4; gEdited = 1; return 1; }
	int l = (y - MOT_Y) / MOT_LH; if (y < MOT_Y || l < 0 || l >= NMOT) { gMotPrevI = -1; return 0; }
	int ly = y - MOT_Y - l * MOT_LH, h = MOT_LH - 4;
	if (x < 76) {
		if (!press) return 0;
		if (ly < 16) gMotTgt[l] = (gMotTgt[l] + 1) % NMTGT;
		else if (x < 38) gMotOn[l] = !gMotOn[l];
		else { if (gMotRec == l) gMotRec = -1; else { gMotRec = l; gMotRecArmed = 1; say("MOTION: REC STARTS AT THE NEXT LOOP"); } }
		gEdited = 1; return 1;
	}
	if (press) gMotDrawL = l;
	if (gMotDrawL != l) return 0;
	int i = (x - 78) * 64 / 238; if (i < 0) i = 0; if (i > 63) i = 63;
	float v = 1 - ly / (float)h; if (v < 0) v = 0; if (v > 1) v = 1;
	if (gMotPrevI < 0) gMotPrevI = i;
	int a = gMotPrevI < i ? gMotPrevI : i, b = gMotPrevI < i ? i : gMotPrevI; float pv = gMot[l][gMotPrevI] / 255.0f;
	for (int j = a; j <= b; j++) { float t = b > a ? (float)(j - (gMotPrevI < i ? a : b)) / (b - a) : 1; if (t < 0) t = -t; gMot[l][j] = (u8)((pv + (v - pv) * t) * 255); }
	gMot[l][i] = (u8)(v * 255); gMotPrevI = i;
	if (!gMotOn[l]) gMotOn[l] = 1;   // drawing a lane switches it on
	gEdited = 1; return 1;
}
static int gModView = 0, gModOn = 0, gLastTX = 0, gLastTY = 0;   // view: 0 matrix, 1 LFO 1+2, 2 draw LFO 3, 3 draw LFO 4, 4 motion
// MATRIX view: the 8 mod slots as two columns, source on the left, destination on the right, a line between them.
// Tap a box to pick from a list; drag along the line to set the amount (centre 0, right +100, left -100).
// The dot on each line rides the source's live value.
static const int MX_SRCS[30] = {SRC_OFF, SRC_LFO1, SRC_LFO2, SRC_LFO3, SRC_LFO4, SRC_ENV2, SRC_VEL, SRC_NOTE, SRC_RAND, SRC_STICK, SRC_XY,
	SRC_M1, SRC_M2, SRC_M3, SRC_M4, SRC_CHAOSX, SRC_CHAOSY, SRC_TILTX, SRC_TILTY, SRC_TWIST, SRC_SPINX, SRC_SPINY, SRC_SHAKE,
	SRC_MIC, SRC_MICBRT, SRC_CAML, SRC_CAMC, SRC_CAMMOVE, SRC_PRESS, SRC_MSLIDE};
static const u8 MX_COL[8] = {13, 11, 14, 10, 12, 9, 6, 15};   // EGA colours per slot
#define MX_Y 40
#define MX_PITCH 25
#define MX_H 19
#define MX_LX 4
#define MX_RX 208
#define MX_BW 108
#define MX_MID 160
#define MX_HALF 44
static int gMxPick = -1, gMxPickDst = 0, gMxDrag = -1, gDrawPrevX = -1;
static const SVoice* mxVoice(void) { for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage < 4) return &gV[i]; return NULL; }
static void boxLine(int x, int y, int w, int h, const u8* c) { rect(x, y, w, 1, c[0], c[1], c[2]); rect(x, y + h - 1, w, 1, c[0], c[1], c[2]); rect(x, y, 1, h, c[0], c[1], c[2]); rect(x + w - 1, y, 1, h, c[0], c[1], c[2]); }
static void drawMatrix(void) {
	char t[24];
	if (gMxPick >= 0) {   // picker: sources in 3 x 10, destinations in 3 x 5
		snprintf(t, sizeof(t), "SLOT %d %s", gMxPick + 1, gMxPickDst ? "DESTINATION" : "SOURCE"); text(6, 40, t, 1, RGB3(TH->val));
		rect(256, 37, 60, 13, RGB3(TH->tab)); text(262, 40, "CANCEL", 1, RGB3(TH->tabTxt));
		int n = gMxPickDst ? DST_COUNT : 30, bh = gMxPickDst ? 30 : 15, pitch = bh + 2, cur = gMxPickDst ? gMod[gMxPick].dst : gMod[gMxPick].src;
		for (int i = 0; i < n; i++) {
			int x = 4 + (i % 3) * 105, y = 53 + (i / 3) * pitch, v = gMxPickDst ? i : MX_SRCS[i];
			const char* nm = gMxPickDst ? DST_NAMES[i] : SRC_NAMES[v]; int on = v == cur;
			const u8* bc = on ? TH->keyOn : TH->tab; rect(x, y, 101, bh, bc[0], bc[1], bc[2]);
			const u8* tc = on ? TH->ink : TH->tabTxt; text(x + 50 - (int)strlen(nm) * 4, y + bh / 2 - 3, nm, 1, tc[0], tc[1], tc[2]);
		}
		return;
	}
	const SVoice* v = mxVoice();
	for (int m = 0; m < NMOD; m++) {
		int y = MX_Y + m * MX_PITCH, on = gMod[m].src != SRC_OFF; float a = gMod[m].amt;
		const u8* c = on ? EGA[MX_COL[m]] : TH->grid; const u8* tc = on ? TH->txt : TH->dim;
		boxLine(MX_LX, y, MX_BW, MX_H, c); boxLine(MX_RX, y, MX_BW, MX_H, c);
		const char* sn = on ? SRC_NAMES[gMod[m].src] : "+ SOURCE";
		text(MX_LX + 6, y + 6, sn, 1, tc[0], tc[1], tc[2]);
		text(MX_RX + 6, y + 6, DST_NAMES[gMod[m].dst], 1, tc[0], tc[1], tc[2]);
		int ly = y + MX_H / 2, x0 = MX_LX + MX_BW, x1 = MX_RX;
		rect(x0, ly - (on ? 1 : 0), x1 - x0, on ? 3 : 1, c[0], c[1], c[2]);
		int w = (int)(a * MX_HALF + (a >= 0 ? 0.5f : -0.5f)); const u8* bc = (a >= 0) ? c : TH->warn;
		if (w > 0) rect(MX_MID, ly + 4, w, 3, bc[0], bc[1], bc[2]); else if (w < 0) rect(MX_MID + w, ly + 4, -w, 3, bc[0], bc[1], bc[2]);
		rect(MX_MID, ly + 3, 1, 5, RGB3(TH->dim));
		if (on) {   // live dot
			float sv = srcValue(gMod[m].src, v); if (sv > 1) sv = 1; if (sv < -1) sv = -1;
			int dx = x0 + 3 + (int)((sv + 1) * 0.5f * (x1 - x0 - 7));
			rect(dx - 2, ly - 2, 5, 5, 255, 255, 255); rect(dx - 3, ly - 1, 7, 3, 255, 255, 255);
		}
		if (m == gMxDrag) { snprintf(t, sizeof(t), "%+d", (int)roundf(a * 100)); text(MX_MID - (int)strlen(t) * 4, y, t, 1, RGB3(TH->val)); }
	}
}
static int gMxHold = -1, gMxHoldDst = 0; static u64 gMxHoldT;   // a press on a slot's box: short = pick, long = clear
static int matrixTouch(int x, int y, int press, int release) {
	if (gMxHold >= 0 && !press) {
		if (release) { gMxPick = gMxHold; gMxPickDst = gMxHoldDst; gMxHold = -1; return 1; }
		if (svcGetSystemTick() - gMxHoldT > SYSCLOCK_ARM11 * 2 / 5) {
			ModSlot* ms = &gMod[gMxHold]; ms->src = SRC_OFF; ms->amt = 0; gModCurve[gMxHold] = 0; gEdited = 1;
			char t[32]; snprintf(t, sizeof(t), "SLOT %d CLEARED", gMxHold + 1); say(t); gMxHold = -2; return 1;
		}
		return 0;
	}
	if (gMxHold == -2) { if (release) gMxHold = -1; return 0; }   // cleared: wait for the finger to lift
	if (release) { int was = gMxDrag >= 0; gMxDrag = -1; return was; }
	if (gMxPick >= 0) {
		if (!press) return 0;
		if (y < 52) { if (x >= 256) { gMxPick = -1; return 1; } return 0; }
		int bh = gMxPickDst ? 30 : 15, c = (x - 4) / 105, r = (y - 53) / (bh + 2), i = r * 3 + c, n = gMxPickDst ? DST_COUNT : 30;
		if (c < 0 || c > 2 || i < 0 || i >= n) return 0;
		ModSlot* ms = &gMod[gMxPick];
		if (gMxPickDst) ms->dst = i;
		else {
			int was = ms->src; ms->src = MX_SRCS[i];
			if (ms->src == SRC_OFF) ms->amt = 0;
			else if (was == SRC_OFF || ms->amt == 0) { ms->amt = 0.5f; gModCurve[gMxPick] = 0; }
		}
		gMxPick = -1; gEdited = 1; return 1;
	}
	if (y < MX_Y) return 0;
	int m = (y - MX_Y) / MX_PITCH; if (m >= NMOD) return 0;
	if (press) {
		if (x < MX_LX + MX_BW || x >= MX_RX) { gMxHold = m; gMxHoldDst = x >= MX_RX; gMxHoldT = svcGetSystemTick(); return 0; }
		if (gMod[m].src == SRC_OFF) { say("PICK A SOURCE FIRST (LEFT BOX)"); return 0; }
		gMxDrag = m;
	}
	if (gMxDrag >= 0) {
		float a = (x - MX_MID) / (float)MX_HALF; if (a > 1) a = 1; if (a < -1) a = -1;
		gMod[gMxDrag].amt = roundf(a * 50) / 50; gEdited = 1; return 1;
	}
	return 0;
}
// ---- picker shared by the shape editors: a grid of named boxes, like the matrix's source list
static void pickGrid(const char* title, const char* const* names, int n, int cur) {
	text(6, 40, title, 1, RGB3(TH->val));
	rect(256, 37, 60, 13, RGB3(TH->tab)); text(262, 40, "CANCEL", 1, RGB3(TH->tabTxt));
	int rows = (n + 2) / 3, bh = (182 / rows) - 2; if (bh > 30) bh = 30;
	for (int i = 0; i < n; i++) {
		int x = 4 + (i % 3) * 105, y = 53 + (i / 3) * (bh + 2), on = i == cur;
		const u8* bc = on ? TH->keyOn : TH->tab; rect(x, y, 101, bh, bc[0], bc[1], bc[2]);
		const u8* tc = on ? TH->ink : TH->tabTxt; text(x + 50 - (int)strlen(names[i]) * 4, y + bh / 2 - 3, names[i], 1, tc[0], tc[1], tc[2]);
	}
}
static int pickHit(int x, int y, int n) {   // -2 cancel, -1 nothing, else the index
	if (y < 52) return x >= 256 ? -2 : -1;
	int rows = (n + 2) / 3, bh = (182 / rows) - 2; if (bh > 30) bh = 30;
	int c = (x - 4) / 105, r = (y - 53) / (bh + 2), i = r * 3 + c;
	return (c < 0 || c > 2 || i < 0 || i >= n) ? -1 : i;
}
// LFO 1+2: shape and sync as tap lists, the wave drawn with its live position, drag across it for the rate
static int gLfPick = -1, gLfPickSync = 0, gLfDrag = -1;
static void drawLfo12(void) {
	char t[32];
	if (gLfPick >= 0) {
		snprintf(t, sizeof(t), "LFO %d %s", gLfPick + 1, gLfPickSync ? "SYNC" : "SHAPE");
		if (gLfPickSync) pickGrid(t, SYNC_NAMES, NSYNC, gLfo[gLfPick].sync); else pickGrid(t, LFO_SHAPES, NLFOSH, gLfo[gLfPick].shape);
		return;
	}
	for (int k = 0; k < 2; k++) {
		LFO* l = &gLfo[k]; int y0 = 40 + k * 100; const u8* c = EGA[k ? 11 : 13];
		boxLine(4, y0, 120, 16, c); text(10, y0 + 5, LFO_SHAPES[l->shape], 1, RGB3(TH->txt));
		boxLine(128, y0, 76, 16, c); snprintf(t, sizeof(t), "%s", l->sync ? SYNC_NAMES[l->sync] : "FREE"); text(134, y0 + 5, t, 1, RGB3(TH->txt));
		if (!l->sync) { snprintf(t, sizeof(t), "%.2f HZ", l->rate); text(316 - (int)strlen(t) * 8, y0 + 5, t, 1, RGB3(TH->val)); }
		snprintf(t, sizeof(t), "LFO %d", k + 1); text(212, y0 + 5, t, 1, RGB3(TH->dim));
		int wx = 4, wy = y0 + 20, ww = 312, wh = 70; boxLine(wx, wy, ww, wh, TH->grid); rect(wx + 1, wy + wh / 2, ww - 2, 1, RGB3(TH->grid));
		int py = -1;
		for (int x = 0; x < ww - 4; x++) {
			float v = lfoShapeAt(l, (float)x / (ww - 4)); if (v > 1) v = 1; if (v < -1) v = -1;
			int y = wy + wh / 2 - (int)(v * (wh / 2 - 4));
			if (py >= 0) { int a = py < y ? py : y, b = py < y ? y : py; rect(wx + 2 + x, a, 1, b - a + 1, c[0], c[1], c[2]); } py = y;
		}
		float ph = l->ph - floorf(l->ph); int dx = wx + 2 + (int)(ph * (ww - 4)); float v = l->val; if (v > 1) v = 1; if (v < -1) v = -1;
		int dy = wy + wh / 2 - (int)(v * (wh / 2 - 4)); rect(dx - 2, dy - 2, 5, 5, 255, 255, 255);
		if (!l->sync) text(wx + 6, wy + wh - 11, "DRAG FOR SPEED", 1, RGB3(TH->dim));
	}
}
static int lfo12Touch(int x, int y, int press, int release) {
	if (release) { int w = gLfDrag >= 0; gLfDrag = -1; return w; }
	if (gLfPick >= 0) {
		if (!press) return 0;
		int i = pickHit(x, y, gLfPickSync ? NSYNC : NLFOSH); if (i == -1) return 0;
		if (i >= 0) { if (gLfPickSync) gLfo[gLfPick].sync = i; else gLfo[gLfPick].shape = i; gEdited = 1; }
		gLfPick = -1; return 1;
	}
	int k = y < 140 ? 0 : 1, y0 = 40 + k * 100;
	if (press && y >= y0 && y < y0 + 16) {
		if (x < 124) { gLfPick = k; gLfPickSync = 0; return 1; }
		if (x >= 128 && x < 204) { gLfPick = k; gLfPickSync = 1; return 1; }
		return 0;
	}
	if (press && y >= y0 + 20 && y < y0 + 90 && !gLfo[k].sync) gLfDrag = k;
	if (gLfDrag >= 0) {   // 0.05 .. 20 Hz across the width, logarithmic
		float t = (x - 4) / 312.0f; t = t < 0 ? 0 : t > 1 ? 1 : t;
		gLfo[gLfDrag].rate = roundf(0.05f * powf(400.0f, t) * 100) / 100; if (gLfo[gLfDrag].rate < 0.05f) gLfo[gLfDrag].rate = 0.05f;
		gEdited = 1; return 1;
	}
	return 0;
}

// FILT+ENV: the area above the keyboard becomes an editor (ENV 1 / ENV 2 / FILTER), so you can play while you shape
static int gShapeOn = 0, gShapeView = 0, gShDrag = -1;
#define SH_Y 38
#define SH_H 76
static const float ENV_SEGW[5] = {64, 34, 64, 34, 80};   // A, H, D, sustain hold, R: widths at their longest
static const float ENV_MAXT[5] = {4, 2, 4, 0, 5};
static void drawStartMenu(void) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	int mw = 200, mh = NSTART * 22 + 44, mx = 200 - mw / 2, my = 120 - mh / 2;
	rect(mx, my, mw, mh, RGB3(TH->bg)); outline(mx, my, mw, mh, RGB3(TH->line)); outline(mx + 2, my + 2, mw - 4, mh - 4, RGB3(TH->grid));
	text(200 - 5 * 8, my + 8, "MENU", 2, RGB3(TH->val));
	for (int i = 0; i < NSTART; i++) {
		int y = my + 30 + i * 22, on = i == gStartSel;
		if (on) rect(mx + 10, y - 3, mw - 20, 16, RGB3(TH->keyOn));
		const u8* c = on ? TH->ink : TH->txt;
		char nm[32]; if (i == 4) snprintf(nm, sizeof(nm), "< THEME: %s >", THEME_NAMES[gTheme]); else snprintf(nm, sizeof(nm), "%s", START_NAMES[i]);   // THEME shows which, < > / L R change it
		text(200 - (int)strlen(nm) * 4, y + 1, nm, 1, c[0], c[1], c[2]);
	}
	text(200 - 16 * 4, my + mh - 12, gStartSel == 4 ? "< > L R: THEME  A: ALL" : "A: OK  B: CLOSE", 1, RGB3(TH->dim));
}
// QUICK SELECT: hold SELECT and push the circle pad toward a place, let go of SELECT to go there. Sixteen spots
// all the way round (the 8 directions and the 8 between): every page, the TAPE, and the transport along the bottom
// (REC, PLAY / STOP straight down, TAPE REC). Centred, nothing happens (a tap of SELECT is still PLAY / STOP).
static int gQuickBar = -1, gQuickIdx = 0, gQuickDir = 0, gQuickOpen = 0;   // open: the web QUICK button's tap version of the ring   // bar: 0 = a spot is lit, -1 = none
#define NQR 16
enum { QA_REC = -1, QA_PLAY = -2, QA_TREC = -3, QA_TAPE = -4 };
static const char* QR_NAME[NQR] = {"PATCH", "OSC A", "OSC B", "SUB", "FILTER", "MOD", "VOICE", "REC", "PLAY", "TAPE REC", "TAPE", "SEQ", "DRUMS", "FX", "HW", "VIS"};
static const int QR_DEST[NQR] = {0, 1, 2, 3, 4, 5, 6, QA_REC, QA_PLAY, QA_TREC, QA_TAPE, 7, 11, 10, 9, 8};   // a page, or an action
static int quickHere(int page) {   // the spot for the page you're on (TAPE when the tape view is up)
	if (page == 7 && gPatView == 2) return 10;
	for (int i = 0; i < NQR; i++) if (QR_DEST[i] == page) return i;
	return 0;
}
static const char* presetName(int st, int ix);
static void blendRect(int x0, int y0, int w, int h, const u8* c, int a) {   // a / 256 of c over what's there
	for (int x = x0; x < x0 + w; x++) for (int y = y0; y < y0 + h; y++) {
		if (x < 0 || x >= fbW || y < 0 || y >= 240) continue;
		u8* p = fb + (x * 240 + (239 - y)) * 3;
		p[0] = (u8)((p[0] * (256 - a) + c[2] * a) >> 8); p[1] = (u8)((p[1] * (256 - a) + c[1] * a) >> 8); p[2] = (u8)((p[2] * (256 - a) + c[0] * a) >> 8);
	}
}
static void quickPanel(int x, int y, int w, int h, int lit) {   // a floating card: shadow, glassy fill, a bright edge
	static const u8 K[3] = {0, 0, 0};
	blendRect(x + 3, y + 3, w, h, K, 140);
	blendRect(x, y, w, h, TH->bg, 232);
	if (lit) blendRect(x, y, w, h, TH->keyOn, 60);
	outline(x, y, w, h, RGB3(lit ? TH->keyOn : TH->line)); rect(x + 1, y + 1, w - 2, 1, RGB3(TH->grid));
}
static int quickTapeRec(void);
static void drawQuick(int page) {   // a layer over the page you're on: the page shows through, dimmed
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	static const u8 K[3] = {0, 0, 0};
	blendRect(0, 0, 400, 240, K, 200);
	const u8* on = TH->keyOn; int here = quickHere(page), cx = 200, cy = 118;
	for (int i = 0; i < NQR; i++) {   // round the ring, clockwise from the top
		float a = i * (6.2831853f / NQR); int x = cx + (int)lroundf(sinf(a) * 158), y = cy - (int)lroundf(cosf(a) * 94);
		const char* nm = QR_NAME[i]; char t[12];
		if (QR_DEST[i] == QA_PLAY) nm = gSeqMode == 1 ? "STOP" : "PLAY";
		if (QR_DEST[i] == QA_TREC && quickTapeRec()) nm = "TAPE STOP";
		snprintf(t, sizeof(t), "%s", nm);
		int w = (int)strlen(t) * 8 + 10, lit = gQuickBar >= 0 && gQuickIdx == i;
		quickPanel(x - w / 2, y - 9, w, 18, lit);
		if (i == here && !lit) outline(x - w / 2 + 1, y - 8, w - 2, 16, RGB3(TH->val));
		const u8* c = lit ? TH->ink : (QR_DEST[i] == QA_REC && gLiveRec) || (QR_DEST[i] == QA_TREC && quickTapeRec()) ? TH->warn : TH->tabTxt;
		if (lit) rect(x - w / 2 + 2, y - 7, w - 4, 14, RGB3(on));
		text(x - (int)strlen(t) * 4, y - 4, t, 1, c[0], c[1], c[2]);
	}
	// the stick, in the middle: where you're pushing
	{ outline(cx - 22, cy - 22, 45, 45, RGB3(TH->grid));
	  if (gQuickBar >= 0) { float a = gQuickIdx * (6.2831853f / NQR); rect(cx - 4 + (int)(sinf(a) * 14), cy - 4 - (int)(cosf(a) * 14), 9, 9, RGB3(on)); }
	  else rect(cx - 3, cy - 3, 7, 7, RGB3(TH->line));
	  const char* h = gQuickOpen ? "TAP ONE  (B: CLOSE)" : "PUSH TOWARD ONE";
	  if (gQuickBar >= 0) { int d = QR_DEST[gQuickIdx]; h = d >= 0 ? PAGE_NAMES[d] : d == QA_TAPE ? "TAPE" : "LET GO OF SELECT"; }
	  text(cx - (int)strlen(h) * 4, cy + 28, h, 1, RGB3(TH->val)); }
}
#define ENV_X0 16                // inset from the edge: a stylus can't reach the last pixels under the bezel
#define ENV_TOP (SH_Y + 14)       // clear of the tabs above and the A/D/S/R readout
#define ENV_BOT (SH_Y + SH_H - 14)   // clear of the PERF buttons below
static AHDSR* shEnv(void) { return gShapeView == 0 ? &gEnv1 : &gEnv2; }
static void envPoints(const AHDSR* e, int hasHold, float* px, float* py) {   // 5 points after the start
	float x = ENV_X0, top = ENV_TOP, bot = ENV_BOT, sus = bot - (bot - top) * e->s;
	float ts[4] = {e->a, hasHold ? e->h : 0, e->d, e->r};
	x += ENV_SEGW[0] * sqrtf(ts[0] / ENV_MAXT[0]); px[0] = x; py[0] = top;
	x += hasHold ? ENV_SEGW[1] * sqrtf(ts[1] / ENV_MAXT[1]) : 0; px[1] = x; py[1] = top;
	x += ENV_SEGW[2] * sqrtf(ts[2] / ENV_MAXT[2]); px[2] = x; py[2] = sus;
	x += ENV_SEGW[3]; px[3] = x; py[3] = sus;
	x += ENV_SEGW[4] * sqrtf(ts[3] / ENV_MAXT[4]); px[4] = x; py[4] = bot;
}
static void drawShapeEditor(void) {
	rect(0, TAB_H, 320, WORK_B - TAB_H, RGB3(TH->bg));
	static const char* ST[3] = {"ENV 1 (AMP)", "ENV 2 (MOD)", "FILTER"};
	for (int i = 0; i < 3; i++) {
		int x = 4 + i * 105, lit = i == gShapeView; rect(x, TAB_H + 2, 101, 14, lit ? TH->keyOn[0] : TH->tab[0], lit ? TH->keyOn[1] : TH->tab[1], lit ? TH->keyOn[2] : TH->tab[2]);
		const u8* c = lit ? TH->ink : TH->tabTxt; text(x + 50 - (int)strlen(ST[i]) * 4, TAB_H + 5, ST[i], 1, c[0], c[1], c[2]);
	}
	char t[48];
	if (gShapeView < 2) {
		AHDSR* e = shEnv(); int hold = gShapeView == 0; float px[5], py[5]; envPoints(e, hold, px, py);
		const u8* c = EGA[gShapeView ? 11 : 13]; float bot = ENV_BOT;
		float lx = ENV_X0, ly = bot;
		for (int k = 0; k < 5; k++) {   // filled shape, then the line
			int x0 = (int)lx, x1 = (int)px[k];
			for (int x = x0; x < x1; x++) { float f = (x - lx) / (px[k] - lx + 1e-6f), y = ly + (py[k] - ly) * f; rect(x, (int)y, 1, (int)(bot - y) + 1, c[0] / 3, c[1] / 3, c[2] / 3); }
			line((int)lx, (int)ly, (int)px[k], (int)py[k], c[0], c[1], c[2]); line((int)lx, (int)ly + 1, (int)px[k], (int)py[k] + 1, c[0], c[1], c[2]);
			lx = px[k]; ly = py[k];
		}
		static const int HAND[4] = {0, 1, 2, 4};
		for (int h = 0; h < 4; h++) { if (h == 1 && !hold) continue; int k = HAND[h]; rect((int)px[k] - 4, (int)py[k] - 4, 9, 9, h == gShDrag ? 255 : c[0], h == gShDrag ? 255 : c[1], h == gShDrag ? 255 : c[2]); }
		if (hold) snprintf(t, sizeof(t), "A %.2f H %.2f D %.2f S %d%% R %.2f", e->a, e->h, e->d, (int)(e->s * 100 + 0.5f), e->r);
		else snprintf(t, sizeof(t), "A %.2f  D %.2f  S %d%%  R %.2f", e->a, e->d, (int)(e->s * 100 + 0.5f), e->r);
		text(314 - (int)strlen(t) * 8, SH_Y, t, 1, RGB3(TH->dim));
	} else {   // filter: the response across the editor, drag the dot (left-right cutoff, up-down resonance)
		int ix = 6, iw = 308, iy = SH_Y + 2, ih = SH_H - 18; int py = -1;
		for (int x = 0; x < iw; x++) {
			float f = 30 * powf(500.0f, (float)x / (iw - 1)), db = 20 * log10f(filterGain(f) + 1e-6f); if (db > 12) db = 12; if (db < -36) db = -36;
			int y = iy + (int)((12 - db) / 48 * (ih - 1));
			rect(ix + x, y, 1, iy + ih - y, RGB3(TH->pad));
			if (py >= 0) { int a = py < y ? py : y, b = py < y ? y : py; rect(ix + x, a, 1, b - a + 1, RGB3(TH->val)); } py = y;
		}
		float hz = baseCutoff(); int cx = ix + (int)(logf(hz / 30) / logf(500.0f) * (iw - 1)), cy = iy + ih - 1 - (int)(gRes * (ih - 6));
		rect(cx - 3, cy - 3, 7, 7, 255, 255, 255);
		static const char* FT[5] = {"LP", "HP", "BP", "NOTCH", "PEAK"};
		for (int i = 0; i < 5; i++) { int x = 6 + i * 46, on = i == gFiltType; rect(x, WORK_B - 15, 42, 12, on ? TH->keyOn[0] : TH->tab[0], on ? TH->keyOn[1] : TH->tab[1], on ? TH->keyOn[2] : TH->tab[2]);
			const u8* tc = on ? TH->ink : TH->tabTxt; text(x + 21 - (int)strlen(FT[i]) * 4, WORK_B - 13, FT[i], 1, tc[0], tc[1], tc[2]); }
		snprintf(t, sizeof(t), "F2 %s", FILT2_NAMES[gFilt2]); rect(240, WORK_B - 15, 76, 12, RGB3(TH->tab)); text(244, WORK_B - 13, t, 1, RGB3(TH->tabTxt));
		snprintf(t, sizeof(t), hz >= 1000 ? "%.1fK RES %d%%" : "%.0f RES %d%%", hz >= 1000 ? hz / 1000 : hz, (int)(gRes * 100 + 0.5f)); text(314 - (int)strlen(t) * 8, SH_Y + 2, t, 1, RGB3(TH->val));
	}
}
static int shapeTouch(int x, int y, int press, int release) {   // 1 = redraw
	if (release) { int w = gShDrag >= 0; gShDrag = -1; return w; }
	if (press && y < TAB_H + 18) { int v = (x - 4) / 105; if (v >= 0 && v < 3) gShapeView = v; return 1; }
	if (gShapeView == 2) {
		if (press && y >= WORK_B - 16) {
			if (x < 236) { int i = (x - 6) / 46; if (i >= 0 && i < 5) { gFiltType = i; gEdited = 1; } }
			else { gFilt2 = (gFilt2 + 1) % 3; gEdited = 1; }
			return 1;
		}
		if (press) gShDrag = 0;
		if (gShDrag < 0) return 0;
		int iw = 308, iy = SH_Y + 2, ih = SH_H - 18;
		float f = 30 * powf(500.0f, (x - 6) / (float)(iw - 1)); if (f < 60) f = 60; if (f > 15000) f = 15000;
		gCutoff = logf(f / 60.0f) / logf(15000.0f / 60.0f);
		float r = (iy + ih - 1 - y) / (float)(ih - 6); gRes = r < 0 ? 0 : r > 1 ? 1 : r;
		gEdited = 1; return 1;
	}
	AHDSR* e = shEnv(); int hold = gShapeView == 0; float px[5], py[5]; envPoints(e, hold, px, py);
	static const int HAND[4] = {0, 1, 2, 4};
	if (press) {   // grab the nearest handle
		float best = 1e9f; gShDrag = -1;
		for (int h = 0; h < 4; h++) { if (h == 1 && !hold) continue; float dx = x - px[HAND[h]], dy = y - py[HAND[h]], d = dx * dx + dy * dy; if (d < best) { best = d; gShDrag = h; } }
		if (best > 50 * 50) gShDrag = -1;
	}
	if (gShDrag < 0) return 0;
	float start = gShDrag == 0 ? ENV_X0 : gShDrag == 1 ? px[0] : gShDrag == 2 ? px[1] : px[3];
	int seg = gShDrag == 0 ? 0 : gShDrag == 1 ? 1 : gShDrag == 2 ? 2 : 4;
	float f = (x - start) / ENV_SEGW[seg]; f = f < 0 ? 0 : f > 1 ? 1 : f; float tt = ENV_MAXT[seg] * f * f;
	if (gShDrag == 0) e->a = roundf(tt * 1000) / 1000;
	else if (gShDrag == 1) e->h = roundf(tt * 100) / 100;
	else if (gShDrag == 2) { e->d = tt < 0.01f ? 0.01f : roundf(tt * 100) / 100; float top = ENV_TOP, bot = ENV_BOT, sv = (bot - y) / (bot - top); e->s = sv < 0 ? 0 : sv > 1 ? 1 : roundf(sv * 100) / 100; }
	else e->r = tt < 0.01f ? 0.01f : roundf(tt * 100) / 100;
	gEdited = 1; return 1;
}
static void drawModView(void) {
	rect(0, TAB_H, 320, 240 - TAB_H, RGB3(TH->bg));
	static const char* VT[5] = {"MATRIX", "LFO 1+2", "LFO 3", "LFO 4", "MOTION"};
	for (int i = 0; i < 5; i++) {
		int x = 4 + i * 63, lit = i == gModView; rect(x, TAB_H + 2, 59, 14, lit ? TH->keyOn[0] : TH->tab[0], lit ? TH->keyOn[1] : TH->tab[1], lit ? TH->keyOn[2] : TH->tab[2]);
		const u8* c = lit ? TH->ink : TH->tabTxt; text(x + 29 - (int)strlen(VT[i]) * 4, TAB_H + 5, VT[i], 1, c[0], c[1], c[2]);
	}
	if (gModView == 4) { drawMotion(); return; }
	if (gModView == 0) { drawMatrix(); return; }
	if (gModView == 1) { drawLfo12(); return; }
	{
		int k = gModView - 2, x0 = 8, y0 = 46, w = 304, h = 150;
		rect(x0, y0, w, h, RGB3(TH->pad)); rect(x0, y0 + h / 2, w, 1, RGB3(TH->grid));
		for (int i = 0; i < 64; i++) {
			int x = x0 + i * w / 64, bw = w / 64, v = (int)(gDl[k][i] * (h / 2 - 2)), mid = y0 + h / 2;
			if (v >= 0) rect(x, mid - v, bw - 1, v + 1, RGB3(TH->line)); else rect(x, mid, bw - 1, -v + 1, RGB3(TH->line));
		}
		int px2 = x0 + (int)(gDlPh[k] * w); rect(px2, y0, 1, h, RGB3(TH->keyOn));
		static const char* SH[5] = {"SINE", "RAMP", "STEPS", "RANDOM", "FLAT"};
		for (int i = 0; i < 5; i++) { int x = 8 + i * 61; rect(x, 202, 57, 16, RGB3(TH->tab)); text(x + 28 - (int)strlen(SH[i]) * 4, 206, SH[i], 1, RGB3(TH->tabTxt)); }
		text(8, 224, "PAINT THE SHAPE. RATE / SYNC: MOD PAGE", 1, RGB3(TH->dim));
	}
}
static int modViewTouch(int x, int y, int press, int release) {   // 1 = redraw
	if (press && y < TAB_H + 18) { int v = (x - 4) / 63; if (v >= 0 && v < 5) { gModView = v; gMxDrag = -1; gMxPick = -1; gLfPick = -1; } return 1; }
	if (gModView == 4) return motionTouch(x, y, press, release);
	if (gModView == 0) return matrixTouch(x, y, press, release);
	if (gModView == 1) return lfo12Touch(x, y, press, release);
	int k = gModView - 2;
	if (press && y >= 202 && y < 218) {   // shape presets
		int b = (x - 8) / 61;
		for (int i = 0; i < 64; i++) {
			float v = b == 0 ? sinf(TWO_PI * i / 64) : b == 1 ? 1 - i / 31.5f : b == 2 ? ((i / 8) % 2 ? 0.8f : -0.8f) * ((i / 16) % 2 ? 0.5f : 1) : b == 3 ? (rand() % 2001) / 1000.0f - 1 : 0;
			if (b == 3 && i % 4) v = gDl[k][i - 1];
			gDl[k][i] = v;
		}
		gEdited = 1; return 1;
	}
	if (y < 46 || y >= 196) { gDrawPrevX = -1; return 0; }
	int i = (x - 8) * 64 / 304; if (i < 0) i = 0; if (i > 63) i = 63;
	float v = 1 - (y - 46) / 75.0f; if (v > 1) v = 1; if (v < -1) v = -1;
	if (press || gDrawPrevX < 0) gDrawPrevX = i;
	int a = gDrawPrevX < i ? gDrawPrevX : i, b = gDrawPrevX < i ? i : gDrawPrevX;
	for (int j = a; j <= b; j++) {   // fill between touch samples so fast strokes leave no gaps
		float t = b > a ? (float)(j - a) / (b - a) : 1; float pv = gDl[k][gDrawPrevX];
		gDl[k][j] = gDrawPrevX < i ? pv + (v - pv) * t : v + (pv - v) * (1 - t);
	}
	gDl[k][i] = v; gDrawPrevX = i;
	if (release) gDrawPrevX = -1;
	gEdited = 1; return 1;
}
// FX CHAIN (MASTER FX page): the chain as a row of blocks, in the order they run. Tap a block: its settings come up
// on the top screen and its types show below the chain (tap one to change the effect). Drag a block sideways and it
// moves there while you listen. Lit = the block is doing something. PUMP and the limiter stay at the end.
static int gChainOn = 0, gChDrag = -1, gChDragX = 0, gChPress = -1, gChPressX = 0, gChSel = -1;
static const char* gSelReq = NULL;   // a param name the top screen should select (the main loop does it)
static void dlyTypeChanged(void);
#define CH_Y (TAB_H + 16)
#define CH_H 18
#define CH_X(i) (6 + (i) * 52)
#define CT_Y (CH_Y + CH_H + 5)   // first type row
#define CT_BH 21   // type buttons: big enough for a thumb
// where type button i of n sits: one row per setting when the block has two (SPACE: delay, reverb), else up to two rows
static void ctRect(int k, int np, int i, int n, int* x, int* y, int* w) {
	int per = np > 1 || n <= 4 ? n : (n + 1) / 2, row = np > 1 ? k : i / per, col = np > 1 ? i : i % per;
	*w = 282 / per - 2; *x = 34 + col * (282 / per); *y = CT_Y + row * (CT_BH + 2);
}
static const char* FXB_FIRST[NFXB] = {"LOFI BITS", "MOD FX", "EQ LOW", "DELAY TYPE", "OTT"};
static const char* FXB_TYPES[NFXB][2] = {{"LOFI BITS", NULL}, {"MOD FX", NULL}, {NULL, NULL}, {"DELAY TYPE", "REVERB TYPE"}, {NULL, NULL}};
static const char* FXB_TLAB[NFXB][2] = {{"BITS", NULL}, {"TYPE", NULL}, {NULL, NULL}, {"DLY", "REV"}, {NULL, NULL}};
static Param* findParamOn(int page, const char* name);
static int ctParams(Param** ps) { int np = 0; if (gChSel >= 0) for (int r = 0; r < 2; r++) { Param* p = FXB_TYPES[gChSel][r] ? findParamOn(10, FXB_TYPES[gChSel][r]) : NULL; if (p) ps[np++] = p; } return np; }
static int fxbOfParam(const char* n) {   // which chain block a MASTER FX setting belongs to, or -1
	if (!strncmp(n, "LOFI", 4)) return 0;
	if (!strncmp(n, "MOD ", 4) || !strcmp(n, "FX COLOR")) return 1;
	if (!strncmp(n, "EQ ", 3)) return 2;
	if (!strncmp(n, "DELAY", 5) || !strncmp(n, "TAPE", 4) || !strncmp(n, "REVERB", 6) || !strcmp(n, "SHIMMER") || !strcmp(n, "FREEZE")) return 3;
	if (!strcmp(n, "OTT")) return 4;
	return -1;
}
static Param* findParamOn(int page, const char* name) { for (Param* p = PAGES[page]; p->name; p++) if (!strcmp(p->name, name)) return p; return NULL; }
static int fxbActive(int b) {
	switch (b) {
	case 0: return gLofiBits || gLofiRate > 0;
	case 1: return gModFx != 0;
	case 2: return gEqLow != 0 || gEqMid != 0 || gEqHigh != 0;
	case 3: return gDlyMix > 0 || gRevMix > 0 || gFreeze;
	default: return gOtt > 0;
	}
}
static void drawChainBox(int x, int b, int lift) {
	int act = fxbActive(b) && gMasterFX && !gDry, y = CH_Y - (lift ? 6 : 0);
	const u8* bg = lift ? TH->keyOn : act ? TH->val : gMasterFX ? TH->tab : TH->bg; const u8* fg = lift || act ? TH->ink : gMasterFX ? TH->tabTxt : TH->grid;
	rect(x, y, 46, CH_H, bg[0], bg[1], bg[2]); outline(x, y, 46, CH_H, RGB3(TH->txt));
	if (b == gChSel && !lift) { outline(x - 2, y - 2, 50, CH_H + 4, RGB3(TH->keyOn)); outline(x - 1, y - 1, 48, CH_H + 2, RGB3(TH->keyOn)); }
	const char* nm = FXB_NAMES[b]; char a[8]; const char* sp = strchr(nm, ' '); int l = sp ? (int)(sp - nm) : (int)strlen(nm); if (l > 5) l = 5; memcpy(a, nm, l); a[l] = 0;   // one word: MOD FX = MOD
	text(x + 23 - l * 4, y + 5, a, 1, fg[0], fg[1], fg[2]);
}
static void drawChainView(void) {
	rect(0, TAB_H, 320, WORK_B - TAB_H, RGB3(TH->bg));
	text(6, TAB_H + 3, "FX CHAIN", 1, RGB3(TH->val));
	rect(258, TAB_H + 1, 56, 12, RGB3(TH->tab)); text(262, TAB_H + 3, "RESET", 1, RGB3(TH->tabTxt));
	if (gDry) { rect(196, TAB_H + 1, 56, 12, RGB3(TH->warn)); text(212, TAB_H + 3, "DRY", 1, RGB3(TH->bg)); } else { rect(196, TAB_H + 1, 56, 12, RGB3(TH->tab)); text(212, TAB_H + 3, "DRY", 1, RGB3(TH->tabTxt)); }
	for (int i = 0; i < NFXB; i++) {
		if (i) { int ax = CH_X(i) - 6; rect(ax, CH_Y + CH_H / 2, 5, 1, RGB3(TH->txt)); px(ax + 3, CH_Y + CH_H / 2 - 1, RGB3(TH->txt)); px(ax + 3, CH_Y + CH_H / 2 + 1, RGB3(TH->txt)); }
		if (i == gChDrag) { outline(CH_X(i), CH_Y, 46, CH_H, RGB3(TH->grid)); continue; }   // the gap it will drop into
		drawChainBox(CH_X(i), gFxOrder[i], 0);
	}
	int fx = CH_X(NFXB) - 6; rect(fx, CH_Y + CH_H / 2, 5, 1, RGB3(TH->txt));
	rect(fx + 6, CH_Y, 50, CH_H, RGB3(TH->grid)); text(fx + 11, CH_Y + 5, "LIMIT", 1, RGB3(TH->txt));
	Param* ps[2]; int np = ctParams(ps), rows = 0;
	for (int k = 0; k < np; k++) {   // the selected block's types, big: tap one to change the effect
		Param* p = ps[k]; int n = (int)p->mx + 1, v = *(int*)p->p;
		int lx, ly, lw; ctRect(k, np, 0, n, &lx, &ly, &lw); text(2, ly + 7, FXB_TLAB[gChSel][np > 1 ? k : (FXB_TLAB[gChSel][0] ? 0 : 1)], 1, RGB3(TH->dim));
		for (int i = 0; i < n; i++) {
			int x0, y0, w; ctRect(k, np, i, n, &x0, &y0, &w); int cc = w / 8; char t[12]; snprintf(t, sizeof(t), "%.*s", cc < 11 ? cc : 11, p->names[i]);
			const u8* bg = i == v ? TH->keyOn : TH->tab; const u8* fg = i == v ? TH->ink : TH->tabTxt;
			rect(x0, y0, w, CT_BH, bg[0], bg[1], bg[2]); text(x0 + w / 2 - (int)strlen(t) * 4, y0 + 7, t, 1, fg[0], fg[1], fg[2]);
			int rr = (y0 - CT_Y) / (CT_BH + 2) + 1; if (rr > rows) rows = rr;
		}
	}
	int hy = CT_Y + rows * (CT_BH + 2) + 2; if (hy > WORK_B - 9) hy = -1;
	if (hy < 0) {}
	else if (gDry) text(6, hy, "DRY: ALL FX OFF. STILL GOOD? THE SOUND IS SOLID", 1, RGB3(TH->warn));
	else if (!gMasterFX) text(6, hy, "MASTER FX IS OFF: THE CHAIN IS BYPASSED", 1, RGB3(TH->warn));
	else if (gChSel < 0) text(6, hy, "TAP A BLOCK TO EDIT IT, DRAG IT TO MOVE IT", 1, RGB3(TH->dim));
	else if (!rows) text(6, hy, "ITS SETTINGS ARE ON THE TOP SCREEN", 1, RGB3(TH->dim));
	if (gChDrag >= 0) { int x = gChDragX - 23; x = x < 0 ? 0 : x > 274 ? 274 : x; drawChainBox(x, gFxOrder[gChDrag], 1); }
}
static int chainTouch(int x, int y, int press, int release) {   // 1 = redraw
	if (release) {
		int r = gChDrag >= 0;
		if (gChDrag < 0 && gChPress >= 0) { gChSel = gFxOrder[gChPress]; gSelReq = FXB_FIRST[gChSel]; r = 1; }   // a tap: edit this block
		gChDrag = -1; gChPress = -1; return r;
	}
	if (press) {
		if (x >= 196 && x < 254 && y >= TAB_H && y < TAB_H + 16) { gDry = !gDry; say(gDry ? "DRY: EFFECTS OFF. IF IT FALLS APART, FIX OSC / OCTAVE / ENVELOPE" : "DRY OFF: EFFECTS BACK"); return 1; }
		if (x >= 258 && y >= TAB_H && y < TAB_H + 16) { u8 d[NFXB]; for (int i = 0; i < NFXB; i++) d[i] = i; fxOrderSet(d); gEdited = 1; say("FX ORDER: DEFAULT"); return 1; }
		if (gChSel >= 0 && y >= CT_Y && y < CT_Y + 2 * (CT_BH + 2) && x >= 34) {   // a type button
			Param* ps[2]; int np = ctParams(ps);
			for (int k = 0; k < np; k++) {
				Param* p = ps[k]; int n = (int)p->mx + 1, hit = -1;
				for (int i = 0; i < n; i++) { int x0, y0, w; ctRect(k, np, i, n, &x0, &y0, &w); if (x >= x0 && x < x0 + w + 2 && y >= y0 && y < y0 + CT_BH + 2) hit = i; }
				if (hit < 0) continue; int i = hit;
				*(int*)p->p = i; gEdited = 1; gSelReq = p->name;
				if (p->p == &gDlyType) dlyTypeChanged();
				char m[40]; snprintf(m, sizeof(m), "%s: %s", p->name, p->names[i]); say(m);
				return 1;
			}
			return 0;
		}
		if (y < CH_Y - 4 || y >= CH_Y + CH_H + 4) return 0;
		for (int i = 0; i < NFXB; i++) if (x >= CH_X(i) && x < CH_X(i) + 46) { gChPress = i; gChPressX = x; return 0; }
		return 0;
	}
	if (gChPress >= 0 && gChDrag < 0 && abs(x - gChPressX) > 8) { gChDrag = gChPress; gChSel = gFxOrder[gChPress]; }   // moved: it's a drag
	if (gChDrag < 0) return 0;
	gChDragX = x;
	int to = (x - 6 + 26) / 52 - (x - 6 + 26 < 0); to = to < 0 ? 0 : to >= NFXB ? NFXB - 1 : to;
	if (to != gChDrag) {   // move it now: you hear the new order while still dragging
		u8 o[NFXB]; for (int i = 0; i < NFXB; i++) o[i] = gFxOrder[i];
		u8 b = o[gChDrag];
		if (to > gChDrag) for (int i = gChDrag; i < to; i++) o[i] = o[i + 1]; else for (int i = gChDrag; i > to; i--) o[i] = o[i - 1];
		o[to] = b; fxOrderSet(o); gChDrag = to; gChPress = to; gEdited = 1;
	}
	return 1;
}
static u16* gCamImg; static int gCamOn;   // (camera, further down)
// ---- bottom views for pages that only had the XY pad: KNOBS (XY PAD mode), VISUALIZER tiles, HARDWARE sensors
static int gVisOnB = 0, gHwOnB = 0, gDrumOnB = 0, gDrumClrArm = 0; static u64 gDrumClrT = 0;   // VIS / HW / DRUMS pages: their own view where the XY pad goes
static void drawDrumGrid(void);
static int gDrumInSeq = 0;   // the SEQ page's DRUMS view is up
static int xyShown(void) { return !gPads && !gModOn && !gGridOn && !gAddEdit && !gShapeOn && !gChainOn && !gVisOnB && !gHwOnB && !gDrumOnB; }
// XY PAD modes: what the two axes move, and the matrix destination that can push them (for the ghost)
static void xyAxes(float** xp, float** yp, int* xd, int* yd) {
	switch (gXyMode) {
	case 1: case 6: *xp = &gMacro[0]; *yp = &gMacro[1]; *xd = *yd = -1; break;   // PERF: the pad plays macros 1 + 2
	case 2: *xp = &gMacro[2]; *yp = &gMacro[3]; *xd = *yd = -1; break;
	case 3: *xp = &gCutoff; *yp = &gRes; *xd = DST_CUTOFF; *yd = DST_RES; break;
	case 4: *xp = &gOsc[0].pos; *yp = &gOsc[1].pos; *xd = DST_APOS; *yd = DST_BPOS; break;
	case 5: *xp = &gFxMix; *yp = &gFxColor; *xd = DST_FXMIX; *yd = DST_FXP; break;
	case 7:   // PAGE: OSC A / B = WT POS + WARP, SUB+NOISE = their levels, VOICE+FX = DRIVE + FX MIX, DRUMS = LEVEL + SWING
		switch (gCurPage) {
		case 1: case 2: { int o = gCurPage - 1; *xp = &gOsc[o].pos; *yp = &gOsc[o].warpAmt; *xd = o ? DST_BPOS : DST_APOS; *yd = o ? DST_BWARP : DST_AWARP; return; }
		case 3: *xp = &gSubLevel; *yp = &gNoiseLevel; *xd = *yd = -1; return;
		case 6: *xp = &gDriveAmt; *yp = &gFxMix; *xd = -1; *yd = DST_FXMIX; return;
		case 11: *xp = &gDrumLevel; *yp = &gSwing; *xd = *yd = -1; return;
		}
		*xp = &gOsc[0].pos; *yp = &gCutoff; *xd = DST_APOS; *yd = DST_CUTOFF; break;
	default: *xp = &gOsc[0].pos; *yp = &gCutoff; *xd = DST_APOS; *yd = DST_CUTOFF; break;
	}
}
static const char* XY_XL[NXY] = {"WT POS >", "MACRO 1 >", "MACRO 3 >", "CUTOFF >", "A WT POS >", "FX MIX >", "MACRO 1 >", ""};
static const char* XY_YL[NXY] = {"^ CUTOFF", "^ MACRO 2", "^ MACRO 4", "^ RES", "^ B WT POS", "^ FX COLOR", "^ MACRO 2", ""};
static void xyRange(const float* v, float* mn, float* mx, const char** nm) {   // the value's range (and name) from its param
	*mn = 0; *mx = 1; if (nm) *nm = "";
	for (int pg = 0; pg < NPAGES; pg++) for (const Param* p = PAGES[pg]; p->name; p++) if (p->p == v && p->type == PT_F) { *mn = p->mn; *mx = p->mx; if (nm) *nm = p->name; return; }
}
static float xyNorm(const float* v) { float mn, mx; xyRange(v, &mn, &mx, NULL); float t = (*v - mn) / (mx - mn > 0 ? mx - mn : 1); return t < 0 ? 0 : t > 1 ? 1 : t; }
static void xySet(float* v, float t) { float mn, mx; xyRange(v, &mn, &mx, NULL); *v = mn + t * (mx - mn); if (v == &gDriveAmt) gForceBuild |= 3; }
static float xyEff(float v, int d) {   // where modulation has the value right now
	if (d < 0) return v;
	float m = modSum(d, newestVoice()); v += d == DST_CUTOFF ? m * 5.0f / 7.97f : m;
	return v < 0 ? 0 : v > 1 ? 1 : v;
}
static int xyModulated(void) { float* a; float* b; int xd, yd; xyAxes(&a, &b, &xd, &yd); return fabsf(xyEff(*a, xd) - *a) > 0.01f || fabsf(xyEff(*b, yd) - *b) > 0.01f; }
static int xyTop(void) { return gXyMode == 6 ? XY_Y + 28 : XY_Y; }
#define XYB_W 64
// PERF sliders: four values that matter most while playing, as bars you can slide
static float* const SL_P[4] = {&gCutoff, &gRes, &gOsc[0].pos, &gFxMix};
static const char* SL_N[4] = {"CUTOFF", "RES", "WT POS", "FX MIX"};
static int gSlDrag = -1;
// PATCH page, bottom screen: the browser. A strip of styles (tap one to see its patches; the strip re-centres on it,
// so the styles either side are always one tap away), the style's patches below (tap one: it loads, edits or not; SELECT + L brings back what you had),
// SAVE on the right (tap twice: saves to a USER slot and asks for a name).
#define BR_Y (XY_Y + 1)
#define BR_SH 16
#define BR_GY (BR_Y + BR_SH + 3)
#define BR_RH 13
#define BR_COLS 3
#define BR_ROWS 6
static int gBrStyle = -1;   // the style shown (-1 = the one playing)
static const char* STYLE_SHORT[NSTYLES] = {"BASS", "LEAD", "PAD", "KEYS", "PLUCK", "CHORD", "SEQ", "TEXTUR", "CHIP", "SPACE", "CHPSET", "MOTION", "QUIRKS", "RAVE", "DRUMS", "GROOVE", "USER"};
static int brStyle(void) { return gBrStyle >= 0 ? gBrStyle : gStyle; }
static int brCount(int st) { return presetCount(st) + (st == NSTYLES - 1); }   // USER: the slots, then INIT PATCH
static int brFirst(void) { int f = stySlot(brStyle()) - 2; if (f < 0) f = 0; if (f > NVSTY - 5) f = NVSTY - 5; return f; }   // a position in VSTY
static void drawBrowser(void) {
	rectP(0, XY_Y, 320, WORK_B - XY_Y, TH->bg);
	int bs = brStyle(), f = brFirst();
	for (int k = 0; k < 5; k++) {   // the style strip
		int st = VSTY[f + k], x = 2 + k * 52, lit = st == bs, here = st == gStyleCur;
		rectP(x, BR_Y, 50, BR_SH, lit ? TH->keyOn : TH->tab);
		const u8* c = lit ? TH->ink : here ? TH->val : TH->tabTxt; text(x + 25 - (int)strlen(STYLE_SHORT[st]) * 4, BR_Y + 4, STYLE_SHORT[st], 1, c[0], c[1], c[2]);
	}
	{ int arm = gSaveArm >= 0 && osGetTime() - gSaveArmT < 3000; rectP(264, BR_Y, 54, BR_SH, arm ? TH->warn : TH->tab); text(291 - (arm ? 5 : 4) * 4, BR_Y + 4, arm ? "AGAIN" : "SAVE", 1, RGB3(TH->tabTxt)); }
	int n = brCount(bs), cw = 320 / BR_COLS;
	for (int i = 0; i < n && i < BR_COLS * BR_ROWS; i++) {
		int x = (i % BR_COLS) * cw + 2, y = BR_GY + (i / BR_COLS) * BR_RH, cur = bs == gStyleCur && i == gPresetCur;
		char nm[16]; const char* src = bs == NSTYLES - 1 && i == NUSER ? "INIT PATCH" : bs == NSTYLES - 1 && gUser[i].magic != PATCH_MAGIC ? "- EMPTY -" : presetName(bs, i);
		snprintf(nm, sizeof(nm), "%.12s", src);
		rectP(x, y, cw - 4, BR_RH - 1, cur ? TH->keyOn : (i / BR_COLS) & 1 ? TH->pad : TH->grid);
		const u8* c = cur ? TH->ink : TH->txt; text(x + 3, y + 2, nm, 1, c[0], c[1], c[2]);
		if (cur && gEdited) text(x + cw - 14, y + 2, "*", 1, c[0], c[1], c[2]);
	}
}
static int browseStep(int ds, int dp, int fresh);
static void browseSave(void);
static int browserTouch(int x, int y) {   // 1 = the sound changed
	if (y >= BR_Y && y < BR_Y + BR_SH) {
		if (x >= 262) { browseSave(); return 0; }
		int k = (x - 2) / 52; if (k < 0) k = 0; if (k > 4) k = 4;
		int st = VSTY[brFirst() + k];
		gBrStyle = st; return 0;
	}
	if (y < BR_GY || y >= BR_GY + BR_ROWS * BR_RH) return 0;
	int i = ((y - BR_GY) / BR_RH) * BR_COLS + x / (320 / BR_COLS), bs = brStyle();
	if (i >= brCount(bs)) return 0;
	gStyle = bs; gPreset = i - 1; gBrStyle = -1;
	return browseStep(0, 1, 1);
}
static void drawXyPad(void) {
	int top = xyTop(), h = XY_Y + XY_H - top;
	rect(0, XY_Y, 320, WORK_B - XY_Y, RGB3(TH->bg));
	if (gXyMode == 6) for (int i = 0; i < 4; i++) {
		int x = 4 + (i & 1) * 158, y = XY_Y + 1 + (i / 2) * 13; float v = *SL_P[i]; v = v < 0 ? 0 : v > 1 ? 1 : v;
		const u8* c = gSlDrag == i ? TH->keyOn : TH->txt; text(x, y + 2, SL_N[i], 1, c[0], c[1], c[2]);
		rect(x + 54, y + 1, 96, 10, RGB3(TH->pad)); rect(x + 54, y + 1, (int)(v * 96), 10, RGB3(TH->val));
		float e = xyEff(v, paramDst(SL_P[i])); if (fabsf(e - v) > 0.01f) rect(x + 54 + (int)(e * 95), y - 1, 2, 14, RGB3(TH->warn));
	}
	float* xp; float* yp; int xd, yd; xyAxes(&xp, &yp, &xd, &yd);
	float xv = xyNorm(xp), yv = xyNorm(yp);
	int cx = 4 + (int)(xv * 311), cy = top + (int)((1 - yv) * (h - 1));
	if (TH->ega) {
		for (int gx = 0; gx <= 8; gx++) for (int y = top; y < top + h; y += 2) px(4 + gx * 39, y, DIM);
		for (int gy = 0; gy <= 4; gy++) for (int x = 4; x < 316; x += 2) px(x, top + gy * (h - 1) / 4, DIM);
	} else {
		rect(4, top, 312, h, RGB3(TH->pad));
		for (int gx = 1; gx < 8; gx++) rect(4 + gx * 39, top, 1, h, RGB3(TH->grid));
		for (int gy = 1; gy < 4; gy++) rect(4, top + gy * h / 4, 312, 1, RGB3(TH->grid));
		frame(4, top, 312, h);
	}
	float ex = xyEff(xv, xd), ey = xyEff(yv, yd);
	if (fabsf(ex - xv) > 0.01f || fabsf(ey - yv) > 0.01f) {   // modulation ghost: a line to where the sound actually is
		int gx = 4 + (int)(ex * 311), gy = top + (int)((1 - ey) * (h - 1));
		for (int k = 0; k <= 16; k++) px(cx + (gx - cx) * k / 16, cy + (gy - cy) * k / 16, RGB3(TH->warn));
		outline(gx - 3, gy - 3, 7, 7, RGB3(TH->warn));
	}
	rect(cx, top, 1, h, LINE); rect(4, cy, 312, 1, LINE);
	rect(cx - 4, cy - 4, 9, 9, TXT); if (TH->ega) rect(cx - 2, cy - 2, 5, 5, BG);
	char xl[24], yl[24];
	if (gXyMode == 7) { float a, b; const char* nx; const char* ny; xyRange(xp, &a, &b, &nx); xyRange(yp, &a, &b, &ny); snprintf(xl, sizeof(xl), "%s >", nx); snprintf(yl, sizeof(yl), "^ %s", ny); }
	else { snprintf(xl, sizeof(xl), "%s", XY_XL[gXyMode]); snprintf(yl, sizeof(yl), "%s", XY_YL[gXyMode]); }
	text(8, top + 4, xl, 1, DIM); text(312 - (int)strlen(yl) * 8, top + h - 12, yl, 1, DIM);
	static const char* M[NXY] = {"WT/CUT", "M1/M2", "M3/M4", "CUT/RES", "A/B", "FX", "PERF", "PAGE"};
	char t[12]; snprintf(t, sizeof(t), "%s>", M[gXyMode]);
	rect(316 - XYB_W, top + 1, XYB_W - 1, 12, RGB3(TH->tab)); text(316 - XYB_W / 2 - (int)strlen(t) * 4, top + 3, t, 1, RGB3(TH->tabTxt));
}
static int xyGlideTick(float dt) {   // 1 = moved (redraw)
	if (gXyTx < 0) return 0;
	static const float T[4] = {0, 0.06f, 0.18f, 0.5f}; float k = 1.0f - expf(-dt / T[gXyGlide ? gXyGlide : 1]);
	float* xp; float* yp; int xd, yd; xyAxes(&xp, &yp, &xd, &yd);
	float cx = xyNorm(xp), cy = xyNorm(yp), nx = cx + (gXyTx - cx) * k, ny = cy + (gXyTy - cy) * k;
	if (fabsf(gXyTx - nx) < 0.002f && fabsf(gXyTy - ny) < 0.002f) { nx = gXyTx; ny = gXyTy; gXyTx = gXyTy = -1; }
	xySet(xp, nx); xySet(yp, ny); gXYmod = ny * 2 - 1;
	return 1;
}
static int xyTouch(int x, int y, int press, int release) {   // 1 = redraw
	int top = xyTop(), h = XY_Y + XY_H - top;
	static int skip = 0;   // a touch that lands on the pad's bottom edge is someone reaching for the row below: it never moves the pad
	if (release) { int r = gSlDrag >= 0; gSlDrag = -1; skip = 0; return r; }
	if (press) skip = y >= XY_Y + XY_H - 8;
	if (skip) return 0;
	if (press && x >= 316 - XYB_W && y >= top && y < top + 14) { gXyMode = (gXyMode + 1) % NXY; gSlDrag = -1; say(XY_NAMES[gXyMode]); return 1; }
	if (press && gXyMode == 6 && y < top) { gSlDrag = (x >= 160) + ((y - XY_Y) / 13 ? 2 : 0); }
	if (gSlDrag >= 0) { float v = (x - (4 + (gSlDrag & 1) * 158 + 54)) / 96.0f; *SL_P[gSlDrag] = v < 0 ? 0 : v > 1 ? 1 : v; gEdited = 1; return 1; }
	if (y < top) return 0;
	float np = (x - 4) / 311.0f, nc = 1.0f - (y - top) / (float)(h - 1);
	np = np < 0 ? 0 : np > 1 ? 1 : np; nc = nc < 0 ? 0 : nc > 1 ? 1 : nc;
	if (gXyGlide) { gXyTx = np; gXyTy = nc; gEdited = 1; return 1; }   // GLIDE: xyGlideTick walks the dot there
	float* xp; float* yp; int xd, yd; xyAxes(&xp, &yp, &xd, &yd); xySet(xp, np); xySet(yp, nc);
	gXYmod = nc * 2 - 1; gEdited = 1;
	return 1;
}
static void drawVisTiles(void) {   // VISUALIZER: one tile per mode, plus OVERLAY
	rect(0, TAB_H, 320, WORK_B - TAB_H, RGB3(TH->bg));
	for (int i = 0; i < 12; i++) {
		int x = 4 + (i % 4) * 79, y = XY_Y + (i / 4) * 32, on = i < NVISM ? i == gVisMode : gVisOverlay;
		const char* nm = i < NVISM ? VISM_NAMES[i] : (gVisOverlay ? "OVERLAY ON" : "OVERLAY OFF");
		rect(x, y, 75, 28, on ? TH->keyOn[0] : TH->tab[0], on ? TH->keyOn[1] : TH->tab[1], on ? TH->keyOn[2] : TH->tab[2]);
		const u8* c = on ? TH->ink : TH->tabTxt; const char* sp = strlen(nm) > 9 ? strchr(nm, ' ') : NULL;
		if (sp) { char a[12]; int l = (int)(sp - nm); memcpy(a, nm, l); a[l] = 0; text(x + 38 - l * 4, y + 5, a, 1, c[0], c[1], c[2]); text(x + 38 - (int)strlen(sp + 1) * 4, y + 15, sp + 1, 1, c[0], c[1], c[2]); }
		else text(x + 38 - (int)strlen(nm) * 4, y + 10, nm, 1, c[0], c[1], c[2]);
	}
}
static int visTilesTouch(int x, int y) {   // press
	int c = (x - 4) / 79, r = (y - XY_Y) / 32, i = r * 4 + c;
	if (y < XY_Y || c < 0 || c > 3 || r < 0 || r > 2) return 0;
	if (i < NVISM) gVisMode = i; else gVisOverlay = !gVisOverlay;
	return 1;
}
static void hwMeter(int x, int y, const char* nm, float v, int bip) {
	text(x, y, nm, 1, RGB3(TH->dim)); rect(x + 56, y + 1, 100, 6, RGB3(TH->pad));
	v = v < -1 ? -1 : v > 1 ? 1 : v;
	if (bip) { int z = x + 106, e = z + (int)(v * 50); rect(z, y, 1, 8, RGB3(TH->txt)); rect(e < z ? e : z, y + 1, abs(e - z) + 1, 6, RGB3(TH->val)); }
	else rect(x + 56, y + 1, (int)((v < 0 ? 0 : v) * 100), 6, RGB3(TH->val));
}
static void drawGrainEd(void); static int grainEdTouch(int x, int y, int press, int release);
static void drawHwView(void) {   // HARDWARE: what the sensors read right now, and the camera
	rect(0, TAB_H, 320, WORK_B - TAB_H, RGB3(TH->bg));
	text(4, XY_Y, "SENSORS", 1, RGB3(TH->val));
	hwMeter(4, XY_Y + 14, "TILT X", gTiltX, 1); hwMeter(4, XY_Y + 26, "TILT Y", gTiltY, 1); hwMeter(4, XY_Y + 38, "TWIST", gTwist, 1);
	hwMeter(4, XY_Y + 50, "SHAKE", gShake, 0); hwMeter(4, XY_Y + 62, "MIC", gMicEnv * 4, 0); hwMeter(4, XY_Y + 74, "LIGHT", gCamLight, 0);
	int cx = 172, cy = XY_Y + 6;
	if (gCamOn && gCamImg) {
		for (int y = 0; y < 60; y++) for (int x = 0; x < 80; x++) { u16 p = gCamImg[(y * 2) * 160 + x * 2]; px(cx + 32 + x, cy + y, (u8)((p >> 11) << 3), (u8)(((p >> 5) & 63) << 2), (u8)((p & 31) << 3)); }
		outline(cx + 31, cy - 1, 82, 62, RGB3(TH->line));
	} else { outline(cx + 31, cy - 1, 82, 62, RGB3(TH->grid)); text(cx + 40, cy + 26, "CAMERA OFF", 1, RGB3(TH->dim)); }
}
static void keyColor(const u8* lo, const u8* hi, float t, u8* out) { t = t < 0 ? 0 : t > 1 ? 1 : t; for (int k = 0; k < 3; k++) out[k] = (u8)(lo[k] + (hi[k] - lo[k]) * t); }
// SEQ STRIP: while the sequencer plays, a thin bar over the keyboard on every page: one segment per step (swing
// shows as uneven widths), notes lit, accents brighter, the playing step brightest
static void drawSeqStrip(void) {
	if (!gSeqPlaying || gSeqLen < 1) return;
	int y = WORK_B - 2; float tot = 0; for (int i = 0; i < gSeqLen; i++) tot += (i & 1) ? 1 - gSwing : 1 + gSwing;
	float x = 4;
	for (int i = 0; i < gSeqLen; i++) {
		float w = ((i & 1) ? 1 - gSwing : 1 + gSwing) / tot * 312; int xi = (int)x, wi = (int)(x + w) - xi - 1;
		const Step* sp = &gSeq[i]; int lk = sp->gate || sp->skip || sp->cut || sp->res || sp->pitch || sp->nudge || sp->rat > 1;
		const u8* c = i == gSeqPos ? TH->keyOn : sp->n ? ((sp->fl & 1) ? TH->val : lk ? TH->warn : TH->txt) : TH->grid;   // accent / locks show in the strip too
		rect(xi, y, wi < 1 ? 1 : wi, 2, c[0], c[1], c[2]);
		x += w;
	}
}
static void drawBottomMain(int base, const u8* on, int helpOn, int latch) {
	(void)helpOn;
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, KB_Y, BG);
	// JUMP chips: one tap to each part of the synth; tap the lit one again for the next view in it (OSC A / B / SUB,
	// the MOD views). PADS lays chord pads over any page.
	const char* TABS[NCHIP] = {"OSC", "FILT", "MOD", "VOICE", "FX", "SEQ", "PADS"};
	if (!gPads && gCurPage >= 1 && gCurPage <= 3) TABS[0] = gCurPage == 1 ? "OSC A" : gCurPage == 2 ? "OSC B" : "SUB";
	for (int i = 0; i < NCHIP; i++) {
		int x = i * 320 / NCHIP, lit = i == 6 ? gPads : !gPads && chipOfPage(gCurPage) == i;
		int w = strlen(TABS[i]) * 8, bw = 320 / NCHIP - 4, cx = x + 160 / NCHIP;
		if (TH->ega) {
			if (lit) { rect(x + 2, 1, bw, TAB_H - 3, HI); text(cx - w / 2, 6, TABS[i], 1, HITXT); }
			else { outline(x + 2, 1, bw, TAB_H - 3, LINE); text(cx - w / 2, 6, TABS[i], 1, TXT); }
		} else {
			if (lit) { rect(x + 2, 1, bw, TAB_H - 2, HI); text(cx - w / 2, 7, TABS[i], 1, HITXT); }
			else { rect(x + 2, 1, bw, TAB_H - 2, RGB3(TH->tab)); text(cx - w / 2, 7, TABS[i], 1, RGB3(TH->tabTxt)); }
		}
	}
	if (gPads && !gGridOn) { drawPads(base); return; }   // chord pads take the whole screen under the tabs
	if (gModOn) { drawModView(); return; }               // MOD page: route / drawn LFOs / motion
	if (gGridOn) { drawGrid(); return; }   // the piano roll takes the whole screen under the tabs
	if (gDrumOnB) { drawDrumGrid(); if (gDgEd) drawDgEd(); return; }   // so does the drum sequencer
	if (gAddEdit) drawAdd(); else if (gCurPage == 0) drawBrowser(); else {
	drawXyPad();
	}
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};
	float ww = KB_WW; const int X0 = KB_X;
	rect(0, KB_Y, 320, 240 - KB_Y, RGB3(TH->kbBg));
	if (TH->ega) rect(0, KB_Y, 320, 1, LINE);
	if (gVoiceMode >= 3) {   // drum kit pads, lower on a pad = harder
		for (int i = 0; i < 10; i++) {
			int d = kitPadOrder(i), x = X0 + (i % 5) * 57, y = KB_Y + 2 + (i / 5) * PAD_ROW, note = kitPadNote(d), lit = (note < 128 && on[note]) || gPadFlash[d] > 0;
			rect(x + 2, y, 54, PAD_ROW - 4, lit ? TH->keyOn[0] : TH->keyW[0], lit ? TH->keyOn[1] : TH->keyW[1], lit ? TH->keyOn[2] : TH->keyW[2]);
			if (TH->ega) outline(x + 2, y, 54, PAD_ROW - 4, RGB3(TH->keyLine));
			const char* nm = kitPadName(d); int tx = x + 29 - (int)strlen(nm) * 4;
			if (lit) text(tx, y + PAD_ROW / 2 - 6, nm, 1, EC(0)); else text(tx, y + PAD_ROW / 2 - 6, nm, 1, TH->ega ? TH->keyLine[0] : 60, TH->ega ? TH->keyLine[1] : 40, TH->ega ? TH->keyLine[2] : 20);
		}
		return;
	}
	if (gScale) {   // scale mode: 14 pads, one per scale degree
		int n = scaleLen();
		for (int i = 0; i < 14; i++) {
			int note = scaleNote(base + gRoot, i), x = X0 + (int)(i * ww), root = i % n == 0;
			int lit = note < 128 && on[note];
			if (lit) rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyOn));
			else if (root) rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->dim));
			else { rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyW)); if (TH->ega) outline(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyLine)); }
			if (lit || root) text(x + (int)ww / 2 - (int)strlen(NOTE_NAMES[note % 12]) * 4, 226, NOTE_NAMES[note % 12], 1, EC(0));
			else text(x + (int)ww / 2 - (int)strlen(NOTE_NAMES[note % 12]) * 4, 226, NOTE_NAMES[note % 12], 1, TH->ega ? TH->keyLine[0] : 60, TH->ega ? TH->keyLine[1] : 40, TH->ega ? TH->keyLine[2] : 20);
		}
		char c[24]; snprintf(c, sizeof(c), "%s %s", NOTE_NAMES[gRoot], SCALE_NAMES[gScale]); text(X0 + 4, KB_Y + 5, c, 1, VAL);
		return;
	}
	for (int i = 0; i < 14; i++) {
		int n = base + (i / 7) * 12 + WN[i % 7], x = X0 + (int)(i * ww); u8 c[3];
		if (on[n]) { keyColor(TH->keyW, TH->keyOn, 0.45f + 0.55f * gNoteVel[n], c); rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, c[0], c[1], c[2]); }   // harder = brighter
		else if (gKeyFade[n] > 0) { keyColor(TH->keyW, TH->keyOn, gKeyFade[n] * 0.6f, c); rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, c[0], c[1], c[2]); if (TH->ega) outline(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyLine)); }
		else { rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyW)); if (TH->ega) outline(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyLine)); }
	}
	for (int i = 0; i < 14; i++) {
		if (!BL[i % 7]) continue;
		int n = base + (i / 7) * 12 + WN[i % 7] + 1, bx = X0 + (int)((i + 1) * ww - ww * 0.3f);
		u8 c[3];
		if (on[n]) { keyColor(TH->keyB, TH->keyOnB, 0.45f + 0.55f * gNoteVel[n], c); rect(bx, KB_Y + 2, (int)(ww * 0.6f), BLK_H, c[0], c[1], c[2]); }
		else if (gKeyFade[n] > 0) { keyColor(TH->keyB, TH->keyOnB, gKeyFade[n] * 0.6f, c); rect(bx, KB_Y + 2, (int)(ww * 0.6f), BLK_H, c[0], c[1], c[2]); }
		else { rect(bx, KB_Y + 2, (int)(ww * 0.6f), BLK_H, RGB3(TH->keyB)); if (TH->ega) outline(bx, KB_Y + 2, (int)(ww * 0.6f), BLK_H, RGB3(TH->keyLine)); }
	}
	char c[8]; snprintf(c, sizeof(c), "C%d", base / 12 - 1);
	if (on[base] || !TH->ega) text(X0 + 3, 228, c, 1, TH->ega ? 0 : 120, TH->ega ? 0 : 90, TH->ega ? 0 : 50); else text(X0 + 3, 228, c, 1, RGB3(TH->keyLine));
}
// PERF row, between the work area and the keyboard: the things you reach for while playing
static int gScaleLast = 2, gChordLast = 1; static float gOctFlash[2];   // OCT- / OCT+ light for a moment when hit
#define NPERF 3
static void drawPerfRow(int latch) {   // the row above the keys: SCALE, PLAY, PANIC. Its top 6 px are a gap: reaching for it doesn't drag the XY pad
	static const char* PB[NPERF] = {"SCALE", "PLAY", "PANIC"};
	rect(0, WORK_B, 320, KB_Y - WORK_B, BG);
	for (int i = 0; i < NPERF; i++) {
		int x = i * 320 / NPERF, bw = 320 / NPERF - 4;
		int lit = (i == 0 && gScale) || (i == 1 && gSeqMode == 1);
		const char* lb = i == 1 && gSeqMode == 1 ? "STOP" : PB[i]; int w = (int)strlen(lb) * 8, tx = x + 160 / NPERF - w / 2;
		if (lit) { rect(x + 2, WORK_B + 1, bw, KB_Y - WORK_B - 3, HI); text(tx, WORK_B + 5, lb, 1, HITXT); }
		else if (TH->ega) { outline(x + 2, WORK_B + 1, bw, KB_Y - WORK_B - 3, LINE); text(tx, WORK_B + 5, lb, 1, TXT); }
		else { rect(x + 2, WORK_B + 1, bw, KB_Y - WORK_B - 3, RGB3(TH->tab)); text(tx, WORK_B + 5, lb, 1, RGB3(TH->tabTxt)); }
	}
	// the column left of the keys: OCT+ over OCT-, then CHORD and LATCH
	static const char* SB[4] = {"OCT+", "OCT-", "CHRD", "LTCH"};
	rect(0, KB_Y, KB_X, 240 - KB_Y, RGB3(TH->kbBg));
	for (int i = 0; i < 4; i++) {
		int y = KB_Y + i * SIDE_H, lit = (i == 0 && gOctFlash[1] > 0) || (i == 1 && gOctFlash[0] > 0) || (i == 2 && gChord) || (i == 3 && latch);
		if (lit) { rect(1, y + 1, KB_X - 3, SIDE_H - 2, HI); text(KB_X / 2 - 16, y + SIDE_H / 2 - 4, SB[i], 1, HITXT); }
		else if (TH->ega) { outline(1, y + 1, KB_X - 3, SIDE_H - 2, LINE); text(KB_X / 2 - 16, y + SIDE_H / 2 - 4, SB[i], 1, TXT); }
		else { rect(1, y + 1, KB_X - 3, SIDE_H - 2, RGB3(TH->tab)); text(KB_X / 2 - 16, y + SIDE_H / 2 - 4, SB[i], 1, RGB3(TH->tabTxt)); }
	}
}
// THEME picker: every theme at once on the bottom screen, each drawn in its own colours; tap one (B or CLOSE leaves)
// the circle pad as a D-pad in lists (START menu, theme picker, the patch browser): push = one step, hold = repeat
static int gStickMenu = 0;
static u32 stickDpad(void) {
	static u32 last = 0; static int held = 0;
	circlePosition c; hidCircleRead(&c); u32 d = 0;
	int ax = abs(c.dx), ay = abs(c.dy);
	if (ax > 90 || ay > 90) d = ay >= ax ? (c.dy > 0 ? KEY_DUP : KEY_DDOWN) : (c.dx > 0 ? KEY_DRIGHT : KEY_DLEFT);
	else if (ax < 50 && ay < 50) d = 0; else d = last;   // in between: keep what it was (no flicker at the edge)
	u32 out = 0;
	if (d != last) { held = 0; out = d; }
	else if (d && ++held >= 18 && (held - 18) % 6 == 0) out = d;
	last = d; return out;
}
static int gThemePick = 0, gBrightSaveDue = 0;
#define TP_COLS 3
#define TP_CH 30
static void drawThemePick(void) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rectP(0, 0, 320, 240, TH->bg);
	text(6, 6, "PICK A THEME", 1, RGB3(TH->val));
	rectP(262, 2, 54, 16, TH->keyOn); text(267, 6, "CLOSE", 1, RGB3(TH->ink));
	for (int i = 0; i < NTHEMES; i++) {
		if (!gThBuilt) themeBuild(); const Theme* t = &gThFix[i]; int x = 4 + (i % TP_COLS) * 105, y = 22 + (i / TP_COLS) * (TP_CH + 4), w = 101;
		rectP(x, y, w, TP_CH, t->bg); outlineP(x, y, w, TP_CH, t->line);
		if (i == gTheme) { outlineP(x - 1, y - 1, w + 2, TP_CH + 2, t->val); outlineP(x + 1, y + 1, w - 2, TP_CH - 2, t->val); }
		text(x + 5, y + 5, t->name, 1, RGB3(t->txt));
		rectP(x + 5, y + 18, 14, 6, t->val); rectP(x + 21, y + 18, 14, 6, t->keyOn); rectP(x + 37, y + 18, 14, 6, t->hi); rectP(x + 53, y + 18, 14, 6, t->warn);
	}
}
static int themePickTouch(int x, int y) {   // 1 = picked or closed
	if (y < 20) { if (x >= 262) { gThemePick = 0; return 1; } return 0; }
	int c = (x - 4) / 105, r = (y - 22) / (TP_CH + 4); if (c < 0 || c >= TP_COLS || r < 0) return 0;
	int i = r * TP_COLS + c; if (i >= NTHEMES) return 0;
	gTheme = i; gThemePick = 0; gBrightSaveDue = 1; char m[32]; snprintf(m, sizeof(m), "THEME: %s", THEME_NAMES[i]); say(m); return 1;
}
static void drawBottom(int base, const u8* on, int helpOn, int latch) {
	if (gThemePick) { drawThemePick(); return; }
	drawBottomMain(base, on, helpOn, latch);
	if ((gPads && !gGridOn) || gModOn || gGridOn || gDrumOnB) return;
	if (gShapeOn && !gGridOn && !gAddEdit) drawShapeEditor();
	if (gChainOn && !gGridOn && !gAddEdit) drawChainView();
	if (gVisOnB && !gGridOn && !gAddEdit) drawVisTiles();
	if (gHwOnB && !gGridOn && !gAddEdit) { if (gGrains) drawGrainEd(); else drawHwView(); }
	if (!gGridOn && !gAddEdit) drawSeqStrip();
	drawPerfRow(latch);
	if (latch) { outline(KB_X, KB_Y, 320 - KB_X, 240 - KB_Y, HI); outline(KB_X + 1, KB_Y + 1, 318 - KB_X, 238 - KB_Y, HI); }   // LATCH: notes hold, and the keyboard says so
	if (gChordKey >= 0 && gVoiceMode < 3 && !((gVoiceMode == 1 || gVoiceMode == 2) && !gArp) && !gGridOn && !gAddEdit && !gShapeOn) {   // chord mode: the chord you're holding, by name
		char t[24]; snprintf(t, sizeof(t), "%s %s", NOTE_NAMES[gChordKey % 12], CHORD_NAMES[gChord]);
		int w = (int)strlen(t) * 8 + 8; rect(6, WORK_B - 20, w, 12, RGB3(TH->tab)); text(10, WORK_B - 18, t, 1, RGB3(TH->tabTxt));
	}
}

// ================================================================ hardware extras (HARDWARE page)
// Everything here is opt-in from the HARDWARE page except the free ones (New 3DS speed, gyro read, headphone check).
static void say(const char* m) { snprintf(gMsg, sizeof(gMsg), "%s", m); gMsgT = 3.0f; }

// ---- New 3DS speed, headphones, backlight beat light, gyro twist + shake
static int gBeatLight = 0, gLcdOK = 0, gLcdFlash = 0;
static u32 gBrightTop = 3, gBrightBot = 3;
static float gLightT = 0;
static void extrasBasicInit(void) {
	osSetSpeedupEnable(true);   // 804 MHz on New 3DS / New 2DS; no effect on an old 2DS
	if (R_SUCCEEDED(gspLcdInit())) {
		gLcdOK = 1;
		GSPLCD_GetBrightness(GSPLCD_SCREEN_TOP, &gBrightTop); GSPLCD_GetBrightness(GSPLCD_SCREEN_BOTTOM, &gBrightBot);
		if (gBrightTop < 1 || gBrightTop > 5) gBrightTop = 3;
		if (gBrightBot < 1 || gBrightBot > 5) gBrightBot = 3;
	}
	HIDUSER_EnableGyroscope();
}
static u32 brTop(void) { return gBright ? (u32)gBright : gBrightTop; }   // gBrightTop / Bot: the system's own, put back on exit
static u32 brBot(void) { return gBright ? (u32)gBright : gBrightBot; }
static void beatLightTick(float dt) {   // brightness jump on each arp / sequencer step, then back; BRIGHTNESS setting
	if (!gLcdOK) return;
	static int applied = 0;
	if (gBright != applied && !gLcdFlash) { applied = gBright; GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, brTop()); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, brBot()); gBrightSaveDue = 1; }
	if (gLcdFlash && (gLightT -= dt) <= 0) { GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, brTop()); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, brBot()); gLcdFlash = 0; }
}
static void beatLightPulse(void) {
	if (!gLcdOK || !gBeatLight) return;
	u32 bt = brTop(), bb = brBot(), t = bt <= 3 ? bt + 2 : bt - 2, b = bb <= 3 ? bb + 2 : bb - 2;
	GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, t); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, b);
	gLcdFlash = 1; gLightT = 0.07f;
}
static void motionTick(float dt) {
	angularRate g; hidGyroRead(&g);
	float tw = g.z / 1800.0f; if (fabsf(tw) < 0.04f) tw = 0;
	if (tw > 1) tw = 1;
	if (tw < -1) tw = -1;
	float k = 1.0f - expf(-dt / 0.06f); gTwist += (tw - gTwist) * k;
	float sx = g.x / 1800.0f, sy = g.y / 1800.0f;   // the other two gyro axes: how fast you tip it forward/back and roll it
	if (fabsf(sx) < 0.04f) sx = 0;
	if (fabsf(sy) < 0.04f) sy = 0;
	sx = sx > 1 ? 1 : sx < -1 ? -1 : sx; sy = sy > 1 ? 1 : sy < -1 ? -1 : sy;
	gSpinX += (sx - gSpinX) * k; gSpinY += (sy - gSpinY) * k;
	static accelVector pa; static int have = 0;
	accelVector a; hidAccelRead(&a);
	if (have) {
		float d = (fabsf((float)a.x - pa.x) + fabsf((float)a.y - pa.y) + fabsf((float)a.z - pa.z)) / 600.0f;
		if (d > 1) d = 1;
		gShake += (d - gShake) * (d > gShake ? 0.7f : 0.05f);
		gShakeD = d;
	}
	pa = a; have = 1;
	static float hpT = 0;
	if ((hpT -= dt) <= 0) {   // headphone check twice a second; OUTPUT AUTO follows it
		hpT = 0.5f; bool hp = false;
		if (R_SUCCEEDED(DSP_GetHeadphoneStatus(&hp))) { int h = hp ? 1 : 0; if (h != gHeadphones) { gHeadphones = h; if (gOutMode == 2) outputApply(); } }
	}
	beatLightTick(dt);
}

// ---- turning audio or pictures into a wavetable (saved as a Serum-style .wav in /3ds/syrup)
#define WT_FR 64
static void wtWriteWav(const char* path, const float* fr, int nf) {
	FILE* f = fopen(path, "wb"); if (!f) return;
	static const char clm[] = "<!>2048 00000000 wavetable (Syrup)";
	u32 clen = sizeof(clm) - 1 + ((sizeof(clm) - 1) & 1), dlen = nf * CYC * 2;
	u32 riff = 4 + (8 + 16) + (8 + clen) + (8 + dlen);
	u8 h[12] = {'R', 'I', 'F', 'F', riff, riff >> 8, riff >> 16, riff >> 24, 'W', 'A', 'V', 'E'}; fwrite(h, 1, 12, f);
	u8 fm[24] = {'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0x44, 0xAC, 0, 0, 0x88, 0x58, 1, 0, 2, 0, 16, 0}; fwrite(fm, 1, 24, f);
	u8 ch[8] = {'c', 'l', 'm', ' ', clen, clen >> 8, 0, 0}; fwrite(ch, 1, 8, f); fwrite(clm, 1, sizeof(clm) - 1, f); if ((sizeof(clm) - 1) & 1) fputc(0, f);
	u8 dh[8] = {'d', 'a', 't', 'a', dlen, dlen >> 8, dlen >> 16, dlen >> 24}; fwrite(dh, 1, 8, f);
	static s16 row[CYC];
	for (int i = 0; i < nf; i++) { for (int n = 0; n < CYC; n++) { float v = fr[i * CYC + n] * 32000; row[n] = v > 32767 ? 32767 : (v < -32768 ? -32768 : (s16)v); } fwrite(row, 2, CYC, f); }
	fclose(f);
}
static void wtFinish(float* fr, int nf) {   // per-frame DC removal, ends blended so each frame loops, one gain for the table
	float pk = 1e-6f;
	for (int i = 0; i < nf; i++) {
		float* c = fr + i * CYC, m = 0;
		for (int n = 0; n < CYC; n++) m += c[n];
		m /= CYC;
		for (int n = 0; n < CYC; n++) c[n] -= m;
		int B = CYC / 16;
		for (int n = 0; n < B; n++) { float u = n / (float)B, a = c[n], b = c[CYC - B + n]; c[CYC - B + n] = b * (1 - u) + a * u; }
		for (int n = 0; n < CYC; n++) if (fabsf(c[n]) > pk) pk = fabsf(c[n]);
	}
	for (int i = 0; i < nf * CYC; i++) fr[i] /= pk;
}
static void wtResample(const float* src, int n, float* dst) {   // n samples -> one CYC frame
	for (int i = 0; i < CYC; i++) { float x = i * (float)n / CYC; int k = (int)x; float u = x - k; dst[i] = src[k % n] * (1 - u) + src[(k + 1) % n] * u; }
}
// audio -> 64 frames: if a steady pitch is found, each frame is one period (clean single cycles);
// otherwise even slices of the recording (granular / spectral textures)
static void audioToFrames(const float* in, int n, int rate, float* fr) {
	int best = 0; float bestR = 0;
	int c0 = n / 2 - 2048; if (c0 < 0) c0 = 0;
	int W = n - c0 > 4096 ? 2048 : (n - c0) / 2, lo = rate / 1000, hi = rate / 60;
	if (hi > W) hi = W;
	float e0 = 1e-9f; for (int i = 0; i < W; i++) e0 += in[c0 + i] * in[c0 + i];
	for (int L = lo; L < hi; L++) {
		float s = 0, e1 = 1e-9f;
		for (int i = 0; i < W; i++) { s += in[c0 + i] * in[c0 + i + L]; e1 += in[c0 + i + L] * in[c0 + i + L]; }
		float r = s / sqrtf(e0 * e1);
		if (r > bestR + 0.02f) { bestR = r; best = L; }
	}
	static float tmp[8192];
	for (int f = 0; f < WT_FR; f++) {
		if (best && bestR > 0.6f) {
			int st = (int)((n - best - 2) * (float)f / (WT_FR - 1));
			for (int k = 0; k < best && st + k + 1 < n; k++) if (in[st + k] <= 0 && in[st + k + 1] > 0) { st += k; break; }   // rising zero crossing
			for (int k = 0; k < best; k++) tmp[k] = in[(st + k) % n];
			wtResample(tmp, best, fr + f * CYC);
		} else {
			int span = n / 8 < 8192 ? n / 8 : 8192, st = (int)((n - span) * (float)f / (WT_FR - 1));
			wtResample(in + st, span, fr + f * CYC);
		}
	}
	wtFinish(fr, WT_FR);
}
// save, rescan, and load the new table into OSC A
static void wtInstall(const char* base, float* fr) {
	char path[96], nm[24];
	for (int k = 1; k < 100; k++) {
		snprintf(nm, sizeof(nm), "%s %d.wav", base, k); snprintf(path, sizeof(path), "sdmc:/3ds/syrup/%s", nm);
		FILE* t = fopen(path, "rb"); if (!t) break; fclose(t);
	}
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777);
	wtWriteWav(path, fr, WT_FR);
	LightLock_Lock(&gMipsLock);
	scanTables(); gTableCount = T_BUILTIN + gNFiles;
	for (int i = 0; i < WCACHE; i++) { gWCache[i].idx = -1; }
	int idx = -1; for (int i = 0; i < gNFiles; i++) if (!strcmp(gFiles[i], nm)) idx = T_BUILTIN + i;
	if (idx >= 0) { gOsc[0].table = idx; tableSelect(&gOsc[0].tb, idx); }
	LightLock_Unlock(&gMipsLock);
	gForceBuild |= 1;
	char m[48]; snprintf(m, sizeof(m), "SAVED %.20s -> OSC A", nm); say(m);
}

// mic: record 1 s, then turn it into a table
static float gMicRecT = 0;
static void micRecStart(void) { if (!gMicOK) { say("MIC NOT AVAILABLE"); return; } gMicRecT = 1.1f; }
// WEB UPLOAD: the Wi-Fi server (the one WI-FI MIDI runs) also takes files at /files
static void kitScanDirs(void); static void netMidiChanged(void); static int gMidiNet; static char gNetAddr[32]; static volatile int gWebUpDone, gWebUpN; static char gWebUpKit[40];
static int gWebUpSay = 0;
static void webUpStart(void) {
#ifdef SYRUP_NO_NET
	say("ON THE WEB VERSION: USE THE SAMPLES BUTTON");
#else
	if (!gMidiNet) { gMidiNet = 1; netMidiChanged(); }
	gWebUpSay = 1; say("WEB UPLOAD: STARTING WI-FI...");
#endif
}
static void webUpTick(void) {
	if (gWebUpSay && strchr(gNetAddr, ':')) { gWebUpSay = 0; static char m[64]; snprintf(m, sizeof m, "OPEN %s/files IN A BROWSER", gNetAddr); say(m); gMsgT = 15; }
	else if (gWebUpSay && gNetAddr[0] && !strchr(gNetAddr, ':')) { gWebUpSay = 0; say(gNetAddr); }
	if (!gWebUpDone) return;
	gWebUpDone = 0; int n = gWebUpN; gWebUpN = 0;
	LightLock_Lock(&gMipsLock); scanTables(); gTableCount = T_BUILTIN + gNFiles; LightLock_Unlock(&gMipsLock);
	if (gWebUpKit[0]) { kitScanDirs(); for (int i = 0; i < gNKitDirs; i++) if (!strcasecmp(gKitRaw[i], gWebUpKit)) gKitDir = i; gKit = KIT_SAMPLE; gDrumWant = KIT_SAMPLE + 100; gWebUpKit[0] = 0; }
	char m[48]; snprintf(m, sizeof m, "%d FILE%s ARRIVED OVER WI-FI", n, n == 1 ? "" : "S"); say(m);
}
// MIC > PAD: a second of the mic, from where the sound starts, becomes the picked drum's sample (kits/MIC)
static float gMicPadT = 0;
static void micPadStart(void) { if (!gMicOK) { say("MIC NOT AVAILABLE"); return; } gMicPadT = 1.1f; }
static void wavWrite16(const char* path, const float* x, int n) {
	FILE* f = fopen(path, "wb"); if (!f) return; u32 rate = 32728, dl = n * 2, rl = 36 + dl, by = rate * 2; u16 one = 1, bits = 16, ba = 2; u32 fl = 16;
	fwrite("RIFF", 4, 1, f); fwrite(&rl, 4, 1, f); fwrite("WAVEfmt ", 8, 1, f); fwrite(&fl, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&by, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 4, 1, f); fwrite(&dl, 4, 1, f); for (int i = 0; i < n; i++) { float v = x[i] > 1 ? 1 : x[i] < -1 ? -1 : x[i]; s16 w = (s16)(v * 32000); fwrite(&w, 2, 1, f); } fclose(f);
}
static void kitScanDirs(void); static int gDgSel;
static void micPadTick(float dt) {
	if (gMicPadT <= 0) return;
	gMicPadT -= dt;
	static const char* PADN[NDRUM] = {"KICK", "SNARE", "HAT", "OPEN HAT", "CLAP", "RIM", "LO TOM", "HI TOM", "BELL", "CRASH"};
	int d = gDgSel >= 0 && gDgSel < NDRUM ? gDgSel : 0;
	char m[48]; snprintf(m, sizeof(m), "HIT THE MIC FOR %s... %.1f", PADN[d], gMicPadT > 0 ? gMicPadT : 0); say(m);
	if (gMicPadT > 0) return;
	u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2; const s16* mb = (const s16*)gMicBuf;
	float* in = (float*)malloc(sizeof(float) * N); float* out = (float*)malloc(sizeof(float) * 32728);
	if (in && out) {
		for (u32 i = 0; i < N; i++) in[i] = mb[(end + i) % N] / 32768.0f;   // oldest to newest
		float pk = 0; for (u32 i = 0; i < N; i++) if (fabsf(in[i]) > pk) pk = fabsf(in[i]);
		u32 st = 0; for (u32 i = 0; i < N; i++) if (fabsf(in[i]) > pk * 0.25f) { st = i > 40 ? i - 40 : 0; break; }   // from just before the hit
		int n = 0; for (; n < 32728; n++) { float x = st + n * 0.5f; u32 a = (u32)x; if (a + 1 >= N) break; float u = x - a; out[n] = (in[a] * (1 - u) + in[a + 1] * u) / (pk > 0.01f ? pk : 0.01f) * 0.9f; }   // 16364 -> 32728 Hz, full level
		for (int i = 0; i < 400 && i < n; i++) out[n - 1 - i] *= i / 400.0f;
		mkdir("sdmc:/3ds/syrup/kits", 0777); mkdir("sdmc:/3ds/syrup/kits/MIC", 0777);
		char pth[96]; snprintf(pth, sizeof(pth), "sdmc:/3ds/syrup/kits/MIC/%s.wav", PADN[d]); wavWrite16(pth, out, n);
		kitScanDirs(); for (int i = 0; i < gNKitDirs; i++) if (!strcmp(gKitDirs[i], "MIC")) gKitDir = i;
		gKit = KIT_SAMPLE; gDrumWant = KIT_SAMPLE + 100; snprintf(m, sizeof(m), "%s SAMPLED (KIT: SAMPLE, MIC)", PADN[d]); say(m);
	}
	free(in); free(out);
}
static void micRecTick(float dt) {
	if (gMicRecT <= 0) return;
	gMicRecT -= dt;
	char m[32]; snprintf(m, sizeof(m), "SING / PLAY INTO THE MIC... %.1f", gMicRecT > 0 ? gMicRecT : 0); say(m);
	if (gMicRecT > 0) return;
	u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2;
	float* in = (float*)malloc(sizeof(float) * N); float* fr = (float*)malloc(sizeof(float) * CYC * WT_FR);
	if (in && fr) {
		const s16* m2 = (const s16*)gMicBuf;
		for (u32 i = 0; i < N; i++) in[i] = m2[(end + i) % N] / 32768.0f;   // oldest to newest
		audioToFrames(in, N, 16364, fr); wtInstall("MIC", fr);
	}
	free(in); free(fr);
}

// ================================================================ more chip-level sound (no CPU in the audio path)

// ---- CHIP layer: the second sound chip's own square-wave generators (PSG channels 8-13), one per note
static float gChipLevel = 0, gOrbit = 0, gOrbitRate = 0.2f; static int gChipDuty = 3, gChipOct = 0;
static const char* DUTY_NAMES[8] = {"12.5%", "25%", "37.5%", "50%", "62.5%", "75%", "87.5%", "SILENT"};
static int gChipOwner[6] = {-1, -1, -1, -1, -1, -1};
static int chipReserved(int c) { return gChipLevel > 0.001f && c >= 8 && c <= 13; }
static int chipRate(float f) {   // a PSG square takes 8 timer ticks per cycle; the timer can't run below ~1 kHz, so fold low notes up
	float r = f * 8 * powf(2.0f, (float)gChipOct);
	while (r < 1100 && r > 0) r *= 2;
	if (r > 4000000) r = 4000000;   // CHIP OCTAVE +3..+5 run the squares past the output rate: they alias back as whines
	return (int)r;
}
static void chipStop(SVoice* v) {
	if (v->chipCh < 8 || v->chipCh > 13) { v->chipCh = -1; return; }   // (a voice cleared to 0 has no chip channel either)
	if (gCsndOK) CSND_SetPlayState(v->chipCh, 0);
	gChipOwner[v->chipCh - 8] = -1; v->chipCh = -1;
}
static void chipStart(SVoice* v, int vi) {
	if (!gCsndOK || gChipLevel <= 0.001f || v->isEcho) return;
	int slot = -1; unsigned oldest = 0xFFFFFFFF;
	for (int i = 0; i < 6; i++) {
		if (!(csndChannels & BIT(8 + i))) continue;
		if (gChipOwner[i] < 0) { slot = i; break; }
		if (gV[gChipOwner[i]].age < oldest) { oldest = gV[gChipOwner[i]].age; slot = i; }
	}
	if (slot < 0) return;
	if (gChipOwner[slot] >= 0) chipStop(&gV[gChipOwner[slot]]);
	int c = 8 + slot; u32 timer = CSND_TIMER(chipRate(v->cur)); if (timer < 0x42) timer = 0x42; if (timer > 0xFFFF) timer = 0xFFFF;
	CSND_SetChnRegsPSG(SOUND_FORMAT_PSG | SOUND_ENABLE | SOUND_CHANNEL(c) | (timer << 16), 0, 0, (CSND_DutyCycle)gChipDuty);
	gChipOwner[slot] = vi; v->chipCh = c;
}

static void chipSet(float lv) {   // main thread: turning the layer off frees channels 8-13 for everything else
	if (lv <= 0.001f && gChipLevel > 0.001f) { for (int i = 0; i < NVS; i++) chipStop(&gV[i]); if (gCsndOK) csndExecCmds(false); }   // (main loop holds the voice lock)
	gChipLevel = lv;
}

// ---- echo for the second sound chip: its two capture units record its own mix into a ring as long as the
// delay, two channels replay those rings (crossed over for ping-pong), and their capture send is the feedback
static float gCsEcho = 0, gCsEchoFb = 0.45f, gEchoWow = 0; static int gCsEchoT = 2;
static const char* CSECHO_T_NAMES[4] = {"1/8", "3/16", "1/4", "3/8"};
static const float CSECHO_BEATS[4] = {0.5f, 0.75f, 1.0f, 1.5f};
static int gEchoUnits[2] = {-1, -1}, gEchoCh[2] = {-1, -1}, gEchoOn = 0, gEchoLen = 0, gEchoBpm = 0;
static s16* gEchoBuf[2];
static int echoReserved(int c) { return gEchoOn && (c == gEchoCh[0] || c == gEchoCh[1]); }
static void csWait(void) { csndExecCmds(false); svcSleepThread(6000000); }   // never spin on the chip's done flag: a wedged chip would hang Syrup forever
static u32 csSend(float l, float r) {   // capture volumes: the chip echo, or CHIP > FX
	if (gThruOn) return (u32)(l * 0.5f * 0x7FFF) | ((u32)(r * 0.5f * 0x7FFF) << 16);   // half level: the capture sums every part
	if (!gEchoOn) return 0;
	return (u32)(l * gCsEcho * 0x7FFF) | ((u32)(r * gCsEcho * 0x7FFF) << 16);
}
static void echoStop(void) {
	if (!gEchoOn || !gCsndOK) { gEchoOn = 0; return; }
	for (int k = 0; k < 2; k++) { CSND_CapEnable(gEchoUnits[k], false); CSND_SetPlayState(gEchoCh[k], 0); }
	csWait();
	for (int k = 0; k < 2; k++) { CSND_ReleaseCapUnit(gEchoUnits[k]); gEchoUnits[k] = -1; }
	gEchoOn = 0;
}
static void echoStart(void) {
	if (!gCsndOK) return;
	echoStop();
	int len = (int)(CSECHO_BEATS[gCsEchoT] * 60.0f / gBPM * 32728); if (len > 32728) len = 32728;
	len &= ~31;
	for (int k = 0; k < 2; k++) {
		if (!gEchoBuf[k]) gEchoBuf[k] = (s16*)linearAlloc(32728 * 2);
		if (!gEchoBuf[k]) return;
		memset(gEchoBuf[k], 0, len * 2); CSND_FlushDataCache(gEchoBuf[k], len * 2);
		u32 u; if (R_FAILED(CSND_AcquireCapUnit(&u))) { if (k) CSND_ReleaseCapUnit(gEchoUnits[0]); say("NO CAPTURE UNIT FREE"); return; }
		gEchoUnits[k] = u;
	}
	int n = 0;   // two channels nobody else uses
	int top = 0;   // the looper's two (top two channels, whether or not they're picked yet) are never touched
	for (int c = NCS - 1; c > 18 && n < 2; c--) if (csndChannels & BIT(c)) {
		if (top < 2) { top++; continue; }
		if (tapeReserved(c) || drumReserved(c) || chipReserved(c)) continue;
		if (gCsOwner[c] >= 0) stopCh(BK_CSND, c);
		gEchoCh[n++] = c;
	}
	if (n < 2) { for (int k = 0; k < 2; k++) CSND_ReleaseCapUnit(gEchoUnits[k]); return; }
	u32 timer = CSND_TIMER(32728);
	for (int k = 0; k < 2; k++) {
		CSND_SetCapRegs(gEchoUnits[k], CAPTURE_REPEAT | CAPTURE_FORMAT_16BIT | CAPTURE_ENABLE, osConvertVirtToPhys(gEchoBuf[k]), len * 2);
		CSND_CapSetTimer(gEchoUnits[k], timer);
	}
	gEchoLen = len; gEchoOn = 1; gEchoBpm = gBPM;
	for (int k = 0; k < 2; k++) {   // replay: left ring -> left out and feeds the right capture (ping-pong)
		int c = gEchoCh[k]; u32 out = k ? ((u32)(0x5000) << 16) : 0x5000, fb = (u32)(gCsEchoFb * 0x7FFF);
		u32 cap = k ? fb : (fb << 16);
		u32 pt = gCsEchoPitch ? CSND_TIMER(32728.0f * powf(2.0f, gCsEchoPitch / 12.0f)) : timer;   // ECHO PITCH: replayed faster / slower than it records
		CSND_SetChnRegs(SOUND_FORMAT_16BIT | SOUND_REPEAT | SOUND_ENABLE | SOUND_CHANNEL(c) | (pt << 16), osConvertVirtToPhys(gEchoBuf[k]), osConvertVirtToPhys(gEchoBuf[k]), len * 2, out, cap);
	}
	for (int k = 0; k < 2; k++) CSND_CapEnable(gEchoUnits[k], true);
	csWait();
}
static void echoTickL(void) {
	int want = gCsEcho > 0.001f && gCsndOK && !gThruOn;   // CHIP > FX needs both capture units
	static int pitchWas = 0;
	if (want && (!gEchoOn || gEchoBpm != gBPM || pitchWas != gCsEchoPitch)) { pitchWas = gCsEchoPitch; echoStart(); }
	else if (!want && gEchoOn) echoStop();
	static float lastFb = -1;
	if (gEchoOn && lastFb != gCsEchoFb) {
		lastFb = gCsEchoFb; u32 fb = (u32)(gCsEchoFb * 0x7FFF);
		CSND_SetVol(gEchoCh[0], 0x5000, fb << 16); CSND_SetVol(gEchoCh[1], 0x5000u << 16, fb); csndExecCmds(false);
	}
	static float wph = 0; static int wowWas = 0;   // ECHO WOW: the replay speed drifts, so each repeat bends (tape echo)
	if (gEchoOn && (gEchoWow > 0.001f || wowWas)) {
		wph += 1.0f / 60; wowWas = gEchoWow > 0.001f;
		float w = 1 + gEchoWow * (0.012f * sinf(TWO_PI * 0.45f * wph) + 0.004f * sinf(TWO_PI * 5.5f * wph));
		float base = 32728.498f * (gCsEchoPitch ? powf(2.0f, gCsEchoPitch / 12.0f) : 1.0f);
		u32 t = CSND_TIMER(base * w); if (t < 0x42) t = 0x42; if (t > 0xFFFF) t = 0xFFFF;
		CSND_SetTimer(gEchoCh[0], t); CSND_SetTimer(gEchoCh[1], t); csndExecCmds(false);
	}
}

// ---- IMA-ADPCM for the looper: the second sound chip decodes 4-bit IMA-ADPCM itself, so loops take a
// quarter of the memory (48 s instead of 12). Encoding happens as the capture comes in.
static const int IMA_STEP[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
	143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
	2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
	18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int IMA_IDX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
typedef struct { int pred, idx; } Ima;
static inline int imaEnc(Ima* s, int x) {
	int step = IMA_STEP[s->idx], d = x - s->pred, code = 0;
	if (d < 0) { code = 8; d = -d; }
	int dq = step >> 3;
	if (d >= step) { code |= 4; d -= step; dq += step; }
	if (d >= step >> 1) { code |= 2; d -= step >> 1; dq += step >> 1; }
	if (d >= step >> 2) { code |= 1; dq += step >> 2; }
	s->pred += (code & 8) ? -dq : dq;
	if (s->pred > 32767) s->pred = 32767;
	if (s->pred < -32768) s->pred = -32768;
	s->idx += IMA_IDX[code]; if (s->idx < 0) s->idx = 0; if (s->idx > 88) s->idx = 88;
	return code;
}
static inline int imaDec(Ima* s, int code) {
	int step = IMA_STEP[s->idx], dq = step >> 3;
	if (code & 4) dq += step;
	if (code & 2) dq += step >> 1;
	if (code & 1) dq += step >> 2;
	s->pred += (code & 8) ? -dq : dq;
	if (s->pred > 32767) s->pred = 32767;
	if (s->pred < -32768) s->pred = -32768;
	s->idx += IMA_IDX[code]; if (s->idx < 0) s->idx = 0; if (s->idx > 88) s->idx = 88;
	return s->pred;
}
static void echoTick(void) { echoTickL(); }   // called inside the main loop's voice lock, so CSND commands never overlap the audio thread
// ---- the recorder: the DSP's own output tap (ndsp capture of the final mix) streams into a small ring; the tape packs it
#define CAPRING (32728 * 2 - (32728 * 2) % 160)
static ndspWaveBuf gCapWB;
static s16* gCapBuf = NULL;
static u32 gCapLast = 0;
static int gThru;
static int chipInMix(void) { return gDrums || gVoiceMode >= 3 || gSubLevel > 0.01f; }   // sounds on the second chip: the recorder only hears them through CHIP > FX
// ---- TAPE: bounce the sequence to 4 tracks. REC starts the sequence (or the song) from the top and records Syrup's
// output through the looper's DSP tap as IMA-ADPCM; the tracks already on tape play along on the second sound chip,
// so you change the patch and bounce the next part over them. SAVE WAV writes each track and the mix to the SD card.
#define NTRK 4
#define TAPE_MAX (32728 * 120)   // 2 minutes a track
static u8* gTp[NTRK][2]; static int gTpHave[NTRK], gTpMute[NTRK]; static float gTpLevel[NTRK] = {0.8f, 0.8f, 0.8f, 0.8f};
static int gTapeLen = 0, gTapeLenSel = 2, gTapeArm = 0, gTapeRec = -1, gTapeRecLen = 0, gTapePos = 0, gTapePlay = 0, gTapeCh[NTRK * 2] = {-1, -1, -1, -1, -1, -1, -1, -1};
static volatile int gTapeState = 0, gTapeSkip = 0;   // 1 waiting for the sequence, 2 dropping what came before it, 3 recording
static int quickTapeRec(void) { return gTapeRec >= 0; }
static int gTapeErase = -1; static u64 gTapeT0 = 0, gTapeEraseT = 0; static Ima gTpEnc[2];
// TRACK SPEED: each track plays back at a musical ratio, so it stays on the bar grid: x2 / x3 / x4 repeat the
// track 2 / 3 / 4 times over the tape, 1/2 / 1/3 / 1/4 stretch it (an octave down at 1/2), 2/3 3/4 4/3 3/2 in between
#define NTSPD 11
static const char* TSPD_NAMES[NTSPD] = {"1/4", "1/3", "1/2", "2/3", "3/4", "1X", "4/3", "3/2", "X2", "X3", "X4"};
static const float TSPD[NTSPD] = {0.25f, 1 / 3.0f, 0.5f, 2 / 3.0f, 0.75f, 1, 4 / 3.0f, 1.5f, 2, 3, 4};
static int gTpSpd[NTRK] = {5, 5, 5, 5};
static int gTapeLast = -1;   // the track recorded last: GRAIN SOURCE TAPE and TAPE > TABLE use it
static const char* TLEN_NAMES[6] = {"4 BARS", "8 BARS", "16 BARS", "32 BARS", "64 BARS", "SONG"};
static int tapeReserved(int c) { if (!gTapePlay) return 0; for (int i = 0; i < NTRK * 2; i++) if (gTapeCh[i] == c) return 1; return 0; }
static void tapeStartPlay(int except);
#define TAPE_LAT (160 * 2)   // samples between a frame being made and the capture seeing it
static int gTapeAlign;   // TAPE ALIGN, ms: + moves new takes earlier, - later (fine trim if they sit late / early on hardware)
static void tapeSeqStarted(void) {   // audio thread, the moment the sequence starts: everything captured before it is dropped
	if (gTapeState == 1) {
		// the capture lags what's played by the DSP's output queue: drop that much more so the new take lines up with the
		// tracks the second chip plays (which start right now, with the sequence, not when REC was pressed)
		int sk = (int)gCapWB.offset + TAPE_LAT + gTapeAlign * 33; sk %= CAPRING; if (sk < 0) sk += CAPRING;
		gTapeSkip = sk; gTapeState = 2; tapeStartPlay(gTapeRec);
	}
}
static int tapeSteps(void) {
	if (gTapeLenSel < 5) return (4 << gTapeLenSel) * 16;
	if (gSongOn && gSongLen > 0) { int n = 0; for (int i = 0; i < gSongLen; i++) { int p = gSong[i], L = p == gPatCur ? gSeqLen : gPat[p].len; n += L < 1 ? 16 : L; } return n; }
	return gSeqLen < 1 ? 16 : gSeqLen;
}
static int tapeSamples(void) { long long n = (long long)(tapeSteps() * (gTapeLenSel < 5 ? 1.0f : seqDiv()) * 32728.0 * 60 / (gBPM * 4)); if (n > TAPE_MAX) n = TAPE_MAX; return (int)(n - n % 2); }   /* bars are bars; a pattern / song is its steps */
static int tapeMono(s16* out, int n) {   // the last recorded track (or the first one there is), mono, from the top
	int t = gTapeLast >= 0 && gTpHave[gTapeLast] ? gTapeLast : -1;
	for (int i = 0; i < NTRK && t < 0; i++) if (gTpHave[i]) t = i;
	if (t < 0 || !gTapeLen) return 0;
	if (n > gTapeLen) n = gTapeLen;
	Ima a = {0, 0}, b = {0, 0};
	for (int i = 0; i < n; i++) { int l = gTp[t][0][i >> 1], r = gTp[t][1][i >> 1], sh = (i & 1) ? 4 : 0;
		out[i] = (s16)((imaDec(&a, (l >> sh) & 15) + imaDec(&b, (r >> sh) & 15)) / 2); }
	return n;
}
static int tapeAlloc(int t) {
	for (int k = 0; k < 2; k++) if (!gTp[t][k] && !(gTp[t][k] = (u8*)linearAlloc(TAPE_MAX / 2 + 16))) {
		for (int j = 0; j < 2; j++) if (gTp[t][j]) { linearFree(gTp[t][j]); gTp[t][j] = NULL; }
		return 0;
	}
	return 1;
}
static void tapeVol(int t) {
	if (!gTapePlay || gTapeCh[t * 2 + 1] < 0) return;
	u32 l = gTpMute[t] ? 0 : (u32)(gTpLevel[t] * 0x8000);
	CSND_SetVol(gTapeCh[t * 2], csMono(l), 0); CSND_SetVol(gTapeCh[t * 2 + 1], csMono(l << 16), 0); csndExecCmds(false);
}
static void tapeStopPlay(void) {
	if (gCsndOK) { for (int i = 0; i < NTRK * 2; i++) if (gTapeCh[i] >= 0) CSND_SetPlayState(gTapeCh[i], 0); csndExecCmds(false); }
	gTapePlay = 0;
}
static void tapeStartPlay(int except) {   // every track on tape (but the one recording), from the top, once
	tapeStopPlay();
	if (!gCsndOK || !gTapeLen) return;
	for (int i = 0; i < NTRK * 2; i++) gTapeCh[i] = -1;
	int n = 0; for (int c = NCS - 1; c >= 0 && n < NTRK * 2; c--) if (csUsable(c)) gTapeCh[n++] = c;
	gTapePlay = 1; int short_ = 0;
	for (int t = 0; t < NTRK; t++) {
		if (!gTpHave[t] || t == except) continue;
		if (gTapeCh[t * 2 + 1] < 0) { short_ = 1; continue; }
		u32 l = gTpMute[t] ? 0 : (u32)(gTpLevel[t] * 0x8000);
		for (int k = 0; k < 2; k++) {
			int c = gTapeCh[t * 2 + k]; stopCh(BK_CSND, c);
			float sp = TSPD[gTpSpd[t]];   // faster than 1X repeats, so a bar-multiple keeps landing on the bar
			u32 flags = SOUND_FORMAT_ADPCM | (sp > 1.001f ? SOUND_REPEAT : SOUND_ONE_SHOT) | SOUND_LINEAR_INTERP | SOUND_ENABLE | SOUND_CHANNEL(c) | (CSND_TIMER((int)lroundf(32728 * sp)) << 16);
			CSND_SetAdpcmState(c, 0, 0, 0); CSND_SetAdpcmState(c, 1, 0, 0); CSND_SetAdpcmReload(c, sp > 1.001f);
			CSND_SetChnRegs(flags, osConvertVirtToPhys(gTp[t][k]), osConvertVirtToPhys(gTp[t][k]), gTapeLen / 2, csMono(k ? l << 16 : l), 0);
		}
	}
	csndExecCmds(false); gTapeT0 = osGetTime();
	if (short_) say("TAPE: NOT ENOUGH CHIP CHANNELS FOR EVERY TRACK");
}
static void tapeRecStop(int keep) {
	ndspSetCapture(NULL); gTapeState = 0; int t = gTapeRec; gTapeRec = -1;
	gSeqMode = 0; seqStop(); tapeStopPlay();
	if (t < 0) return;
	if (keep) {
		if (gCsndOK) for (int k = 0; k < 2; k++) CSND_FlushDataCache(gTp[t][k], gTapeRecLen / 2);
		gTpHave[t] = 1; gTapeLast = t; if (!gTapeLen) gTapeLen = gTapeRecLen;
		char m[40]; snprintf(m, sizeof(m), "TAPE: TRACK %d RECORDED", t + 1); say(m);
		for (int i = 1; i <= NTRK; i++) if (!gTpHave[(t + i) % NTRK]) { gTapeArm = (t + i) % NTRK; break; }   // the next empty track is armed
	} else say("TAPE: STOPPED, NOTHING KEPT");
}
static int gThru;
static void tapeRecGo(void) {
	int t = gTapeArm;
	if (!gCapBuf) gCapBuf = (s16*)linearAlloc(CAPRING * 4);
	if (!gCapBuf || !tapeAlloc(t)) { say("NOT ENOUGH MEMORY FOR THE TAPE"); return; }
	if (chipInMix() && !gThru) { gThru = 1; say("TAPE: CHIP > FX ON SO DRUMS + SUB RECORD"); }   // the tape records the DSP mix; this brings the second chip into it
	gTapeRecLen = gTapeLen ? gTapeLen : tapeSamples(); gTapePos = 0; gTpEnc[0] = gTpEnc[1] = (Ima){0, 0};
	memset(&gCapWB, 0, sizeof(gCapWB)); gCapWB.data_pcm16 = gCapBuf; gCapWB.nsamples = CAPRING; gCapWB.offset = 0; gCapLast = 0;
	ndspSetCapture(&gCapWB);
	gTapeRec = t; gTapeState = 1;
	seqStop(); gSongPos = 0; gSeqMode = 1;   // the sequence (or song) from the top; the tracks on tape start with it
	char m[48]; snprintf(m, sizeof(m), "TAPE: RECORDING TRACK %d (%d S)", t + 1, gTapeRecLen / 32728); say(m);
}

// ---- count-in and click, and live recording into the sequencer
static s16* gClickBuf[2];
static int gClickN = 0, gClickCh = -1;   // (the tests count clicks and check the channel)
static void clickNow(int acc) {   // a short tick on a free second-chip channel (the tape recorder hears only the DSP)
	if (!gCsndOK) return;
	if (!gClickBuf[0]) for (int k = 0; k < 2; k++) {
		if (!(gClickBuf[k] = (s16*)linearAlloc(1200 * 2))) return;
		for (int i = 0; i < 1200; i++) { float t = i / 32728.0f; gClickBuf[k][i] = (s16)(sinf(TWO_PI * (k ? 2000 : 1300) * t) * expf(-t * 110) * 0.7f * 32767); }
		CSND_FlushDataCache(gClickBuf[k], 1200 * 2);
	}
	int c = -1; for (int i = NCS - 1; i >= 0; i--) if (csUsable(i) && gCsOwner[i] < 0) { c = i; break; }
	if (c < 0) return;
	gClickN++; gClickCh = c;
	u32 v = (u32)((acc ? 0x7000 : 0x4400) * gClickLevel * 1.4f); if (v > 0x7FFF) v = 0x7FFF;   // straight to the speakers: no capture send, so CHIP > FX can't put it on tape
	CSND_SetChnRegs(SOUND_FORMAT_16BIT | SOUND_ONE_SHOT | SOUND_LINEAR_INTERP | SOUND_ENABLE | SOUND_CHANNEL(c) | (CSND_TIMER(32728) << 16),
		osConvertVirtToPhys(gClickBuf[acc]), osConvertVirtToPhys(gClickBuf[acc]), 1200 * 2, v | (v << 16), 0);
	csndExecCmds(false);
}
static void undoMark(void);
static float seqNowSteps(void) { float l = stepLen(gSeqPos < 0 ? 0 : gSeqPos); return gSeqClock + (l > 0 ? gSeqPh / l : 0); }
static void liveUp(void) {   // the held notes end: their length is how long they were held, to the nearest step
	if (!gLvN) return;
	int L = (int)(seqNowSteps() - gLvT0 + 0.5f); if (L < 1) L = 1; if (L > NSTEP) L = NSTEP;
	for (int i = 0; i < gLvN; i++) stepSetLen(gLvStep, gLvNote[i], L);
	gLvN = 0;
}
static int recPos(int* nudge, int* ahead) {   // where a note played now goes, by REC QUANT
	int Lp = gSeqLen < 1 ? 1 : gSeqLen, p = gSeqPos < 0 ? 0 : gSeqPos; float f = gSeqPh / stepLen(p);   // 0..1 through this step
	*nudge = 0;
	if (gRecQ == 1) {   // 8ths: the nearest even step
		float pos = p + f; int q = (int)(pos / 2 + 0.5f) * 2; *ahead = q - p; return ((q % Lp) + Lp) % Lp;
	}
	if (gRecQ == 3) {   // 32NDS: the step, or half a step later (NUDGE +50), whichever is nearer
		int late = f >= 0.75f; *ahead = late; *nudge = (!late && f >= 0.25f) ? 50 : 0; return (p + late) % Lp;
	}
	int late = f >= 0.5f; *ahead = late;
	if (gRecQ == 2) { int nd = (int)lroundf((late ? f - 1 : f) * 100); *nudge = nd < -50 ? -50 : nd > 50 ? 50 : nd; }   // OFF: keep the timing
	return (p + late) % Lp;
}
static void liveDown(const int* notes, int n, float vel) {   // a note played while recording: onto the grid (REC QUANT)
	liveUp();
	if (!gSeqPlaying || n <= 0) return;
	int nd, ah, st = recPos(&nd, &ah); int fresh = !gSeq[st].n;
	for (int i = 0; i < n && i < 4; i++) { int nt = notes[i] - gSeqTr; stepSetNote(st, nt, 1); gLvNote[i] = nt; }
	gTapNote = notes[0] - gSeqTr;
	gSeq[st].vel = (u8)(vel * 255 > 255 ? 255 : vel * 255); if (fresh || gRecQ >= 2) gSeq[st].nudge = (s8)nd;
	gLvN = n < 4 ? n : 4; gLvStep = st; gLvT0 = (float)(gSeqClock + ah);
}
// MIDI: notes come one at a time; keys struck together (same step, the first still held) build one chord
static void liveAdd(const int* notes, int n, float vel) {
	if (gLvN && gSeqPlaying) {
		int nd, ah, st = recPos(&nd, &ah);
		if (st == gLvStep) {
			for (int i = 0; i < n && gLvN < 4; i++) { int nt = notes[i] - gSeqTr; stepSetNote(gLvStep, nt, 1); gLvNote[gLvN++] = nt; }
			return;
		}
	}
	liveDown(notes, n, vel);
}
// TAP REC: knock on the table while live REC; the mic hears it, harder = louder, onto the grid like a played note
static float gSqTapLevel = 0, gSqTapHold = 0;
static void seqTapTick(float dt) {   // main thread, voice lock held
	if (!gLiveRec || !gTapRec || !gSeqPlaying || !gMicOK || !gMicBuf || gDrumRec) return;
	u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2; const s16* m = (const s16*)gMicBuf;
	float pk = 0; for (int i = 0; i < 300; i++) { float v = fabsf(m[(end + N - 1 - i) % N] / 32768.0f); if (v > pk) pk = v; }
	if (gSqTapHold > 0) { gSqTapHold -= dt; gSqTapLevel += (pk - gSqTapLevel) * 0.2f; return; }
	if (pk > 0.2f && pk > gSqTapLevel * 2.5f + 0.05f) {
		float vel = (pk - 0.2f) / 0.6f; if (vel > 1) vel = 1; vel = 0.3f + 0.7f * vel;
		int nd, ah, st = recPos(&nd, &ah);
		if (gTapRec == 1) {   // NOTES: the last pitch you played or put on the roll
			int nt = gTapNote >= 0 ? gTapNote : (gSeqBase >= 0 ? gSeqBase : gridNote(0)); int fresh = !gSeq[st].n;
			stepSetNote(st, nt, 1); gSeq[st].vel = (u8)(vel * 255); if (fresh || gRecQ == 2) gSeq[st].nudge = (s8)nd;
			voiceOn(snapScale(nt + gSeqTr), vel);   // you hear it now; the sequence plays it from the next loop
		} else if (gSeq[st].n) { gSeq[st].vel = (u8)(vel * 255); if (gSeq[st].vel > 200) gSeq[st].fl |= 1; else gSeq[st].fl &= ~1; }   // VELOCITY: re-sets that step (hard = accent)
		gSqTapHold = 0.09f; gVisBeat = vel;
	}
	gSqTapLevel += (pk - gSqTapLevel) * 0.2f;
}
static void liveRecGo(void) {
	undoMark();   // UNDO (in PAT) takes the whole take back
	if (gSeqBase < 0) gSeqBase = gridNote(0);
	gLiveRec = 1; gLvN = 0; seqStop(); gSongPos = 0; gSeqMode = 1;
	say("LIVE REC: PLAY THE KEYS (ANY PAGE) OR MIDI");
}
static void liveRecStop(void) { liveUp(); gLiveRec = 0; if (gPreFor == 2) gPreFor = 0; patStore(); say("LIVE REC OFF"); }
static void prerollStart(int what) {   // what: 1 tape, 2 live sequencer
	if (!gPreroll) { if (what == 1) tapeRecGo(); else liveRecGo(); return; }
	if (what == 2) { gSeqMode = 0; seqStop(); }
	gPreFor = what; gPreBeats = gPreroll * 4.0f; gPreDone = 0;
	if (gClick) clickNow(1);
	char m[32]; snprintf(m, sizeof(m), "COUNT-IN: %d", (int)gPreBeats); say(m);
}
static void tapeRecStart(void) { prerollStart(1); }
static void liveRecToggle(void) { if (gLiveRec || gPreFor == 2) liveRecStop(); else prerollStart(2); }
static void prerollTick(float dt) {   // main loop, voice lock held
	if (gPreFor) {
		int before = (int)gPreDone; gPreDone += dt * gBPM / 60.0f; int now = (int)gPreDone;
		if (gPreDone >= gPreBeats) { int w = gPreFor; gPreFor = 0; if (w == 1) tapeRecGo(); else liveRecGo(); return; }
		if (now != before) { if (gClick) clickNow(now % 4 == 0); char m[32]; snprintf(m, sizeof(m), "COUNT-IN: %d", (int)(gPreBeats - now)); say(m); }
		return;
	}
}
static long long gDrClk = -1;   // the drum clock's 16ths (when the drums run on their own)
static void clickTick(void) {   // audio thread, every frame: the metronome on the beat, from the clock that's running
	static long long last = -1; long long q = -1;
	if (gSeqPlaying) q = (long long)(gSeqClock * seqDiv() + 1e-4);   // 16ths
	else if (gDrums && gDrClk >= 0) q = gDrClk;
	if (q < 0) { last = -1; return; }
	int on = gClick == 3 || (gClick == 2 && ((gTapeRec >= 0 && gTapeState == 3) || gLiveRec));
	if (q != last) { if (on && q % 4 == 0) clickNow(q % 16 == 0); last = q; }
}
static void tapeTick(void) {   // main thread, voice lock held
	if (gTapePlay && gTapeRec < 0 && osGetTime() - gTapeT0 > (u64)gTapeLen * 1000 / 32728 + 300) tapeStopPlay();
	if (gTapeRec < 0) return;
	if (gSeqMode != 1 && gTapeState >= 2) { tapeRecStop(0); return; }   // the sequence was stopped
	if (gTapeState < 2) return;
	u32 off = gCapWB.offset; int n = (int)off - (int)gCapLast; if (n < 0) n += CAPRING;
	if (n <= 0) return;
	DSP_InvalidateDataCache(gCapBuf, CAPRING * 4);
	if (gTapeState == 2) {
		int drop = (gTapeSkip - (int)gCapLast + CAPRING) % CAPRING, d = drop < n ? drop : n;
		gCapLast = (gCapLast + d) % CAPRING; n -= d; if (d == drop) gTapeState = 3;
	}
	int t = gTapeRec;
	while (gTapeState == 3 && n > 0 && gTapePos < gTapeRecLen) {
		int run = CAPRING - gCapLast; if (run > n) run = n; if (run > gTapeRecLen - gTapePos) run = gTapeRecLen - gTapePos;
		for (int k = 0; k < 2; k++) {
			const s16* in = gCapBuf + gCapLast * 2 + k; u8* d = gTp[t][k];
			for (int i = 0; i < run; i++) { int p = gTapePos + i, c = imaEnc(&gTpEnc[k], in[i * 2]); if (p & 1) d[p >> 1] |= c << 4; else d[p >> 1] = (u8)c; }
		}
		gTapePos += run; gCapLast = (gCapLast + run) % CAPRING; n -= run;
	}
	if (gTapePos >= gTapeRecLen) tapeRecStop(1);
}
static void tapeErase(int t) {
	gTpHave[t] = 0; int any = 0; for (int i = 0; i < NTRK; i++) any |= gTpHave[i];
	if (!any) gTapeLen = 0;   // an empty tape: the length can change again
	char m[32]; snprintf(m, sizeof(m), any ? "TRACK %d ERASED" : "TAPE EMPTY", t + 1); say(m);
}
static void wavHdr(FILE* f, int n) {
	int bytes = n * 4, h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 32728, 32728 * 4, 0x00100004, 0x61746164, bytes};
	fwrite(h, 4, 11, f);
}
static void tapeExport(void) {   // each track and the mix, 16-bit WAV at the console's 32728 Hz
	if (!gTapeLen) { say("THE TAPE IS EMPTY"); return; }
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777); mkdir("sdmc:/3ds/syrup/tape", 0777);
	FILE* f[NTRK + 1] = {0}; Ima st[NTRK][2]; memset(st, 0, sizeof(st)); long long rq[NTRK][2] = {{0}}; int rv[NTRK][2] = {{0}};
	for (int t = 0; t < NTRK; t++) if (gTpHave[t]) { char p[48]; snprintf(p, sizeof(p), "sdmc:/3ds/syrup/tape/track%d.wav", t + 1); f[t] = fopen(p, "wb"); if (f[t]) wavHdr(f[t], gTapeLen); }
	f[NTRK] = fopen("sdmc:/3ds/syrup/tape/mix.wav", "wb"); if (f[NTRK]) wavHdr(f[NTRK], gTapeLen);
	static s16 tb[2048 * 2]; static float mx[2048 * 2];
	for (int p = 0; p < gTapeLen; p += 2048) {
		int m = gTapeLen - p < 2048 ? gTapeLen - p : 2048; memset(mx, 0, sizeof(float) * m * 2);
		for (int t = 0; t < NTRK; t++) {
			if (!gTpHave[t]) continue;
			float g = gTpMute[t] ? 0 : gTpLevel[t], sp = TSPD[gTpSpd[t]];
			for (int k = 0; k < 2; k++) for (int i = 0; i < m; i++) {   // at its TRACK SPEED: read ahead / hold, faster ones wrap
				double src = (double)(p + i) * sp; long long si = (long long)src;
				if (sp > 1.001f) si %= gTapeLen;
				int v = 0;
				if (si < gTapeLen) {
					if (si < rq[t][k]) { rq[t][k] = 0; st[t][k] = (Ima){0, 0}; rv[t][k] = 0; }   // wrapped: decode from the top
					while (rq[t][k] <= si) { int q = (int)rq[t][k], b = gTp[t][k][q >> 1]; rv[t][k] = imaDec(&st[t][k], (q & 1) ? b >> 4 : b & 15); rq[t][k]++; }
					v = rv[t][k];
				}
				tb[i * 2 + k] = (s16)v; mx[i * 2 + k] += v * g;
			}
			if (f[t]) fwrite(tb, 4, m, f[t]);
		}
		for (int i = 0; i < m * 2; i++) { float v = mx[i]; tb[i] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
		if (f[NTRK]) fwrite(tb, 4, m, f[NTRK]);
	}
	int ok = f[NTRK] != NULL; for (int t = 0; t <= NTRK; t++) if (f[t]) fclose(f[t]);
	say(ok ? "SAVED: /3DS/SYRUP/TAPE (TRACKS + MIX.WAV)" : "COULDN'T WRITE TO THE SD CARD");
}
// the TAPE view (PAT > TAPE): length, REC / PLAY / SAVE WAV, and the 4 tracks
#define TP_Y (XY_Y + 18)
#define TP_RY (TP_Y + 22)
#define TP_RH 32
#define TP_OPT_Y (TP_RY + NTRK * TP_RH + 14)
static void drawTapeView(void) {
	static const char* TB[5] = {"< PAT", "", "REC", "PLAY", "WAV"};
	for (int i = 0; i < 5; i++) {
		int x = 4 + i * 63, lit = (i == 2 && gTapeRec >= 0) || (i == 3 && gTapePlay && gTapeRec < 0);
		char lab[20]; if (i == 2 && gPreFor == 1) { snprintf(lab, sizeof(lab), "IN %d", (int)(gPreBeats - gPreDone) + 1); } else if (i == 1) snprintf(lab, sizeof(lab), "%s", gTapeLen ? "LOCKED" : TLEN_NAMES[gTapeLenSel]); else snprintf(lab, sizeof(lab), "%s", i == 3 && gTapePlay && gTapeRec < 0 ? "STOP" : i == 2 && gTapeRec >= 0 ? "STOP" : TB[i]);
		const u8* bg = i == 2 ? (gTapeRec >= 0 ? TH->warn : TH->keyOn) : lit ? TH->keyOn : TH->tab;
		rectP(x, TP_Y, 59, 16, bg); const u8* tc = (i == 2 || lit) ? TH->ink : TH->tabTxt; text(x + 30 - (int)strlen(lab) * 4, TP_Y + 4, lab, 1, tc[0], tc[1], tc[2]);
	}
	float dur = (gTapeRec >= 0 ? gTapeRecLen : gTapeLen) / 32728.0f;
	float head = gTapeRec >= 0 ? gTapePos / (float)(gTapeRecLen > 0 ? gTapeRecLen : 1) : gTapePlay && gTapeLen ? (osGetTime() - gTapeT0) / 1000.0f / dur : -1;
	for (int t = 0; t < NTRK; t++) {
		int y = TP_RY + t * TP_RH, arm = t == gTapeArm, rec = t == gTapeRec;
		rectP(4, y, 40, TP_RH - 4, rec ? TH->warn : arm ? TH->keyOn : TH->tab);
		char nm[4]; snprintf(nm, sizeof(nm), "%d", t + 1); const u8* tc = (rec || arm) ? TH->ink : TH->tabTxt; text(20, y + 4, nm, 1, tc[0], tc[1], tc[2]);
		text(8, y + 15, rec ? "REC" : arm ? "ARM" : "", 1, tc[0], tc[1], tc[2]);
		rectP(48, y, 72, TP_RH - 4, TH->pad);   // the strip of tape
		if (gTpHave[t] && !rec) { rectP(48, y + 6, 72, TP_RH - 16, gTpMute[t] ? TH->grid : TH->val); }
		else if (rec && head >= 0) rectP(48, y + 6, (int)(72 * head), TP_RH - 16, TH->warn);
		else text(64, y + 10, "EMPTY", 1, RGB3(TH->dim));
		if (head >= 0 && head <= 1) rectP(48 + (int)(71 * head), y, 1, TP_RH - 4, TH->keyOn);
		{ int on = gTpSpd[t] != 5; rectP(124, y, 40, TP_RH - 4, on ? TH->keyOn : TH->tab); const u8* c = on ? TH->ink : TH->tabTxt;   // TRACK SPEED: left half slower, right half faster
		  text(144 - (int)strlen(TSPD_NAMES[gTpSpd[t]]) * 4, y + 10, TSPD_NAMES[gTpSpd[t]], 1, c[0], c[1], c[2]); text(126, y + 1, "-", 1, c[0], c[1], c[2]); text(155, y + 1, "+", 1, c[0], c[1], c[2]); }
		rectP(168, y, 40, TP_RH - 4, gTpMute[t] ? TH->warn : TH->tab); text(172, y + 10, "MUTE", 1, RGB3(gTpMute[t] ? TH->ink : TH->tabTxt));
		rectP(212, y + 11, 70, 3, TH->tab); rectP(212, y + 10, (int)(70 * gTpLevel[t]), 5, TH->val); rectP(212 + (int)(69 * gTpLevel[t]) - 1, y + 5, 3, 15, TH->keyOn);
		int er = gTapeErase == t && osGetTime() - gTapeEraseT < 3000;
		rectP(286, y, 30, TP_RH - 4, er ? TH->warn : TH->tab); text(297, y + 10, "X", 1, RGB3(er ? TH->ink : TH->tabTxt));
	}
	char m[60];
	if (gTapeLen || gTapeRec >= 0) snprintf(m, sizeof(m), "%d:%02d  %s", (int)dur / 60, (int)dur % 60, gThru ? "DRUMS GO ON TAPE" : "DRUMS: CHIP>FX TO TAPE THEM");
	else snprintf(m, sizeof(m), "REC PLAYS THE %s FROM THE TOP", gTapeLenSel == 5 && gSongOn ? "SONG" : "PATTERN");
	text(4, TP_RY + NTRK * TP_RH + 2, m, 1, RGB3(TH->dim));
	char a[24], c2[24]; snprintf(a, sizeof(a), "PREROLL %s", PRE_NAMES[gPreroll]); snprintf(c2, sizeof(c2), "CLICK %s", CLICK_NAMES[gClick]);
	rectP(4, TP_OPT_Y, 152, 16, TH->tab); text(80 - (int)strlen(a) * 4, TP_OPT_Y + 4, a, 1, RGB3(TH->tabTxt));
	rectP(162, TP_OPT_Y, 152, 16, TH->tab); text(238 - (int)strlen(c2) * 4, TP_OPT_Y + 4, c2, 1, RGB3(TH->tabTxt));
}
static int gTpDrag = -1;
static int tapeTouch(int x, int y, int press, int release) {
	if (release) { gTpDrag = -1; return 1; }
	if (press && y >= TP_Y && y < TP_Y + 17) {
		int b = (x - 4) / 63;
		if (b == 0) gPatView = 1;
		if (b == 1) { if (gTapeLen) say("ERASE EVERY TRACK TO CHANGE THE LENGTH"); else gTapeLenSel = (gTapeLenSel + 1) % 6; }
		if (b == 2) { if (gPreFor == 1) { gPreFor = 0; say("COUNT-IN STOPPED"); } else if (gTapeRec >= 0) tapeRecStop(0); else tapeRecStart(); }
		if (b == 3 && gTapeRec < 0) { if (gTapePlay) tapeStopPlay(); else if (gTapeLen) tapeStartPlay(-1); else say("THE TAPE IS EMPTY"); }
		if (b == 4 && gTapeRec < 0) tapeExport();
		return 1;
	}
	if (press && y >= TP_OPT_Y && y < TP_OPT_Y + 16) { if (x < 160) gPreroll = (gPreroll + 1) % 3; else gClick = (gClick + 1) % 4; return 1; }
	int t = (y - TP_RY) / TP_RH;
	if (press) gTpDrag = -1;
	if (y < TP_RY || t < 0 || t >= NTRK) return press;
	if (press && x < 44) { if (gTapeRec < 0) gTapeArm = t; return 1; }
	if (press && x >= 168 && x < 208) { gTpMute[t] = !gTpMute[t]; tapeVol(t); return 1; }
	if (press && x >= 122 && x < 166) {   // TRACK SPEED: - / + step round the speeds, heard at once
		gTpSpd[t] = (gTpSpd[t] + (x >= 144 ? 1 : -1) + NTSPD) % NTSPD;   // round and round: past X4 comes 1/4 again
		if (gTapePlay && gTpHave[t] && gTapeRec != t) {   // heard now, not just from the next play
			float sp = TSPD[gTpSpd[t]];
			for (int k = 0; k < 2; k++) { int c = gTapeCh[t * 2 + k]; if (c < 0) continue;
				CSND_SetTimer(c, CSND_TIMER((int)lroundf(32728 * sp))); CSND_SetAdpcmReload(c, sp > 1.001f); CSND_SetLooping(c, sp > 1.001f ? 1 : 2); }
			csndExecCmds(false);
		}
		char m[40]; snprintf(m, sizeof(m), "TRACK %d SPEED %s", t + 1, TSPD_NAMES[gTpSpd[t]]); say(m); return 1;
	}
	if (press && x >= 286) {
		if (gTapeRec == t) return 1;
		if (gTapeErase == t && osGetTime() - gTapeEraseT < 3000) { gTapeErase = -1; tapeErase(t); }
		else if (gTpHave[t]) { gTapeErase = t; gTapeEraseT = osGetTime(); say("TAP X AGAIN TO ERASE THE TRACK"); }
		return 1;
	}
	if (press && x >= 208 && x < 286) gTpDrag = t;
	if (gTpDrag >= 0) { float v = (x - 212) / 69.0f; gTpLevel[gTpDrag] = v < 0 ? 0 : v > 1 ? 1 : v; tapeVol(gTpDrag); return 1; }
	return press;
}
// ---- CHIP > FX: the second sound chip's parts (sub, chip square, drums and kits, chip effects) skip the speakers and
// go to its two capture units instead; DSP channels 19 / 20 replay that capture through a filter and the master
// effects. Both chips run off one clock (32728.498 Hz), so the replay never drifts; it lags the capture by ~10 ms.
static int gThru = 0, gThruUnits[2] = {-1, -1}; static float gThruCut = 1.0f, gThruRes = 0.1f;
#define THRU_N 2048
static s16* gThruBuf[2]; static ndspWaveBuf gThruWB[2];
static void thruStop(void) {
	if (!gThruOn) return;
	gThruOn = 0;
	for (int k = 0; k < 2; k++) { ndspChnWaveBufClear(THRU_CH0 + k); if (gThruUnits[k] >= 0) { CSND_CapEnable(gThruUnits[k], false); } }
	if (gCsndOK) csWait();
	for (int k = 0; k < 2; k++) if (gThruUnits[k] >= 0) { CSND_ReleaseCapUnit(gThruUnits[k]); gThruUnits[k] = -1; }
}
static void thruMix(void) {
	for (int k = 0; k < 2; k++) {
		float mix[12] = {0}; mix[k] = 2.0f;   // back to full level (the capture ran at half)
		float s0 = busSend(0), s1 = busSend(1);
		mix[4] = mix[0] * s0; mix[5] = mix[1] * s0; mix[8] = mix[0] * s1; mix[9] = mix[1] * s1;
		ndspChnSetMix(THRU_CH0 + k, mix);
	}
}
static float gThruCutEff = 1.0f;   // CHIP CUTOFF after the patch's envelope / LFO
static void thruFilter(void) {
	float hz = 40.0f * powf(2.0f, gThruCutEff * 9.0f); if (hz > 15000) hz = 15000;
	for (int k = 0; k < 2; k++) {
		if (gThruCutEff >= 0.999f) { ndspChnIirBiquadSetEnable(THRU_CH0 + k, false); continue; }
		ndspChnIirBiquadSetParamsLowPassFilter(THRU_CH0 + k, hz, 0.707f + gThruRes * 6.0f); ndspChnIirBiquadSetEnable(THRU_CH0 + k, true);
	}
}
static void thruStart(void) {
	if (!gCsndOK || !gAudioOK || gThruOn) return;
	echoStop();
	for (int k = 0; k < 2; k++) {
		if (!gThruBuf[k]) gThruBuf[k] = (s16*)linearAlloc(THRU_N * 2);
		u32 u; if (!gThruBuf[k] || R_FAILED(CSND_AcquireCapUnit(&u))) { if (k && gThruUnits[0] >= 0) { CSND_ReleaseCapUnit(gThruUnits[0]); gThruUnits[0] = -1; } say("CHIP > FX: NO CAPTURE UNIT FREE"); gThru = 0; return; }
		gThruUnits[k] = u;
		memset(gThruBuf[k], 0, THRU_N * 2); CSND_FlushDataCache(gThruBuf[k], THRU_N * 2);
	}
	for (int k = 0; k < 2; k++) {
		int c = THRU_CH0 + k;
		if (gDspOwner[c] >= 0) freeVoice(&gV[gDspOwner[c]]);
		ndspChnWaveBufClear(c); ndspChnReset(c);
		ndspChnSetFormat(c, NDSP_FORMAT_MONO_PCM16); ndspChnSetInterp(c, NDSP_INTERP_NONE); ndspChnSetRate(c, NDSP_SAMPLE_RATE);
		CSND_SetCapRegs(gThruUnits[k], CAPTURE_REPEAT | CAPTURE_FORMAT_16BIT | CAPTURE_ENABLE, osConvertVirtToPhys(gThruBuf[k]), THRU_N * 2);
		CSND_CapSetTimer(gThruUnits[k], CSND_TIMER(32728));
	}
	gThruOn = 1;   // from here the chip parts send to the capture (csSend) and go quiet on the speakers
	for (int k = 0; k < 2; k++) CSND_CapEnable(gThruUnits[k], true);
	csndExecCmds(false);
	svcSleepThread(8000000);   // let the capture get ahead before the DSP starts reading behind it
	thruMix(); thruFilter();
	for (int k = 0; k < 2; k++) {
		memset(&gThruWB[k], 0, sizeof(ndspWaveBuf));
		gThruWB[k].data_vaddr = gThruBuf[k]; gThruWB[k].nsamples = THRU_N; gThruWB[k].looping = true;
		ndspChnWaveBufAdd(THRU_CH0 + k, &gThruWB[k]);
	}
}
static void thruTick(void) {   // main loop, voice lock held
	int want = gThru && gCsndOK && gAudioOK;
	if (want && !gThruOn) thruStart();
	else if (!want && gThruOn) thruStop();
	if (!gThruOn) return;
	float eff = gThruCut;
	if (gThruEnv != 0) { float e = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].env2 > e) e = gV[i].env2; eff += gThruEnv * e; }   // the newest notes' ENV 2
	if (gThruLfo != 0) eff += gThruLfo * gLfo[0].val;
	eff = eff < 0 ? 0 : eff > 1 ? 1 : eff;
	static float lc = -1, lr = -1; if (fabsf(lc - eff) > 0.004f || lr != gThruRes) { lc = eff; lr = gThruRes; gThruCutEff = eff; thruFilter(); }
	static int f = 0; if (++f % 15 == 0) thruMix();   // follow the FX sends
}
static int tapeMono(s16* out, int n);
static void kitToTable(void);
static void loopToTable(void) {   // TAPE > TABLE: the last track you recorded becomes a wavetable
	int n = gTapeLen > 32728 * 12 ? 32728 * 12 : gTapeLen;
	s16* raw = n > 0 ? (s16*)malloc(sizeof(s16) * n) : NULL; n = raw ? tapeMono(raw, n) : 0;
	if (!n) { free(raw); say("RECORD A TAPE TRACK FIRST"); return; }
	float* in = (float*)malloc(sizeof(float) * n); float* fr = (float*)malloc(sizeof(float) * CYC * WT_FR);
	if (in && fr) { for (int i = 0; i < n; i++) in[i] = raw[i] / 32768.0f; audioToFrames(in, n, 32728, fr); wtInstall("TAPE", fr); }
	free(in); free(fr); free(raw);
}

// ---- grain engine: the DSP plays short windowed slices from a source, two channels offset by half a grain;
// the CPU only refills a slice once the DSP has finished with it (so the DSP sets the pace).
// GRAINS LOOP plays the source by itself; GRAINS KEYS makes it an oscillator: pitch from the keys, loudness
// from the voice envelope, and with the TABLE source the position follows OSC A's WT POS (and its mods).
static int gGrains = 0, gGrainSrc = 1; static float gGrainSize = 0.35f, gGrainScan = 0.5f, gGrainSpray = 0.2f, gGrainPitch = 0, gGrainLevel = 0.8f;
static const char* GRAIN_MODES[3] = {"OFF", "LOOP", "KEYS"};
static const char* GRAIN_SRCS[3] = {"TAPE", "TABLE", "MIC"};
#define GR_CH0 21
#define GR_N 4
#define GR_MAX 8192
static s16* gGrBuf[GR_N]; static ndspWaveBuf gGrWB[GR_N]; static s16* gGrPad = NULL; static ndspWaveBuf gGrPadWB;
static float gGrPos = 0; static int gGrHold = 0, gGrBuildN = 0;   // GRAIN EDITOR: a finger on the waveform holds the position there
static s16* gGrSrcBuf = NULL; static int gGrSrcLen = 0, gGrSrcRate = 32728, gGrSrcKind = -1; static char gGrSrcTab[40] = "";
#define GR_SRCMAX (32728 * 16)
static int grainReserved(int c) { return gGrains && (c == GR_CH0 || c == GR_CH0 + 1); }
static int grainBuild(void) {   // (re)make the source buffer for the chosen source; 0 = nothing to play yet
	if (!gGrSrcBuf && !(gGrSrcBuf = (s16*)linearAlloc(GR_SRCMAX * 2))) return 0;
	if (gGrainSrc == 0) {   // TAPE: the last track you recorded
		int n = tapeMono(gGrSrcBuf, GR_SRCMAX); if (!n) return 0;
		gGrSrcLen = n; gGrSrcRate = 32728;
	} else if (gGrainSrc == 1) {   // OSC A's table: each frame as six cycles of middle C, frame after frame
		Table* tb = &gOsc[0].tb; int nf = tb->nFrames < 1 ? 1 : tb->nFrames; const float P = 32728.0f / 261.63f; int per = (int)(P * 6);
		float pk = tb->peak > 1e-6f ? tb->peak : 1;
		LightLock_Lock(&gMipsLock);
		int n = 0;
		for (int f = 0; f < nf && n + per < GR_SRCMAX; f++) {
			const float* c = tb->frames + f * CYC;
			for (int i = 0; i < per; i++) { float x = fmodf(i / P, 1.0f) * CYC; int k = (int)x; float u = x - k; float v = (c[k] * (1 - u) + c[(k + 1) % CYC] * u) / pk; gGrSrcBuf[n++] = (s16)(v * 26000); }
		}
		snprintf(gGrSrcTab, sizeof(gGrSrcTab), "%s", tb->name);
		LightLock_Unlock(&gMipsLock);
		gGrSrcLen = n; gGrSrcRate = 32728;
	} else {   // MIC: whatever the mic heard over the last second or so, oldest first
		if (!gMicOK || !gMicBuf) return 0;
		u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2; const s16* m = (const s16*)gMicBuf;
		if (N > GR_SRCMAX) N = GR_SRCMAX;
		for (u32 i = 0; i < N; i++) gGrSrcBuf[i] = m[(end + i) % N];
		gGrSrcLen = N; gGrSrcRate = 16364;
	}
	DSP_FlushDataCache(gGrSrcBuf, gGrSrcLen * 2);
	gGrSrcKind = gGrainSrc; gGrBuildN++;
	return gGrSrcLen > 64;
}
static void grainMix(float g) {   // both grain channels: level, and into the master chain when it's on
	for (int k = 0; k < 2; k++) {
		float mix[12] = {0}, pan = k ? 0.65f : 0.35f;
		if (masterActive()) { mix[4] = sqrtf(1 - pan) * g; mix[5] = sqrtf(pan) * g; } else { mix[0] = sqrtf(1 - pan) * g; mix[1] = sqrtf(pan) * g; }
		ndspChnSetMix(GR_CH0 + k, mix);
	}
}
static void grainsTick(void) {   // main thread, every frame
	if (!gGrains || !gAudioOK) return;
	if (gGrSrcKind != gGrainSrc || gGrSrcLen <= 64 || (gGrainSrc == 1 && strcmp(gGrSrcTab, gOsc[0].tb.name))) { if (!grainBuild()) return; }
	static int live = 0; if (gGrainSrc == 1 && !strcmp(gOsc[0].tb.name, "CAM LIVE") && ++live >= 15) { live = 0; grainBuild(); }   // follow the live camera table
	float ratio = 1, g = gGrainLevel;
	if (gGrains == 2) {   // KEYS: newest sounding voice sets pitch and loudness
		SVoice* nv = NULL; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && (!nv || gV[i].age > nv->age)) nv = &gV[i];
		if (!nv) { grainMix(0); return; }
		ratio = nv->cur / 261.63f; g *= nv->env * nv->env * (0.35f + 0.65f * nv->vel) * 1.4f;
	}
	grainMix(g * gMaster);
	int len = (int)(512 + gGrainSize * 7000); if (len > GR_MAX) len = GR_MAX;
	float rate = gGrSrcRate * ratio * powf(2.0f, gGrainPitch / 12.0f);
	if (rate > 190000) rate = 190000;
	for (int b = 0; b < GR_N; b++) {
		if (gGrWB[b].status == NDSP_WBUF_QUEUED || gGrWB[b].status == NDSP_WBUF_PLAYING) continue;
		if (!gGrBuf[b] && !(gGrBuf[b] = (s16*)linearAlloc(GR_MAX * 2))) return;
		int L = gGrSrcLen;
		if (gGrHold) {}   // the editor has it
		else if (gGrains == 2 && gGrainSrc == 1) {   // TABLE in KEYS: position = OSC A's WT POS (with mods) plus SCAN drift
			static float drift = 0; drift += len * 0.5f * (gGrainScan * 2 - 1); while (drift < 0) drift += L; while (drift >= L) drift -= L;
			gGrPos = gPosEff[0] * (L - len) + (gGrainScan != 0.5f ? drift : 0);
		} else gGrPos += len * 0.5f * (gGrainScan * 2 - 1);   // SCAN: 50% = frozen, above = forwards, below = backwards
		while (gGrPos < 0) gGrPos += L;
		while (gGrPos >= L) gGrPos -= L;
		float gm = modSum(DST_GRPOS, (SVoice*)newestVoice());   // GRAIN POS in the matrix: an LFO (or anything) sweeps the read point, +-half the source
		int st = (int)gGrPos + (int)(gm * 0.5f * L) + (int)((rand() % 1000 / 1000.0f - 0.5f) * gGrainSpray * gGrSrcRate);
		st %= L; if (st < 0) st += L;
		for (int i = 0; i < len; i++) {   // Hann window
			float w = 0.5f - 0.5f * cosf(TWO_PI * i / len);
			gGrBuf[b][i] = (s16)(gGrSrcBuf[(st + i) % L] * w);
		}
		DSP_FlushDataCache(gGrBuf[b], len * 2);
		int ch = GR_CH0 + (b & 1);
		memset(&gGrWB[b], 0, sizeof(ndspWaveBuf)); gGrWB[b].data_vaddr = gGrBuf[b]; gGrWB[b].nsamples = len;
		ndspChnSetRate(ch, rate);
		ndspChnWaveBufAdd(ch, &gGrWB[b]);
	}
}
static void grainsChanged(void) {
	if (!gAudioOK) return;
	for (int c = GR_CH0; c <= GR_CH0 + 1; c++) {
		if (gDspOwner[c] >= 0) stopCh(BK_DSP, c);
		ndspChnWaveBufClear(c);
		if (gGrains) { ndspChnReset(c); ndspChnSetInterp(c, NDSP_INTERP_LINEAR); ndspChnSetFormat(c, NDSP_FORMAT_MONO_PCM16); }
	}
	for (int b = 0; b < GR_N; b++) gGrWB[b].status = NDSP_WBUF_DONE;
	gGrPos = 0; gGrSrcKind = -1;
	if (!gGrains) return;
	if (!grainBuild()) { say(gGrainSrc == 0 ? "GRAIN SOURCE TAPE: RECORD A TAPE TRACK FIRST" : "GRAIN SOURCE MIC: MIC NOT AVAILABLE"); return; }
	if (!gGrPad && (gGrPad = (s16*)linearAlloc(GR_MAX))) { memset(gGrPad, 0, GR_MAX); DSP_FlushDataCache(gGrPad, GR_MAX); }
	if (gGrPad) {   // start the second stream half a grain late so the two overlap evenly
		memset(&gGrPadWB, 0, sizeof(gGrPadWB)); gGrPadWB.data_vaddr = gGrPad; gGrPadWB.nsamples = (int)(512 + gGrainSize * 7000) / 2;
		ndspChnSetRate(GR_CH0 + 1, gGrSrcRate); ndspChnWaveBufAdd(GR_CH0 + 1, &gGrPadWB);
	}
	if (gGrainSrc == 2) say("GRAINS: GRABBED THE LAST SECOND OF MIC (PICK MIC AGAIN TO REGRAB)");
}

// ---- GRAIN EDITOR: HARDWARE page with GRAINS on, in place of the sensor view. The source waveform, the grain
// (its length, where it is now) and the spray zone around it. Drag on it: left / right = position (held while
// your finger is down, SCAN carries on from there when you let go), up / down = GRAIN SIZE.
#define GE_X 4
#define GE_W 312
#define GE_Y (XY_Y + 12)
#define GE_H 62
static void drawGrainEd(void) {
	rect(0, TAB_H, 320, WORK_B - TAB_H, RGB3(TH->bg));
	char s[48]; int len = (int)(512 + gGrainSize * 7000); if (len > GR_MAX) len = GR_MAX;
	snprintf(s, sizeof(s), "GRAINS %s  SOURCE %s", GRAIN_MODES[gGrains], GRAIN_SRCS[gGrainSrc]); text(4, XY_Y, s, 1, RGB3(TH->val));
	snprintf(s, sizeof(s), "%dMS", (int)(len * 1000.0f / (gGrSrcRate > 0 ? gGrSrcRate : 32728))); text(316 - (int)strlen(s) * 8, XY_Y, s, 1, RGB3(TH->txt));
	rect(GE_X, GE_Y, GE_W, GE_H, RGB3(TH->kbBg)); outline(GE_X - 1, GE_Y - 1, GE_W + 2, GE_H + 2, RGB3(TH->line));
	int L = gGrSrcLen;
	if (!gGrSrcBuf || L <= 64) { text(GE_X + 8, GE_Y + GE_H / 2 - 4, gGrainSrc == 0 ? "RECORD A TAPE TRACK FIRST" : "NO SOURCE YET", 1, RGB3(TH->dim)); return; }
	static u8 pk[GE_W]; static int pkFor = -1;   // waveform peaks, worked out once per source, scaled to fill the box
	if (pkFor != gGrBuildN) { pkFor = gGrBuildN; static int raw[GE_W]; int top = 1;
		for (int x = 0; x < GE_W; x++) { int a = (int)((long long)x * L / GE_W), b = (int)((long long)(x + 1) * L / GE_W), m = 0;
			for (int i = a; i < b; i += 4) { int v = abs(gGrSrcBuf[i]); if (v > m) m = v; }
			raw[x] = m; if (m > top) top = m; }
		for (int x = 0; x < GE_W; x++) pk[x] = (u8)((long long)raw[x] * (GE_H / 2 - 3) / top); }
	float pos = gGrPos / L, w = (float)len / L, sp = gGrainSpray * gGrSrcRate / (float)L;
	int cx = GE_X + (int)(pos * GE_W), sw = (int)(sp * GE_W), gw = (int)(w * GE_W); if (gw < 2) gw = 2;
	for (int x = cx - sw; x < cx + sw + gw; x++) { int xx = GE_X + ((x - GE_X) % GE_W + GE_W) % GE_W; rect(xx, GE_Y, 1, GE_H, RGB3(TH->grid)); }   // spray zone (wraps)
	int my = GE_Y + GE_H / 2;
	for (int x = 0; x < GE_W; x++) rect(GE_X + x, my - pk[x], 1, pk[x] * 2 + 1, RGB3(TH->dim));
	for (int x = cx; x < cx + gw; x++) { int xx = GE_X + ((x - GE_X) % GE_W + GE_W) % GE_W; rect(xx, my - pk[xx - GE_X], 1, pk[xx - GE_X] * 2 + 1, RGB3(TH->val)); rect(xx, GE_Y, 1, 2, RGB3(TH->val)); rect(xx, GE_Y + GE_H - 2, 1, 2, RGB3(TH->val)); }   // the grain
	rect(cx, GE_Y, 1, GE_H, gGrHold ? WARN : RGB3(TH->hi));
	const char* sc = gGrainScan > 0.51f ? "FORWARD" : gGrainScan < 0.49f ? "BACKWARD" : "HOLD";
	snprintf(s, sizeof(s), "SCAN %s  SPRAY %d%%", sc, (int)lrintf(gGrainSpray * 100)); text(4, GE_Y + GE_H + 4, s, 1, RGB3(TH->dim));
	text(316 - 13 * 8, GE_Y + GE_H + 4, "<> POS ^ SIZE", 1, RGB3(TH->dim));
}
static int grainEdTouch(int x, int y, int press, int release) {   // 1 = it changed something
	static int y0; static float s0;
	if (release) { if (gGrHold) { gGrHold = 0; return 1; } return 0; }
	if (gGrSrcLen <= 64) return 0;
	if (press) { if (y < GE_Y - 6 || y > GE_Y + GE_H + 6) return 0; y0 = y; s0 = gGrainSize; gGrHold = 1; }
	if (!gGrHold) return 0;
	float f = (x - GE_X) / (float)GE_W; if (f < 0) f = 0; if (f > 0.999f) f = 0.999f;
	gGrPos = f * gGrSrcLen;
	float sz = s0 + (y0 - y) / (float)GE_H; gGrainSize = sz < 0 ? 0 : sz > 1 ? 1 : sz;
	return 1;
}

// ---- camera: brightness / colour mod sources, and photo -> wavetable (each image row is a frame)
static int gCamOn = 0, gCamOK = 0, gCamWant = 0, gCamFrames = 0;
static Handle gCamEv = 0;
static u16* gCamBuf = NULL; static u16* gCamImg = NULL;
static u32 gCamXfer = 0;
static void camStop(void) {
	if (!gCamOK) return;
	CAMU_StopCapture(PORT_CAM1); CAMU_Activate(SELECT_NONE);
	if (gCamEv) { svcCloseHandle(gCamEv); gCamEv = 0; }
	camExit(); gCamOK = 0;
}
static void camStart(void) {
	if (gCamOK) return;
	if (!gCamBuf) { gCamBuf = (u16*)malloc(160 * 120 * 2); gCamImg = (u16*)malloc(160 * 120 * 2); }
	if (!gCamBuf || !gCamImg || R_FAILED(camInit())) { say("CAMERA NOT AVAILABLE"); gCamOn = 0; gCamWant = 0; gCamVid = 0; return; }
	CAMU_SetSize(SELECT_OUT1, SIZE_QQVGA, CONTEXT_A);
	CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetNoiseFilter(SELECT_OUT1, true); CAMU_SetAutoExposure(SELECT_OUT1, true); CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	CAMU_GetMaxBytes(&gCamXfer, 160, 120); CAMU_SetTransferBytes(PORT_CAM1, gCamXfer, 160, 120);
	CAMU_Activate(SELECT_OUT1);
	CAMU_ClearBuffer(PORT_CAM1); CAMU_StartCapture(PORT_CAM1);
	CAMU_SetReceiving(&gCamEv, gCamBuf, PORT_CAM1, 160 * 120 * 2, (s16)gCamXfer);
	gCamOK = 1; gCamFrames = 0;
}

// ---- video wavetables: LIVE turns every camera picture into OSC A's table as it changes (rows -> frames);
// RECORD CLIP grabs 64 moments over ~8 s, one waveform each, so WT POS (or WT SCAN) plays through time
static int gCamVid = 0, gVidN = 0; static float gWtScan = 0;
static const char* VID_NAMES[3] = {"OFF", "LIVE", "RECORD"};
static float* gVidFr = NULL; static float gVidT = 0;
static float camLum(u16 p) { return 0.3f * (p >> 11) / 31.0f + 0.59f * ((p >> 5) & 63) / 63.0f + 0.11f * (p & 31) / 31.0f; }
static void vidInstallLive(const float* fr) {   // into OSC A's working table, no file
	LightLock_Lock(&gMipsLock);
	Table* tb = &gOsc[0].tb; memcpy(tb->frames, fr, sizeof(float) * CYC * WT_FR); tb->nFrames = WT_FR;
	float pk = 1e-6f; for (int i = 0; i < WT_FR * CYC; i += 4) { float v = fabsf(fr[i]); if (v > pk) pk = v; }
	tb->peak = pk; snprintf(tb->name, sizeof(tb->name), "CAM LIVE");
	LightLock_Unlock(&gMipsLock);
	gForceBuild |= 1;
}
static void vidFrame(void) {   // camTick: a new picture is in gCamImg
	static float row[160];
	if (!gVidFr && !(gVidFr = (float*)malloc(sizeof(float) * CYC * WT_FR))) { gCamVid = 0; say("NOT ENOUGH MEMORY FOR VIDEO"); return; }
	if (gCamVid == 1) {
		if (gVidT < 0.12f) return;   // ~8 table updates a second
		gVidT = 0;
		for (int f = 0; f < WT_FR; f++) {
			int y = f * 119 / (WT_FR - 1);
			for (int x = 0; x < 160; x++) row[x] = camLum(gCamImg[y * 160 + x]);
			for (int pass = 0; pass < 2; pass++) for (int x = 1; x < 159; x++) row[x] = (row[x - 1] + 2 * row[x] + row[x + 1]) * 0.25f;
			wtResample(row, 160, gVidFr + f * CYC);
		}
		wtFinish(gVidFr, WT_FR); vidInstallLive(gVidFr);
	} else if (gCamVid == 2) {
		if (gVidT < 0.125f) return;   // 64 frames over 8 s
		gVidT = 0;
		for (int x = 0; x < 160; x++) { float a = 0; for (int y = 30; y < 90; y += 2) a += camLum(gCamImg[y * 160 + x]); row[x] = a / 30; }
		for (int pass = 0; pass < 3; pass++) for (int x = 1; x < 159; x++) row[x] = (row[x - 1] + 2 * row[x] + row[x + 1]) * 0.25f;
		wtResample(row, 160, gVidFr + gVidN * CYC);
		gVidN++;
		char m[40]; snprintf(m, sizeof(m), "RECORDING VIDEO %d / %d", gVidN, WT_FR); say(m);
		if (gVidN >= WT_FR) {
			wtFinish(gVidFr, WT_FR); wtInstall("VID", gVidFr);
			gCamVid = 0; gVidN = 0; if (gWtScan <= 0) gWtScan = 1;
			say("VIDEO TABLE SAVED: WT SCAN PLAYS IT BACK");
		}
	}
}
static void vidChanged(void) {
	gVidN = 0; gVidT = 1;
	if (gCamVid == 0) {   // back to OSC A's chosen table
		LightLock_Lock(&gMipsLock); tableSelect(&gOsc[0].tb, gOsc[0].table); LightLock_Unlock(&gMipsLock); gForceBuild |= 1;
	} else if (gCamVid == 2) say("POINT THE CAMERA: RECORDING STARTS NOW");
}

// ---- additive editor (bottom screen): 24 harmonic bars you draw with the stylus. The table it makes sweeps from
// the fundamental alone (WT POS 0) to the full spectrum (100%), so WT POS works like a brightness / build control.
static int gAddEdit = 0, gAddDirty = 0; static float gAddH[24] = {1, 0.5f, 0.33f, 0.25f, 0.2f, 0.17f, 0.14f, 0.12f, 0.11f, 0.1f, 0.09f, 0.08f, 0.08f, 0.07f, 0.07f, 0.06f, 0.06f, 0.05f, 0.05f, 0.05f, 0.05f, 0.04f, 0.04f, 0.04f};
static float* gAddFr = NULL;
static void addBuild(void) {
	if (!gAddFr && !(gAddFr = (float*)malloc(sizeof(float) * CYC * WT_FR))) return;
	static float sn[CYC]; static int init = 0; if (!init) { for (int i = 0; i < CYC; i++) sn[i] = sinf(TWO_PI * i / CYC); init = 1; }
	for (int f = 0; f < WT_FR; f++) {
		float* o = gAddFr + f * CYC; memset(o, 0, sizeof(float) * CYC);
		float reveal = f / (float)(WT_FR - 1) * 24;
		for (int h = 0; h < 24; h++) {
			float g = reveal - h + 1; if (g <= 0) continue; if (g > 1) g = 1;
			float a = gAddH[h] * g; if (a < 0.002f) continue;
			for (int i = 0; i < CYC; i++) o[i] += a * sn[(i * (h + 1)) & (CYC - 1)];
		}
	}
	wtFinish(gAddFr, WT_FR);
}
static void addApply(void) { addBuild(); if (gAddFr) { vidInstallLive(gAddFr); snprintf(gOsc[0].tb.name, sizeof(gOsc[0].tb.name), "ADDITIVE"); } }
static void drawAdd(void) {
	rect(0, XY_Y, 320, WORK_B - XY_Y, RGB3(TH->bg));
	text(4, XY_Y + 2, "DRAW HARMONICS 1-24  (OSC A, LIVE)", 1, DIM);
	for (int h = 0; h < 24; h++) {
		int x = 4 + h * 13, top = XY_Y + 14, hgt = WORK_B - 4 - top, v = (int)(gAddH[h] * hgt);
		rect(x, top, 11, hgt, RGB3(TH->pad));
		if (v > 0) rect(x, top + hgt - v, 11, v, (h & 3) == 0 ? TH->val[0] : TH->line[0], (h & 3) == 0 ? TH->val[1] : TH->line[1], (h & 3) == 0 ? TH->val[2] : TH->line[2]);
	}
}
static void addTouch(int x, int y) {
	int h = (x - 4) / 13; if (h < 0 || h > 23) return;
	int top = XY_Y + 14, hgt = WORK_B - 4 - top; float v = 1 - (y - top) / (float)hgt; if (v < 0) v = 0; if (v > 1) v = 1;
	if (v < 0.03f) v = 0;
	gAddH[h] = v * v;   // squared: finer control of quiet harmonics
	gAddDirty = 1;
}
// RULE > TABLE: an elementary cellular automaton; each generation becomes one frame
static void ruleTable(void) {
	static const int RULES[8] = {30, 90, 110, 150, 45, 73, 105, 54};
	int rule = RULES[rand() % 8]; u8 row[64], nx[64];
	float* fr = (float*)malloc(sizeof(float) * CYC * WT_FR); if (!fr) return;
	for (int i = 0; i < 64; i++) row[i] = rand() % 4 == 0;
	row[32] = 1;
	static float w[64];
	for (int f = 0; f < WT_FR; f++) {
		for (int i = 0; i < 64; i++) w[i] = row[i] ? 1.0f : -1.0f;
		for (int pass = 0; pass < 2; pass++) for (int i = 0; i < 64; i++) w[i] = (w[(i + 63) & 63] + 2 * w[i] + w[(i + 1) & 63]) * 0.25f;
		wtResample(w, 64, fr + f * CYC);
		for (int i = 0; i < 64; i++) { int k = row[(i + 63) & 63] << 2 | row[i] << 1 | row[(i + 1) & 63]; nx[i] = (rule >> k) & 1; }
		memcpy(row, nx, 64);
	}
	wtFinish(fr, WT_FR);
	char nm[16]; snprintf(nm, sizeof(nm), "RULE%d", rule); wtInstall(nm, fr);
	free(fr);
}
static void camToTable(void);
static void camTick(void) {
	int need = gCamOn || gCamWant || gCamVid;
	if (need && !gCamOK) camStart();
	if (!need && gCamOK) camStop();
	if (!gCamOK || !gCamEv) return;
	if (svcWaitSynchronization(gCamEv, 0) != 0) return;   // frame not ready yet: never block
	svcCloseHandle(gCamEv); gCamEv = 0;
	memcpy(gCamImg, gCamBuf, 160 * 120 * 2); gCamFrames++;
	CAMU_SetReceiving(&gCamEv, gCamBuf, PORT_CAM1, 160 * 120 * 2, (s16)gCamXfer);
	float L = 0, C = 0;
	for (int i = 0; i < 160 * 120; i += 7) { u16 p = gCamImg[i]; float r = (p >> 11) / 31.0f, g = ((p >> 5) & 63) / 63.0f, b = (p & 31) / 31.0f; L += 0.3f * r + 0.59f * g + 0.11f * b; C += r - b; }
	int cnt = 160 * 120 / 7 + 1;
	gCamLight += (L / cnt - gCamLight) * 0.4f; gCamColor += (C / cnt - gCamColor) * 0.4f;
	{   // CAM MOTION: how much the picture changed since the last frame (wave a hand in front of it)
		static u8 prevY[160 * 120 / 13 + 1]; static int have = 0; int d = 0, j = 0;
		for (int i = 0; i < 160 * 120; i += 13, j++) { u16 p = gCamImg[i]; int y = ((p >> 11) * 2 + ((p >> 5) & 63) + (p & 31)) ; if (have) d += abs(y - prevY[j]); prevY[j] = (u8)y; }
		float mv = have ? (float)d / j / 14.0f : 0; mv = mv > 1 ? 1 : mv; have = 1;
		gCamMove += (mv - gCamMove) * (mv > gCamMove ? 0.6f : 0.15f);
	}
	if (gCamWant && gCamFrames > 6) { camToTable(); gCamWant = 0; }   // a few frames in, so exposure has settled
	if (gCamVid && gCamFrames > 6) vidFrame();
}
static void camToTable(void) {
	float* fr = (float*)malloc(sizeof(float) * CYC * WT_FR); static float row[160];
	if (!fr) return;
	for (int f = 0; f < WT_FR; f++) {
		int y = f * 119 / (WT_FR - 1);
		for (int x = 0; x < 160; x++) { u16 p = gCamImg[y * 160 + x]; row[x] = 0.3f * (p >> 11) / 31.0f + 0.59f * ((p >> 5) & 63) / 63.0f + 0.11f * (p & 31) / 31.0f; }
		for (int pass = 0; pass < 2; pass++) for (int x = 1; x < 159; x++) row[x] = (row[x - 1] + 2 * row[x] + row[x + 1]) * 0.25f;   // soften
		wtResample(row, 160, fr + f * CYC);
	}
	wtFinish(fr, WT_FR); wtInstall("CAM", fr); free(fr);
}

// ---- link: tempo + sequencer step over local wireless (UDS) or infrared, to another 2DS/3DS running Syrup
static const char* LINK_NAMES[6] = {"OFF", "WIFI HOST", "WIFI JOIN", "IR SEND", "IR RECEIVE", "IR MIDI IN"};
static int gLink = 0, gLinkUp = 0, gLinkPeers = 0;
static float gLinkRx = 0;   // seconds since the last packet
static udsBindContext gUdsBind;
static u32* gIrMem = NULL;
static Thread gIrThread = NULL;
static volatile int gIrQuit = 0, gIrStep = -1, gIrBpm = 0;
typedef struct { u8 magic, step; u16 bpm; } LinkPkt;
#define LINK_COMM 0x53595255
static void irRecvMain(void* a) {
	(void)a;
	while (!gIrQuit) {
		u8 b[2]; u32 n = 0;
		if (R_SUCCEEDED(iruRecvData(b, 2, 0, &n, true)) && n == 2 && (b[0] & 0xF0) == 0xA0) { gIrBpm = 40 + b[1]; gIrStep = b[0] & 15; }
		else svcSleepThread(2000000);
	}
}
// IR MIDI IN: raw MIDI bytes from the Syrup IR-MIDI adapter (an RP2040 on USB, sending IrDA at 115200)
typedef struct { u8 run, cur, need, nd, sysex, d[2]; } MidiIn;   // one raw MIDI input's parser state
static void midiParse(const u8* p, int n, MidiIn* m);
static volatile int gIrMidiBytes = 0;
static void irMidiMain(void* a) {
	(void)a; MidiIn run = {0};
	while (!gIrQuit) {
		u8 b[1]; u32 n = 0;
		if (R_SUCCEEDED(iruRecvData(b, 1, 0, &n, true)) && n == 1) { midiParse(b, 1, &run); gIrMidiBytes++; }
		else svcSleepThread(1000000);
	}
}
static void linkStop(void) {
	if (gLink == 1 || gLink == 2) { if (gLinkUp) { if (gLink == 1) udsDestroyNetwork(); else udsDisconnectNetwork(); udsUnbind(&gUdsBind); } udsExit(); }
	gIrQuit = 1;
	if (gIrMem) { iruExit(); free(gIrMem); gIrMem = NULL; }   // shutting IR down releases a waiting receive
	if (gIrThread) { if (R_SUCCEEDED(threadJoin(gIrThread, 1000000000ULL))) threadFree(gIrThread); gIrThread = NULL; }
	gLinkUp = 0; gLinkPeers = 0;
}
static int gLinkPrev = 0;
static void linkChanged(void) {
	int want = gLink; gLink = gLinkPrev; linkStop(); gLink = want; gLinkPrev = want;
	if (want == 1 || want == 2) {
		if (R_FAILED(udsInit(0x3000, NULL))) { say("LOCAL WIRELESS NOT AVAILABLE"); gLink = gLinkPrev = 0; return; }
		udsNetworkStruct net;
		if (want == 1) {
			udsGenerateDefaultNetworkStruct(&net, LINK_COMM, 0, 4);
			if (R_SUCCEEDED(udsCreateNetwork(&net, "syrup", 5, &gUdsBind, 1, UDS_DEFAULT_RECVBUFSIZE))) { gLinkUp = 1; say("HOSTING - OTHER SYRUPS CAN JOIN"); }
			else say("COULD NOT START HOSTING");
		} else {
			static u8 scan[0x4000]; udsNetworkScanInfo* nets = NULL; size_t total = 0;
			say("SEARCHING...");
			if (R_SUCCEEDED(udsScanBeacons(scan, sizeof(scan), &nets, &total, LINK_COMM, 0, NULL, false)) && total > 0 &&
				R_SUCCEEDED(udsConnectNetwork(&nets[0].network, "syrup", 5, &gUdsBind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, 1, UDS_DEFAULT_RECVBUFSIZE))) { gLinkUp = 1; say("JOINED - FOLLOWING THE HOST"); }
			else say("NO HOST FOUND - SELECT JOIN AGAIN");
			free(nets);
		}
	} else if (want == 3 || want == 4 || want == 5) {
		gIrMem = (u32*)memalign(0x1000, 0x1000);
		if (!gIrMem || R_FAILED(iruInit(gIrMem, 0x1000))) { say("INFRARED NOT AVAILABLE"); free(gIrMem); gIrMem = NULL; gLink = gLinkPrev = 0; return; }
		gLinkUp = 1;
		if (want == 4) { gIrQuit = 0; gIrThread = threadCreate(irRecvMain, NULL, 0x2000, 0x30, -2, false); say("IR: POINT THE CONSOLES AT EACH OTHER"); }
		else if (want == 5) { IRU_SetBitRate(3); gIrQuit = 0; gIrThread = threadCreate(irMidiMain, NULL, 0x2000, 0x2C, -2, false); say("IR MIDI: POINT THE ADAPTER AT THE 2DS"); }
		else say("IR SENDING TEMPO");
	}
}
static void linkFollow(int step, int bpm) {   // receiver: take the tempo, snap the sequencer step
	gLinkRx = 0;
	if (bpm >= 40 && bpm <= 240 && bpm != gBPM) gBPM = bpm;
	if (step >= 0 && gSeqMode == 1 && gSeqPlaying) { gSeqPos = step % (gSeqLen < 1 ? 1 : gSeqLen); gSeqPh = 0; seqFire(); }
}
static void linkTick(float dt) {
	gLinkRx += dt;
	if (!gLinkUp) return;
	static int lastStep = -2; static float hb = 0;
	int stepNow = gSeqPlaying ? gSeqPos : -1, send = 0;
	if (stepNow != lastStep && stepNow >= 0) send = 1;
	if ((hb -= dt) <= 0) { hb = 0.5f; send = 1; }   // tempo heartbeat
	lastStep = stepNow;
	if (gLink == 1 && send) { LinkPkt p = {'S', (u8)(stepNow < 0 ? 255 : stepNow), (u16)gBPM}; udsSendTo(UDS_BROADCAST_NETWORKNODEID, 1, UDS_SENDFLAG_Default, &p, sizeof(p)); }
	if (gLink == 3 && send) { u8 b[2] = {(u8)(0xA0 | (stepNow < 0 ? 15 : (stepNow & 15))), (u8)(gBPM - 40)}; iruSendData(b, 2, false); }
	if (gLink == 2) {
		LinkPkt p; size_t got = 0; u16 src;
		while (R_SUCCEEDED(udsPullPacket(&gUdsBind, &p, sizeof(p), &got, &src)) && got == sizeof(p)) { if (p.magic == 'S') linkFollow(p.step == 255 ? -1 : p.step, p.bpm); got = 0; }
	}
	if (gLink == 4 && gIrBpm) { int st = gIrStep; gIrStep = -1; linkFollow(st == 15 ? -1 : st, gIrBpm); gIrBpm = 0; }
	if (gLink == 1) { udsConnectionStatus cs; if (R_SUCCEEDED(udsGetConnectionStatus(&cs))) gLinkPeers = cs.total_nodes > 0 ? cs.total_nodes - 1 : 0; }
}


// ================================================================ Wi-Fi MIDI
// One network bring-up shared by the downloader, the updater and the MIDI server.
static LightLock gNetLock; static int gNetRef = 0; static u32* gSocMem = NULL;
static int netUp(void) {
	LightLock_Lock(&gNetLock);
	if (gNetRef == 0) {
		gSocMem = (u32*)memalign(0x1000, 0x100000);
		if (!gSocMem || R_FAILED(socInit(gSocMem, 0x100000))) { free(gSocMem); gSocMem = NULL; LightLock_Unlock(&gNetLock); return 0; }
	}
	gNetRef++;
	LightLock_Unlock(&gNetLock);
	return 1;
}
static void netDown(void) {
	LightLock_Lock(&gNetLock);
	if (gNetRef > 0 && --gNetRef == 0) { socExit(); free(gSocMem); gSocMem = NULL; }
	LightLock_Unlock(&gNetLock);
}

// MIDI from the network lands in a ring (network thread writes, main loop reads)
#define MQ 256
static u8 gMq[MQ][3]; static volatile int gMqW = 0, gMqR = 0;
// The audio tick drains gMq (Wi-Fi MIDI, IR, the phone remote) every ~4.9 ms instead of the main loop's 60 Hz; the
// few heavy messages (program change, patch prev/next, new acid pattern) go on to gMainQ for the main loop.
static u8 gMainQ[MQ][3]; static volatile int gMainQW = 0, gMainQR = 0;
static void mainQPush(u8 a, u8 b, u8 c) { int n = (gMainQW + 1) % MQ; if (n == gMainQR) return; gMainQ[gMainQW][0] = a; gMainQ[gMainQW][1] = b; gMainQ[gMainQW][2] = c; __sync_synchronize(); gMainQW = n; }
static void midiPush(u8 a, u8 b, u8 c) { int n = (gMqW + 1) % MQ; if (n == gMqR) return; gMq[gMqW][0] = a; gMq[gMqW][1] = b; gMq[gMqW][2] = c; __sync_synchronize(); gMqW = n; }
static int midiLen(u8 st) {   // data bytes after a status byte
	if (st >= 0xF8) return 0;
	switch (st & 0xF0) { case 0xC0: case 0xD0: return 1; case 0xF0: return st == 0xF2 ? 2 : (st == 0xF1 || st == 0xF3) ? 1 : 0; default: return 2; }
}
// raw MIDI byte stream (IR, WebSocket, raw UDP): a byte-at-a-time state machine, so a message may arrive split across
// packets (IR delivers ONE byte per read: the old parser needed the whole message in one call and dropped every note).
// Running status, real-time bytes (clock, start, stop) anywhere - even inside a message - and sysex skipped.
static void midiByte(MidiIn* m, u8 b) {
	if (b >= 0xF8) { midiPush(b, 0, 0); return; }   // real-time: on its own, doesn't break the message around it
	if (b == 0xF0) { m->sysex = 1; m->cur = 0; m->nd = 0; return; }
	if (b == 0xF7) { m->sysex = 0; return; }
	if (m->sysex) { if (!(b & 0x80)) return; m->sysex = 0; }
	if (b & 0x80) {   // a status byte: starts a message (a half-finished one is dropped)
		int k = midiLen(b); m->nd = 0;
		m->run = b < 0xF0 ? b : 0;   // system common cancels running status
		if (!k) { midiPush(b, 0, 0); m->cur = 0; return; }
		m->cur = b; m->need = (u8)k; return;
	}
	if (!m->cur) { if (!m->run) return; m->cur = m->run; m->need = (u8)midiLen(m->run); m->nd = 0; }   // running status
	m->d[m->nd++] = b;
	if (m->nd >= m->need) { midiPush(m->cur, m->d[0], m->need > 1 ? m->d[1] : 0); m->nd = 0; if (m->cur >= 0xF0) m->cur = 0; }
}
static void midiParse(const u8* p, int n, MidiIn* m) { for (int i = 0; i < n; i++) midiByte(m, p[i]); }

// FM-1 LINK timed packets: "SL" 1, then records of a 4-byte sender time (24 MHz ticks) + one MIDI message. Each
// message is played at its sender time + a learned clock offset + NET BUFFER, from the audio tick: the network's
// wobble becomes one steady delay.
#define TQN 512
static struct { u64 due; u8 st, a, b; } gTq[TQN];
static volatile int gTqW = 0, gTqR = 0;
// the system tick runs at SYSCLOCK_ARM11 (268,111,856 Hz) on every model, with or without the New 3DS speedup;
// convert exactly (the old integer divide by 268 ran 417 ppm fast)
static inline u64 nowUs(void) { u64 t = svcGetSystemTick(); return t / SYSCLOCK_ARM11 * 1000000ULL + t % SYSCLOCK_ARM11 * 1000000ULL / SYSCLOCK_ARM11; }
static void timedParse(const u8* p, int n) {
	static u32 lastStamp; static s64 sTicks; static s64 offset; static u64 lastArr; static int have = 0;
	u64 now = nowUs(); int i = 3;
	while (i + 5 <= n) {
		u32 t = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((u32)p[i + 3] << 24); i += 4;
		u8 st = p[i]; if (!(st & 0x80)) return;
		int k = midiLen(st); if (i + 1 + k > n) return;
		u8 a = k > 0 ? p[i + 1] : 0, b = k > 1 ? p[i + 2] : 0; i += 1 + k;
		s32 d = (s32)(t - lastStamp);
		if (!have || d > 24 * 30000000 || d < -24 * 1000000) { have = 1; sTicks = 0; offset = (s64)now; lastArr = now; d = 0; }
		lastStamp = t; sTicks += d;
		s64 sUs = sTicks / 24, cand = (s64)now - sUs;
		offset += (s64)(now - lastArr) / 10000;   // let it drift up 100 ppm (the two crystals, a slower route)
		if (cand < offset) offset = cand;        // the fastest packet sets the line
		lastArr = now;
		u64 due = (u64)(sUs + offset) + (u64)(gNetLat * 1000.0f);
		int nw = (gTqW + 1) % TQN; if (nw == gTqR) continue;   // full: drop
		gTq[gTqW].due = due; gTq[gTqW].st = st; gTq[gTqW].a = a; gTq[gTqW].b = b; __sync_synchronize(); gTqW = nw;
	}
}

static int gMidiNet = 0; static volatile int gNetRun = 0, gNetClients = 0;
static char gNetAddr[32] = "", gNetPeer[32] = "";
static Thread gNetThread = NULL;

#ifndef SYRUP_NO_NET
// ---- SHA-1 + base64, only for the WebSocket handshake
static void sha1(const u8* m, u32 len, u8 out[20]) {   // short messages only (the handshake key)
	u8 buf[192]; if (len > 119) len = 119;
	memcpy(buf, m, len); buf[len] = 0x80;
	u32 tot = ((len + 8) / 64 + 1) * 64; memset(buf + len + 1, 0, tot - len - 1);
	u64 bits = (u64)len * 8; for (int j = 0; j < 8; j++) buf[tot - 8 + j] = (u8)(bits >> (56 - 8 * j));
	u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	for (u32 o = 0; o < tot; o += 64) {
		const u8* blk = buf + o; u32 w[80];
		for (int t = 0; t < 16; t++) w[t] = (u32)blk[t * 4] << 24 | (u32)blk[t * 4 + 1] << 16 | (u32)blk[t * 4 + 2] << 8 | blk[t * 4 + 3];
		for (int t = 16; t < 80; t++) { u32 x = w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16]; w[t] = x << 1 | x >> 31; }
		u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int t = 0; t < 80; t++) {
			u32 f, kk;
			if (t < 20) { f = (b & c) | (~b & d); kk = 0x5A827999; } else if (t < 40) { f = b ^ c ^ d; kk = 0x6ED9EBA1; }
			else if (t < 60) { f = (b & c) | (b & d) | (c & d); kk = 0x8F1BBCDC; } else { f = b ^ c ^ d; kk = 0xCA62C1D6; }
			u32 tmp = (a << 5 | a >> 27) + f + e + kk + w[t]; e = d; d = c; c = b << 30 | b >> 2; b = a; a = tmp;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	for (int j = 0; j < 5; j++) { out[j * 4] = h[j] >> 24; out[j * 4 + 1] = h[j] >> 16; out[j * 4 + 2] = h[j] >> 8; out[j * 4 + 3] = h[j]; }
}
static void b64(const u8* in, int n, char* out) {
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int o = 0;
	for (int i = 0; i < n; i += 3) {
		u32 v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
		out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63]; out[o++] = i + 1 < n ? T[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? T[v & 63] : '=';
	}
	out[o] = 0;
}
#include "remote_html.h"
static int sendAll(int s, const void* d, int n) { const char* p = (const char*)d; while (n > 0) { int k = send(s, p, n, 0); if (k <= 0) { if (k < 0 && errno == EAGAIN) { svcSleepThread(1000000); continue; } return 0; } p += k; n -= k; } return 1; }
#define NCLI 8
typedef struct { int s, ws; char buf[2048]; int n; MidiIn run; u8 closing, mir; u64 t; FILE* up; int left; } Cli;   // mir: a MIRROR page listening   // t: last activity (ms); up / left: a file being uploaded
static void cliClose(Cli* c) { if (c->mir) { c->mir = 0; if (--gMirClients <= 0) { gMirClients = 0; gMirOn = 0; } } if (c->up) { fclose(c->up); c->up = NULL; } if (c->s >= 0) close(c->s); c->s = -1; c->ws = 0; c->n = 0; c->closing = 0; }
// WEB UPLOAD (WI-FI MIDI on, then http://<the 3DS>:8080/files): the page turns any audio the browser can read into
// 32728 Hz mono WAVs and POSTs them one by one, raw, to /up?d=<folder>&n=<name>; then /done and Syrup rescans.
// d = kits/<NAME> (a SAMPLE kit) or tables (a wavetable WAV for OSC A / B). Names are cleaned; nothing leaves /3ds/syrup.
static volatile int gWebUpDone = 0, gWebUpN = 0; static char gWebUpKit[40] = "";
// MIRROR / STREAM: the 3DS only speaks plain http, and the web Syrup lives on https (a browser won't let an https page
// talk to plain http), so the 3DS hands out this loader: it fetches Syrup for the web (the SD card's copy first, then
// GitHub Pages) and starts it here, on the 3DS's own address, where it can talk to the 3DS.
static int knobsJson(char* j, int cap);
static const char MIRROR_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Syrup mirror</title>"
"<style>body{background:#000;color:#e8c48a;font:14px ui-monospace,Menlo,monospace;padding:20px}</style></head><body><div id='m'>LOADING SYRUP...</div><script>"
"var U=['/syrup.html','https://syrup-synth.github.io/syrup/','https://syrup-synth.github.io/syrup-3ds/'];"
"(async function(){for(var i=0;i<U.length;i++){try{var r=await fetch(U[i]);if(!r.ok)continue;var t=await r.text();if(t.indexOf('SYRUP_MIRROR')<0)continue;"
"document.open();document.write(t);document.close();return}catch(e){}}"
"document.getElementById('m').textContent='COULD NOT LOAD SYRUP. This computer needs the internet the first time (or copy index.html to 3ds/syrup/web/ on the SD card).'})();"
"</script></body></html>";
static const char FILES_HTML[] =
"<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>SYRUP FILES</title>"
"<style>body{background:#000;color:#5ff;font:16px monospace;padding:14px}h1{font-size:22px}input,select,button{font:inherit;background:#033;color:#fff;border:1px solid #5ff;padding:8px;margin:6px 0;width:100%;box-sizing:border-box}#log{white-space:pre-wrap;color:#ccc}</style></head><body>"
"<h1>SYRUP: SEND FILES</h1><label>SEND AS</label><select id=t><option value=kit>DRUM KIT (SAMPLE kit folder)</option><option value=tab>WAVETABLE</option></select>"
"<label>KIT FOLDER NAME</label><input id=k value=WEB maxlength=24><label>FILES (name drums KICK, SNARE, HAT, OPEN HAT, CLAP, RIM, LO TOM, HI TOM, BELL, CRASH)</label>"
"<input type=file id=f multiple accept='audio/*,.wav'><button id=go>SEND TO THE 3DS</button><div id=log></div><script>"
"var L=document.getElementById('log');function log(t){L.textContent+=t+'\\n'}"
"function clean(n){return n.replace(/\\.[^.]*$/,'').replace(/[^A-Za-z0-9 _-]/g,'').slice(0,40)||'file'}"
"function wav(x){var N=x.length,b=new DataView(new ArrayBuffer(44+N*2)),w=function(o,t){for(var i=0;i<t.length;i++)b.setUint8(o+i,t.charCodeAt(i))};"
"w(0,'RIFF');b.setUint32(4,36+N*2,true);w(8,'WAVEfmt ');b.setUint32(16,16,true);b.setUint16(20,1,true);b.setUint16(22,1,true);b.setUint32(24,32728,true);b.setUint32(28,65456,true);b.setUint16(32,2,true);b.setUint16(34,16,true);w(36,'data');b.setUint32(40,N*2,true);"
"for(var i=0;i<N;i++)b.setInt16(44+i*2,Math.max(-1,Math.min(1,x[i]))*32000,true);return b.buffer}"
"async function one(f,kit,ac){var d=kit?'kits/'+clean(document.getElementById('k').value||'WEB'):'tables',body;"
"if(!kit&&/\\.wav$/i.test(f.name))body=await f.arrayBuffer();else{var buf=await ac.decodeAudioData(await f.arrayBuffer()),len=Math.min(Math.ceil(buf.duration*32728),32728*(kit?3:12)),"
"off=new OfflineAudioContext(1,len,32728),s=off.createBufferSource();s.buffer=buf;s.connect(off.destination);s.start();body=wav((await off.startRendering()).getChannelData(0))}"
"var r=await fetch('/up?d='+encodeURIComponent(d)+'&n='+encodeURIComponent(clean(f.name)+'.wav'),{method:'POST',body:body});log((r.ok?'OK  ':'FAIL ')+f.name)}"
"document.getElementById('go').onclick=async function(){var fs=document.getElementById('f').files,kit=document.getElementById('t').value=='kit',ac=new (window.AudioContext||window.webkitAudioContext)();"
"if(!fs.length){log('PICK SOME FILES FIRST');return}for(var i=0;i<fs.length;i++){try{await one(fs[i],kit,ac)}catch(e){log('FAIL '+fs[i].name)}}await fetch('/done',{method:'POST'});log('DONE: '+(kit?'KIT = SAMPLE on the 3DS':'TABLES rescanned'))}"
"</script></body></html>";
static int upArg(const char* q, const char* key, char* out, int n) {   // a query value, %-decoded, only [A-Za-z0-9 _.-/]
	const char* p = strstr(q, key); if (!p) return 0; p += strlen(key); int k = 0;
	while (*p && *p != '&' && *p != ' ' && k < n - 1) {
		int ch = *p++; if (ch == '%' && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) { char h[3] = {p[0], p[1], 0}; ch = (int)strtol(h, NULL, 16); p += 2; } else if (ch == '+') ch = ' ';
		if (isalnum(ch) || ch == ' ' || ch == '_' || ch == '-' || ch == '.' || ch == '/') out[k++] = (char)ch;
	}
	out[k] = 0; return k > 0;
}
static void upMore(Cli* c, const char* b, int k);
static void upBegin(Cli* c) {   // POST /up?d=..&n=..: start writing the body to the SD card
	char d[64], nm[64], path[200]; const char* q = c->buf + 8;
	int len = 0; const char* cl = strstr(c->buf, "Content-Length:"); if (!cl) cl = strstr(c->buf, "content-length:"); if (cl) len = atoi(cl + 15);
	int ok = upArg(q, "d=", d, sizeof d) && upArg(q, "n=", nm, sizeof nm) && !strstr(d, "..") && !strstr(nm, "..") && !strchr(nm, '/') && len > 44 && len < 8 * 1024 * 1024;
	int kit = ok && !strncmp(d, "kits/", 5) && d[5] && !strchr(d + 5, '/'), tab = ok && !strcmp(d, "tables");
	if (!kit && !tab) { static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; sendAll(c->s, bad, sizeof(bad) - 1); shutdown(c->s, SHUT_WR); c->closing = 1; c->n = 0; return; }
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777);
	if (kit) { mkdir("sdmc:/3ds/syrup/kits", 0777); snprintf(path, sizeof path, "sdmc:/3ds/syrup/%s", d); mkdir(path, 0777); snprintf(path, sizeof path, "sdmc:/3ds/syrup/%s/%s", d, nm); snprintf(gWebUpKit, sizeof gWebUpKit, "%s", d + 5); }
	else snprintf(path, sizeof path, "sdmc:/3ds/syrup/%s", nm);
	c->up = fopen(path, "wb"); c->left = len;
	if (!c->up) { static const char bad[] = "HTTP/1.1 500 Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; sendAll(c->s, bad, sizeof(bad) - 1); shutdown(c->s, SHUT_WR); c->closing = 1; c->n = 0; return; }
	char* body = strstr(c->buf, "\r\n\r\n") + 4; int have = c->n - (int)(body - c->buf); if (have > c->left) have = c->left;
	if (have > 0) { fwrite(body, 1, have, c->up); c->left -= have; }
	c->n = 0; upMore(c, NULL, 0);
}
static void upMore(Cli* c, const char* b, int k) {   // more body bytes; at the end: close the file, answer
	if (k > 0) { if (k > c->left) k = c->left; fwrite(b, 1, k, c->up); c->left -= k; }
	if (c->left > 0) return;
	fclose(c->up); c->up = NULL; gWebUpN++;
	static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"; sendAll(c->s, ok, sizeof(ok) - 1);
	shutdown(c->s, SHUT_WR); c->closing = 1; c->n = 0; c->t = osGetTime();
}
static void cliHttp(Cli* c) {   // request complete in c->buf
	c->buf[c->n] = 0;
	char* key = strstr(c->buf, "Sec-WebSocket-Key:");
	if (key) {
		key += 18; while (*key == ' ') key++;
		char k[80]; int i = 0; while (key[i] && key[i] != '\r' && key[i] != '\n' && i < 60) { k[i] = key[i]; i++; } k[i] = 0;
		strcat(k, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
		u8 h[20]; char acc[40]; sha1((const u8*)k, strlen(k), h); b64(h, 20, acc);
		char r[256]; int n = snprintf(r, sizeof(r), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
		if (!sendAll(c->s, r, n)) { cliClose(c); return; }
		c->ws = 1; c->n = 0; gNetClients++;
		if (!strncmp(c->buf, "GET /mirror", 11)) { c->mir = 1; gMirR = gMirW; gMirClients++; gMirOn = 1; gMirNew = 1; }   // a MIRROR: everything from a full snapshot on
		return;
	}
	if (!strncmp(c->buf, "POST /up?", 9)) { upBegin(c); return; }
	if (!strncmp(c->buf, "POST /done", 10)) { gWebUpDone = 1; static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"; sendAll(c->s, ok, sizeof(ok) - 1); }
	else if (!strncmp(c->buf, "GET /files", 10)) {   // WEB UPLOAD: the page for sending samples / tables to the SD card
		char hdr[160]; int len = (int)strlen(FILES_HTML);
		int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", len);
		if (sendAll(c->s, hdr, n)) sendAll(c->s, FILES_HTML, len);
	}
	else if (!strncmp(c->buf, "GET /mirror", 11) || !strncmp(c->buf, "GET /stream", 11)) {   // MIRROR / STREAM: a loader that brings in Syrup for the web
		char hdr[160]; int len = (int)strlen(MIRROR_HTML);
		int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", len);
		if (sendAll(c->s, hdr, n)) sendAll(c->s, MIRROR_HTML, len);
	}
	else if (!strncmp(c->buf, "GET /syrup.html", 15)) {   // the web page itself, from the SD card (3ds/syrup/web/index.html) for use with no internet
		FILE* f = fopen("sdmc:/3ds/syrup/web/index.html", "rb");
		if (!f) { static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; sendAll(c->s, nf, sizeof(nf) - 1); }
		else { fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET); char hdr[160];
			int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", len);
			if (sendAll(c->s, hdr, n)) { static char fb[8192]; size_t k; while ((k = fread(fb, 1, sizeof(fb), f)) > 0) if (!sendAll(c->s, fb, (int)k)) break; }
			fclose(f); }
	}
	else if (!strncmp(c->buf, "GET /knobs", 10)) {   // the phone's KNOBS page: what the 8 knobs turn on the page you're on, and where they are
		static char j[1024]; int o = knobsJson(j, sizeof(j));
		char hdr[180]; int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", o);
		if (sendAll(c->s, hdr, n)) sendAll(c->s, j, o);
	}
	else if (!strncmp(c->buf, "GET /favicon", 12)) {   // browsers ask for an icon: answer quickly with nothing
		static const char nf[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"; sendAll(c->s, nf, sizeof(nf) - 1);
	} else {
		char hdr[160]; int len = (int)strlen(REMOTE_HTML);
		int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", len);
		if (sendAll(c->s, hdr, n)) sendAll(c->s, REMOTE_HTML, len);
	}
	// don't close straight away: closing with data still queued can reset the connection and the browser shows
	// "page failed to load". Half-close, then wait for the browser to hang up (or 3 s).
	shutdown(c->s, SHUT_WR); c->closing = 1; c->n = 0; c->t = osGetTime();
}
static void cliWs(Cli* c) {   // parse as many frames as are complete
	for (;;) {
		u8* p = (u8*)c->buf; if (c->n < 2) return;
		int op = p[0] & 15, masked = p[1] & 0x80; u32 len = p[1] & 127, hl = 2;
		if (len == 126) { if (c->n < 4) return; len = p[2] << 8 | p[3]; hl = 4; }
		else if (len == 127) { cliClose(c); gNetClients--; return; }
		if (masked) hl += 4;
		if (len > sizeof(c->buf) - 16) { cliClose(c); gNetClients--; return; }
		if ((u32)c->n < hl + len) return;
		u8* d = p + hl;
		if (masked) for (u32 i = 0; i < len; i++) d[i] ^= p[hl - 4 + (i & 3)];
		if (op == 8) { u8 cl[2] = {0x88, 0}; sendAll(c->s, cl, 2); cliClose(c); gNetClients--; return; }
		if (op == 9 && len < 126) { u8 pg[2 + 125]; pg[0] = 0x8A; pg[1] = (u8)len; memcpy(pg + 2, d, len); sendAll(c->s, pg, 2 + len); }   // pong, in one write
		if (op == 1 || op == 2) midiParse(d, len, &c->run);
		int used = hl + len; memmove(c->buf, c->buf + used, c->n - used); c->n -= used;
	}
}
// ---- AppleMIDI / RTP-MIDI (Network MIDI on Mac, iOS apps, rtpMIDI on Windows, many DAWs)
static u32 gSsrc = 0x53595255;
static u64 netNow(void) { return osGetTime() * 10; }   // 100 us units
static int gUdData = -1; static volatile int gPeerOk = 0; static struct sockaddr_in gPeerData;   // the session's data socket + who invited us
// MIDI OUT over the same session: an RTP-MIDI packet with one message (no journal)
static void rtpSend(u8 st, u8 a, u8 b) {
	if (!gPeerOk || gUdData < 0) return;
	static u16 seq = 0; u32 ts = (u32)netNow();
	u8 r[16] = {0x80, 0x61, (u8)(seq >> 8), (u8)seq, (u8)(ts >> 24), (u8)(ts >> 16), (u8)(ts >> 8), (u8)ts, (u8)(gSsrc >> 24), (u8)(gSsrc >> 16), (u8)(gSsrc >> 8), (u8)gSsrc, 3, st, a, b};
	seq++; sendto(gUdData, r, 16, 0, (struct sockaddr*)&gPeerData, sizeof(gPeerData));
}
static void amReply(int s, struct sockaddr_in* a, const u8* pk, int n) {
	if (n < 4) return;
	if (pk[2] == 'I' && pk[3] == 'N' && n >= 16) {   // invitation -> accept
		if (s == gUdData) { gPeerData = *a; gPeerOk = 1; }   // the data port's invitation: where MIDI OUT goes
		u8 r[32] = {0xFF, 0xFF, 'O', 'K'}; memcpy(r + 4, pk + 4, 8); r[12] = gSsrc >> 24; r[13] = gSsrc >> 16; r[14] = gSsrc >> 8; r[15] = gSsrc; memcpy(r + 16, "Syrup 2DS", 10);
		sendto(s, r, 26, 0, (struct sockaddr*)a, sizeof(*a));
		if (n > 16) snprintf(gNetPeer, sizeof(gNetPeer), "%.20s", (const char*)pk + 16);
	} else if (pk[2] == 'C' && pk[3] == 'K' && n >= 36) {   // clock sync: answer counts 0 and 1
		u8 r[36]; memcpy(r, pk, 36); r[4] = gSsrc >> 24; r[5] = gSsrc >> 16; r[6] = gSsrc >> 8; r[7] = gSsrc;
		u64 t = netNow(); int cnt = pk[8];
		if (cnt > 1) return;
		int off = cnt == 0 ? 20 : 28; r[8] = cnt + 1;
		for (int j = 0; j < 8; j++) r[off + j] = (u8)(t >> (56 - 8 * j));
		sendto(s, r, 36, 0, (struct sockaddr*)a, sizeof(*a));
	} else if (pk[2] == 'B' && pk[3] == 'Y') { gNetPeer[0] = 0; gPeerOk = 0; }
}
static void rtpMidi(const u8* p, int n) {
	if (n < 13 || (p[0] & 0xC0) != 0x80) return;
	int i = 12; u8 f = p[i]; int len = f & 15; i++;
	if (f & 0x80) { if (i >= n) return; len = len << 8 | p[i]; i++; }
	int end = i + len; if (end > n) end = n;
	u8 run = 0; int first = 1;
	while (i < end) {
		if (!first || (f & 0x20)) { while (i < end && (p[i] & 0x80)) i++; i++; }   // delta time
		first = 0; if (i >= end) break;
		u8 st = p[i];
		if (st == 0xF0 || st == 0xF7) { i++; while (i < end && !(p[i] & 0x80)) i++; if (i < end && (p[i] == 0xF7 || p[i] == 0xF0)) i++; continue; }
		if (st & 0x80) { i++; if (st < 0xF0) run = st; } else st = run;
		if (!st) break;
		int k = midiLen(st); if (i + k > end) break;
		if ((k > 0 && (p[i] & 0x80)) || (k > 1 && (p[i + 1] & 0x80))) break;   // malformed: stop at it
		midiPush(st, k > 0 ? p[i] : 0, k > 1 ? p[i + 1] : 0); i += k;
	}
}
static int udpOpen(int port) {
	int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return -1;
	struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
	if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) { close(s); return -1; }
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	return s;
}
static void netMain(void* arg) {
	(void)arg;
	if (!netUp()) { snprintf(gNetAddr, sizeof(gNetAddr), "NO WI-FI"); gNetRun = 0; return; }
	struct in_addr ip; ip.s_addr = gethostid();
	snprintf(gNetAddr, sizeof(gNetAddr), "%s:8080", inet_ntoa(ip));
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(8080); a.sin_addr.s_addr = INADDR_ANY;
	int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (ls < 0 || bind(ls, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(ls, 4) < 0) { snprintf(gNetAddr, sizeof(gNetAddr), "PORT 8080 BUSY"); if (ls >= 0) close(ls); netDown(); gNetRun = 0; return; }
	fcntl(ls, F_SETFL, fcntl(ls, F_GETFL, 0) | O_NONBLOCK);
	int uc = udpOpen(5004), ud = udpOpen(5005), ur = udpOpen(5008); gUdData = ud;   // 5008: plain MIDI bytes (FM-1 LINK and friends)
	int ub = socket(AF_INET, SOCK_DGRAM, 0); u64 beaconT = 0; MidiIn rawRun = {0};
	if (ub >= 0) { int on = 1; setsockopt(ub, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)); }
	static Cli cl[NCLI]; for (int i = 0; i < NCLI; i++) { cl[i].up = NULL; cl[i].left = 0; cl[i].s = -1; cl[i].ws = 0; cl[i].n = 0; memset(&cl[i].run, 0, sizeof(MidiIn)); cl[i].closing = 0; cl[i].mir = 0; cl[i].t = 0; }
	gNetClients = 0;
	while (gNetRun) {
		int idle = 1;
		int s = accept(ls, NULL, NULL);
		if (s >= 0) {
			fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
			int slot = -1; for (int i = 0; i < NCLI; i++) if (cl[i].s < 0) { slot = i; break; }
			if (slot < 0) {   // full of idle browser pre-connections: drop the oldest that isn't a live remote
				for (int i = 0; i < NCLI; i++) if (!cl[i].ws && (slot < 0 || cl[i].t < cl[slot].t)) slot = i;
				if (slot >= 0) cliClose(&cl[slot]);
			}
			if (slot < 0) close(s); else { cl[slot].s = s; cl[slot].ws = 0; cl[slot].mir = 0; cl[slot].n = 0; memset(&cl[slot].run, 0, sizeof(MidiIn)); cl[slot].closing = 0; cl[slot].t = osGetTime(); }
			idle = 0;
		}
		for (int i = 0; i < NCLI; i++) {
			Cli* c = &cl[i]; if (c->s < 0) continue;
			if (!c->ws && osGetTime() - c->t > (c->closing ? 3000 : 10000)) { cliClose(c); continue; }
			if (c->closing) { char junk[256]; int k = recv(c->s, junk, sizeof(junk), 0); if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) cliClose(c); continue; }
			if (c->up) {   // an upload: the body straight to the file
				char tmp[4096]; int k = recv(c->s, tmp, sizeof(tmp), 0);
				if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { fclose(c->up); c->up = NULL; cliClose(c); continue; }
				if (k > 0) { upMore(c, tmp, k); idle = 0; c->t = osGetTime(); }
				continue;
			}
			int k = recv(c->s, c->buf + c->n, sizeof(c->buf) - 1 - c->n, 0);
			if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { if (c->ws) gNetClients--; cliClose(c); continue; }
			if (k < 0) continue;
			c->n += k; idle = 0; c->t = osGetTime();
			if (c->ws) cliWs(c);
			else { c->buf[c->n] = 0; if (strstr(c->buf, "\r\n\r\n")) cliHttp(c); else if (c->n >= (int)sizeof(c->buf) - 1) cliClose(c); }
		}
		if (gMirClients > 0 && gMirR != gMirW) {   // MIRROR: whole events out, one WebSocket frame to every mirror
			static u8 fr[4 + 16384]; u32 r = gMirR, n = (gMirW - r) & (MIRQ - 1), take = 0; if (n > 16384) n = 16384;
			while (take + 3 <= n) { u32 L = gMirQ[(r + take + 1) & (MIRQ - 1)] | (u32)gMirQ[(r + take + 2) & (MIRQ - 1)] << 8; if (take + 3 + L > n) break; take += 3 + L; }
			if (take) {
				int h = 2; fr[0] = 0x82; if (take < 126) fr[1] = (u8)take; else { fr[1] = 126; fr[2] = (u8)(take >> 8); fr[3] = (u8)take; h = 4; }
				for (u32 i = 0; i < take; i++) fr[h + i] = gMirQ[(r + i) & (MIRQ - 1)];
				__sync_synchronize(); gMirR = (r + take) & (MIRQ - 1);
				for (int i = 0; i < NCLI; i++) if (cl[i].s >= 0 && cl[i].ws && cl[i].mir && !sendAll(cl[i].s, fr, h + (int)take)) { gNetClients--; cliClose(&cl[i]); }
				idle = 0;
			}
		}
		u8 pk[512]; struct sockaddr_in from; socklen_t fl;
		for (int u = 0; u < 2; u++) {
			int us = u ? ud : uc; if (us < 0) continue;
			for (int t = 0; t < 8; t++) {
				fl = sizeof(from); int k = recvfrom(us, pk, sizeof(pk), 0, (struct sockaddr*)&from, &fl);
				if (k <= 0) break;
				idle = 0;
				if (k >= 4 && pk[0] == 0xFF && pk[1] == 0xFF) amReply(us, &from, pk, k);
				else if (u) rtpMidi(pk, k);
			}
		}
		if (ur >= 0) for (int t = 0; t < 16; t++) {
			fl = sizeof(from); int k = recvfrom(ur, pk, sizeof(pk), 0, (struct sockaddr*)&from, &fl);
			if (k <= 0) break;
			idle = 0;
			if (!gNetPeer[0] || strcmp(gNetPeer, "FM-1 LINK")) snprintf(gNetPeer, sizeof(gNetPeer), "FM-1 LINK");
			if (k >= 3 && pk[0] == 'S' && pk[1] == 'L' && pk[2] == 1) timedParse(pk, k); else midiParse(pk, k, &rawRun);
		}
		if (ub >= 0 && osGetTime() - beaconT > 2000) {   // "I'm here": lets an FM-1 LINK find this 2DS without typing an address
			beaconT = osGetTime();
			static const char bc[] = "SYRUP 5008";
			struct sockaddr_in b; memset(&b, 0, sizeof(b)); b.sin_family = AF_INET; b.sin_port = htons(5009);
			b.sin_addr.s_addr = 0xFFFFFFFFu; sendto(ub, bc, sizeof(bc) - 1, 0, (struct sockaddr*)&b, sizeof(b));
			b.sin_addr.s_addr = (u32)gethostid() | htonl(0xFFu); sendto(ub, bc, sizeof(bc) - 1, 0, (struct sockaddr*)&b, sizeof(b));   // x.y.z.255
		}
		if (idle) svcSleepThread(1000000);   // 1 ms: low latency, still mostly asleep
	}
	for (int i = 0; i < NCLI; i++) cliClose(&cl[i]);
	close(ls); if (uc >= 0) close(uc); if (ud >= 0) close(ud); if (ur >= 0) close(ur); if (ub >= 0) close(ub);
	gNetClients = 0; gNetPeer[0] = 0;
	netDown();
}
#endif
static void netMidiChanged(void) {
#ifdef SYRUP_NO_NET
	if (gMidiNet) { say("NO NETWORK IN THIS BUILD"); gMidiNet = 0; }
#else
	if (gMidiNet && !gNetThread) {
		gNetRun = 1; gNetAddr[0] = 0;
		gNetThread = threadCreate(netMain, NULL, 0x8000, 0x30, -2, false);
		if (!gNetThread) { gNetRun = 0; gMidiNet = 0; say("COULD NOT START WI-FI MIDI"); }
	} else if (!gMidiNet && gNetThread) {
		gNetRun = 0; threadJoin(gNetThread, U64_MAX); threadFree(gNetThread); gNetThread = NULL; say("WI-FI MIDI OFF");
	}
#endif
}

// ---- what incoming MIDI does (main loop, inside the voice lock)
static int gMidiHeld[16], gMidiClk = 0; static float gMidiBend = 0, gMidiVel = 0.8f;
static u64 gMidiClkT = 0;
// CHORD mode reaches MIDI too: a key plays the chord (in semitones: the controller's key isn't a scale degree)
static u8 gMidiCh[16][4], gMidiChN[16];
static int midiChord(int n, int* out) {
	int c = gChord < 0 || gChord >= NCHORD ? 0 : gChord;
	if (!c || gVoiceMode >= 3 || ((gVoiceMode == 1 || gVoiceMode == 2) && !gArp)) { out[0] = n; return 1; }
	int k = 0; for (int i = 0; i < 4; i++) if (CHORDS[c][i] >= 0 && n + CHORDS[c][i] < 128) out[k++] = n + CHORDS[c][i];
	return k;
}
static void midiPool(float v) { int pool[64], k = 0; for (int i = 0; i < gMidiHeldN; i++) for (int j = 0; j < gMidiChN[i]; j++) pool[k++] = gMidiCh[i][j]; arpSetPool(pool, k, v); }
static void midiRestrike(void) { if (gArp) midiPool(gMidiVel); else for (int i = 0; i < gMidiHeldN; i++) for (int j = 0; j < gMidiChN[i]; j++) voiceOn(gMidiCh[i][j], gMidiVel); }
static void midiNoteOn(int n, float v) {
	int ch[4], k = midiChord(n, ch);
	if (gMidiHeldN < 16) { gMidiChN[gMidiHeldN] = (u8)k; for (int i = 0; i < k; i++) gMidiCh[gMidiHeldN][i] = (u8)ch[i]; gMidiHeld[gMidiHeldN++] = n; }
	gMidiVel = v;
	if (gArp) midiPool(v); else for (int i = 0; i < k; i++) voiceOn(ch[i], v);
	if (gLiveRec) liveAdd(ch, k, v);   // keys hit together land on one step as a chord
}
static void midiNoteOff(int n) {
	int ch[4], k = 0;
	for (int i = 0; i < gMidiHeldN; i++) if (gMidiHeld[i] == n) {
		k = gMidiChN[i]; for (int j = 0; j < k; j++) ch[j] = gMidiCh[i][j];
		gMidiHeldN--; gMidiHeld[i] = gMidiHeld[gMidiHeldN]; gMidiChN[i] = gMidiChN[gMidiHeldN]; memcpy(gMidiCh[i], gMidiCh[gMidiHeldN], 4); break;
	}
	if (!k) { ch[0] = n; k = 1; }
	if (gLiveRec && gLvN) for (int i = 0; i < gLvN; i++) if (gLvNote[i] == ch[0] - gSeqTr) { liveUp(); break; }
	if (gArp) midiPool(gMidiVel); else for (int i = 0; i < k; i++) voiceOff(ch[i]);
}
// MIDI LEARN (START menu): the control picked on the page you're on follows the next CC that moves. Up to 16
// bindings, any channel, kept on the SD card. A CC already used by Syrup (74, 71, 70, 1, 16-19, 102-104) is taken
// over by a binding. Learning the same CC or control again replaces it.
#define NLEARN 16
static struct { u8 cc, page, idx, used; } gLearnMap[NLEARN];
static const Param* gLearnP = NULL; static int gLearnPage = 0, gLearnIdx = 0, gLearnLoaded = 0;
static const char* LEARN_PATH = "sdmc:/3ds/syrup/midilearn.bin";
static void learnLoad(void) { if (gLearnLoaded) return; gLearnLoaded = 1; FILE* f = safeRead(LEARN_PATH); if (f) { if (fread(gLearnMap, sizeof(gLearnMap), 1, f) != 1) memset(gLearnMap, 0, sizeof(gLearnMap)); fclose(f); } }
static void learnSave(void) { mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777); FILE* f = safeOpen(LEARN_PATH); if (f) safeClose(f, LEARN_PATH, fwrite(gLearnMap, sizeof(gLearnMap), 1, f) == 1); }
static int gLearnSaveDue = 0;
static int learnable(const Param* p) { return p && p->names != PRESS_RIGHT && p->names != TABLE_NAME_DUMMY && p->names != PRESET_DUMMY && (p->type != PT_E || p->dirty == 0); }
static void learnArm(int page, int idx) {
	learnLoad();
	Param* p = page > 0 && idx >= 0 ? &PAGES[page][idx] : NULL;
	if (!learnable(p)) { say("MIDI LEARN: PICK A CONTROL ON A PAGE FIRST (D-PAD), THEN LEARN"); return; }
	gLearnP = p; gLearnPage = page; gLearnIdx = idx;
	char m[56]; snprintf(m, sizeof(m), "MIDI LEARN: MOVE A KNOB FOR %s", p->name); say(m); gMsgT = 6;
}
static int learnApply(u8 cc, u8 val) {   // 1 = the CC was learned or is bound
	if (gLearnP && cc != 120 && cc != 123) {
		int slot = -1;
		for (int i = 0; i < NLEARN; i++) if (gLearnMap[i].used && (gLearnMap[i].cc == cc || (gLearnMap[i].page == gLearnPage && gLearnMap[i].idx == gLearnIdx))) { slot = i; break; }
		if (slot < 0) for (int i = 0; i < NLEARN; i++) if (!gLearnMap[i].used) { slot = i; break; }
		if (slot < 0) slot = 0;
		gLearnMap[slot].cc = cc; gLearnMap[slot].page = (u8)gLearnPage; gLearnMap[slot].idx = (u8)gLearnIdx; gLearnMap[slot].used = 1;
		char m[56]; snprintf(m, sizeof(m), "CC %d -> %s", cc, gLearnP->name); say(m); gMsgT = 4; gLearnP = NULL; gLearnSaveDue = 1;
		return 1;
	}
	int hit = 0;
	for (int i = 0; i < NLEARN; i++) if (gLearnMap[i].used && gLearnMap[i].cc == cc && gLearnMap[i].page < NPAGES) {
		Param* p = &PAGES[gLearnMap[i].page][gLearnMap[i].idx]; if (!learnable(p)) continue;
		float t = val / 127.0f;
		if (p->type == PT_F) *(float*)p->p = p->mn + t * (p->mx - p->mn);
		else if (p->type == PT_I) *(int*)p->p = (int)lroundf(p->mn + t * (p->mx - p->mn));
		else { int n = (int)p->mx - (int)p->mn + 1; int k = (int)(t * n); if (k >= n) k = n - 1; *(int*)p->p = (int)p->mn + k; }
		gEdited = 1; hit = 1;
	}
	return hit;
}
// the phone's KNOBS page: the first 8 settings on the page you're on that a knob can turn (as MIDI LEARN sees them)
static const Param* knobParam(int page, int k) {
	if (page < 0 || page >= NPAGES) return NULL;
	for (int i = 0, n = 0; PAGES[page][i].name; i++) { const Param* p = &PAGES[page][i]; if (!learnable(p) || p->mx <= p->mn) continue; if (n++ == k) return p; }
	return NULL;
}
static float paramNorm(const Param* p) {
	float v = p->type == PT_F ? *(float*)p->p : (float)*(int*)p->p, t = (v - p->mn) / (p->mx - p->mn);
	return t < 0 ? 0 : t > 1 ? 1 : t;
}
static void paramSetNorm(const Param* p, float t) {
	if (p->type == PT_F) *(float*)p->p = p->mn + t * (p->mx - p->mn);
	else if (p->type == PT_I) *(int*)p->p = (int)lroundf(p->mn + t * (p->mx - p->mn));
	else { int n = (int)p->mx - (int)p->mn + 1; int k = (int)(t * n); if (k >= n) k = n - 1; *(int*)p->p = (int)p->mn + k; }
}
static int knobsJson(char* j, int cap) {   // {"page":..,"k":[[name, 0-127 or -1] x 8],"drums":..,"bpm":..}
	int o = snprintf(j, cap, "{\"page\":\"%s\",\"k\":[", PAGE_NAMES[gCurPage < 0 || gCurPage >= NPAGES ? 0 : gCurPage]);
	for (int k = 0; k < 8 && o < cap - 64; k++) { const Param* p = knobParam(gCurPage, k); o += snprintf(j + o, cap - o, "%s[\"%s\",%d]", k ? "," : "", p ? p->name : "", p ? (int)(paramNorm(p) * 127) : -1); }
	o += snprintf(j + o, cap - o, "],\"drums\":%d,\"bpm\":%d}", gDrums, gBPM);
	return o;
}
// the phone's PADS: CC 105 a pad (1 + pad * 3 + band; 0 = let go), 106 slid onto one (MORPH), 107 a STRUM string (1-24; 0 = done),
// 108 the PROGRESSION row (1 next progression, 2 bass, 3-6 its chords, 7 AUTO). Played by the main loop like taps on the pads.
static u8 gRemQ[32][2]; static int gRemW = 0, gRemR = 0;
static void remQPush(u8 a, u8 b) { int n = (gRemW + 1) & 31; if (n == gRemR) return; gRemQ[gRemW][0] = a; gRemQ[gRemW][1] = b; gRemW = n; }
static int padsTouch(int base, int x, int y, int press, float* velOut);
static int remPads(int base, int latch) {   // main loop: 1 = the screens changed
	int ch = 0;
	while (gRemR != gRemW) {
		int a = gRemQ[gRemR][0], v = gRemQ[gRemR][1]; gRemR = (gRemR + 1) & 31; float pv = 0.8f;
		if ((a == 105 || a == 106) && v > 0 && v <= 21) { int i = (v - 1) / 3, band = (v - 1) % 3; padsTouch(base, 2 + i * 45 + 21, PAD_Y + (2 - band) * PAD_H / 3 + PAD_H / 6, a == 105, &pv); }
		else if (a == 105 || (a == 107 && v == 0)) { gStrumZone = -1; if (gLiveRec) liveUp(); if (!latch && gPadSel >= 0 && a == 105) playSet(NULL, 0, 0.8f); }
		else if (a == 107 && v <= 24) padsTouch(base, 2 + (v - 1) * 316 / 24 + 6, STRUM_Y + (240 - STRUM_Y) / 2, gStrumZone < 0, &pv);
		else if (a == 108) { static const int PX[8] = {0, 40, 110, 138, 176, 214, 252, 290}; if (v >= 1 && v <= 7) padsTouch(base, PX[v], PROG_Y + 6, 1, &pv); }
		ch = 1;
	}
	return ch;
}
static int midiDrumPad(int a, int on, int vel, int inTick);
static void acidGen(void);
// one MIDI message: inTick = from the audio tick (only light work there; patch changes etc. go to the main loop)
static int midiApply(u8 st, u8 a, u8 b, int inTick) {
	int ch = 0;
	if (inTick && ((st & 0xF0) == 0xC0 || ((st & 0xF0) == 0xB0 && ((a >= 102 && a <= 108) || gLearnP)))) { mainQPush(st, a, b); return 0; }
	int mc = st & 15;
	if (mc == 9 && ((st & 0xF0) == 0x90 || (st & 0xF0) == 0x80) && midiDrumPad(a, (st & 0xF0) == 0x90 && b, b, inTick)) return 1;   // channel 10: the drum machine's sounds
	switch (st & 0xF0) {
	case 0xD0: {   // channel pressure (aftertouch): MPE - that channel's note; else every note
		float pr = a / 127.0f; int hit = 0;
		if (gMpe && mc > 0) for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].mch == mc) { gV[i].press = pr; hit = 1; }
		if (!hit) gPressAll = pr;
		break; }
	case 0xA0: { float pr = b / 127.0f; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].note == a) gV[i].press = pr; break; }   // poly aftertouch
	case 0x90: if (b) { gMidiChNow = mc; midiNoteOn(a, 0.25f + 0.75f * b / 127.0f); gMidiChNow = -1; break; } /* fall through: velocity 0 = off */
	case 0x80: midiNoteOff(a); break;
	case 0xE0: {
		float bv = (((b << 7) | a) - 8192) / 8192.0f;
		if (gMpe && mc > 0) { for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].mch == mc) gV[i].mbend = bv * gMpeRange; }   // MPE: this note only, +-48 semitones
		else gMidiBend = bv;
		break; }
	case 0xC0: { int c = presetCount(gStyle); if (c > 0) { gPreset = a % c; presetLoad(gStyle, gPreset); ch = 1; } break; }
	case 0xB0: {
		float v = b / 127.0f; ch = 1;
		if (learnApply(a, b)) break;   // learned bindings first
		if (a >= 20 && a <= 27) { const Param* kp = knobParam(gCurPage, a - 20); if (kp) { paramSetNorm(kp, b / 127.0f); gEdited = 1; } break; }   // the phone's KNOBS: this page's settings
		if (a >= 105 && a <= 108) { remQPush(a, b); break; }   // the phone's PADS (the main loop plays them, it knows the octave)
		if (a == 74 && gMpe && mc > 0) { for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].mch == mc) gV[i].mslide = v; break; }   // MPE slide (Y axis): that note's own
		if (a == 74) gSlideAll = v;
		if (a == 74) gCutoff = v; else if (a == 71) gRes = v; else if (a == 70) { gOsc[0].pos = v; }
		else if (a == 1) gMacro[0] = v; else if (a >= 16 && a <= 19) gMacro[a - 16] = v;
		else if (a == 102 && b) { int c = presetCount(gStyle); if (c > 0) { gPreset = (gPreset + c - 1) % c; presetLoad(gStyle, gPreset); } }
		else if (a == 103 && b) { int c = presetCount(gStyle); if (c > 0) { gPreset = (gPreset + 1) % c; presetLoad(gStyle, gPreset); } }
		else if (a == 104 && b) { acidGen(); if (gSeqMode != 1) gSeqMode = 1; }
		else if (a == 123 || a == 120) { allOff(); gMidiHeldN = 0; if (gArp) arpSetPool(NULL, 0, 0.8f); }
		else ch = 0;
		break; }
	case 0xF0:
		if (st == 0xFA || st == 0xFB) { gSeqMode = 1; ch = 1; gMidiClk = 0; }
		else if (st == 0xFC) { gSeqMode = 0; ch = 1; }
		else if (st == 0xF8) {   // follow an external clock: 24 per beat
			if (++gMidiClk % 24 == 0) {
				u64 t = osGetTime();
				if (gMidiClkT) { float bpm = 60000.0f / (float)(t - gMidiClkT); if (bpm > 40 && bpm < 240) { gBPM = (int)(gBPM * 0.7f + bpm * 0.3f + 0.5f); } }
				gMidiClkT = t;
			}
		}
		break;
	}
	return ch;
}
static int tqDrain(int inTick) {   // timed messages that are due (voice lock held)
	int ch = 0; u64 now = nowUs();
	while (gTqR != gTqW && (gTq[gTqR].due <= now || gTq[gTqR].due > now + 2000000)) {
		ch |= midiApply(gTq[gTqR].st, gTq[gTqR].a, gTq[gTqR].b, inTick);
		gTqR = (gTqR + 1) % TQN;
	}
	return ch;
}
static volatile int gTqUi = 0;
static volatile int gTickMidi = 0;   // 1 while the audio tick is draining gMq
static int mqDrain(int inTick) {
	int ch = 0;
	while (gMqR != gMqW) {
		u8 st = gMq[gMqR][0], a = gMq[gMqR][1], b = gMq[gMqR][2]; __sync_synchronize(); gMqR = (gMqR + 1) % MQ;
		ch |= midiApply(st, a, b, inTick);
	}
	return ch;
}
static int midiTick(void) {   // returns 1 when something on screen changed
	int ch = tqDrain(0) | gTqUi; gTqUi = 0;
	if (!gTickMidi) ch |= mqDrain(0);   // no audio tick running (audio off): the main loop takes it
	while (gMainQR != gMainQW) {
		u8 st = gMainQ[gMainQR][0], a = gMainQ[gMainQR][1], b = gMainQ[gMainQR][2]; gMainQR = (gMainQR + 1) % MQ;
		ch |= midiApply(st, a, b, 0);
	}
	return ch;
}

// ---- wavetable downloads over Wi-Fi: the Syrup Pack lives in the GitHub repo (wavetables/ + index.txt)
#define PACK_URL "https://raw.githubusercontent.com/syrup-synth/syrup/main/wavetables/"
static volatile int gDlState = 0, gDlDone = 0, gDlTotal = 0, gDlNew = 0;
static volatile int gUpState = 0;   // updater: 0 idle, 1 checking, 2 downloading, 3 installed, 4 up to date, 5 failed   // state: 0 idle, 1 running, 2 finished, 3 failed
static char gDlErr[40];
static Thread gDlThread = NULL;
#ifndef SYRUP_NO_NET
#include <curl/curl.h>
typedef struct { char* p; size_t n, cap; } MemBuf;
static size_t dlToMem(void* d, size_t sz, size_t nm, void* u) {
	MemBuf* m = (MemBuf*)u; size_t k = sz * nm;
	if (m->n + k + 1 > m->cap) { size_t c = (m->n + k + 1) * 2; char* q = (char*)realloc(m->p, c); if (!q) return 0; m->p = q; m->cap = c; }
	memcpy(m->p + m->n, d, k); m->n += k; m->p[m->n] = 0; return k;
}
static size_t dlToFile(void* d, size_t sz, size_t nm, void* u) { return fwrite(d, sz, nm, (FILE*)u); }
static const char* curlWords(int r) {
	switch (r) {
	case 6: return "NO INTERNET (DNS)"; case 7: return "CAN'T CONNECT"; case 18: return "DOWNLOAD CUT OFF";
	case 23: return "SAVE FAILED"; case 28: return "TIMED OUT"; case 35: return "SECURE LINK FAILED"; case 56: return "CONNECTION DROPPED";
	default: return "NETWORK ERROR";
	}
}
static int httpGet(const char* url, void* sink, int toFile) {
	CURL* c = curl_easy_init(); if (!c) return 0;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Syrup-3DS");
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toFile ? dlToFile : dlToMem);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, sink);
	CURLcode r = curl_easy_perform(c); long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (r != CURLE_OK) snprintf(gDlErr, sizeof(gDlErr), "%s (%d)", curlWords((int)r), (int)r);
	else if (code != 200) snprintf(gDlErr, sizeof(gDlErr), "SERVER SAID %ld", code);
	return r == CURLE_OK && code == 200;
}
static void urlName(char* out, int len, const char* nm) {   // spaces -> %20
	int o = 0; for (; *nm && o < len - 4; nm++) { if (*nm == ' ') { memcpy(out + o, "%20", 3); o += 3; } else out[o++] = *nm; }
	out[o] = 0;
}
static void dlMain(void* arg) {
	(void)arg;
	if (!netUp()) { snprintf(gDlErr, sizeof(gDlErr), "WI-FI NOT AVAILABLE"); gDlState = 3; return; }
	curl_global_init(CURL_GLOBAL_DEFAULT);
	MemBuf idx = {0};
	int ok = httpGet(PACK_URL "index.txt", &idx, 0);
	if (ok && idx.p) {
		mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777);
		int total = 0; for (char* q = idx.p; *q; q++) if (*q == '\n') total++;
		gDlTotal = total;
		char* line = strtok(idx.p, "\r\n");
		while (line) {
			char path[128], part[136], url[256], enc[160];
			snprintf(path, sizeof(path), "sdmc:/3ds/syrup/%s", line);
			FILE* t = fopen(path, "rb");
			if (t) fclose(t);   // already have it
			else {
				snprintf(part, sizeof(part), "%s.part", path);
				urlName(enc, sizeof(enc), line); snprintf(url, sizeof(url), PACK_URL "%s", enc);
				FILE* f = fopen(part, "wb");
				int good = f && httpGet(url, f, 1);
				if (f) fclose(f);
				if (good) { rename(part, path); gDlNew++; } else { remove(part); ok = 0; }
			}
			gDlDone++;
			line = strtok(NULL, "\r\n");
		}
	}
	free(idx.p);
	curl_global_cleanup();
	netDown();
	gDlState = ok ? 2 : 3;
}
#endif
static void dlStart(void) {
#ifdef SYRUP_NO_NET
	say("NO NETWORK IN THIS BUILD");
#else
	if (gDlState == 1 || gUpState == 1 || gUpState == 2) return;
	if (gDlThread) { threadJoin(gDlThread, U64_MAX); threadFree(gDlThread); gDlThread = NULL; }
	gDlState = 1; gDlDone = gDlTotal = gDlNew = 0; gDlErr[0] = 0;
	gDlThread = threadCreate(dlMain, NULL, 0x10000, 0x38, -2, false);
	if (!gDlThread) { gDlState = 3; snprintf(gDlErr, sizeof(gDlErr), "COULD NOT START"); }
#endif
}
static void dlTick(void) {   // progress in the footer; when done, rescan so the new tables show up
	if (gDlState == 1) { char m[48]; snprintf(m, sizeof(m), "DOWNLOADING WAVETABLES %d/%d", gDlDone, gDlTotal); say(m); }
	else if (gDlState == 2 || gDlState == 3) {
		LightLock_Lock(&gMipsLock);
		scanTables(); gTableCount = T_BUILTIN + gNFiles;
		for (int i = 0; i < WCACHE; i++) gWCache[i].idx = -1;
		LightLock_Unlock(&gMipsLock);
		char m[48];
		if (gDlState == 2) snprintf(m, sizeof(m), "GOT %d NEW WAVETABLES - SEE OSC TABLE", gDlNew);
		else snprintf(m, sizeof(m), "DOWNLOAD FAILED: %.26s", gDlErr[0] ? gDlErr : "?");
		say(m); gDlState = 0;
	}
}


// ---- in-app updater: compare the release's version.txt with this build, then download and install the new CIA
#define REL_URL "https://github.com/syrup-synth/syrup/releases/latest/download/"
// where this copy of Syrup was launched from: the Homebrew Launcher passes the .3dsx path as argv[0]
extern int __system_argc; extern char** __system_argv;
static const char* selfPath(void) {
	if (__system_argc > 0 && __system_argv && __system_argv[0] && !strncmp(__system_argv[0], "sdmc:/", 6) && strstr(__system_argv[0], ".3dsx")) return __system_argv[0];
	return "sdmc:/3ds/syrup.3dsx";
}
#define UP_EXPECT "sdmc:/3ds/syrup/update_expected.txt"   // the build an update installed; checked at the next start
static volatile u32 gUpGot = 0, gUpTotal = 0; static volatile int gUpSd = 0;
static char gUpVer[12];
#ifndef SYRUP_NO_NET
typedef struct { Handle cia; FILE* f; FILE* copy; u64 off; Result err; } UpSink;
static size_t upWrite(void* d, size_t sz, size_t nm, void* u) {
	UpSink* k = (UpSink*)u; u32 n = sz * nm, w = 0;
	if (k->f) { w = fwrite(d, 1, n, k->f); }
	else { Result rr = FSFILE_Write(k->cia, &w, k->off, d, n, FS_WRITE_FLUSH); if (R_FAILED(rr)) { k->err = rr; return 0; } if (k->copy) fwrite(d, 1, n, k->copy); }   // the CIA also goes to SD, for FBI
	k->off += w; gUpGot = (u32)k->off; return w;
}
static int upProgress(void* c, curl_off_t dlt, curl_off_t dl, curl_off_t ult, curl_off_t ul) { (void)c; (void)dl; (void)ult; (void)ul; if (dlt > 0) gUpTotal = (u32)dlt; return 0; }
static Result ciaInstallFile(const char* path) {   // AM from a file on the SD card, 256 KB at a time
	FILE* f = fopen(path, "rb"); if (!f) return -1;
	Result rr = amInit(); Handle h = 0;
	if (R_SUCCEEDED(rr)) rr = AM_StartCiaInstall(MEDIATYPE_SD, &h);
	if (R_SUCCEEDED(rr)) {
		static u8 buf[256 * 1024]; u64 off = 0; size_t n;
		fseek(f, 0, SEEK_END); gUpTotal = (u32)ftell(f); fseek(f, 0, SEEK_SET);
		while (R_SUCCEEDED(rr) && (n = fread(buf, 1, sizeof(buf), f)) > 0) { u32 w = 0; rr = FSFILE_Write(h, &w, off, buf, (u32)n, 0); off += n; gUpGot = (u32)off; }
		if (R_SUCCEEDED(rr)) rr = AM_FinishCiaInstall(h); else AM_CancelCIAInstall(h);
	}
	amExit(); fclose(f);
	return rr;
}
static void upMain(void* arg) {
	(void)arg;
	if (!netUp()) { snprintf(gDlErr, sizeof(gDlErr), "WI-FI NOT AVAILABLE"); gUpState = 5; return; }
	curl_global_init(CURL_GLOBAL_DEFAULT);
	MemBuf v = {0};
	int ok = httpGet(REL_URL "version.txt", &v, 0);
	if (ok && v.p) {
		// version.txt: "BUILD SHA" (older releases: just the SHA). A higher build number is newer.
		int rb = 0; char rs[8] = ""; sscanf(v.p, "%d %7s", &rb, rs);
		if (rb > 0) snprintf(gUpVer, sizeof(gUpVer), "BUILD %d", rb); else snprintf(gUpVer, sizeof(gUpVer), "%.7s", v.p);
		if (rb > 0 ? rb <= SYRUP_BUILD : !strncmp(gUpVer, SYRUP_VER, 7)) gUpState = 4;
		else {
			gUpState = 2; gUpGot = gUpTotal = 0;
			int hb = envIsHomebrew();   // .3dsx from the Homebrew Launcher: replace the file; installed CIA: download, then install it
			// CIA: the whole file goes to the SD card first, then AM installs it from there in big blocks - the way FBI does.
			// (Installing straight from the download, the AM handle sat open for the whole transfer and the install failed,
			// leaving only the copy for FBI.) If AM still refuses, the CIA stays on the SD card for FBI and its error shows.
			UpSink k = {0}; ok = 1; gUpGot = gUpTotal = 0;
			char hbTmp[300]; snprintf(hbTmp, sizeof(hbTmp), "%s.part", selfPath());
			const char* tmp = hb ? hbTmp : "sdmc:/cias/syrup.cia.part";
			mkdir("sdmc:/cias", 0777); k.f = fopen(tmp, "wb"); if (!k.f) { snprintf(gDlErr, sizeof(gDlErr), "CAN'T WRITE TO SD"); ok = 0; }
			if (ok) {
				CURL* c = curl_easy_init();
				curl_easy_setopt(c, CURLOPT_URL, hb ? REL_URL "syrup.3dsx" : REL_URL "syrup.cia");
				curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
				curl_easy_setopt(c, CURLOPT_USERAGENT, "Syrup-3DS"); curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L); curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
				curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
				curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, upWrite); curl_easy_setopt(c, CURLOPT_WRITEDATA, &k);
				curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, upProgress); curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
				CURLcode r = curl_easy_perform(c); long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
				curl_easy_cleanup(c);
				ok = r == CURLE_OK && code == 200 && k.off > 1024;
				if (!ok) { if (r != CURLE_OK) snprintf(gDlErr, sizeof(gDlErr), "%s (%d)", curlWords((int)r), (int)r); else snprintf(gDlErr, sizeof(gDlErr), "SERVER SAID %ld", code); }
			}
			const char* fin = hb ? selfPath() : "sdmc:/cias/syrup.cia";
			if (k.f) fclose(k.f);
			if (ok) { remove(fin); rename(tmp, fin); } else remove(tmp);
			if (ok && !hb) {   // install the CIA from the SD card
				gUpState = 6; gUpGot = 0;
				Result rr = ciaInstallFile(fin);
				if (R_FAILED(rr)) { snprintf(gDlErr, sizeof(gDlErr), "%08lX", (unsigned long)rr); gUpSd = 1; }   // stays on SD for FBI
				else remove(fin);
			}
			gUpState = ok ? 3 : 5;
			if (ok) { mkdir("sdmc:/3ds/syrup", 0777); FILE* e = fopen(UP_EXPECT, "w"); if (e) { fprintf(e, "%s\n", gUpVer); fclose(e); } }
		}
	} else gUpState = 5;
	free(v.p);
	curl_global_cleanup(); netDown();
}
#endif
// ---- NET > TABLE: data-bending. Fetch a random web page and play its raw bytes as 64 wavetable frames.
// (The Wi-Fi chip never hands apps raw radio noise, so live network data is the closest thing.)
static float* gNetFr = NULL; static volatile int gNetTab = 0;   // 1 fetching, 2 ready, 3 failed
#ifndef SYRUP_NO_NET
static void netTabMain(void* arg) {
	(void)arg;
	if (!netUp()) { snprintf(gDlErr, sizeof(gDlErr), "WI-FI NOT AVAILABLE"); gNetTab = 3; return; }
	curl_global_init(CURL_GLOBAL_DEFAULT);
	MemBuf b = {0};
	int ok = httpGet("https://en.wikipedia.org/wiki/Special:Random", &b, 0);
	if (ok && b.p && b.n > 8192) {
		if (!gNetFr) gNetFr = (float*)malloc(sizeof(float) * CYC * WT_FR);
		if (gNetFr) {
			size_t start = b.n / 8, span = b.n - start, seg = span / WT_FR; if (seg > 4096) seg = 4096; if (seg < 64) seg = 64;
			static float tmp[4096];
			for (int f = 0; f < WT_FR; f++) {
				size_t o = start + (span - seg) * f / (WT_FR - 1);
				for (size_t i = 0; i < seg; i++) tmp[i] = ((u8)b.p[o + i] - 128) / 128.0f;
				for (size_t i = 1; i + 1 < seg; i++) tmp[i] = (tmp[i - 1] + 2 * tmp[i] + tmp[i + 1]) * 0.25f;   // take the worst fizz off
				wtResample(tmp, (int)seg, gNetFr + f * CYC);
			}
			wtFinish(gNetFr, WT_FR); gNetTab = 2;
		} else { snprintf(gDlErr, sizeof(gDlErr), "OUT OF MEMORY"); gNetTab = 3; }
	} else { if (ok) snprintf(gDlErr, sizeof(gDlErr), "PAGE TOO SMALL"); gNetTab = 3; }
	free(b.p);
	curl_global_cleanup(); netDown();
}
#endif
static void netTabStart(void) {
#ifdef SYRUP_NO_NET
	say("NO NETWORK IN THIS BUILD");
#else
	if (gDlState == 1 || gUpState == 1 || gUpState == 2 || gNetTab == 1) return;
	if (gDlThread) { threadJoin(gDlThread, U64_MAX); threadFree(gDlThread); gDlThread = NULL; }
	gNetTab = 1; gDlErr[0] = 0; say("GRABBING BYTES OFF THE INTERNET...");
	gDlThread = threadCreate(netTabMain, NULL, 0x10000, 0x38, -2, false);
	if (!gDlThread) { gNetTab = 0; say("COULD NOT START"); }
#endif
}
static void netTabTick(void) {
	if (gNetTab == 2) { gNetTab = 0; wtInstall("NET", gNetFr); }
	else if (gNetTab == 3) { gNetTab = 0; char m[48]; snprintf(m, sizeof(m), "NET > TABLE FAILED: %.24s", gDlErr); say(m); }
}
static void upCheckTook(void) {   // at start: did the last update actually load?
	FILE* e = fopen(UP_EXPECT, "r"); if (!e) return;
	char v[16] = ""; if (!fgets(v, sizeof(v), e)) v[0] = 0; fclose(e);
	int eb = 0; if (sscanf(v, "BUILD %d", &eb) != 1) eb = 0;
	if (eb ? eb <= SYRUP_BUILD : !strncmp(v, SYRUP_VER, 7)) { remove(UP_EXPECT); return; }   // it did
	v[strcspn(v, "\r\n")] = 0;
	static char m[60];
	if (envIsHomebrew()) snprintf(m, sizeof(m), "UPDATE %.9s DIDN'T LOAD: OPEN SYRUP FROM HOME MENU?", v);
	else snprintf(m, sizeof(m), "UPDATE %.9s DIDN'T LOAD: FBI > SD > cias > syrup.cia", v);
	say(m); gMsgT = 20; remove(UP_EXPECT);
}
static void upStart(void) {
#ifdef SYRUP_NO_NET
	say("NO NETWORK IN THIS BUILD");
#else
	if (gDlState == 1 || gUpState == 1 || gUpState == 2) return;   // one network job at a time
	if (gDlThread) { threadJoin(gDlThread, U64_MAX); threadFree(gDlThread); gDlThread = NULL; }
	gUpState = 1; gDlErr[0] = 0;
	gDlThread = threadCreate(upMain, NULL, 0x10000, 0x38, -2, false);
	if (!gDlThread) { gUpState = 5; snprintf(gDlErr, sizeof(gDlErr), "COULD NOT START"); }
#endif
}
static void upTick(void) {
	char m[48];
	switch (gUpState) {
	case 1: say("CHECKING FOR AN UPDATE..."); break;
	case 2: if (gUpTotal) snprintf(m, sizeof(m), "DOWNLOADING %s  %d%%", gUpVer, (int)(gUpGot * 100.0f / gUpTotal)); else snprintf(m, sizeof(m), "DOWNLOADING %s  %dK", gUpVer, (int)(gUpGot / 1024)); say(m); break;
	case 6: if (gUpTotal) snprintf(m, sizeof(m), "INSTALLING %s  %d%%", gUpVer, (int)(gUpGot * 100.0f / gUpTotal)); else snprintf(m, sizeof(m), "INSTALLING %s", gUpVer); say(m); break;
	case 3: if (gUpSd) { snprintf(m, sizeof(m), "AM SAID %.8s: FBI > cias > syrup.cia", gDlErr); gMsgT = 30; } else if (envIsHomebrew()) snprintf(m, sizeof(m), "UPDATED %.20s - CLOSE + REOPEN", strrchr(selfPath(), '/') ? strrchr(selfPath(), '/') + 1 : selfPath()); else snprintf(m, sizeof(m), "UPDATED TO %s - CLOSE AND REOPEN SYRUP", gUpVer); say(m); gMsgT = 30; gUpState = 0; gUpSd = 0; break;
	case 4: snprintf(m, sizeof(m), "UP TO DATE (BUILD %.7s)", SYRUP_VER); say(m); gUpState = 0; break;
	case 5: snprintf(m, sizeof(m), "UPDATE FAILED: %.30s", gDlErr[0] ? gDlErr : "?"); say(m); gMsgT = 30; gUpState = 0; break;
	}
}

static void extrasExit(void) {
	linkStop(); camStop();
	if (gLcdOK) { GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, gBrightTop); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, gBrightBot); gspLcdExit(); }
	HIDUSER_DisableGyroscope();
}

// ---- HARDWARE page left panel (CPU)
static void meter(int x, int y, int w, float v, int bipolar, const char* label) {
	text(x, y, label, 1, DIM);
	int bx = x + 80, bw = w - 80;
	rect(bx, y + 1, bw, 6, RGB3(TH->pad));
	if (bipolar) { int c = bx + bw / 2, e = c + (int)(v * bw / 2); rect(e < c ? e : c, y + 1, abs(e - c) + 1, 6, TXT); rect(c, y, 1, 8, DIM); }
	else rect(bx, y + 1, (int)(v * bw), 6, TXT);
}
static void drawHardwarePanel(void) {
	frame(4, 30, 196, 194);
	char s[48];
	snprintf(s, sizeof(s), "HEADPHONES %s", gHeadphones ? "IN" : "OUT"); text(10, 36, s, 1, TXT);
	if (gLink) { snprintf(s, sizeof(s), "LINK %s %s", LINK_NAMES[gLink], gLinkUp ? (gLink == 1 ? "" : (gLinkRx < 1.5f ? "SYNCED" : "WAITING")) : "OFF"); text(10, 48, s, 1, TXT);
		if (gLink == 1 && gLinkUp) { snprintf(s, sizeof(s), "%d JOINED", gLinkPeers); text(10, 60, s, 1, DIM); } }
	else text(10, 48, "LINK OFF", 1, DIM);
	meter(10, 100, 184, gMicEnv, 0, "BREATH");
	meter(10, 112, 184, gTiltX, 1, "TILT L/R");
	meter(10, 124, 184, gTiltY, 1, "TILT F/B");
	meter(10, 136, 184, gTwist, 1, "TWIST");
	meter(10, 148, 184, gShake, 0, "SHAKE");
	if (gCamOK && gCamImg) {   // camera thumbnail 80x60
		for (int y = 0; y < 60; y++) for (int x = 0; x < 80; x++) { u16 p = gCamImg[(y * 2) * 160 + x * 2]; px(112 + x, 160 + y, (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3); }
		meter(10, 164, 96, gCamLight, 0, "LIGHT");
		meter(10, 176, 96, gCamColor, 1, "COLOR");
	} else text(10, 166, gCamOn ? "CAMERA STARTING" : "CAMERA OFF", 1, DIM);
	text(10, 212, "SYRUP V" SYRUP_VERSION "  BUILD " BUILD_STR, 1, DIM);
}

// ================================================================ drums: synthesised kit on 3 CSND channels, pattern presets, tempo-locked
static int gDrums = 0, gKit = 0, gKickPat = 7, gSnarePat = 6, gHatPat = 6, gDrumBuilt = -1;   // the rows play from the grid (MINE); a new card starts with FOUR FLOOR / BACKBEAT / 8THS pasted in
static float gDrumLevel = 0.8f;
// NDRUM 10 (defined at the keyboard): kick snare closed-hat | open-hat clap rim low-tom high-tom cowbell crash (the last 7 for DRUM KIT patches)
#define NDCH 7
static int gDrumCh[NDCH] = {-1, -1, -1, -1, -1, -1, -1}, gDrumStep = -1;
static float gDrumPh = 0;
static s16* gDrumSmp[NDRUM]; static s16* gDrumBack[NDRUM]; static int gDrumLen[NDRUM]; static volatile int gDrumWant = -1;   // kits build into the back set, then swap: hits never read a half-built drum static int gDrumPoly = 1;   // DRUM POLY
static const char* KIT_NAMES[NKIT] = {"808", "909", "LOFI", "CHIP", "PERC", "WAVE", "SAMPLE"};
static const char* KICKP_NAMES[9] = {"OFF", "FOUR FLOOR", "BREAK", "TRAP", "DNB", "HALFTIME", "BOOM BAP", "MINE", "EUCLID"};
static const char* SNAREP_NAMES[8] = {"OFF", "BACKBEAT", "BREAK", "TRAP", "DNB", "CLAP 2+4", "MINE", "EUCLID"};
static const char* HATP_NAMES[8] = {"OFF", "8THS", "16THS", "OFFBEAT", "TRAP ROLL", "SHUFFLE", "MINE", "EUCLID"};
static const u16 KICKP[8] = {0, 0x8888, 0x8224, 0x8022, 0x8020, 0x8000, 0x8124, 0};
static const u16 SNAREP[7] = {0, 0x0808, 0x0849, 0x0080, 0x0808, 0x0808, 0};
static const u16 HATP[7] = {0, 0xAAAA, 0xFFFF, 0x2222, 0xFDEF, 0x9249, 0};
// MINE: your own pattern, a velocity (0 = rest) per step per drum, recorded by tapping on the mic
static u8 gMyDrum[3][DSTEP];
// Per step and per row on the DRUMS grid (all 10 drums, row = drum):
// RATCHET 1-4 hits in the step, a CONDITION (Elektron-style trig conditions), and each row's own RATE and LENGTH
// (PICO's PAD TIME: polymeter and half / double / dotted time). DRUM VOICE: each drum's own sound settings.
static const char* COND_NAMES[NCOND] = {"ALWAYS", "90%", "75%", "50%", "25%", "10%", "1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4", "4:4", "FILL", "!FILL", "PRE", "!PRE", "NEI", "!NEI", "1ST", "!1ST"};
#define NDRATE 7
static const char* DRATE_NAMES[NDRATE] = {"X1", "1/2", "X2", "3/4", "3/2", "1/4", "1/8"};
static const float DRATE[NDRATE] = {1, 0.5f, 2, 0.75f, 1.5f, 0.25f, 0.125f};
static u8 gDrRat[10][DSTEP], gDrCond[10][DSTEP], gDrRate[10], gDrLen[10] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
typedef struct { s8 tune, penv; u8 pdec, decay, tone, drive, level, pan; } DVoice;   // tune / penv semitones; pdec ms/2; decay, level %; tone, pan 100 = centre
static const DVoice DV_DEF = {0, 0, 20, 100, 100, 0, 100, 100};
static DVoice gDV[10] = {{0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100},
	{0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100}, {0, 0, 20, 100, 100, 0, 100, 100}};
static int gDrFill = 0;   // FILL held
static int gDrIdx[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // each row's step now (its own length and rate)
static u8 gDrumX[7][DSTEP];   // the rest of the kit on the DRUMS grid: OPEN HAT, CLAP, RIM, LO TOM, HI TOM, BELL, CRASH (drums 3-9), a velocity per step
// EUCLID: HITS spread as evenly as possible over STEPS, turned by ROTATE. Rows shorter than 16 run polymetric.
static int gEuHits[3] = {5, 2, 11}, gEuRot[3] = {0, 4, 0}, gEuLen = 16, gEuPos = 0, gEuIdx = -1;
static const int EU_IDX[3] = {8, 7, 7};
static int euOn(int r, int i) {   // Bresenham form of Bjorklund's pattern: same spacing, first hit on step 1
	int n = gEuLen, k = gEuHits[r]; if (k <= 0 || n <= 0) return 0; if (k >= n) return 1;
	int j = ((i - gEuRot[r]) % n + n) % n;
	return (j * k) % n < k;
}
static int drumRowOn(int r, int pat, int i) {   // a preset pattern's step, for copying into MINE
	if (pat == EU_IDX[r]) return i < gEuLen && euOn(r, i);
	const u16* pt = r == 0 ? KICKP : r == 1 ? SNAREP : HATP;
	return (pt[pat] & (0x8000 >> i)) != 0;
}
static const int MINE_IDX[3] = {7, 6, 6};
static int* const DPAT[3] = {&gKickPat, &gSnarePat, &gHatPat};
static int gDrumRec = 0;   // 0 off, 1 kick, 2 snare, 3 hat, 4 clear
static const char* DREC_NAMES[4] = {"OFF", "TAP KICK", "TAP SNARE", "TAP HAT"};   // (clearing is on the DRUMS grid: CLR)
static float gTapLevel = 0, gTapHold = 0;
static const u8 HATVEL[16] = {255, 120, 200, 110, 255, 120, 200, 140, 255, 120, 200, 110, 255, 130, 220, 160};
static int gNzCh[2] = {-1, -1}; static float gNzVol[2];   // hardware noise drums: retired (they wedged the sound chip); CHIP kit is now sampled LFSR noise
static int drumReserved(int c) {
	if (gVoiceMode >= 3) { for (int k = 0; k < NDCH; k++) if (c == gDrumCh[k]) return 1; return 0; }   // kit patch: all 7
	return gDrums && (c == gDrumCh[0] || c == gDrumCh[1] || c == gDrumCh[2]);
}
static u32 csSend(float l, float r);

static float wdRead(float pos, float ph) {   // OSC A's wavetable at pos, phase ph (cycles), about -1..1
	Table* tb = &gOsc[0].tb; if (!tb->frames || tb->nFrames < 1) return sinf(TWO_PI * ph);
	if (pos < 0) pos = 0; if (pos > 1) pos = 1;
	float fp = pos * (tb->nFrames - 1); int f0 = (int)fp; if (f0 > tb->nFrames - 2) f0 = tb->nFrames > 1 ? tb->nFrames - 2 : 0;
	float u = tb->nFrames > 1 ? fp - f0 : 0; const float* A = tb->frames + f0 * CYC; const float* B = tb->frames + (tb->nFrames > 1 ? f0 + 1 : f0) * CYC;
	float a = rd(A, ph), b = rd(B, ph), pk = tb->peak > 0.01f ? tb->peak : 1;
	return (a + (b - a) * u) / pk;
}
static unsigned gWaveSig = 0;
static unsigned waveSig(void) { return (unsigned)(uintptr_t)gOsc[0].tb.frames ^ ((unsigned)gOsc[0].tb.nFrames << 20) ^ (unsigned)(gOsc[0].pos * 40); }
static float metal808(float t, float k) {   // the 808's cymbal / hat source: six square oscillators at odd ratios (k scales them)
	static const float F[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
	float s = 0; for (int j = 0; j < 6; j++) s += fmodf(t * F[j] * k, 1.0f) < 0.5f ? 1.0f : -1.0f;
	return s / 6;
}
// DRUM VOICE, baked into the sample after it's synthesised: DECAY time-warps its envelope (shorter or up to twice as
// long), PITCH ENV sweeps the pitch from TUNE + P.ENV down to TUNE over P.DECAY, TONE darkens (< centre) or thins
// (> centre), DRIVE saturates. TUNE, LEVEL and PAN are applied live when it plays.
static float* gDrF = NULL; static float* gDrE = NULL; static int gDrCap = 0;
static int dvBaked(const DVoice* v) { return v->penv || v->decay != 100 || v->tone != 100 || v->drive; }

// ---- SAMPLE kit. A folder of WAVs: which drum each file is comes from its name (KICK, SNARE, HAT, OPEN HAT, CLAP,
// RIM, LO TOM, HI TOM, BELL / COWBELL, CRASH / CYMBAL / RIDE, or BD SD HH OH CP RS LT HT CB CY, or 1..10). Any WAV
// (8 / 16 / 24 / 32-bit, mono or stereo, any rate) is mixed to mono and resampled to 32728 Hz. A drum with no file
// plays the 808 one. Tune, decay, tone, drive, level and pan (the SOUND editor) work on samples too.
static void kitScanDirs(void) {
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777); mkdir("sdmc:/3ds/syrup/kits", 0777); mkdir("sdmc:/3ds/syrup/kits/MY KIT", 0777);
	char keep[32]; snprintf(keep, sizeof(keep), "%s", gNKitDirs && gKitDir < gNKitDirs ? gKitDirs[gKitDir] : "");
	gNKitDirs = 0; DIR* d = opendir("sdmc:/3ds/syrup/kits");
	if (d) { struct dirent* e; while ((e = readdir(d)) && gNKitDirs < 16) { if (e->d_name[0] == '.') continue;
		char pth[160]; snprintf(pth, sizeof(pth), "sdmc:/3ds/syrup/kits/%s", e->d_name); DIR* t = opendir(pth); if (!t) continue; closedir(t);
		snprintf(gKitRaw[gNKitDirs], 64, "%.63s", e->d_name); snprintf(gKitDirs[gNKitDirs], 32, "%.31s", e->d_name); for (char* c = gKitDirs[gNKitDirs]; *c; c++) *c = (char)toupper((unsigned char)*c); gNKitDirs++; }
		closedir(d); }
	for (int i = 0; i < gNKitDirs; i++) for (int j = i + 1; j < gNKitDirs; j++) if (strcmp(gKitDirs[j], gKitDirs[i]) < 0) { char t[64]; memcpy(t, gKitDirs[i], 32); memcpy(gKitDirs[i], gKitDirs[j], 32); memcpy(gKitDirs[j], t, 32); memcpy(t, gKitRaw[i], 64); memcpy(gKitRaw[i], gKitRaw[j], 64); memcpy(gKitRaw[j], t, 64); }
	for (int i = 0; i < gNKitDirs; i++) gKitDirP[i] = gKitDirs[i];
	if (!gNKitDirs) gKitDirP[0] = "(NONE)";
	gKitDir = 0; for (int i = 0; i < gNKitDirs; i++) if (!strcmp(gKitDirs[i], keep)) gKitDir = i;
	for (int pg = 0; pg < NPAGES; pg++) for (Param* p = PAGES[pg]; p->name; p++) if (p->p == &gKitDir) p->mx = (float)(gNKitDirs > 0 ? gNKitDirs - 1 : 0);
}
static int kitFileDrum(const char* fname) {   // which drum a file is for, from its name; -1 = none
	char b[64]; int n = 0; for (const char* c = fname; *c && *c != '.' && n < 63; c++) b[n++] = (char)toupper((unsigned char)*c); b[n] = 0;
	if (n >= 1 && n <= 2 && isdigit((unsigned char)b[0])) { int k = atoi(b); return k >= 1 && k <= 10 ? k - 1 : -1; }
	static const char* EX[NDRUM][3] = {{"BD", "K", NULL}, {"SD", "S", NULL}, {"HH", "CH", "CHH"}, {"OH", "OHH", NULL}, {"CP", NULL, NULL}, {"RS", "RIM", NULL}, {"LT", NULL, NULL}, {"HT", NULL, NULL}, {"CB", NULL, NULL}, {"CY", "CC", NULL}};
	for (int d = 0; d < NDRUM; d++) for (int k = 0; k < 3; k++) if (EX[d][k] && !strcmp(b, EX[d][k])) return d;
	// words, most particular first (OPEN HAT before HAT, LO TOM / HI TOM before TOM)
	if (strstr(b, "OPEN") || strstr(b, "OHAT") || strstr(b, "OPHAT")) return 3;
	if (strstr(b, "CRASH") || strstr(b, "CYMB") || strstr(b, "RIDE")) return 9;
	if (strstr(b, "HAT") || strstr(b, "HIHAT") || strstr(b, "HH")) return 2;
	if (strstr(b, "KICK") || strstr(b, "BASS") || strstr(b, "KIK")) return 0;
	if (strstr(b, "SNARE") || strstr(b, "SNR")) return 1;
	if (strstr(b, "CLAP")) return 4;
	if (strstr(b, "RIM") || strstr(b, "STICK")) return 5;
	if (strstr(b, "LOTOM") || strstr(b, "LO TOM") || strstr(b, "LOW TOM") || strstr(b, "TOM1") || strstr(b, "TOM 1") || strstr(b, "FLOOR")) return 6;
	if (strstr(b, "HITOM") || strstr(b, "HI TOM") || strstr(b, "HIGH TOM") || strstr(b, "TOM2") || strstr(b, "TOM 2")) return 7;
	if (strstr(b, "TOM")) return 6;
	if (strstr(b, "BELL") || strstr(b, "COW")) return 8;
	if (strstr(b, "SNAP") || strstr(b, "SHAK") || strstr(b, "PERC")) return 5;
	return -1;
}
static int wavMono(const char* path, float* out, int cap) {   // any WAV -> mono at 32728 Hz; returns samples
	FILE* f = fopen(path, "rb"); if (!f) return 0;
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	if (len < 44 || len > 24 * 1024 * 1024) { fclose(f); return 0; }
	u8* b = (u8*)malloc(len); if (!b) { fclose(f); return 0; }
	if (fread(b, 1, len, f) != (size_t)len) { free(b); fclose(f); return 0; } fclose(f);
	int fmt = 1, ch = 1, bits = 16; u32 rate = 32728; const u8* data = NULL; u32 dlen = 0;
	if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
		long o = 12;
		while (o + 8 <= len) { u32 cl = rd32(b + o + 4);
			if (!memcmp(b + o, "fmt ", 4)) { fmt = rd16(b + o + 8); ch = rd16(b + o + 10); rate = rd32(b + o + 12); bits = rd16(b + o + 22); if (fmt == 0xFFFE && cl >= 26) fmt = rd16(b + o + 32); }
			else if (!memcmp(b + o, "data", 4)) { data = b + o + 8; dlen = cl; if (o + 8 + (long)dlen > len) dlen = len - o - 8; }
			o += 8 + cl + (cl & 1); }
	}
	int n = 0;
	if (data && ch >= 1 && ch <= 8 && rate >= 4000 && rate <= 192000 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
		int bps = bits / 8 * ch; u32 total = dlen / bps; double step = rate / 32728.0;
		for (n = 0; n < cap; n++) {
			double x = n * step; u32 i0 = (u32)x; if (i0 + 1 >= total) break; float u = (float)(x - i0), v[2];
			for (int k = 0; k < 2; k++) { float acc = 0; const u8* sp = data + (i0 + k) * bps;
				for (int c = 0; c < ch; c++, sp += bits / 8) {
					if (bits == 8) acc += (sp[0] - 128) / 128.0f; else if (bits == 16) acc += (s16)rd16(sp) / 32768.0f;
					else if (bits == 24) acc += ((int)((sp[0] << 8) | (sp[1] << 16) | ((u32)sp[2] << 24)) >> 8) / 8388608.0f;
					else if (fmt == 3) { float fv; memcpy(&fv, sp, 4); acc += fv; } else acc += (int)rd32(sp) / 2147483648.0f; }
				v[k] = acc / ch; }
			out[n] = v[0] * (1 - u) + v[1] * u;
		}
		if (n == cap) for (int i = 0; i < 256 && i < n; i++) out[n - 1 - i] *= i / 256.0f;   // cut short: fade the end
	}
	free(b); return n;
}
static int kitSampleLoad(int d, float* out, int cap) {   // the current SAMPLE folder's file for drum d
	if (!gNKitDirs) return 0;
	char dir[160]; snprintf(dir, sizeof(dir), "sdmc:/3ds/syrup/kits/%s", gKitRaw[gKitDir < gNKitDirs ? gKitDir : 0]);
	DIR* dd = opendir(dir); if (!dd) return 0;
	struct dirent* e; int n = 0;
	while ((e = readdir(dd))) { const char* nm = e->d_name; size_t l = strlen(nm); if (l < 5 || strcasecmp(nm + l - 4, ".wav") || kitFileDrum(nm) != d) continue;
		char pth[256]; snprintf(pth, sizeof(pth), "%s/%s", dir, nm); n = wavMono(pth, out, cap); if (n > 0) break; }
	closedir(dd); return n;
}
static int drumShape(float* x, int n, int L0, const DVoice* dv, float sr, int cap) {   // returns the new length
	int len = n;
	float dm = dv->decay / 100.0f; if (dm < 0.1f) dm = 0.1f;
	if (dv->decay != 100) {
		float e = 0, rel = expf(-1.0f / (0.012f * sr)), pk = 0;
		for (int i = 0; i < n; i++) { float a = fabsf(x[i]); e = a > e ? a : e * rel; gDrE[i] = e; if (e > pk) pk = e; }
		float fl = pk * 1e-4f + 1e-9f;
		len = (int)(L0 * dm); if (len > n) len = n; if (len < 64) len = 64 < n ? 64 : n;
		for (int i = 0; i < len; i++) {
			float src = i / dm; int k = (int)src; float u = src - k, et = k + 1 < n ? gDrE[k] * (1 - u) + gDrE[k + 1] * u : 0;
			float g = et / (gDrE[i] > fl ? gDrE[i] : fl); if (g > 40) g = 40; x[i] *= g;
		}
	}
	if (dv->penv) {   // read faster (higher) at the start, settling to 1
		float tau = (dv->pdec < 1 ? 1 : dv->pdec) * 0.002f; double pos = 0; int j = 0;
		for (; j < cap && pos < len - 1; j++) {
			int k = (int)pos; float u = (float)(pos - k);
			gDrE[j] = x[k] * (1 - u) + x[k + 1] * u;
			pos += powf(2.0f, dv->penv * expf(-(j / sr) / tau) / 12.0f);
		}
		memcpy(x, gDrE, sizeof(float) * j); len = j;
	}
	if (dv->tone != 100) {
		float t = (dv->tone - 100) / 100.0f, lp = 0;
		if (t < 0) { float a = powf(10.0f, t * 2.0f); for (int i = 0; i < len; i++) { lp += (x[i] - lp) * a; x[i] = lp; } }   // darker: one-pole low-pass
		else { float a = 1 - t * 0.97f; for (int i = 0; i < len; i++) { lp += (x[i] - lp) * a; x[i] = (x[i] - lp) * (1 + t); } }   // thinner: take the lows away
	}
	if (dv->drive) { float g = 1 + dv->drive * 0.08f, nrm = 1.0f / tanhf(g); for (int i = 0; i < len; i++) x[i] = tanhf(x[i] * g) * nrm; }
	int f = len < 256 ? len / 4 : 64; for (int i = 0; i < f; i++) x[len - 1 - i] *= i / (float)f;   // no click at the end
	return len;
}
static void drumBuild(int kitArg) {   // synthesise kick / snare / hat into linear memory for CSND
	if (kitArg == KIT_SAMPLE) kitScanDirs();
	int kit = kitArg == KIT_SAMPLE ? 0 : kitArg;   // SAMPLE: the 808 drums under the files (for any drum without one)
	const float sr = 32728;
	static const float MAXS[NDRUM] = {0.6f, 0.3f, 0.12f, 0.5f, 0.35f, 0.1f, 0.5f, 0.4f, 0.35f, 1.4f};
	int len[NDRUM] = {(int)(sr * 0.6f), (int)(sr * 0.3f), (int)(sr * (kit == 1 ? 0.12f : 0.07f)), (int)(sr * (kit == 1 ? 0.5f : 0.4f)), (int)(sr * 0.35f),
		(int)(sr * 0.08f), (int)(sr * 0.5f), (int)(sr * 0.4f), (int)(sr * 0.3f), (int)(sr * (kit == 2 ? 0.9f : 1.4f))};
	static const float PERCLEN[NDRUM] = {0.16f, 0.1f, 0.12f, 0.35f, 0.3f, 0.1f, 0.45f, 0.35f, 0.25f, 0.12f};   // PERC kit
	if (kit == 4) for (int d = 0; d < NDRUM; d++) len[d] = (int)(sr * PERCLEN[d]);
	static const float WAVELEN[NDRUM] = {0.55f, 0.25f, 0.08f, 0.4f, 0.3f, 0.06f, 0.4f, 0.35f, 0.3f, 1.2f};   // WAVE kit
	if (kit == 5) for (int d = 0; d < NDRUM; d++) len[d] = (int)(sr * WAVELEN[d]);
	gWaveSig = waveSig();
	unsigned seed = 1234;
	unsigned lfsr = 0x7FFF; float lph = 0;   // CHIP kit: Game Boy style noise register
	for (int d = 0; d < NDRUM; d++) {
		int cap = (int)(sr * MAXS[d] * 2.1f);   // room for DECAY up to twice as long
		if (!gDrumBack[d]) gDrumBack[d] = (s16*)linearAlloc(cap * 2 + 64);
		if (!gDrumBack[d]) return;
		if (!gDrF) { gDrCap = (int)(sr * 1.4f * 2.1f) + 16; gDrF = (float*)malloc(sizeof(float) * gDrCap); gDrE = (float*)malloc(sizeof(float) * gDrCap); }
		s16* out = gDrumBack[d];
		const DVoice* dvc = &gDV[d]; int bake = gDrF && gDrE && dvBaked(dvc), L0 = len[d];
		int Ls = L0; if (bake) { float m = dvc->decay > 100 ? dvc->decay / 100.0f : 1; if (dvc->penv < 0) m *= 1.6f; Ls = (int)(L0 * m); if (Ls > cap) Ls = cap; }
		float ph = 0, ph2 = 0, lp = 0, bp1 = 0, bp2 = 0, hp = 0, prev = 0;
		for (int i = 0; i < Ls; i++) {
			float t = i / sr, v = 0;
			seed = seed * 1664525u + 1013904223u; float nz = ((seed >> 9) & 0xFFFF) / 32768.0f - 1;
			if (kit == 5) {   // WAVE kit: every drum made of OSC A's wavetable (at its WT POS)
				float P = gOsc[0].pos, rp = ((seed >> 8) & 0xFFFF) / 65536.0f, tn = wdRead(P, rp);   // table noise: the table read at random phases
				hp = tn - prev; prev = tn;
				switch (d) {
				case 0: { float f = 45 + 210 * expf(-t * 32); ph += f / sr; float ps = P + (1 - P) * expf(-t * 22);   // kick: from the bright end of the table down to WT POS
					v = tanhf(wdRead(ps, ph) * 2.6f) * expf(-t * 4.0f) + (t < 0.003f ? sinf(TWO_PI * 1200 * t) * expf(-t * 900) * 0.5f : 0); break; }
				case 1: { ph += 190 * (1 + 0.35f * expf(-t * 45)) / sr; v = wdRead(P, ph) * expf(-t * 20) * 0.55f + hp * expf(-t * 15) * 0.55f; break; }   // snare
				case 2: case 3: case 9: { ph += 6300.0f / sr; v = hp * (0.5f + 0.5f * wdRead(1 - P, ph)) * expf(-t * (d == 2 ? 40 : d == 3 ? 7 : 2.4f)) * (d == 9 ? 0.7f : d == 2 ? 1.6f : 0.9f); if (v > 1) v = 1; if (v < -1) v = -1; break; }   // hats, crash: ring-modded table noise
				case 4: { float tt = t < 0.03f ? fmodf(t, 0.01f) : t - 0.02f; bp1 += (hp - bp1) * 0.5f; v = bp1 * expf(-tt * (t < 0.03f ? 170 : 15)) * 1.6f; break; }   // clap
				case 5: { ph += 1650.0f / sr; v = wdRead(P, ph) * expf(-t * 70) * 0.7f; break; }   // rim
				case 6: case 7: { float f0 = d == 6 ? 105 : 165, f = f0 * (1 + 0.6f * expf(-t * 25)); ph += f / sr; v = tanhf(wdRead(P + (1 - P) * expf(-t * 30), ph) * 1.3f) * expf(-t * 8) * 0.8f; break; }   // toms
				default: { ph += 560.0f / sr; ph2 += 837.0f / sr; { float x = wdRead(P, ph), y2 = wdRead(P, ph2); v = tanhf((x * y2 * 2.2f + x * 0.35f) * 1.4f) * expf(-t * 8) * 0.9f; } break; }   // bell: two table voices, ring-modulated
				}
			} else if (kit == 4) {   // PERC kit: shaker, maracas, cabasa, tambourine, guiro, claves, congas, bongo, woodblock
				hp = nz - prev; prev = nz;
				switch (d) {
				case 0: { float e = t < 0.018f ? t / 0.018f : expf(-(t - 0.018f) * 28); lp += (hp - lp) * 0.7f; v = lp * e * 0.9f; break; }   // shaker: a swish
				case 1: v = hp * expf(-t * 45) * 0.8f; break;   // maracas: a sharp tick of seeds
				case 2: { float e = t < 0.004f ? t / 0.004f : expf(-(t - 0.004f) * 30); v = hp * e * (0.7f + 0.3f * sinf(TWO_PI * 3100 * t)) * 0.8f; break; }   // cabasa: beads on metal
				case 3: { float j = sinf(TWO_PI * 5300 * t) + sinf(TWO_PI * 7100 * t) * 0.8f + sinf(TWO_PI * 9400 * t) * 0.6f;   // tambourine: jingles
					v = (hp * 0.6f + hp * j * 0.35f) * expf(-t * 11) * 0.6f + (t < 0.003f ? nz * 0.3f : 0); break; }
				case 4: { float ph4 = fmodf(t, 0.022f); v = hp * expf(-ph4 * 220) * (t < 0.26f ? 1 : 0) * (1 - t * 2.5f) * 0.9f; break; }   // guiro: a run of scrapes
				case 5: v = sinf(TWO_PI * 2500 * t) * expf(-t * 42) * 0.8f; break;   // claves
				case 6: case 7: {   // congas: tuned skin with a slap on top
					float f0 = d == 6 ? 190 : 300, f = f0 * (1 + 0.15f * expf(-t * 40)); ph += f / sr;
					v = sinf(TWO_PI * ph) * expf(-t * (d == 6 ? 8 : 10)) * 0.85f + (t < 0.004f ? hp * 0.45f : 0); break; }
				case 8: { float f = 420 * (1 + 0.1f * expf(-t * 60)); ph += f / sr; v = sinf(TWO_PI * ph) * expf(-t * 17) * 0.8f + (t < 0.003f ? hp * 0.35f : 0); break; }   // bongo
				default: v = (sinf(TWO_PI * 1050 * t) * 0.7f + sinf(TWO_PI * 1720 * t) * 0.4f) * expf(-t * 48) * 0.8f; break;   // woodblock
				}
			} else if (d == 0) {   // kick: falling sine, a click, kit-shaped decay
				// 808: long low boom, gentle sweep. 909: short punchy thud with a hard noise click. LOFI: a saturated boom-bap thump
				float f0 = kit == 1 ? 56 : (kit == 2 ? 52 : 45), f1 = kit == 3 ? 400 : kit == 1 ? 330 : kit == 2 ? 150 : 115;
				float sw = kit == 1 ? 48 : kit == 0 ? 22 : 35, dec = kit == 0 ? 1.5f : (kit == 1 ? 6.5f : 4.5f);
				float f = f0 + (f1 - f0) * expf(-t * sw);
				ph += f / sr; v = sinf(TWO_PI * ph) * expf(-t * dec);
				if (kit == 1) { hp = nz - prev; prev = nz; if (t < 0.006f) v += hp * 0.7f * (1 - t / 0.006f); v = tanhf(v * 1.6f); }
				else if (t < 0.004f) v += nz * (kit == 0 ? 0.15f : 0.4f);
				if (t < 0.003f && kit != 3) v += sinf(TWO_PI * 1200 * t) * expf(-t * 900) * (kit == 0 ? 0.3f : 0.6f);   // a short 1.2 kHz click: the beater, so it reads on small speakers
				if (kit == 3) v = v > 0 ? 0.8f : -0.8f, v *= floorf(expf(-t * 6) * 15) / 15;
				if (kit == 2) v = tanhf(v * 3.0f) * 0.85f;
			} else if (d == 1) {   // snare: body tone + filtered noise
				lp += (nz - lp) * (kit == 2 ? 0.22f : 0.55f);
				if (kit == 0) {   // 808: two tuned skins (180 / 330 Hz) and a thin, high "snappy"
					hp = nz - prev; prev = nz;
					v = sinf(TWO_PI * 180 * t) * expf(-t * 20) * 0.55f + sinf(TWO_PI * 330 * t) * expf(-t * 28) * 0.35f + hp * expf(-t * 19) * 0.45f;
				} else if (kit == 1) {   // 909: a pitched-down body and a loud, bright, longer noise
					ph += 195 * (1 + 0.4f * expf(-t * 60)) / sr; hp = nz - prev; prev = nz;
					v = tanhf((sinf(TWO_PI * ph) * expf(-t * 24) * 0.5f + (hp * 0.6f + nz * 0.4f) * expf(-t * 11) * 0.85f) * 1.4f);
				} else {   // LOFI (and CHIP's base): a fat low body and dark, lowpassed noise
					float body = sinf(TWO_PI * (kit == 2 ? 160 : 185) * t) * expf(-t * 18);
					float sn = (kit == 2 ? lp * 1.6f : nz - lp * 0.6f) * expf(-t * (kit == 2 ? 12 : 17));
					v = body * 0.55f + sn * (kit == 3 ? 0.3f : 0.8f);
				}
				if (kit == 3) {   // 15-bit LFSR clocked at 9 kHz, 4-bit stepped decay
					lph += 9000.0f / sr; while (lph >= 1) { lph -= 1; unsigned b = (lfsr ^ (lfsr >> 1)) & 1; lfsr = (lfsr >> 1) | (b << 14); }
					float e = floorf(expf(-t * 15) * 15) / 15; v = ((lfsr & 1) ? 0.6f : -0.6f) * e + (t < 0.03f ? (fmodf(t * 220, 1) < 0.5f ? 0.3f : -0.3f) * e : 0);
				}
			} else if (d >= 3) {   // the DRUM KIT extras
				float e;
				switch (d) {
				case 3:   // open hat: the closed hat's noise, long tail
					hp = nz - prev; prev = nz; bp1 += (hp - bp1) * 0.6f;
					if (kit == 0) { float m = metal808(t, 1); float mh = m - bp2; bp2 = m; v = mh * expf(-t * 7.0f) * 0.55f; }   // 808: metal, not noise
					else if (kit == 1) { float m = metal808(t, 1.7f); float mh = m - bp2; bp2 = m; v = (hp * 0.5f + mh * 0.4f) * expf(-t * 5.5f) * 0.6f; }
					else v = (kit == 2 ? bp1 : hp) * expf(-t * 8.0f) * 0.55f;
					if (kit == 3) { lph += 30000.0f / sr; while (lph >= 1) { lph -= 1; unsigned b = (lfsr ^ (lfsr >> 1)) & 1; lfsr = ((lfsr >> 1) & ~0x40u) | (b << 6) | (b << 14); }
						v = ((lfsr & 1) ? 0.4f : -0.4f) * (floorf(expf(-t * 8) * 15) / 15); }
					break;
				case 4: {   // clap: three quick bursts then a tail of band-passed noise
					float tt = t < 0.03f ? fmodf(t, 0.01f) : t - 0.02f;
					bp1 += (nz - bp1) * 0.45f; bp2 += (bp1 - bp2) * 0.45f; float band = bp1 - bp2;
					e = expf(-tt * (t < 0.03f ? 180 : (kit == 1 ? 22 : 16))); v = band * e * 2.2f;
					if (kit == 3) v = ((lfsr ^= (lfsr << 3) ^ (lfsr >> 5), lfsr & 1) ? 0.5f : -0.5f) * floorf(e * 15) / 15;
					break; }
				case 5:   // rim: short tuned click
					v = (sinf(TWO_PI * 1650 * t) * 0.7f + sinf(TWO_PI * 520 * t) * 0.5f) * expf(-t * 140) + (t < 0.002f ? nz * 0.5f : 0);
					if (kit == 3) v = v > 0 ? 0.5f * expf(-t * 120) : -0.5f * expf(-t * 120);
					break;
				case 6: case 7: {   // toms: falling sine
					float f0 = d == 6 ? 95 : 150, f = f0 * (1 + 0.6f * expf(-t * 25));
					ph += f / sr; v = sinf(TWO_PI * ph) * expf(-t * (d == 6 ? 7 : 9)) + (t < 0.003f ? nz * 0.25f : 0);
					if (kit == 3) v = (v > 0 ? 0.65f : -0.65f) * floorf(expf(-t * (d == 6 ? 7 : 9)) * 15) / 15 * (d == 7 ? 0.75f : 1);
					if (kit == 2) v = tanhf(v * 2.0f) * 0.85f;
					break; }
				case 8: {   // cowbell: two square tones (540 + 800 Hz, the 808's), band-limited a little
					ph += 540 / sr; ph2 += 800 / sr;
					float sq = (fmodf(ph, 1) < 0.5f ? 0.5f : -0.5f) + (fmodf(ph2, 1) < 0.5f ? 0.5f : -0.5f);
					lp += (sq - lp) * (kit == 3 ? 1.0f : 0.35f); v = lp * (t < 0.01f ? 1.0f : expf(-(t - 0.01f) * 9)) * (kit == 3 ? 0.35f : 0.55f);
					break; }
				default: {   // crash: bright noise, slow decay
					hp = nz - prev; prev = nz; lp += (hp - lp) * 0.6f;
					v = (hp * 0.6f + lp * 0.4f) * expf(-t * (kit == 2 ? 4.5f : 2.6f)) * 0.5f;
					if (kit == 0) { float m = metal808(t, 1.3f); float mh = m - bp2; bp2 = m; v = (mh * 0.6f + hp * 0.25f) * expf(-t * 2.2f) * 0.5f; }
					if (kit == 3) { lph += 30000.0f / sr; while (lph >= 1) { lph -= 1; unsigned b = (lfsr ^ (lfsr >> 1)) & 1; lfsr = (lfsr >> 1) | (b << 14); }
						v = ((lfsr & 1) ? 0.3f : -0.3f) * floorf(expf(-t * 2.6f) * 15) / 15; }
					break; }
				}
				if (kit == 2) v = roundf(v * 10) / 10;   // LOFI: coarse steps
			} else {   // hat: high-passed noise, very short
				hp = nz - prev; prev = nz;
				if (kit == 0) { float m = metal808(t, 1); float mh = m - bp2; bp2 = m; v = mh * expf(-t * 60) * 0.7f; }   // 808: the metal squares, clipped short
				else if (kit == 1) { float m = metal808(t, 1.7f); float mh = m - bp2; bp2 = m; v = (hp * 0.55f + mh * 0.45f) * expf(-t * 32) * 0.75f; }   // 909: noise + brighter metal
				else { bp1 += (hp - bp1) * 0.5f; v = bp1 * expf(-t * 40) * 0.9f; }   // LOFI: a dull, dusty tick
				if (kit == 3) {   // short 7-bit LFSR mode at 30 kHz: the metallic chip hat
					lph += 30000.0f / sr; while (lph >= 1) { lph -= 1; unsigned b = (lfsr ^ (lfsr >> 1)) & 1; lfsr = ((lfsr >> 1) & ~0x40u) | (b << 6) | (b << 14); }
					v = ((lfsr & 1) ? 0.45f : -0.45f) * (floorf(expf(-t * 60) * 15) / 15);
				}
			}
			if (kit == 2) {   // LOFI: every drum through an old sampler: ~8 kHz, 6-bit
				static float hold; if (i % 4 == 0) hold = v; v = roundf(hold * 32) / 32;
			}
			if (kit == 3) v *= 0.6f;   // CHIP: square waves are all edge - as loud by peak, much louder to the ear (-4.4 dB)
			if (v > 1) v = 1;
			if (v < -1) v = -1;
			if (bake) gDrF[i] = v; else out[i] = (s16)(v * 30000);
		}
		if (bake) { int n = drumShape(gDrF, Ls, L0, dvc, sr, cap); for (int i = 0; i < n; i++) { float v = gDrF[i]; out[i] = (s16)((v > 1 ? 1 : v < -1 ? -1 : v) * 30000); } len[d] = n; }
		if (kitArg == KIT_SAMPLE && gDrF) {   // SAMPLE: the folder's file instead (the 808 drum above stays if there's none)
			float m = dvc->decay > 100 ? dvc->decay / 100.0f : 1; int room = (int)(cap / m); if (room > gDrCap) room = gDrCap;
			int n = kitSampleLoad(d, gDrF, room);
			if (n > 0) {
				if (gDrE && dvBaked(dvc)) n = drumShape(gDrF, n, n, dvc, sr, cap);
				if (n > cap) n = cap;
				for (int i = 0; i < n; i++) { float v = gDrF[i]; out[i] = (s16)((v > 1 ? 1 : v < -1 ? -1 : v) * 30000); } len[d] = n;
			}
		}
		if (gCsndOK) CSND_FlushDataCache(out, len[d] * 2);
	}
	for (int d = 0; d < NDRUM; d++) { s16* t = gDrumSmp[d]; gDrumSmp[d] = gDrumBack[d]; gDrumBack[d] = t; gDrumLen[d] = len[d]; }   // swap in (the old set keeps ringing out)
	gDrumBuilt = kitArg;
}
static void kitToTable(void) {   // KIT > TABLE: the drum kit you have (with your SOUND edits) as a wavetable, 8 drums x 8 frames
	if (gDrumBuilt != gKit || !gDrumSmp[0]) drumBuild(gKit);
	if (!gDrumSmp[0]) { say("NO DRUM KIT TO TAKE"); return; }
	static const int D[DT_N] = {0, 1, 6, 4, 2, 5, 8, 9}; float* sig[DT_N]; int len[DT_N]; int ok = 1;
	for (int g = 0; g < DT_N; g++) { int d = D[g], n = gDrumLen[d]; sig[g] = n > 0 ? (float*)malloc(sizeof(float) * n) : NULL; len[g] = n; if (!sig[g]) { ok = 0; continue; } for (int i = 0; i < n; i++) sig[g][i] = gDrumSmp[d][i] / 32768.0f; }
	float* fr = ok ? (float*)malloc(sizeof(float) * CYC * WT_FR) : NULL;
	if (fr) { drumTableFrames(fr, sig, len); char b[12]; snprintf(b, sizeof(b), "KIT %s", KIT_NAMES[gKit]); wtInstall(b, fr); } else say("NOT ENOUGH MEMORY");
	free(fr); for (int g = 0; g < DT_N; g++) free(sig[g]);
}
static void drumService(void) {   // main loop, OUTSIDE the voice lock: a kit takes ~0.2 s to synthesise on the 3DS,
	int k = gDrumWant;             // and holding the lock that long stalled the drum clock (the skip on DRUMS ON / KIT)
	if (k < 0) return;
	gDrumWant = -1; int force = k >= 100; k %= 100; if (force || gDrumBuilt != k) drumBuild(k);   // +100: rebuild anyway (the WAVE kit follows OSC A)
}
static void drumPickChannels(void) {
	gNzCh[0] = gNzCh[1] = -1;
	int n = 0;
	for (int k = 0; k < NDCH; k++) gDrumCh[k] = -1;
	for (int c = 16; c < NCS && n < NDCH; c++) if (gCsndOK && (csndChannels & BIT(c)) && !chipReserved(c) && !echoReserved(c)) gDrumCh[n++] = c;
}
static int gDrumHitN[NDRUM];   // hits played per drum (the tests count them)
static int gDrumLastCh = -1; static u8 gChFade[32]; static float gChG[32]; static u32 gChVol[32], gChCap[32];   // a channel fading out: frames left, its gain, the volumes it started at
static u32 volScale(u32 v, float g) { u32 l = (u32)((v & 0xFFFF) * g), r = (u32)((v >> 16) * g); return l | (r << 16); }
static void drumFade(int c, int frames) { if (c < 0 || c >= 32) return; gChFade[c] = (u8)frames; gChG[c] = 1; }
static int drumFadeTick(void) {   // audio thread, each 5 ms frame: fading channels step down, then stop
	int any = 0;
	for (int c = 0; c < 32; c++) if (gChFade[c]) {
		gChG[c] *= gChFade[c] > 2 ? 0.75f : 0.3f; gChFade[c]--;
		if (!gChFade[c]) CSND_SetPlayState(c, 0); else CSND_SetVol(c, volScale(gChVol[c], gChG[c]), volScale(gChCap[c], gChG[c]));
		any = 1;
	}
	return any;
}
static void drumHit(int d, float vel) {   // from the audio thread (voice lock held)
	mirNote('D', d, vel);
	if (d >= 0 && d < NDRUM && vel > 0.05f) { gPadFlash[d] = 1; gDrumHitN[d]++; }
	static const int DCH[NDRUM] = {0, 1, 2, 2, 3, 1, 4, 4, 5, 6};   // open hat chokes the closed hat (same channel), rim cuts the snare
	static const int PCH[NDRUM] = {0, 1, 2, 3, 4, 5, 6, 6, 5, 1};   // PERC: each shaker on its own channel; the drums share
	int perc = gKit == 4, ch = perc ? PCH[d] : DCH[d];
	if (d == 0 && !perc && vel > 0.05f) gKickT = 0;   // PUMP RATE KICK: duck the synth under it
	int c = gDrumCh[ch]; if (c < 0) c = gDrumCh[ch % 3];
	if (c < 0 || !gDrumSmp[d]) return;
	// No clicks: a drum is never cut off mid-sound. Each channel slot has a partner channel borrowed from the free ones; a
	// new hit goes to whichever of the two isn't sounding, and the one that was fades out over a few ms (a choke: open
	// hat after closed, rim after snare, or the same drum again) - or, with DRUM POLY, rings on longer before it fades.
	int fadeOld = -1, fadeFr = 2;
	if (!(gKit == 3 && d >= 1 && d <= 2)) {
		static int alt[NDCH] = {-1, -1, -1, -1, -1, -1, -1}, onAlt[NDCH], lastD[NDCH] = {-1, -1, -1, -1, -1, -1, -1};
		int slot = gDrumCh[ch] >= 0 ? ch : ch % 3, main_ = gDrumCh[slot];
		int ok = alt[slot] >= 0 && csUsable(alt[slot]) && gCsOwner[alt[slot]] < 0 && alt[slot] != main_;
		for (int k = 0; ok && k < NDCH; k++) if (gDrumCh[k] == alt[slot] || (k != slot && alt[k] == alt[slot])) ok = 0;
		if (!ok) { alt[slot] = -1; onAlt[slot] = 0;
			for (int i = 8; i < NCS && alt[slot] < 0; i++) if (csUsable(i) && gCsOwner[i] < 0) { int used = 0; for (int k = 0; k < NDCH; k++) if (gDrumCh[k] == i || alt[k] == i) used = 1; if (!used) alt[slot] = i; } }
		if (alt[slot] >= 0 && main_ >= 0) {
			int was = onAlt[slot] ? alt[slot] : main_;
			c = onAlt[slot] ? main_ : alt[slot]; onAlt[slot] = !onAlt[slot];
			fadeOld = was; fadeFr = gDrumPoly && lastD[slot] == d ? 8 : 2;   // POLY: the last hit rings ~40 ms over the new one
		}
		lastD[slot] = d;
	}
	static const s8 TROW[NDRUM] = {0, 1, 2, 2, 1, 1, -1, -1, -1, 2};   // which TUNE moves each drum: kick; snare, clap, rim; hats, crash
	float tm = (perc || TROW[d] < 0 ? 1.0f : powf(2.0f, gDrumTune[TROW[d]] / 12.0f)) * powf(2.0f, gDV[d].tune / 12.0f);   // + the drum's own TUNE
	u32 tmr = CSND_TIMER(32728 * tm); if (tmr < 0x42) tmr = 0x42; if (tmr > 0xFFFF) tmr = 0xFFFF;
	u32 flags = SOUND_FORMAT_16BIT | SOUND_ONE_SHOT | SOUND_LINEAR_INTERP | SOUND_ENABLE | SOUND_CHANNEL(c) | (tmr << 16);
	static const float DG[NDRUM] = {0.5f, 0.5f, 0.3f, 0.28f, 0.55f, 0.5f, 0.45f, 0.45f, 0.3f, 0.28f}, DP[NDRUM] = {0.5f, 0.5f, 0.62f, 0.62f, 0.45f, 0.42f, 0.36f, 0.62f, 0.55f, 0.42f};
	static const float PG[NDRUM] = {0.5f, 0.45f, 0.4f, 0.35f, 0.4f, 0.4f, 0.55f, 0.5f, 0.5f, 0.4f}, PP[NDRUM] = {0.4f, 0.6f, 0.56f, 0.62f, 0.45f, 0.4f, 0.38f, 0.55f, 0.62f, 0.5f};
	float g = gDrumLevel * vel * (perc ? PG[d] : DG[d]) * gDV[d].level / 100.0f, pan = (perc ? PP[d] : DP[d]) + (gDV[d].pan - 100) / 100.0f;
	pan = pan < 0 ? 0 : pan > 1 ? 1 : pan;
	u32 l = (u32)(g * sqrtf(1 - pan) * 1.41f * 0x7FFF), r = (u32)(g * sqrtf(pan) * 1.41f * 0x7FFF); if (l > 0xFFFF) l = 0xFFFF; if (r > 0xFFFF) r = 0xFFFF; u32 vol = l | (r << 16);   // (an ACCENT can push past full)
	if (fadeOld >= 0) drumFade(fadeOld, fadeFr);
	gChFade[c] = 0; gDrumLastCh = c; gChVol[c] = gThruOn ? 0 : csMono(vol); gChCap[c] = csSend(l / 32768.0f, r / 32768.0f);
	CSND_SetPlayState(c, 0);
	if (gKit == 3 && d >= 1 && d <= 2 && gNzCh[d - 1] >= 0) {   // CHIP kit: snare and hat straight from the hardware noise generators
		int nc = gNzCh[d - 1]; gNzVol[d - 1] = g; CSND_SetPlayState(nc, 0);
		u32 t = CSND_TIMER((d == 2 ? 30000 : 9000) * tm); if (t < 0x42) t = 0x42;
		CSND_SetChnRegsNoise(SOUND_FORMAT_PSG | SOUND_ENABLE | SOUND_CHANNEL(nc) | (t << 16), vol, csSend(l / 32768.0f, r / 32768.0f));
		return;
	}
	CSND_SetChnRegs(flags, osConvertVirtToPhys(gDrumSmp[d]), osConvertVirtToPhys(gDrumSmp[d]), gDrumLen[d] * 2, gThruOn ? 0 : csMono(vol), csSend(l / 32768.0f, r / 32768.0f));
}
static void rowHit(int r, float vel) {   // a pattern hit on row r (kick / snare / hat): MUTE and CHANCE first
	if (gDrumMute & (1 << r)) return;
	if (gDrumChance[r] < 0.999f && rand() % 1000 >= (int)(gDrumChance[r] * 1000)) return;
	drumHit(r, vel);
}
static void drumTick(float dt) {   // audio thread: 16th-note clock, follows the sequencer when it plays
	if (drumFadeTick()) csndExecCmds(false);
	if (!gDrums || !gCsndOK || gDrumBuilt < 0) { gDrumStep = -1; gDrClk = -1; return; }
	if (gKit == 3) {   // noise drums have no envelope of their own: decay them from here every 5 ms
		int any = 0;
		for (int k = 0; k < 2; k++) if (gNzCh[k] >= 0 && gNzVol[k] > 0.001f) {
			gNzVol[k] *= k ? 0.62f : 0.80f;
			u32 v = (u32)(gNzVol[k] * 0x5A00); v |= v << 16;
			if (gNzVol[k] <= 0.001f) CSND_SetPlayState(gNzCh[k], 0); else CSND_SetVol(gNzCh[k], v, csSend(gNzVol[k], gNzVol[k]));
			any = 1;
		}
		if (any) csndExecCmds(false);
	}
	if (gMirror) return;   // a mirror's drums play the hits the 3DS sends, not their own clock
	int step = -1; float frac = 0;
	static long long clk = 0;   // 16ths since the drums (or the sequence) started
	if (gSeqPlaying) { static int lastSeq = -1; if (gSeqPos != lastSeq) { step = gSeqPos & 15; lastSeq = gSeqPos; } clk = gSeqClock; float l = stepLen(gSeqPos < 0 ? 0 : gSeqPos); frac = l > 0 ? gSeqPh / l : 0; }
	else {
		float len = (gDrumStep & 1) ? 1.0f - gSwing : 1.0f + gSwing;
		gDrumPh += dt * gBPM / 60.0f * 4.0f;
		if (gDrumStep < 0) { gDrumStep = 15; gDrumPh = len; clk = -1; }
		if (gDrumPh >= len) { gDrumPh -= len; step = (gDrumStep + 1) & 15; clk++; len = (step & 1) ? 1.0f - gSwing : 1.0f + gSwing; }
		frac = gDrumPh / len; if (frac > 0.999f) frac = 0.999f;
	}
	if (clk < 0) return;
	gDrClk = clk;
	int any = 0;
	if (step >= 0) {
		gDrumStep = step;
		if ((step & 3) == 0) gBeatPh = step / 4;   // keep the sidechain pump on the kick grid
		int eu = gEuLen >= 16 ? step : gEuPos % gEuLen; gEuPos++; gEuIdx = eu;
		for (int r = 0; r < 3; r++) if (*DPAT[r] == EU_IDX[r] && euOn(r, eu)) { rowHit(r, r == 2 ? HATVEL[eu] / 255.0f : (eu == gEuRot[r] % gEuLen ? 1.0f : 0.85f)); any = 1; }
	}
	// every row on its own clock: RATE x the 16ths, LENGTH steps, then RATCHETS and CONDITIONS per step
	static long long last[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; static int ratLeft[10]; static double ratNext[10], ratStep[10]; static float ratVel[10];
	static int pre[10]; static long long startClk = -1;
	if (clk == 0 && frac < 0.05f && startClk != 0) { for (int r = 0; r < 10; r++) { last[r] = -1; ratLeft[r] = 0; pre[r] = 0; } }
	startClk = clk;
	double pos16 = gSeqPlaying ? ((double)clk + frac) * seqDiv() : (double)clk + frac;   // the sequence's steps may be 32nds or 8ths
	for (int r = 0; r < 10; r++) {
		if (r < 3 && *DPAT[r] == EU_IDX[r]) { gDrIdx[r] = -1; continue; }
		double rp = pos16 * DRATE[gDrRate[r]]; long long n = (long long)(rp + 1e-6);
		int L = gDrLen[r] < 1 ? 16 : gDrLen[r];
		if (ratLeft[r] > 0 && rp >= ratNext[r]) { ratLeft[r]--; ratNext[r] += ratStep[r]; if (r < 3) rowHit(r, ratVel[r]); else drumHit(r, ratVel[r]); any = 1; }
		if (n == last[r]) continue;
		last[r] = n; int i = (int)(n % L), cyc = (int)(n / L); gDrIdx[r] = i; ratLeft[r] = 0;
		int v = r < 3 ? gMyDrum[r][i] : gDrumX[r - 3][i]; if (!v) continue;
		int c = gDrCond[r][i], ok = c ? condTest(c, cyc, &pre[r], pre[r ? r - 1 : 9]) : 1;
		if (!ok) continue;
		float vel = v / 255.0f; if (gDrAcc[i]) { vel *= 1.0f + gDrAccAmt; if (vel > 1.6f) vel = 1.6f; }   // ACCENT: louder (past full: harder)
		if (r < 3) rowHit(r, vel); else drumHit(r, vel); any = 1;
		int nr = gDrRat[r][i] + 1;
		if (nr > 1) { ratLeft[r] = nr - 1; ratStep[r] = 1.0 / nr; ratNext[r] = (double)n + ratStep[r]; ratVel[r] = vel * 0.8f; }
	}
	if (any) csndExecCmds(false);
}
static int midiDrumPad(int a, int on, int vel, int inTick) {   // channel 10 (General MIDI drums, the phone's DRUM PADS): 1 = it was a drum
	static const u8 GM[NDRUM] = {36, 38, 42, 46, 39, 37, 45, 50, 56, 49};
	int d = -1; for (int k = 0; k < NDRUM; k++) if (GM[k] == a) d = k;
	if (d < 0) { if (a == 35) d = 0; else if (a == 40) d = 1; else if (a == 44) d = 2; else if (a == 41 || a == 43) d = 6; else if (a == 47 || a == 48) d = 7; else if (a == 51 || a == 53) d = 8; else if (a == 57 || a == 52 || a == 55) d = 9; }
	if (d < 0) return 0;
	if (on) {
		if (gDrums && gDrumBuilt >= 0 && gCsndOK) { drumHit(d, 0.3f + 0.7f * vel / 127.0f); csndExecCmds(false); gBeatFlash = 1; }
		else if (!inTick) say("DRUM PADS: TURN THE DRUMS ON");
	}
	return 1;
}
static void myDrumLoad(void);
static void drumsChanged(void) {   // main thread
	myDrumLoad();
	{ int pt[3] = {gKickPat, gSnarePat, gHatPat}; for (int r = 0; r < 3; r++) if (pt[r] != MINE_IDX[r]) drumPaste(r, pt[r], 0); }   // a factory pattern set directly: into the grid
	if (gDrums) {
		if (!gCsndOK) { say("DRUMS NEED THE SECOND SOUND CHIP"); gDrums = 0; return; }
		if (gDrumBuilt != gKit) gDrumWant = gKit;   // built after the lock is let go (drumService)
		drumPickChannels(); gDrumStep = -1; gEuPos = 0;
	} else if (gCsndOK && gVoiceMode < 3) { for (int d = 0; d < 3; d++) if (gDrumCh[d] >= 0) CSND_SetPlayState(gDrumCh[d], 0); for (int k = 0; k < 2; k++) if (gNzCh[k] >= 0) CSND_SetPlayState(gNzCh[k], 0); csndExecCmds(false); }
}
// PANIC, after the notes, sequencer and arp: the loop (kept recorded: LOOPER PLAY brings it back), live REC, the tape,
// the chip echo's tail, the drum machine, a held shake, and anything still ringing on the second chip
static int gProgAuto;
// PANIC really stops everything: notes cut (not released), the reverb / delay / FREEZE tails emptied, GRAINS off,
// held MIDI keys, the arp, strummed strings, the sequencer's ringing notes - then the rest (panicRest)
static void fxTailsClear(void) {
	for (int i = 0; i < NCOMB; i++) { Comb* c[2] = {&gCbL[i], &gCbR[i]}; for (int k = 0; k < 2; k++) if (c[k]->b) { memset(c[k]->b, 0, c[k]->n * sizeof(float)); c[k]->st = 0; } }
	for (int i = 0; i < NAP; i++) { APass* q[2] = {&gApL[i], &gApR[i]}; for (int k = 0; k < 2; k++) if (q[k]->b) memset(q[k]->b, 0, q[k]->n * sizeof(float)); }
	memset(gSprBuf, 0, sizeof(gSprBuf)); dlyTypeChanged(); gRevEnv = gRevGate = gRevHold = 0;
	for (int c = 0; c < 2; c++) { if (gMdl[c]) memset(gMdl[c], 0, MDL * sizeof(float)); if (gOfDl[c]) memset(gOfDl[c], 0, OFDL * sizeof(float)); }
	if (gPs) memset(gPs, 0, PSN * sizeof(float)); memset(gApZ, 0, sizeof(gApZ)); memset(gPhFb, 0, sizeof(gPhFb)); memset(gCombFb, 0, sizeof(gCombFb));
}
static void panicRest(void);
static int gPanicClr = 0;   // frames left to keep emptying the FX: audio already on its way in refills them
static void panicAll(void) {
	for (int i = 0; i < NVS; i++) if (gV[i].active) freeVoice(&gV[i]);
	gHeldN = 0; gArpN = 0; gArpOnN = 0; gStrN = 0; gMidiHeldN = 0; gSqNn = 0; gStrumOffN = 0; gSlideOn = 0; gBassNote = -1; gBassStep = -1;
	gFreeze = 0; fxTailsClear(); gPanicClr = 15;
	if (gGrains) { gGrains = 0; grainsChanged(); }
	panicRest();
}
static void panicRest(void) {
	if (gLiveRec || gPreFor == 2) liveRecStop();
	tapeStopPlay(); echoStop();
	if (gDrums) { gDrums = 0; drumsChanged(); }
	gShakePend = 0; gProgAuto = 0;
	if (gCsndOK) { for (int c = 0; c < NCS; c++) if ((csndChannels & BIT(c)) && gCsOwner[c] < 0) CSND_SetPlayState(c, 0); csndExecCmds(false); }
}
// ---- DRUM KIT patches (voice MODE 808 KIT .. CHIP KIT): every key is a drum. Notes follow General MIDI from C1 (36)
// up, folded every two octaves, so an FM-1, a DAW or Drambo plays the right drums and the keyboard always has a full kit.
static const s8 KIT_GM[24] = {0, 5, 1, 4, 1, 6, 2, 6, 2, 6, 3, 7, 7, 9, 7, 9, 9, 8, 2, 9, 8, 9, 5, 9};   // 36..59 -> sound
static const s8 PERC_GM[24] = {8, 8, 7, 7, 6, 7, 6, 9, 9, 2, 1, 5, 5, 4, 4, 5, 9, 9, 7, 6, 3, 3, 0, 3};   // 60..83 (bongos .. jingle bell)
static int kitSound(int note) {
	if (gVoiceMode == 7) { int k = ((note - 60) % 24 + 24) % 24; return PERC_GM[k]; }
	int k = ((note - 36) % 24 + 24) % 24; return KIT_GM[k];
}
static const char* KIT_PAD_NAMES_D[NDRUM] = {"KICK", "SNARE", "CL HAT", "OP HAT", "CLAP", "RIM", "LO TOM", "HI TOM", "BELL", "CRASH"};
static const char* KIT_PAD_NAMES_P[NDRUM] = {"SHAKER", "MARACA", "CABASA", "TAMB", "GUIRO", "CLAVES", "CONGA L", "CONGA H", "BONGO", "WOOD"};
static const int KIT_PAD_NOTE_D[NDRUM] = {36, 38, 42, 46, 39, 37, 41, 48, 56, 49};
static const int KIT_PAD_NOTE_P[NDRUM] = {82, 70, 69, 80, 73, 75, 64, 63, 60, 76};
static const int KIT_PAD_ORDER_D[10] = {2, 3, 9, 8, 5, 0, 1, 4, 6, 7};   // top row: hats, crash, bell, rim; bottom: kick, snare, clap, toms
static const int KIT_PAD_ORDER_P[10] = {0, 1, 2, 3, 4, 6, 7, 8, 5, 9};   // top row: the shakers; bottom: skins and wood
static const char* kitPadName(int d) { return gVoiceMode == 7 ? KIT_PAD_NAMES_P[d] : KIT_PAD_NAMES_D[d]; }
static int kitPadNote(int d) { return gVoiceMode == 7 ? KIT_PAD_NOTE_P[d] : KIT_PAD_NOTE_D[d]; }
static int kitDrumOf(int nt) { if (gVoiceMode < 3) return -1; for (int d = 0; d < 10; d++) if (kitPadNote(d) == nt) return d; return -1; }
static int kitPadOrder(int i) { return gVoiceMode == 7 ? KIT_PAD_ORDER_P[i] : KIT_PAD_ORDER_D[i]; }
static void kitEnsure(void) {   // main thread: build the patch's kit and claim its channels
	if (gVoiceMode < 3 || !gCsndOK) return;
	int kit = gVoiceMode - 3;
	if (gDrumBuilt != kit || gKit != kit) { gKit = kit; gDrumWant = kit; }
	if (gDrumCh[NDCH - 1] < 0 && gDrumCh[3] < 0) drumPickChannels();
	for (int k = 0; k < NDCH; k++) if (gDrumCh[k] >= 0 && gCsOwner[gDrumCh[k]] >= 0) { CSND_SetPlayState(gDrumCh[k], 0); gCsOwner[gDrumCh[k]] = -1; }
}
// ONE KIT: a kit patch (voice MODE 808 KIT .. WAVE KIT) and the drum machine play the same kit. They used to build
// over each other (only one kit fits in memory at a time), so the drums skipped and switched sounds. Whichever you
// changed last wins and the other follows.
static void kitSync(void) {   // main loop
	static int lastKit = -1, lastMode = -1;
	if (gVoiceMode >= 3) {
		if (gVoiceMode != lastMode) gKit = gVoiceMode - 3;          // picked a kit patch / mode: the drum machine follows
		else if (gKit != lastKit) gVoiceMode = gKit + 3;            // changed the drum machine's KIT: the keys follow
		if (gDrums && gDrumBuilt != gKit) gDrumWant = gKit;
	}
	lastKit = gKit; lastMode = gVoiceMode;
}
static void kitHitSound(int d, float vel);
static void shakeQTick(void) {   // audio thread, after the clocks: a held shake lands on the next 16th / 8th
	static int last = -1;
	int pos = gSeqPlaying ? gSeqPos : (gDrums && gDrumStep >= 0 ? gDrumStep : -1);
	if (pos < 0) { last = -1; if (gShakePend > 0) { kitHitSound(0, gShakePend); gShakePend = 0; } return; }   // the clock stopped: play it now
	if (pos == last) return;
	last = pos;
	if (gShakePend > 0 && (gShakeQ != 2 || !(pos & 1))) { kitHitSound(0, gShakePend); gShakePend = 0; }
}
static void kitHitSound(int d, float vel) {   // voice lock held
	if (d >= 0 && d < NDRUM) gPadFlash[d] = 1;
	if (!gCsndOK || gDrumBuilt != gVoiceMode - 3) return;
	drumHit(d, vel * gMaster * 1.25f);
	csndExecCmds(false);
	gBeatFlash = 1;
}
static void kitNoteOn(int note, float vel) { kitHitSound(kitSound(note), vel); }
// MIC TAP: knock or tap near the mic; each hit plays the drum now and is written into MINE at the nearest 16th,
// as loud as you hit it. Switching to a row copies its current pattern into MINE first, so you overdub.
static int gMyDrumLoaded = 0;
#define DRM2 0x324D5244   // "DRM2": ratchets, conditions, rates, lengths, drum voices after the grid
#define DRM3 0x334D5244   // "DRM3": 64-step rows
static void myDrumSave(void) {
	if (!gMyDrumLoaded) return; FILE* f = safeOpen("sdmc:/3ds/syrup/mydrums.bin"); if (!f) return;
	u32 m = DRM3;
	safeClose(f, "sdmc:/3ds/syrup/mydrums.bin", fwrite(&m, 4, 1, f) == 1 && fwrite(gMyDrum, sizeof(gMyDrum), 1, f) == 1 && fwrite(gDrumX, sizeof(gDrumX), 1, f) == 1
		&& fwrite(gDrRat, sizeof(gDrRat), 1, f) == 1 && fwrite(gDrCond, sizeof(gDrCond), 1, f) == 1 && fwrite(gDrRate, sizeof(gDrRate), 1, f) == 1
		&& fwrite(gDrLen, sizeof(gDrLen), 1, f) == 1 && fwrite(gDV, sizeof(gDV), 1, f) == 1 && fwrite(gDrAcc, sizeof(gDrAcc), 1, f) == 1);
}
static void drumPaste(int r, int v, int undo);
static void drumFix(void) {
	for (int r = 0; r < 10; r++) { if (gDrRate[r] >= NDRATE) gDrRate[r] = 0; if (gDrLen[r] < 1 || gDrLen[r] > DSTEP) gDrLen[r] = 16;
		for (int i = 0; i < DSTEP; i++) { if (gDrRat[r][i] > 3) gDrRat[r][i] = 0; if (gDrCond[r][i] >= NCOND) gDrCond[r][i] = 0; } }
	for (int i = 0; i < DSTEP; i++) gDrAcc[i] = gDrAcc[i] ? 1 : 0;
}
static void myDrumLoad(void) { if (gMyDrumLoaded) return; gMyDrumLoaded = 1; FILE* f = safeRead("sdmc:/3ds/syrup/mydrums.bin");
	if (!f) { drumPaste(0, 1, 0); drumPaste(1, 1, 0); drumPaste(2, 1, 0); return; }   // nothing saved yet: a plain beat to start from
	u32 m = 0;
	if (fread(&m, 4, 1, f) == 1 && m == DRM3) {   // 64-step rows
		if (!(fread(gMyDrum, sizeof(gMyDrum), 1, f) == 1 && fread(gDrumX, sizeof(gDrumX), 1, f) == 1 && fread(gDrRat, sizeof(gDrRat), 1, f) == 1 && fread(gDrCond, sizeof(gDrCond), 1, f) == 1
			&& fread(gDrRate, sizeof(gDrRate), 1, f) == 1 && fread(gDrLen, sizeof(gDrLen), 1, f) == 1 && fread(gDV, sizeof(gDV), 1, f) == 1)) { memset(gMyDrum, 0, sizeof(gMyDrum)); memset(gDrumX, 0, sizeof(gDrumX)); }
		if (fread(gDrAcc, sizeof(gDrAcc), 1, f) != 1) memset(gDrAcc, 0, sizeof(gDrAcc));
	} else {   // before pages: 16-step rows, into page 1
		fseek(f, 0, SEEK_SET);
		static u8 my[3][16], dx[7][16], rt[10][16], cd[10][16], ra[10], ln[10], ac[16]; static DVoice dv[10];
		memset(my, 0, sizeof(my)); memset(dx, 0, sizeof(dx));
		if (fread(my, sizeof(my), 1, f) == 1 && fread(dx, sizeof(dx), 1, f) == 1) {
			for (int r = 0; r < 3; r++) memcpy(gMyDrum[r], my[r], 16); for (int r = 0; r < 7; r++) memcpy(gDrumX[r], dx[r], 16);
			if (fread(&m, 4, 1, f) == 1 && m == DRM2 && fread(rt, sizeof(rt), 1, f) == 1 && fread(cd, sizeof(cd), 1, f) == 1 && fread(ra, sizeof(ra), 1, f) == 1 && fread(ln, sizeof(ln), 1, f) == 1 && fread(dv, sizeof(dv), 1, f) == 1) {
				for (int r = 0; r < 10; r++) { memcpy(gDrRat[r], rt[r], 16); memcpy(gDrCond[r], cd[r], 16); gDrRate[r] = ra[r]; gDrLen[r] = ln[r] <= 16 ? ln[r] : 16; }
				memcpy(gDV, dv, sizeof(dv));
				if (fread(ac, sizeof(ac), 1, f) == 1) memcpy(gDrAcc, ac, 16);   // (files before ACCENT stop here)
			}
		}
	}
	drumFix(); fclose(f);
}
static void drumRecChanged(void) {
	myDrumLoad();
	if (gDrumRec > 3) gDrumRec = 0;
	if (!gDrumRec) { myDrumSave(); return; }
	int r = gDrumRec - 1;
	if (*DPAT[r] != MINE_IDX[r]) {   // start from what's playing on that row
		for (int i = 0; i < DSTEP; i++) gMyDrum[r][i] = drumRowOn(r, *DPAT[r], i & 15) ? (r == 2 ? HATVEL[i & 15] : 230) : 0;
		*DPAT[r] = MINE_IDX[r];
	}
	if (!gDrums) { gDrums = 1; drumsChanged(); }
	say("TAP THE 2DS NEAR THE MIC - HARDER = LOUDER");
}
// DRUMS page: the whole bottom screen is the drum sequencer - every drum of the kit, 16 steps. Rows 1-3 (KICK,
// SNARE, CL HAT) are the drum machine's patterns: a tap on one of their steps makes that row MINE, starting from what
// it was playing. Tap a row's name to hear it and pick it. CLR (twice) clears them all.
// Every edit is one SELECT+L undo.
#define DG_Y (TAB_H + 2 + gDgShift)   // gDgShift: 16 when the grid sits in the SEQ page, under its buttons
#define DG_LX 54
#define DG_RH (gDgShift ? 16 : 18)
#define DG_PGY (DG_TOP + DG_ROWS * DG_RH + 1)   // the PAGE strip under the rows
#define DG_TOP (DG_Y + 18)
#define DG_ROWS 10
static int gDrumSaveDue = 0, gDgSel = 0, gTapRow = -1;   // the picked row (its name lit) and MIC TAP's row (-1 off)
static int drumCellOn(int r, int i) {
	if (r >= 3) return gDrumX[r - 3][i] != 0;
	int pat = *DPAT[r];
	if (pat == MINE_IDX[r]) return gMyDrum[r][i] != 0;
	return drumRowOn(r, pat, i);
}
static int drumCellVel(int r, int i) { return r >= 3 ? gDrumX[r - 3][i] : *DPAT[r] == MINE_IDX[r] ? gMyDrum[r][i] : 230; }
static int gDgPg = 0, gDgFollow = 1;   // drum grid page (16 steps each), FOLLOW the playhead
#define DP0 (gDgPg * 16)
static const int DG_DRUM[DG_ROWS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};   // row -> kit drum
static int gBeatNext = 0;
static int clrArmed(void) { if (gDrumClrArm && osGetTime() - gDrumClrT > 4000) gDrumClrArm = 0; return gDrumClrArm; }   // CLR waits 4 s for a row (or CLR again)
static int drumLongest(void) { int L = 16; for (int r = 0; r < DG_ROWS; r++) if (gDrLen[r] > L) L = gDrLen[r]; return L; }
static int drumPlayPage(void) { int r = gDgSel >= 0 && gDgSel < DG_ROWS ? gDgSel : 0; return (gSeqPlaying || (gDrums && gDrumStep >= 0)) && gDrIdx[r] >= 0 ? gDrIdx[r] / 16 : -1; }
static void drumGridMark(void);
static int dgPageTouch(int x) {   // PAGE strip: a page; one past the end makes the rows that long (repeating what's there); COPY>
	if (x < DG_LX) { gDgFollow = !gDgFollow; say(gDgFollow ? "FOLLOW: THE PAGE TURNS WITH THE PLAYHEAD" : "FOLLOW OFF"); return 1; }
	int pw = (320 - DG_LX - 52) / 4, i = (x - DG_LX) / pw, L = drumLongest();
	if (i >= 4) {   // COPY> this page onto the next, every row
		if (gDgPg >= 3) { say("THE LAST PAGE"); return 0; }
		drumGridMark(); myDrumLoad(); int a = DP0, b = a + 16;
		for (int r = 0; r < DG_ROWS; r++) { u8* v = r < 3 ? gMyDrum[r] : gDrumX[r - 3]; memcpy(v + b, v + a, 16); memcpy(gDrRat[r] + b, gDrRat[r] + a, 16); memcpy(gDrCond[r] + b, gDrCond[r] + a, 16);
			if (r < 3) *DPAT[r] = MINE_IDX[r]; if (gDrLen[r] == L && gDrLen[r] < b + 16) gDrLen[r] = (u8)(b + 16); }
		memcpy(gDrAcc + b, gDrAcc + a, 16); gDgPg++; gDgFollow = 0; gDrumSaveDue = 1;
		char m[40]; snprintf(m, sizeof(m), "PAGE %d COPIED TO PAGE %d", gDgPg, gDgPg + 1); say(m); return 1;
	}
	if (i == gDgPg) { gDgFollow = !gDgFollow; say(gDgFollow ? "FOLLOW ON" : "FOLLOW OFF: THIS PAGE STAYS"); return 1; }
	gDgPg = i; if (gSeqPlaying || gDrums) gDgFollow = 0;
	if (i * 16 >= L) {   // a new page: rows as long as the longest grow to here, what they play repeats into it
		drumGridMark(); myDrumLoad(); int nl = (i + 1) * 16;
		for (int r = 0; r < DG_ROWS; r++) if (gDrLen[r] == L) { u8* v = r < 3 ? gMyDrum[r] : gDrumX[r - 3];
			for (int k = L; k < nl; k++) { v[k] = v[k % L]; gDrRat[r][k] = gDrRat[r][k % L]; gDrCond[r][k] = gDrCond[r][k % L]; }
			if (r < 3) *DPAT[r] = MINE_IDX[r]; gDrLen[r] = (u8)nl; }
		for (int k = L; k < nl; k++) gDrAcc[k] = gDrAcc[k % L];
		gDrumSaveDue = 1; char m[32]; snprintf(m, sizeof(m), "DRUMS: %d STEPS", nl); say(m);
	}
	return 1;
}
static void drawDrumGrid(void) {
	rectP(0, DG_Y - 2, 320, 240 - DG_Y + 2, TH->bg);
	char t[24];
	snprintf(t, sizeof(t), "%s", gDrums ? "DR ON" : "DR OFF"); rectP(2, DG_Y, 52, 14, gDrums ? TH->keyOn : TH->tab); { const u8* c = gDrums ? TH->ink : TH->tabTxt; text(28 - (int)strlen(t) * 4, DG_Y + 3, t, 1, c[0], c[1], c[2]); }
	snprintf(t, sizeof(t), "%s >", KIT_NAMES[gKit]); rectP(57, DG_Y, 52, 14, TH->tab); text(83 - (int)strlen(t) * 4, DG_Y + 3, t, 1, RGB3(TH->tabTxt));
	rectP(112, DG_Y, 40, 14, TH->tab); text(132 - 4 * 4, DG_Y + 3, "BEAT", 1, RGB3(TH->tabTxt));   // whole-kit rhythm presets, one tap each
	{ int a = clrArmed(); rectP(155, DG_Y, 36, 14, a ? TH->warn : TH->tab); text(173 - 3 * 4, DG_Y + 3, a ? "ALL" : "CLR", 1, RGB3(a ? TH->ink : TH->tabTxt)); }
	rectP(194, DG_Y, 36, 14, gTapRow >= 0 ? TH->warn : TH->tab); text(200, DG_Y + 3, "MIC", 1, RGB3(gTapRow >= 0 ? TH->ink : TH->tabTxt));   // MIC TAP onto the picked row
	rectP(233, DG_Y, 40, 14, gDrFill ? TH->keyOn : TH->tab); text(237, DG_Y + 3, "FILL", 1, RGB3(gDrFill ? TH->ink : TH->tabTxt));   // held: FILL conditions play
	rectP(276, DG_Y, 40, 14, gAccMode ? TH->keyOn : TH->tab); text(284, DG_Y + 3, "ACC", 1, RGB3(gAccMode ? TH->ink : TH->tabTxt));   // ACC: tap steps to accent their column
	int running = gSeqPlaying || (gDrums && gDrumStep >= 0);
	float cw = (320 - DG_LX - 2) / 16.0f;
	for (int i = 0; i < 16; i++) if (gDrAcc[DP0 + i]) rectP(DG_LX + (int)(i * cw) + 1, DG_TOP - 4, (int)cw - 2, 3, TH->warn);   // accented columns: a bar over them
	for (int r = 0; r < DG_ROWS; r++) {
		int y = DG_TOP + r * DG_RH, d = DG_DRUM[r], lit = gPadFlash[d] > 0;
		rectP(2, y, DG_LX - 4, DG_RH - 2, lit ? TH->keyOn : r == gTapRow ? TH->warn : r == gDgSel ? TH->line : TH->tab);
		const char* nm = kitPadName(d); const u8* lc = lit ? TH->ink : TH->tabTxt;
		text(DG_LX / 2 - 1 - (int)strlen(nm) * 4, y + DG_RH / 2 - 4, nm, 1, lc[0], lc[1], lc[2]);
		if (gDrRate[r] || gDrLen[r] != drumLongest()) rectP(3, y + 1, 3, 3, TH->warn);   // this row runs its own RATE / LENGTH
		int play = !running ? -1 : (r < 3 && *DPAT[r] == EU_IDX[r]) ? (gSeqPlaying ? (gSeqPos & 15) : gDrumStep) : gDrIdx[r];
		for (int c = 0; c < 16; c++) {
			int i = DP0 + c, x = DG_LX + (int)(c * cw), w = (int)cw - 2, on = drumCellOn(r, i), at = i == play, past = i >= gDrLen[r];
			const u8* c = on ? (at ? TH->keyOn : past ? TH->dim : TH->val) : (at ? TH->line : past ? TH->bg : (i & 3) == 0 ? TH->grid : TH->pad);
			rectP(x, y, w, DG_RH - 2, c);
			if (on) { int v = drumCellVel(r, i); if (v < 200) rectP(x, y, w, (DG_RH - 2) * (255 - v) / 255, TH->grid);   // softer: a shorter bar
				for (int k = 0; k < gDrRat[r][i]; k++) rectP(x + 2 + k * 4, y + DG_RH - 6, 2, 3, TH->bg);   // ratchets: a dot per extra hit
				if (gDrCond[r][i]) { rectP(x + w - 4, y, 4, 4, TH->warn); } }   // a condition: the corner
			if (curShown() && r == gDcR && i == gDcI) { outline(x - 1, y - 1, w + 2, DG_RH, RGB3(TH->keyOn)); outline(x, y, w, DG_RH - 2, RGB3(TH->ink)); }   // the circle pad's cursor
		}
	}
	{   // PAGES: 4 x 16 steps; the longest row says how many are in use
		int y = DG_PGY, used = (drumLongest() + 15) / 16, pw = (320 - DG_LX - 52) / 4, pp = drumPlayPage();
		rectP(0, y, 320, 240 - y, TH->bg); text(4, y + 2, gDgFollow ? "F" : "PG", 1, RGB3(gDgFollow ? TH->val : TH->dim));
		for (int i = 0; i < 4; i++) { int x = DG_LX + i * pw, cur = i == gDgPg, has = i < used;
			rectP(x + 1, y, pw - 2, 11, cur ? TH->keyOn : has ? TH->tab : TH->bg); if (!has && !cur) outlineP(x + 1, y, pw - 2, 11, TH->grid);
			char t[4]; snprintf(t, sizeof(t), "%d", i + 1); const u8* c = cur ? TH->ink : has ? TH->tabTxt : TH->dim; text(x + pw / 2 - 4, y + 2, t, 1, c[0], c[1], c[2]);
			if (i == pp) rectP(x + 3, y + 4, 3, 3, cur ? TH->ink : TH->keyOn); }
		int cx = DG_LX + 4 * pw; rectP(cx + 1, y, 320 - cx - 2, 11, gDgPg < 3 ? TH->tab : TH->bg); text(cx + 4, y + 2, "COPY>", 1, RGB3(TH->tabTxt));
	}
}
static void drumCellSet(int r, int i, int on);
// HOLD a step: its editor (VELOCITY, RATCHET, CONDITION). HOLD a drum's name: its SOUND (and the row's RATE / LENGTH).
static int gDgEdR = 0, gDgEdI = 0, gDgDrag = -1, gDvPlay = 0;
static const char* DV_NAMES[8] = {"TUNE", "P.ENV", "P.DEC", "DECAY", "TONE", "DRIVE", "LEVEL", "PAN"};
static const int DV_MIN[8] = {-24, -24, 1, 10, 0, 0, 0, 0}, DV_MAX[8] = {24, 24, 200, 200, 200, 100, 150, 200};
#define DV_RH 14
static int drumCellOn(int r, int i);
static void drumEuclid(int r, int d) {   // a row's hits +-1, spread evenly over its length (Bresenham, like EUCLID)
	int L = gDrLen[r] < 1 ? 16 : gDrLen[r], n = 0; for (int i = 0; i < L; i++) if (drumCellOn(r, i)) n++;
	n += d; if (n < 0) n = 0; if (n > L) n = L;
	drumGridMark(); myDrumLoad(); if (r < 3) *DPAT[r] = MINE_IDX[r];
	for (int i = 0; i < DSTEP; i++) { int on = i < L && n > 0 && (i * n) % L < n; u8 v = on ? (r == 2 ? HATVEL[i & 15] : 230) : 0; if (r < 3) gMyDrum[r][i] = v; else gDrumX[r - 3][i] = v; }
	memset(gDrRat[r], 0, DSTEP); memset(gDrCond[r], 0, DSTEP); gDrumSaveDue = 1;
}
static void drumRotate(int r, int d) {   // shift the row along its own length (hits, ratchets, conditions together)
	int L = gDrLen[r] < 1 ? 16 : gDrLen[r]; drumGridMark(); myDrumLoad(); if (r < 3) *DPAT[r] = MINE_IDX[r];
	u8* v = r < 3 ? gMyDrum[r] : gDrumX[r - 3]; u8 a[DSTEP], b[DSTEP], c[DSTEP];
	for (int i = 0; i < L; i++) { int j = ((i + d) % L + L) % L; a[j] = v[i]; b[j] = gDrRat[r][i]; c[j] = gDrCond[r][i]; }
	memcpy(v, a, L); memcpy(gDrRat[r], b, L); memcpy(gDrCond[r], c, L); gDrumSaveDue = 1;
}
static int dvGet(const DVoice* v, int k) { switch (k) { case 0: return v->tune; case 1: return v->penv; case 2: return v->pdec; case 3: return v->decay; case 4: return v->tone; case 5: return v->drive; case 6: return v->level; default: return v->pan; } }
static void dvSet(DVoice* v, int k, int x) { if (x < DV_MIN[k]) x = DV_MIN[k]; if (x > DV_MAX[k]) x = DV_MAX[k];
	switch (k) { case 0: v->tune = (s8)x; break; case 1: v->penv = (s8)x; break; case 2: v->pdec = (u8)x; break; case 3: v->decay = (u8)x; break; case 4: v->tone = (u8)x; break; case 5: v->drive = (u8)x; break; case 6: v->level = (u8)x; break; default: v->pan = (u8)x; } }
static void dvFmt(int k, int x, char* o, int n) {
	if (k <= 1) snprintf(o, n, "%+d", x); else if (k == 2) snprintf(o, n, "%dMS", x * 2); else if (k == 4) snprintf(o, n, x == 100 ? "0" : "%+d", x - 100);
	else if (k == 7) snprintf(o, n, x == 100 ? "C" : x < 100 ? "L%d" : "R%d", x < 100 ? 100 - x : x - 100); else snprintf(o, n, "%d%%", x);
}
#define DE_Y (DG_TOP + 2)
static void drawDgEd(void) {
	rectP(0, DG_TOP - 2, 320, 240 - DG_TOP + 2, TH->bg); rectP(0, DG_TOP - 2, 320, 1, TH->line);
	char t[40]; int r = gDgEdR;
	rectP(262, DE_Y, 54, 16, TH->keyOn); text(273, DE_Y + 4, "DONE", 1, RGB3(TH->ink));
	if (gDgEd == 1) {
		int i = gDgEdI, v = r < 3 ? gMyDrum[r][i] : gDrumX[r - 3][i];
		snprintf(t, sizeof(t), "%s  STEP %d", kitPadName(DG_DRUM[r]), i + 1); text(6, DE_Y + 4, t, 1, RGB3(TH->val));
		if (curShown()) { static const int FY[3] = {24, 50, 76}; outline(2, DE_Y + FY[gDgSelF % 3], 316, 20, RGB3(TH->keyOn)); }   // the circle pad's setting
		text(6, DE_Y + 30, "VEL", 1, RGB3(TH->txt)); rectP(60, DE_Y + 26, 250, 14, TH->pad); rectP(60, DE_Y + 26, 250 * v / 255, 14, TH->val);
		text(6, DE_Y + 56, "RATCHET", 1, RGB3(TH->txt));
		for (int k = 0; k < 4; k++) { int on = gDrRat[r][i] == k; rectP(84 + k * 58, DE_Y + 52, 54, 16, on ? TH->keyOn : TH->tab); char b[4]; snprintf(b, sizeof(b), "%d", k + 1); text(84 + k * 58 + 23, DE_Y + 56, b, 1, RGB3(on ? TH->ink : TH->tabTxt)); }
		text(6, DE_Y + 82, "IF", 1, RGB3(TH->txt));
		rectP(84, DE_Y + 78, 30, 16, TH->tab); text(95, DE_Y + 82, "<", 1, RGB3(TH->tabTxt)); rectP(282, DE_Y + 78, 30, 16, TH->tab); text(293, DE_Y + 82, ">", 1, RGB3(TH->tabTxt));
		const char* cn = COND_NAMES[gDrCond[r][i]]; text(198 - (int)strlen(cn) * 4, DE_Y + 82, cn, 1, RGB3(gDrCond[r][i] ? TH->warn : TH->val));
		text(6, DE_Y + 108, "1:2 = 1ST OF EVERY 2 LOOPS OF THE ROW", 1, RGB3(TH->dim));
		text(6, DE_Y + 120, "FILL = WHILE FILL IS HELD; PRE = LAST", 1, RGB3(TH->dim));
		text(6, DE_Y + 132, "IF ON THIS ROW; NEI = THE ROW ABOVE", 1, RGB3(TH->dim));
		rectP(6, DE_Y + 150, 96, 16, TH->warn); text(14, DE_Y + 154, "TAKE OUT", 1, RGB3(TH->ink));
	} else {
		snprintf(t, sizeof(t), "%s SOUND", kitPadName(DG_DRUM[r])); text(6, DE_Y + 4, t, 1, RGB3(TH->val));
		rectP(196, DE_Y, 58, 16, TH->tab); text(205, DE_Y + 4, "RESET", 1, RGB3(TH->tabTxt));
		if (curShown()) { int f = gDgSelF % 12, yr = DE_Y + 22 + 8 * DV_RH + 1;
			if (f < 8) outline(2, DE_Y + 21 + f * DV_RH, 314, DV_RH, RGB3(TH->keyOn)); else outline(f & 1 ? 162 : 2, f < 10 ? yr : yr + 20, f & 1 ? 154 : 156, 18, RGB3(TH->keyOn)); }
		for (int k = 0; k < 8; k++) {
			int y = DE_Y + 22 + k * DV_RH, x = dvGet(&gDV[r], k); char b[12];
			text(6, y + 3, DV_NAMES[k], 1, RGB3(TH->txt));
			rectP(64, y, 200, 13, TH->pad);
			int z = DV_MIN[k] < 0 ? 0 : k == 4 || k == 7 ? 100 : DV_MIN[k];   // fill from the centre / zero
			int xp = 64 + 200 * (x - DV_MIN[k]) / (DV_MAX[k] - DV_MIN[k]), zp = 64 + 200 * (z - DV_MIN[k]) / (DV_MAX[k] - DV_MIN[k]);
			rectP(xp < zp ? xp : zp, y + 2, abs(xp - zp) + 1, 9, TH->val); rectP(xp - 1, y, 3, 13, TH->keyOn);
			dvFmt(k, x, b, sizeof(b)); text(268, y + 3, b, 1, RGB3(TH->val));
		}
		int y = DE_Y + 22 + 8 * DV_RH + 2;
		{ int L = gDrLen[r] < 1 ? 16 : gDrLen[r], n = 0; for (int i = 0; i < L; i++) if (drumCellOn(r, i)) n++; char e[8]; snprintf(e, sizeof(e), "%d", n);   // EUCLID: that many hits, spread evenly
		  int y2 = y + 20; text(6, y2 + 4, "EUCL", 1, RGB3(TH->txt)); rectP(50, y2, 24, 16, TH->tab); text(58, y2 + 4, "<", 1, RGB3(TH->tabTxt)); text(98 - (int)strlen(e) * 4, y2 + 4, e, 1, RGB3(TH->val)); rectP(124, y2, 24, 16, TH->tab); text(132, y2 + 4, ">", 1, RGB3(TH->tabTxt));
		  text(166, y2 + 4, "ROT", 1, RGB3(TH->txt)); rectP(198, y2, 24, 16, TH->tab); text(206, y2 + 4, "<", 1, RGB3(TH->tabTxt)); rectP(264, y2, 24, 16, TH->tab); text(272, y2 + 4, ">", 1, RGB3(TH->tabTxt)); }
		text(6, y + 4, "RATE", 1, RGB3(TH->txt)); rectP(50, y, 24, 16, TH->tab); text(58, y + 4, "<", 1, RGB3(TH->tabTxt)); text(98 - (int)strlen(DRATE_NAMES[gDrRate[r]]) * 4, y + 4, DRATE_NAMES[gDrRate[r]], 1, RGB3(TH->val)); rectP(124, y, 24, 16, TH->tab); text(132, y + 4, ">", 1, RGB3(TH->tabTxt));
		snprintf(t, sizeof(t), "%d", gDrLen[r]);
		text(166, y + 4, "LEN", 1, RGB3(TH->txt)); rectP(198, y, 24, 16, TH->tab); text(206, y + 4, "<", 1, RGB3(TH->tabTxt)); text(242 - (int)strlen(t) * 4, y + 4, t, 1, RGB3(TH->val)); rectP(264, y, 24, 16, TH->tab); text(272, y + 4, ">", 1, RGB3(TH->tabTxt));
	}
}
static int dgEdTouch(int x, int y, int press) {   // 1 = redraw
	int r = gDgEdR;
	if (press && y >= DE_Y && y < DE_Y + 16 && x >= 262) { gDgEd = 0; gDgDrag = -1; return 1; }
	if (gDgEd == 1) {
		int i = gDgEdI; u8* vp = r < 3 ? &gMyDrum[r][i] : &gDrumX[r - 3][i];
		if ((press && y >= DE_Y + 24 && y < DE_Y + 42) || gDgDrag == 100) { if (press) { drumGridMark(); gDgDrag = 100; } int v = (x - 60) * 255 / 250; *vp = (u8)(v < 8 ? 8 : v > 255 ? 255 : v); gDrumSaveDue = 1; return 1; }
		if (!press) return 0;
		if (y >= DE_Y + 52 && y < DE_Y + 68 && x >= 84) { int k = (x - 84) / 58; if (k > 3) k = 3; drumGridMark(); gDrRat[r][i] = (u8)k; gDrumSaveDue = 1; return 1; }
		if (y >= DE_Y + 78 && y < DE_Y + 94) { int d = x < 120 ? -1 : x >= 276 ? 1 : 0; if (!d) return 0; drumGridMark(); gDrCond[r][i] = (u8)((gDrCond[r][i] + NCOND + d) % NCOND); gDrumSaveDue = 1; return 1; }
		if (y >= DE_Y + 150 && y < DE_Y + 166 && x < 104) { drumGridMark(); drumCellSet(r, i, 0); gDgEd = 0; return 1; }
		return 0;
	}
	// SOUND
	if (press && y >= DE_Y && y < DE_Y + 16 && x >= 196 && x < 254) { gDV[r] = DV_DEF; gDrumWant = gKit + 100; gDvPlay = r + 1; gDrumSaveDue = 1; return 1; }
	int k = (y - DE_Y - 22) / DV_RH;
	if (press && k >= 0 && k < 8 && y < DE_Y + 22 + 8 * DV_RH && x >= 56) gDgDrag = k;
	if (gDgDrag >= 0 && gDgDrag < 8) { int v = DV_MIN[gDgDrag] + (x - 64) * (DV_MAX[gDgDrag] - DV_MIN[gDgDrag]) / 200; dvSet(&gDV[r], gDgDrag, v); gDrumSaveDue = 1; return 1; }
	int y2 = DE_Y + 22 + 8 * DV_RH + 2;
	if (press && y >= y2 + 20 && y < y2 + 36) {   // EUCLID < > / ROTATE < >
		if (x >= 50 && x < 74) drumEuclid(r, -1); else if (x >= 124 && x < 148) drumEuclid(r, 1);
		else if (x >= 198 && x < 222) drumRotate(r, -1); else if (x >= 264 && x < 288) drumRotate(r, 1); else return 0;
		return 1;
	}
	if (press && y >= y2 && y < y2 + 16) {
		if (x >= 50 && x < 74) gDrRate[r] = (u8)((gDrRate[r] + NDRATE - 1) % NDRATE);
		else if (x >= 124 && x < 148) gDrRate[r] = (u8)((gDrRate[r] + 1) % NDRATE);
		else if (x >= 198 && x < 222) { if (gDrLen[r] > 1) gDrLen[r]--; }
		else if (x >= 264 && x < 288) { if (gDrLen[r] < DSTEP) gDrLen[r]++; }
		else return 0;
		gDrumSaveDue = 1; return 1;
	}
	return 0;
}
// CIRCLE PAD in the editors: a cursor on the piano roll and the drum grid (A puts / takes a note or hit, hold A opens
// its step editor, B on the drum grid opens the row's SOUND), and in the step editors up / down picks a setting and
// left / right changes it (B closes). The cursor shows for a few seconds after the pad moves; then A is CHORD again.
static int stickStep(float j, float* acc, int* held, float dt) {   // a pushed pad: one step, a pause, then faster the further it's pushed
	float a = fabsf(j); if (a < 0.5f) { *held = 0; *acc = 0; return 0; }   // calmer: a firmer push to move, a longer pause before it repeats, slower repeats
	int d = j > 0 ? 1 : -1;
	if (!*held) { *held = 1; *acc = -1.4f; return d; }
	*acc += dt * (4 + 10 * (a - 0.5f) / 0.5f); if (*acc >= 1) { *acc -= 1; return d; }
	return 0;
}
static int dgEdRelease(void) {   // let go: a SOUND slider rebuilds the kit (baked settings) and plays the drum
	if (gDgEd == 2 && gDgDrag >= 0 && gDgDrag < 8) { int k = gDgDrag; gDgDrag = -1; if (k != 0 && k != 6 && k != 7) gDrumWant = gKit + 100; gDvPlay = gDgEdR + 1; return 1; }
	gDgDrag = -1; return 0;
}
static void drumRowToMine(int r) {   // a pattern row (KICK / SNARE / HAT) becomes editable, starting from what it plays
	if (r >= 3 || *DPAT[r] == MINE_IDX[r]) return;
	for (int k = 0; k < DSTEP; k++) gMyDrum[r][k] = drumRowOn(r, *DPAT[r], k & 15) ? (r == 2 ? HATVEL[k & 15] : 230) : 0;
	*DPAT[r] = MINE_IDX[r];
}
// FACTORY PATTERNS are a paste: picking one writes it into the row of the grid, which then plays and edits like
// anything you drew (rows play from the grid; only EUCLID stays a live generator, it can run shorter than 16).
// gDrumSel is what the KICK / SNARE / HAT setting shows: the last pattern pasted, or MINE once you've edited the row.
static void drumPaste(int r, int v, int undo) {
	myDrumLoad();   // first: a first-run load pastes the starter beat, which mustn't land on top of this
	gDrumSel[r] = v;
	if (v == MINE_IDX[r]) { *DPAT[r] = v; return; }
	if (v == EU_IDX[r]) {   // EUCLID: its hits written into the row, the row as long as EUCLID STEPS
		if (undo) drumGridMark();
		int L = gEuLen < 2 ? 16 : gEuLen > 16 ? 16 : gEuLen;
		for (int k = 0; k < DSTEP; k++) gMyDrum[r][k] = k < L && euOn(r, k) ? (r == 2 ? HATVEL[k & 15] : 230) : 0;
		gDrLen[r] = (u8)L; *DPAT[r] = MINE_IDX[r]; gDrumSaveDue = 1; return;
	}
	if (undo) drumGridMark();
	myDrumLoad();
	for (int k = 0; k < DSTEP; k++) gMyDrum[r][k] = drumRowOn(r, v, k & 15) ? (r == 2 ? HATVEL[k & 15] : 230) : 0;
	*DPAT[r] = MINE_IDX[r]; gDrumSaveDue = 1;
}
// BEATS: whole-kit rhythm presets, pasted into all 10 rows at once (then edit them like your own). Rows: kick, snare,
// closed hat, open hat, clap, rim, low tom, high tom, bell, crash. x = a hit, . = rest.
#define NBEAT NBEAT_
#define BEAT_NAMES BEAT_NAMES_   // (the names are up top, for the DRUMS settings list)
static const char* BEATS[NBEAT][10] = {
	{"x...x...x...x...", "", "x...x...x...x...", "..x...x...x...x.", "....x.......x..."},
	{"x......x..x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", "..............x."},
	{"x......x..x.....", "........x.......", "x.x.x.x.xxx.x.xx", "", "........x......."},
	{"x.........x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", "", "", "", "", "", "", "x..............."},
	{"x.x.......xx....", "....x..x.x..x..x", "x.x.x.x.x.x.x.x."},
	{"x...x...x...x...", "....x.......x...", "x...x...x...x...", "..x...x...x...x.", "....x.......x..."},
	{"x..x..x...x..x..", "....x..x.x..x...", "xxxxxxxxxxxxxxxx", "", "", "...x......x....."},
	{"x.......x.x.....", "........x.......", "x.x.x.x.x.x.x.x.", "", "", "", "..............x."},
	{"x...x...x...x...", "...x..x....x..x.", "x.x.x.x.x.x.x.x."},
	{"x.....x...x.....", "....x.......x...", "x.xxx.xxx.xxx.xx", "", "", "..x..x....x..x..", "", "", "x..x..x...x.x..."},
	{"x.........x..x..", "....x.......x...", "..x...x...x...x.", "", "", ".x.....x........"},
	{"x.....x...x.....", "", "x.x.x.x.x.x.x.x.", "", "....x.......x...", "", "", ".......x......x."},
	{"x.......x.x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", "", "", "", "", "", "", "x..............."},
	{"x.....x.x.....x.", "....x.......x...", "x.xx.xx.xx.xx.xx", "", "", "", "", "", "", ""},
	// the classic preset-machine rhythms (CR-78 / rhythm-box style) and Latin grooves
	{"x.......x.......", "", "x.xxx.xxx.xxx.xx", "", "", "....x.......x...", "......x.......x.", "..x.x.....x.x..."},   // CUMBIA: guacharaca on the hat, congas on the toms
	{"x..xx..xx..xx..x", "", "x.x.x.x.x.x.x.x.", "", "", "x..x..x...x..x.."},   // BOSSA: surdo-style kick, the bossa clave on the rim
	{"x..xx..xx..xx..x", "", "xxxxxxxxxxxxxxxx", "", "", "x.x..x.x.x..x.x.", "", "", "x.x.x..x.x.x..x."},
	{"x.......x.......", "", "", "", "", "x..x...x..x.x...", "......x.......x.", "...x..x...x....."},   // RUMBA: the rumba clave
	{"x.....x.x.......", "", "x.x.x.x.x.x.x.x.", "", "", "x..x..x...x.x...", "...........x.x.."},
	{"x.......x.......", "", "x.x.x.x.x.x.x.x.", "", "", "....x.......x...", "", "........x.x.x...", "x...x...x...x..."},   // CHA-CHA: the cha-cha-cha on the high tom
	{"x...x...x.x.x...", "", "", "", "", "x..x..x.x..x..x."},
	{"x.......x.......", "", "", "", "", "x..x..x...x.x...", "..x.......x.....", "...x......x.x...", "x.x.xx.xx.x.xx.x"},   // MAMBO: cowbell on the bell
	{"x.......x.......", "x.xxx.x.x.xxx.x.", "", "", "", "", "", "", "", "x..............."},
	{"x...x...x...x...", "", "....x.......x...", "x...x.x.x...x.x."},   // SWING: the ride on the open hat (turn SWING up)
	{"x.....x.x...", "...x.....x..", "xxxxxxxxxxxx"},   // SLOW ROCK: 12/8, every row 12 steps at 3/4 speed
	{"x...........", "", "....x...x...", "", "", "....x...x..."},   // WALTZ: 3/4, 12 steps
	{"........x.......", "", "x.x.x.x.x.x.x.x.", "", "", "........x......."},   // REGGAE: one drop
	{"x.......x.......", "....x.......x...", "x..xx..xx..xx..x"},
	{"x...x...x...x...", "", "", "..x...x...x...x.", "....x.......x..."},
	{"x.....x...x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", "", "", "", "..........x..x..", "...x...x........"},
};
static const u8 BEAT_RATE[NBEAT] = {[24] = 3};   // SLOW ROCK: 3/4 speed (triplet eighths)
static void beatPaste(int b) {
	if (b < 0 || b >= NBEAT) return;
	drumGridMark(); myDrumLoad();
	int L = 0; for (int r = 0; r < DG_ROWS; r++) { int n = BEATS[b][r] ? (int)strlen(BEATS[b][r]) : 0; if (n > L) L = n; } if (L < 1 || L > 16) L = 16;
	for (int r = 0; r < DG_ROWS; r++) {
		const char* row = BEATS[b][r] ? BEATS[b][r] : "";
		gDrLen[r] = (u8)L; gDrRate[r] = BEAT_RATE[b]; memset(gDrRat[r], 0, DSTEP); memset(gDrCond[r], 0, DSTEP); memset(r < 3 ? gMyDrum[r] : gDrumX[r - 3], 0, DSTEP);
		for (int i = 0; i < 16; i++) {
			int on = i < (int)strlen(row) && row[i] == 'x'; u8 v = on ? (r == 2 ? HATVEL[i] : r == 0 || r == 1 ? 230 : 210) : 0;
			if (r < 3) gMyDrum[r][i] = v; else gDrumX[r - 3][i] = v;
		}
		if (r < 3) { *DPAT[r] = MINE_IDX[r]; gDrumSel[r] = MINE_IDX[r]; }
	}
	gBeatSel = b; gDrumSaveDue = 1; gBeatFromPatch = 0;
	if (!gDrums) { gDrums = 1; drumsChanged(); }
	char m[40]; snprintf(m, sizeof(m), "BEAT: %s (SELECT+L UNDOES)", BEAT_NAMES[b]); say(m);
}
static void drumCellSet(int r, int i, int on) {
	myDrumLoad(); drumRowToMine(r); if (r < 3) gDrumSel[r] = MINE_IDX[r];
	u8 v = on ? (r == 2 ? HATVEL[i] : 230) : 0;
	if (r >= 3) gDrumX[r - 3][i] = v; else gMyDrum[r][i] = v;
	if (on) { gDrRat[r][i] = 0; gDrCond[r][i] = 0; }   // a new hit starts plain
	gDrumSaveDue = 1;
	if (!gDrums) { gDrums = 1; drumsChanged(); }
	if (on && !gSeqPlaying) kitHitSound(DG_DRUM[r], v / 255.0f);   // hear it
}
static void drumRowClear(int r) { myDrumLoad(); drumRowToMine(r); if (r >= 3) memset(gDrumX[r - 3], 0, DSTEP); else memset(gMyDrum[r], 0, DSTEP); memset(gDrRat[r], 0, DSTEP); memset(gDrCond[r], 0, DSTEP); gDrumSaveDue = 1; }
static void drumSnapGet(u8* o) { myDrumLoad(); int p[3] = {gKickPat, gSnarePat, gHatPat};
	#define SNP(x) memcpy(o, x, sizeof(x)); o += sizeof(x);
	SNP(gMyDrum) SNP(gDrumX) memcpy(o, p, 12); o += 12; SNP(gDrRat) SNP(gDrCond) SNP(gDrRate) SNP(gDrLen) SNP(gDrAcc)
	#undef SNP
}
static void drumSnapSet(const u8* in) { int p[3];
	#define SNG(x) memcpy(x, in, sizeof(x)); in += sizeof(x);
	SNG(gMyDrum) SNG(gDrumX) memcpy(p, in, 12); in += 12; SNG(gDrRat) SNG(gDrCond) SNG(gDrRate) SNG(gDrLen) SNG(gDrAcc)
	#undef SNG
	gKickPat = p[0]; gSnarePat = p[1]; gHatPat = p[2]; gDrumSaveDue = 1;
}
static float gVisT = 0;
// ---- MIRROR: the snapshots the 3DS sends (main loop) and what a mirror does with everything it gets
static float* const MIRLV[] = {&gStick, &gXYmod, &gTiltX, &gTiltY, &gMicEnv, &gTwist, &gShake, &gCamLight, &gCamColor, &gChaosX, &gChaosY, &gSpinX, &gSpinY,
	&gMicBright, &gCamMove, &gPressAll, &gStick2X, &gStick2Y, &gSlideAll, &gCutoff, &gRes, &gOsc[0].pos, &gOsc[1].pos, &gMacro[0], &gMacro[1], &gMacro[2], &gMacro[3]};
#define NMIRLV ((int)(sizeof(MIRLV) / sizeof(MIRLV[0])))
typedef struct { s16 page, sel, help, oct, latch, style, preset, brFocus, theme, visMode, visOverlay, patView, startMenu, startSel, quickBar, quickIdx,
	seqMode, seqPos, bpm, drums, kit, pads, gridOn, seqLen, patCur, liveRec, tapeRec, padSel, prog, voiceMode, drumStep, arp; float visT, swing; } MirUI;
typedef struct { Step st[NSTEP]; s16 len, base; u8 rowLen[8], rowHits[8], rowRot[8], rowNote[8]; } MirSeq;
static MirUI gMirUI; static volatile int gMirUIn = 0, gMirVis = 0; static float gMirBend = 0; static u8 gMirHeld[128];   // gMirVis: the mirror stays on the VISUALIZER (STREAM)
static void mirSeqGet(MirSeq* q) { memcpy(q->st, gSeq, sizeof(gSeq)); q->len = (s16)gSeqLen; q->base = (s16)gSeqBase; memcpy(q->rowLen, gRowLen, 8); memcpy(q->rowHits, gRowHits, 8); memcpy(q->rowRot, gRowRot, 8); memcpy(q->rowNote, gRowNote, 8); }
static void mirSend(int page, int selv, int help, int oct, int latch, float bend) {   // 3DS main loop, while a mirror listens
	static int fc = 0; static SPatch lastP; static MirSeq lastQ; static u8 lastR[2048]; static MirUI lastU; static int force = 1;
	if (gMirNew) { gMirNew = 0; force = 1; }
	fc++;
	if (force || !(fc & 1)) { float v[NMIRLV + 1]; for (int i = 0; i < NMIRLV; i++) v[i] = *MIRLV[i]; v[NMIRLV] = bend; mirEmit('L', v, sizeof(v)); }
	if (force || fc % 3 == 0) {
		MirUI u; memset(&u, 0, sizeof(u));
		u.page = (s16)page; u.sel = (s16)selv; u.help = (s16)help; u.oct = (s16)oct; u.latch = (s16)latch; u.style = (s16)gStyle; u.preset = (s16)gPreset; u.brFocus = (s16)gBrFocus;
		u.theme = (s16)gTheme; u.visMode = (s16)gVisMode; u.visOverlay = (s16)gVisOverlay; u.patView = (s16)gPatView; u.startMenu = (s16)gStartMenu; u.startSel = (s16)gStartSel;
		u.quickBar = (s16)gQuickBar; u.quickIdx = (s16)gQuickIdx; u.seqMode = (s16)gSeqMode; u.seqPos = (s16)gSeqPos; u.bpm = (s16)gBPM; u.drums = (s16)gDrums; u.kit = (s16)gKit;
		u.pads = (s16)gPads; u.gridOn = (s16)gGridOn; u.seqLen = (s16)gSeqLen; u.patCur = (s16)gPatCur; u.liveRec = (s16)gLiveRec; u.tapeRec = (s16)gTapeRec; u.padSel = (s16)gPadSel;
		u.prog = (s16)gProg; u.voiceMode = (s16)gVoiceMode; u.drumStep = (s16)gDrumStep; u.arp = (s16)gArp; u.visT = gVisT; u.swing = gSwing;
		if (force || memcmp(&u, &lastU, sizeof(u)) || fc % 30 == 0) { lastU = u; mirEmit('U', &u, sizeof(u)); }
	}
	if (force || fc % 10 == 0) {   // the patch, when it changes (the live values above don't count: they move all the time)
		SPatch p; memset(&p, 0, sizeof(p)); patchCapture(&p); SPatch m = p;
		m.cutoff = m.res = 0; m.osc[0].pos = m.osc[1].pos = 0; for (int i = 0; i < 4; i++) m.macro[i] = 0; m.bpm = 0;
		if (force || memcmp(&m, &lastP, sizeof(m))) { lastP = m; mirEmit('P', &p, sizeof(p)); }
	}
	if (force || fc % 15 == 0) {
		MirSeq q; memset(&q, 0, sizeof(q)); mirSeqGet(&q); if (force || memcmp(&q, &lastQ, sizeof(q))) { lastQ = q; mirEmit('Q', &q, sizeof(q)); }
		u8 r[2048]; memset(r, 0, sizeof(r)); drumSnapGet(r); if (force || memcmp(r, lastR, sizeof(r))) { memcpy(lastR, r, sizeof(r)); mirEmit('R', r, sizeof(r)); }
		u8 nt[64]; int k = 0; for (int i = 0; i < NVS && k < 64; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) nt[k++] = (u8)gV[i].note;
		mirEmit('V', nt, k);   // what's sounding: a mirror lets go of anything else (a lost note-off can't hang)
	}
	force = 0;
}
static void mirFeed(const u8* p, int n) {   // the mirror: a batch of events from the 3DS
	int i = 0;
	while (i + 3 <= n) {
		u8 t = p[i]; int L = p[i + 1] | p[i + 2] << 8; const u8* d = p + i + 3; i += 3 + L; if (i > n) break;
		float x = L >= 2 ? d[1] / 150.0f : 0;
		switch (t) {
		case 'N': if (L >= 2) { voiceOn(d[0], x); gMirHeld[d[0] & 127] = d[1] ? d[1] : 1; } break;
		case 'O': if (L >= 1) { voiceOff(d[0]); gMirHeld[d[0] & 127] = 0; } break;
		case 'F': if (L >= 2) { voiceFade(d[0], x); gMirHeld[d[0] & 127] = 0; } break;
		case 'A': allOff(); memset(gMirHeld, 0, sizeof(gMirHeld)); break;
		case 'D': if (L >= 2 && gDrums && gDrumBuilt >= 0) drumHit(d[0], x); break;
		case 'G': if (L >= 3) { for (int k = 0; k < NVS; k++) { SVoice* v = &gV[k]; if (v->active && !v->isEcho && v->note == d[0] && v->stage < 4) { v->note = d[1]; v->freq = mtof((float)d[1]); v->slide = d[2]; } }
			gMirHeld[d[1] & 127] = gMirHeld[d[0] & 127]; if (d[0] != d[1]) gMirHeld[d[0] & 127] = 0; } break;
		case 'L': { int m = L / 4; for (int k = 0; k < m && k <= NMIRLV; k++) { float f; memcpy(&f, d + k * 4, 4); if (k < NMIRLV) *MIRLV[k] = f; else gMirBend = f; } break; }
		case 'U': if (L == (int)sizeof(MirUI)) { memcpy(&gMirUI, d, sizeof(MirUI)); gMirUIn = 1; } break;
		case 'P': if (L == (int)sizeof(SPatch)) { SPatch sp; memcpy(&sp, d, sizeof(sp)); int drums = gDrums, kit = gKit, bpm = gBPM;
			patchApplyRaw(&sp); gBPM = bpm; if (gDrums != drums || gKit != kit) { gDrums = drums; gKit = kit; drumsChanged(); }   // the drums follow the 3DS's own state ('U')
			for (int k = 0; k < 128; k++) if (gMirHeld[k]) voiceOn(k, gMirHeld[k] / 150.0f);   // what was sounding goes on in the new sound
			} break;
		case 'Q': if (L == (int)sizeof(MirSeq)) { MirSeq q; memcpy(&q, d, sizeof(q)); memcpy(gSeq, q.st, sizeof(gSeq)); gSeqLen = q.len < 1 ? 16 : q.len > NSTEP ? NSTEP : q.len; gSeqBase = q.base;
			memcpy(gRowLen, q.rowLen, 8); memcpy(gRowHits, q.rowHits, 8); memcpy(gRowRot, q.rowRot, 8); memcpy(gRowNote, q.rowNote, 8); } break;
		case 'R': if (L == 2048) drumSnapSet(d); break;
		case 'V': { u8 on[128] = {0}; for (int k = 0; k < L; k++) on[d[k] & 127] = 1;
			for (int k = 0; k < NVS; k++) { SVoice* v = &gV[k]; if (v->active && !v->isEcho && v->stage != 4 && !on[v->note & 127]) { v->stage = v->stage2 = 4; } }
			for (int k = 0; k < 128; k++) if (!on[k]) gMirHeld[k] = 0; break; }
		}
	}
}
static void mirUiApply(void) {   // the mirror's main loop: the 3DS's drums, tempo and the rest of the state ('U')
	const MirUI* u = &gMirUI;
	if (u->theme >= 0 && u->theme < NTHEMES) gTheme = u->theme;
	gStyle = u->style; gPreset = u->preset; gBrFocus = u->brFocus; gVisMode = u->visMode; gVisOverlay = u->visOverlay; gPatView = u->patView; gStartMenu = u->startMenu; gStartSel = u->startSel;
	gQuickBar = u->quickBar; gQuickIdx = u->quickIdx; gSeqMode = u->seqMode; gSeqPos = u->seqPos; gSeqPlaying = u->seqMode == 1 && u->seqPos >= 0; gBPM = u->bpm; gPads = u->pads; gGridOn = u->gridOn;
	gPatCur = u->patCur; gLiveRec = u->liveRec; gPadSel = u->padSel; gProg = u->prog; gVoiceMode = u->voiceMode; gDrumStep = u->drumStep; gArp = u->arp; gVisT = u->visT; gSwing = u->swing;
	if (u->drums != gDrums || u->kit != gKit) { gKit = u->kit; gDrums = u->drums; drumsChanged(); }
}
static void drumGridMark(void) { static HistE e; histSnap(&e, 2, 0); histPush(gUndoH, &gUndoN, &e); gRedoN = 0; histFlags(); }
static int gHoldR = -1, gHoldI = -1, gHoldOff = 0, gHoldMoved = 0, gHoldX0 = 0, gHoldY0 = 0, gHoldVel = 0, gHoldV0 = 0; static u64 gHoldT = 0;   // a press waiting to become a HOLD
static int drumGridTouch(int x, int y, int press) {   // 1 = redraw
	static int paint = -1;
	if (gDgEd) return dgEdTouch(x, y, press);
	if (!press && gHoldR >= 0 && gHoldI >= 0 && gHoldOff && !gHoldVel) {   // a lit step, dragged straight up or down: its VELOCITY
		int dx = x - gHoldX0, dy = y - gHoldY0;
		if (abs(dy) > 8 && abs(dy) > abs(dx) * 2) { gHoldVel = 1; gHoldOff = 0; int rr = gHoldR; gHoldV0 = rr < 3 ? gMyDrum[rr][gHoldI] : gDrumX[rr - 3][gHoldI]; }
	}
	if (gHoldVel && gHoldR >= 0) {
		int v = gHoldV0 - (y - gHoldY0) * 2; v = v < 8 ? 8 : v > 255 ? 255 : v; u8* vp = gHoldR < 3 ? &gMyDrum[gHoldR][gHoldI] : &gDrumX[gHoldR - 3][gHoldI];
		if (*vp != v) { *vp = (u8)v; gDrumSaveDue = 1; char m[24]; snprintf(m, sizeof(m), "VELOCITY %d", v * 127 / 255); say(m); return 1; }
		return 0;
	}
	if (y < DG_TOP) {
		if (x >= 233 && x < 274) { if (!gDrFill) { gDrFill = 1; return 1; } return 0; }   // FILL: while held
		if (!press) return 0;
		if (x >= 274) { gAccMode = !gAccMode; say(gAccMode ? "ACCENT: TAP STEPS TO ACCENT THEIR COLUMN (ACC AGAIN: DONE)" : "ACCENT DONE"); return 1; }
		if (x < 55) { gDrums = !gDrums; drumsChanged(); return 1; }
		if (x < 110) { gKit = (gKit + 1) % NKIT; gDrumWant = gKit; return 1; }
		if (x < 153) { beatPaste(gBeatNext); gBeatNext = (gBeatNext + 1) % NBEAT; return 1; }   // BEAT: the next rhythm each tap
		if (x >= 192 && x < 233) {   // MIC: knocks near the mic play and record onto the picked row (tap a row's name to pick)
			if (gTapRow >= 0) { gTapRow = -1; gDrumRec = 0; gDrumSaveDue = 1; say("MIC TAP OFF"); }
			else { gTapRow = gDgSel; if (!gDrums) { gDrums = 1; drumsChanged(); } drumGridMark(); char m[48]; snprintf(m, sizeof(m), "MIC TAP: KNOCK FOR %s (TAP A NAME TO SWITCH)", kitPadName(DG_DRUM[gTapRow])); say(m); }
			return 1;
		}
		if (x < 192) {   // CLR: twice
			if (!clrArmed()) { gDrumClrArm = 1; gDrumClrT = osGetTime(); say("CLR: TAP A DRUM'S NAME TO CLEAR ITS ROW, CLR AGAIN FOR ALL"); return 1; }
			drumGridMark(); for (int r = 0; r < DG_ROWS; r++) drumRowClear(r); gDrumClrArm = 0; say("DRUMS CLEARED (SELECT+L UNDOES)"); return 1;
		}
		return 0;
	}
	if (y >= DG_PGY) return press ? dgPageTouch(x) : 0;   // the PAGE strip
	int r = (y - DG_TOP) / DG_RH; if (r < 0 || r >= DG_ROWS) return 0;
	if (gAccMode && x >= DG_LX) {   // ACC: a tap marks / unmarks the whole column
		if (!press) return 0;
		int i = (int)((x - DG_LX) / ((320 - DG_LX - 2) / 16.0f)); if (i < 0 || i > 15) return 0; i += DP0;
		drumGridMark(); gDrAcc[i] = !gDrAcc[i]; gDrumSaveDue = 1; return 1;
	}
	if (x < DG_LX) {   // the name: hear it and pick the row; after CLR: clear that row; HOLD: its SOUND
		if (!press) {
			if (gHoldR == r && gHoldI == -2 && osGetTime() - gHoldT > 450) { gHoldR = -1; gDgEd = 2; gDgEdR = r; gDgDrag = -1; return 1; }
			return 0;
		}
		gHoldR = r; gHoldI = -2; gHoldT = osGetTime();
		if (clrArmed()) { gDrumClrArm = 0; drumGridMark(); drumRowClear(r); char m[48]; snprintf(m, sizeof(m), "%s CLEARED (SELECT+L UNDOES)", kitPadName(DG_DRUM[r])); say(m); return 1; }
		gDgSel = r; if (gTapRow >= 0) gTapRow = r;   /* a tap plays the drum and picks the row (no double-tap clear: people tap to hear it) */
		if (gCsndOK && gDrumBuilt == gKit) { if (gDrumCh[0] < 0) drumPickChannels(); drumHit(DG_DRUM[r], 0.85f); csndExecCmds(false); } else if (gCsndOK) gDrumWant = gKit; return 1;
	}
	int i = (int)((x - DG_LX) / ((320 - DG_LX - 2) / 16.0f)); if (i < 0 || i > 15) return 0; i += DP0;
	if (press) gDrumClrArm = 0;   // painting steps: CLR is off again
	if (press) {   // an empty step goes on now; a lit one waits: let go = off, hold = its editor, drag = paint off along the row
		drumGridMark(); gHoldR = r; gHoldI = i; gHoldT = osGetTime(); gHoldMoved = 0; gHoldX0 = x; gHoldY0 = y; gHoldVel = 0;
		if (drumCellOn(r, i)) { gHoldOff = 1; paint = -1; } else { gHoldOff = 0; paint = 1; drumCellSet(r, i, 1); }
		return 1;
	}
	if (gHoldR >= 0 && gHoldI >= 0 && (r != gHoldR || i != gHoldI)) {   // moved off: it's a drag
		gHoldMoved = 1; if (gHoldOff) { drumCellSet(gHoldR, gHoldI, 0); paint = 0; gHoldOff = 0; } gHoldR = -1;
	}
	if (gHoldR >= 0 && gHoldI >= 0 && !gHoldMoved && !gHoldVel && osGetTime() - gHoldT > 450) {   // HOLD: the step's editor
		if (!drumCellOn(gHoldR, gHoldI)) drumCellSet(gHoldR, gHoldI, 1);
		gDgEd = 1; gDgEdR = gHoldR; gDgEdI = gHoldI; gHoldR = -1; gHoldOff = 0; paint = -1; gDgDrag = -1; return 1;
	}
	if (paint >= 0 && drumCellOn(r, i) != paint) { drumCellSet(r, i, paint); return 1; }
	return 0;
}
static int drumGridRelease(void) {   // the stylus came up
	int d = 0;
	if (gDrFill) { gDrFill = 0; d = 1; }
	if (gDgEd) return dgEdRelease() | d;
	if (gHoldR >= 0 && gHoldI >= 0 && gHoldOff) { drumCellSet(gHoldR, gHoldI, 0); d = 1; }   // a quick tap on a lit step: off
	gHoldR = -1; gHoldOff = 0; gHoldVel = 0;
	return d;
}
static void drumTapTick(float dt) {   // main thread (inside the voice lock), 60 times a second
	if (gDvPlay && gDrumWant < 0 && gCsndOK && gDrumBuilt == gKit) { if (gDrumCh[0] < 0) drumPickChannels(); drumHit(gDvPlay - 1, 0.85f); csndExecCmds(false); gDvPlay = 0; }   // hear the SOUND edit
	if (gTapRow < 0 && gDrumRec) gTapRow = gDrumRec - 1;   // MIC TAP from the menu / a patch: same thing
	if (gTapRow < 0 || !(gDrumOnB || gDrumInSeq) || !gDrums || !gMicOK || !gMicBuf || gDrumBuilt < 0) return;   // only on the DRUMS page: knocks elsewhere aren't drums
	u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2; const s16* m = (const s16*)gMicBuf;
	float pk = 0; for (int i = 0; i < 300; i++) { float v = fabsf(m[(end + N - 1 - i) % N] / 32768.0f); if (v > pk) pk = v; }   // last ~18 ms
	if (gTapHold > 0) { gTapHold -= dt; gTapLevel += (pk - gTapLevel) * 0.2f; return; }
	if (pk > 0.2f && pk > gTapLevel * 2.5f + 0.05f) {
		float vel = (pk - 0.2f) / 0.6f; if (vel > 1) vel = 1; vel = 0.3f + 0.7f * vel;
		int r = gTapRow;
		int st = gDrIdx[r] >= 0 ? gDrIdx[r] : (gDrumStep < 0 ? 0 : gDrumStep);   // the row's own step (its RATE / LENGTH)
		if (!gSeqPlaying && gDrRate[r] == 0) { float len = (gDrumStep & 1) ? 1.0f - gSwing : 1.0f + gSwing; if (gDrumPh > len * 0.5f) st = (st + 1) % (gDrLen[r] < 1 ? 16 : gDrLen[r]); }   // nearest 16th
		drumRowToMine(r); if (r >= 3) gDrumX[r - 3][st] = (u8)(vel * 255); else gMyDrum[r][st] = (u8)(vel * 255); gDrumSaveDue = 1;
		if (gCsndOK) { drumHit(DG_DRUM[r], vel); csndExecCmds(false); }
		gTapHold = 0.09f; gVisBeat = vel;
	}
	gTapLevel += (pk - gTapLevel) * 0.2f;
}
static void drawMasterPanel(void) {
	frame(4, 30, 196, 194);
	static const char* BL[8] = {"LOFI", "MOD FX", "EQ", "DELAY", "REVERB", "OTT", "PUMP", "LIMIT"};
	int on[8] = {gLofiBits || gLofiRate > 0, gModFx != 0, gEqLow != 0 || gEqMid != 0 || gEqHigh != 0, gDlyMix > 0 || gFx == 1, gRevMix > 0 || gFx == 3 || gFreeze, gOtt > 0, gPump > 0, 1};
	int act = masterActive();
	text(10, 36, act ? "WHOLE SOUND -> CHAIN" : (gMasterFX ? "WAITING FOR FX BUS" : "MASTER FX OFF"), 1, act ? TH->val[0] : TH->warn[0], act ? TH->val[1] : TH->warn[1], act ? TH->val[2] : TH->warn[2]);
	int ord[8], k = 0;   // in FX ORDER (SPACE = delay + reverb), then pump and limiter
	for (int f = 0; f < NFXB; f++) { int b = gFxOrder[f]; if (b == 3) { ord[k++] = 3; ord[k++] = 4; } else ord[k++] = b == 4 ? 5 : b; }
	ord[k++] = 6; ord[k++] = 7;
	for (int j = 0; j < 8; j++) {
		int i = ord[j], y = 52 + j * 16, bi = i <= 2 ? i : i <= 4 ? 3 : i == 5 ? 4 : -1, sel = gChSel >= 0 && bi == gChSel;
		if (on[i] && act) { rect(10, y - 2, 92, 12, HI); text(14, y, BL[i], 1, HITXT); }
		else { rect(10, y - 2, 92, 12, RGB3(TH->pad)); text(14, y, BL[i], 1, DIM); }
		if (sel) outline(8, y - 4, 96, 16, RGB3(TH->keyOn));   // the block being edited
		if (j < 7) rect(55, y + 10, 1, 4, LINE);
	}
	char s[32];
	if (gModFx) { snprintf(s, sizeof(s), "%s", MODFX_NAMES[gModFx]); text(110, 68, s, 1, TXT); }
	if (gShimmer) text(110, 116, "SHIMMER", 1, TXT);
	if (gFreeze) text(110, 126, "FREEZE", 1, WARN);
	float cpu = gFxTicks / (268111856.0f * 160 / DSPRATE) * 100;
	snprintf(s, sizeof(s), "FX CPU %d%%", (int)(cpu + 0.5f)); text(10, 186, s, 1, DIM);
	rect(10, 198, 184, 6, RGB3(TH->pad)); rect(10, 198, (int)(184 * (cpu > 100 ? 1 : cpu / 100)), 6, cpu > 60 ? TH->warn[0] : TH->txt[0], cpu > 60 ? TH->warn[1] : TH->txt[1], cpu > 60 ? TH->warn[2] : TH->txt[2]);
	text(10, 210, "SUB / DRUMS / TAPE: DRY", 1, DIM);
}
// GESTURE DEMO (top-screen help on the piano roll and drum pages): a little grid where a finger acts out each
// gesture in turn, with its name under it. 2.4 s a gesture.
static void demoFinger(int fx, int fy, int down) {
	const u8* c = TH->txt; rect(fx - 2, fy - 2, 5, 5, RGB3(c));
	if (down) { outline(fx - 5, fy - 5, 11, 11, RGB3(TH->val)); }
}
static void gestureDemo(int X, int Y, int roll) {
	enum { CW = 22, CH = 13, NC = 8 };
	int NR = roll ? 4 : 3, ng = roll ? 6 : 5;
	u64 ms = osGetTime(); int g = (int)((ms / 2400) % ng); float t = (ms % 2400) / 2400.0f;
	for (int r = 0; r < NR; r++) for (int c = 0; c < NC; c++) { int x = X + c * CW, y = Y + r * CH; rect(x, y, CW - 2, CH - 2, RGB3(c % 4 == 0 ? TH->grid : TH->pad)); }
	#define CX_(c) (X + (c) * CW + CW / 2 - 1)
	#define CY_(r) (Y + (r) * CH + CH / 2 - 1)
	#define CELL(c, r, w, hh, col) rect(X + (c) * CW, Y + (r) * CH + (CH - 2 - (hh)), (w) * CW - 2, (hh), RGB3(col))
	float u = t < 0.15f ? 0 : t > 0.85f ? 1 : (t - 0.15f) / 0.7f;   // the move, eased in the middle of the 2.4 s
	const char* cap1 = ""; const char* cap2 = "";
	if (!roll) switch (g) {
		case 0: if (t > 0.3f) CELL(2, 1, 1, CH - 2, TH->val); demoFinger(CX_(2), CY_(1), t > 0.25f && t < 0.45f); cap1 = "TAP A STEP:"; cap2 = "ON / OFF"; break;
		case 1: { int e = 1 + (int)(u * 5); for (int c = 1; c <= e; c++) CELL(c, 0, 1, CH - 2, TH->val); demoFinger(CX_(1) + (int)(u * 5 * CW), CY_(0), 1); cap1 = "DRAG ALONG A ROW:"; cap2 = "PAINT STEPS"; break; }
		case 2: { int h = CH - 2 - (int)(u * 8); CELL(3, 2, 1, h, TH->val); demoFinger(CX_(3), CY_(2) - 6 + (int)(u * 14), 1); cap1 = "PRESS, DRAG DOWN:"; cap2 = "SOFTER (UP = LOUDER)"; break; }
		case 3: { CELL(5, 1, 1, CH - 2, TH->val); int k = (int)(t * 1.6f * 10); if (k > 10) k = 10; rect(X + 5 * CW, Y + CH * 3 - 1, k * 2, 2, RGB3(TH->keyOn));
			if (t > 0.65f) for (int i = 0; i < 3; i++) rect(X + 5 * CW + 3 + i * 6, Y + CH + 2, 3, 3, RGB3(TH->ink));
			demoFinger(CX_(5), CY_(1), 1); cap1 = "HOLD A STEP:"; cap2 = "RATCHET, IF, VELOCITY"; break; }
		default: { int k = (int)(t * 1.6f * 10); if (k > 10) k = 10; rect(X - 2, Y, 2, CH * 3 - 2, RGB3(TH->keyOn)); rect(X - 2, Y + CH * 3 - 1, k * 2, 2, RGB3(TH->keyOn));
			demoFinger(X + 4, CY_(0), 1); cap1 = "HOLD A ROW'S NAME:"; cap2 = "ITS SOUND, RATE, LENGTH"; break; }
	} else switch (g) {
		case 0: if (t > 0.3f) CELL(2, 1, 1, CH - 2, TH->val); demoFinger(CX_(2), CY_(1), t > 0.25f && t < 0.45f); cap1 = "TAP A CELL:"; cap2 = "A NOTE IN (TAP IT: OUT)"; break;
		case 1: { int L = 1 + (int)(u * 3); CELL(1, 2, L, CH - 2, TH->val); demoFinger(CX_(1) + (int)(u * 3 * CW), CY_(2), 1); cap1 = "DRAG SIDEWAYS:"; cap2 = "A LONGER NOTE"; break; }
		case 2: { int c = 1 + (int)(u * 3 + 0.5f), r = 3 - (int)(u * 2 + 0.5f); CELL(c, r, 1, CH - 2, TH->val); demoFinger(CX_(1) + (int)(u * 3 * CW), CY_(3) - (int)(u * 2 * CH), 1); cap1 = "DRAG A NOTE:"; cap2 = "MOVE IT"; break; }
		case 3: { int h = CH - 2 - (int)(u * 8); CELL(4, 1, 1, h, TH->val); demoFinger(CX_(4), CY_(1) - 4 + (int)(u * 14), 1); cap1 = "DRAG A NOTE DOWN:"; cap2 = "SOFTER (UP = LOUDER)"; break; }
		case 4: { float a = t < 0.3f ? 0 : (t - 0.3f) / 0.6f; if (a > 1) a = 1; int fx = t < 0.3f ? CX_(1) : CX_(1) + (int)(a * 6 * CW), fy = t < 0.3f ? CY_(3) - (int)(t / 0.3f * 2 * CH) : CY_(1);
			for (int c = 2; c <= 6; c += 2) if (fx < CX_(c) - 4) CELL(c, 1, 1, CH - 2, TH->val);
			demoFinger(fx, fy, 1); cap1 = "EMPTY CELL, UP, THEN"; cap2 = "SCRIBBLE: ERASE NOTES"; break; }
		default: { CELL(5, 2, 1, CH - 2, TH->val); int k = (int)(t * 1.6f * 10); if (k > 10) k = 10; rect(X + 5 * CW, Y + CH * 4 - 1, k * 2, 2, RGB3(TH->keyOn));
			demoFinger(CX_(5), CY_(2), 1); cap1 = "HOLD A STEP:"; cap2 = "THE STEP EDITOR"; break; }
	}
	#undef CX_
	#undef CY_
	#undef CELL
	int cy = Y + NR * CH + 4; text(X, cy, cap1, 1, RGB3(TH->val)); text(X, cy + 10, cap2, 1, RGB3(TH->txt));
	for (int i = 0; i < ng; i++) rect(X + NC * CW - ng * 6 + i * 6, cy + 2, 4, 4, RGB3(i == g ? TH->keyOn : TH->grid));   // which of the gestures
}
static void drawDrumsPanel(void) {   // the top screen: what's playing and how to drive the grid (the sequencer itself is below)
	frame(4, 30, 196, 194);
	char s[40]; snprintf(s, sizeof(s), "%s KIT  %d BPM  %s", KIT_NAMES[gKit], gBPM, gDrums ? "ON" : "OFF"); text(10, 36, s, 1, gDrums ? TH->val[0] : TH->dim[0], gDrums ? TH->val[1] : TH->dim[1], gDrums ? TH->val[2] : TH->dim[2]);
	int r = gDgSel; snprintf(s, sizeof(s), "%s: %s, %d STEPS", kitPadName(DG_DRUM[r]), DRATE_NAMES[gDrRate[r]], gDrLen[r]); text(10, 52, s, 1, TXT);
	if (gSwing > 0.01f) { snprintf(s, sizeof(s), "SWING %d%%", (int)(gSwing * 100 + 0.5f)); text(10, 64, s, 1, DIM); }
	text(10, 78, "GRID: BOTTOM SCREEN", 1, RGB3(TH->dim));
	gestureDemo(14, 96, 0);
	text(10, 210, gSeqPlaying ? "FOLLOWING THE SEQUENCER" : "OWN CLOCK (BPM, SWING)", 1, DIM);
}

// ---------------------------------------------------------------- graphics chip (PICA200) visuals
// The UI is still drawn by the CPU. The left panel of the top screen is rendered by the GPU through
// citro2d into an off-screen target at 2x size; the display-transfer engine scales it down (free
// anti-aliasing) into a linear buffer laid out like the framebuffer, which is then copied into the
// panel. Glow and trails use additive blending, done by the GPU.
#define VIS_W 200      // screen columns covered (x VIS_X .. VIS_X+VIS_W), multiple of 8
#define VIS_X 4
#define VIS_SS 2
static C3D_RenderTarget* gVisTgt = NULL;
static C3D_RenderTarget* gFullTgt = NULL;
static u8* gFullLin = NULL;
static u8* gVisLin = NULL;
static float gVisEnv = 0;
#define TRAILS 10
#define TPTS 64
static float gTrail[2][TRAILS][TPTS], gTrailPos[2][TRAILS];
static int gTrailHead[2], gTrailN[2];
static float gTrailLastPos[2] = {-1, -1}, gTrailLastSum[2];

static void gpuInit(void) {
	if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return;
	if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { C3D_Fini(); return; }
	C2D_Prepare();
	gVisTgt = C3D_RenderTargetCreate(240 * VIS_SS, VIS_W * VIS_SS, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	gVisLin = (u8*)linearAlloc(240 * VIS_W * 3);
	gFullTgt = C3D_RenderTargetCreate(240 * VIS_SS, 400 * VIS_SS, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	gFullLin = (u8*)linearAlloc(240 * 400 * 3);
	gGpuOK = gVisTgt && gVisLin && gFullTgt && gFullLin;
}
static void gpuExit(void) {
	if (gVisTgt) C3D_RenderTargetDelete(gVisTgt);
	if (gVisLin) linearFree(gVisLin);
	if (gFullTgt) C3D_RenderTargetDelete(gFullTgt);
	if (gFullLin) linearFree(gFullLin);
	if (gGpuHung) return;   // a stuck GPU queue would block C3D_Fini forever
	if (gGpuOK || gVisTgt) { C2D_Fini(); C3D_Fini(); }
}

static inline u32 rgba(float r, float g, float b, float a) {
	if (r > 1) r = 1;
	if (g > 1) g = 1;
	if (b > 1) b = 1;
	if (a > 1) a = 1;
	if (a < 0) a = 0;
	return C2D_Color32f(r, g, b, a);
}
static void vLine(float x0, float y0, float x1, float y1, u32 c, float th) { C2D_DrawLine(x0 * VIS_SS, y0 * VIS_SS, c, x1 * VIS_SS, y1 * VIS_SS, c, th * VIS_SS, 0.5f); }
static void vTri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) { C2D_DrawTriangle(x0 * VIS_SS, y0 * VIS_SS, c, x1 * VIS_SS, y1 * VIS_SS, c, x2 * VIS_SS, y2 * VIS_SS, c, 0.5f); }
static void vRect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x * VIS_SS, y * VIS_SS, 0.5f, w * VIS_SS, h * VIS_SS, c); }
static void blendAdd(int on) {
	C2D_Flush();
	if (on) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
	else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}
// amber -> magenta -> cyan as the filter opens / the envelope rises
static void hue(float t, float* r, float* g, float* b) {   // EGA: snap to the theme's 4-colour ramp
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	if (TH->ega) { int i = (int)(t * 3.999f); const u8* c = EGA[TH->acc[i]]; *r = c[0] / 255.0f; *g = c[1] / 255.0f; *b = c[2] / 255.0f; return; }
	if (TH->rampOn) { float x = t * 2; int i = x >= 1 ? 1 : 0; float u = x - i; const u8* a = TH->ramp[i]; const u8* b2 = TH->ramp[i + 1];
		*r = (a[0] + (b2[0] - a[0]) * u) / 255.0f; *g = (a[1] + (b2[1] - a[1]) * u) / 255.0f; *b = (a[2] + (b2[2] - a[2]) * u) / 255.0f; return; }
	if (t < 0.5f) { float u = t * 2; *r = 1.0f; *g = 0.62f * (1 - u) + 0.3f * u; *b = 0.15f * (1 - u) + 0.8f * u; }
	else { float u = (t - 0.5f) * 2; *r = 1.0f * (1 - u) + 0.3f * u; *g = 0.3f * (1 - u) + 0.95f * u; *b = 0.8f * (1 - u) + 1.0f * u; }
}
static void glowLine(const float* xs, const float* ys, int n, float r, float g, float b, float k) {
	blendAdd(1);
	static const float THK[3] = {7.0f, 3.5f, 1.4f}, AL[3] = {0.12f, 0.28f, 1.0f};
	for (int p = 0; p < 3; p++) {
		u32 c = rgba(r * (p == 2 ? 1 : 0.9f) + (p == 2 ? 0.25f : 0), g + (p == 2 ? 0.2f : 0), b + (p == 2 ? 0.2f : 0), AL[p] * k);
		for (int i = 0; i + 1 < n; i++) vLine(xs[i], ys[i], xs[i + 1], ys[i + 1], c, THK[p]);
	}
	blendAdd(0);
}
static void sampleCycle(const float* cyc, float* out, float* peakOut) {
	float pk = 0;
	for (int i = 0; i < CYC; i += 8) { float v = fabsf(cyc[i]); if (v > pk) pk = v; }
	if (pk < 1e-4f) pk = 1;
	for (int i = 0; i < TPTS; i++) { float v = cyc[i * (CYC - 1) / (TPTS - 1)] / pk; out[i] = (v == v && v < 4 && v > -4) ? v : 0; }
	if (peakOut) *peakOut = pk;
}
static void trailPush(int o, float pos) {
	float s[TPTS]; sampleCycle(gOsc[o].cyc, s, NULL);
	float sum = 0; for (int i = 0; i < TPTS; i += 4) sum += s[i];
	if (fabsf(pos - gTrailLastPos[o]) < 0.004f && fabsf(sum - gTrailLastSum[o]) < 0.05f) return;
	gTrailLastPos[o] = pos; gTrailLastSum[o] = sum;
	gTrailHead[o] = (gTrailHead[o] + 1) % TRAILS;
	memcpy(gTrail[o][gTrailHead[o]], s, sizeof(s)); gTrailPos[o][gTrailHead[o]] = pos;
	if (gTrailN[o] < TRAILS) gTrailN[o]++;
}

// 3D wavetable: slices stacked in depth, drawn back to front as solid ribbons, slowly swaying
typedef struct { float yaw, cx, cy, k, cs, sn; } Proj;
static void proj(const Proj* P, float xn, float v, float z, float* sx, float* sy) {
	float X = xn * 74 * P->k, Z = (z - 0.5f) * 120 * P->k, Y = v * 26 * P->k;
	float cs = P->cs, sn = P->sn;
	float xr = X * cs - Z * sn, zr = X * sn + Z * cs;
	float p = 1.0f / (1.0f + zr / (420.0f * P->k));
	*sx = P->cx + xr * p; *sy = P->cy - Y * p - zr * 0.52f * p;
}
static void visWavetable(int o, float cx, float cy, float k) {
	Osc* os = &gOsc[o]; Table* tb = &os->tb;
	float cr, cg, cb; hue(gCutoff * 0.7f + gVisEnv * 0.3f, &cr, &cg, &cb);
	Proj P = {0.55f + 0.22f * sinf(gVisT * 0.45f), cx, cy, k};
	P.cs = cosf(P.yaw); P.sn = sinf(P.yaw);
	int nf = tb->nFrames < 1 ? 1 : tb->nFrames, S = nf < 20 ? nf : 20;
	float pk = tb->peak > 1e-4f ? tb->peak : 1;
	float xs[48], ys[48], bx[48], by[48];
	for (int s = S - 1; s >= 0; s--) {
		float z = S > 1 ? s / (float)(S - 1) : 0;
		int f = S > 1 ? (int)(z * (nf - 1) + 0.5f) : 0;
		const float* fr = tb->frames + f * CYC;
		for (int i = 0; i < 48; i++) {
			float xn = i / 47.0f * 2 - 1, v = os->on ? fr[i * (CYC - 1) / 47] / pk : 0;
			proj(&P, xn, v, z, &xs[i], &ys[i]);
			proj(&P, xn, -1.15f, z, &bx[i], &by[i]);
		}
		float near = 1.0f - fabsf(z - gPosEff[o]) * 6; if (near < 0) near = 0;
		float depth = 0.35f + 0.65f * (1 - z);
		u32 fill = TH->ega ? rgba(TH->bg[0] / 255.0f, TH->bg[1] / 255.0f, TH->bg[2] / 255.0f, 0.97f) : rgba(0.07f + 0.05f * near, 0.045f + 0.03f * near, 0.02f, 0.96f);
		for (int i = 0; i + 1 < 48; i++) { vTri(xs[i], ys[i], xs[i + 1], ys[i + 1], bx[i], by[i], fill); vTri(xs[i + 1], ys[i + 1], bx[i + 1], by[i + 1], bx[i], by[i], fill); }
		u32 lc = rgba(cr * depth * (0.45f + 0.55f * near), cg * depth * (0.45f + 0.55f * near), cb * depth * (0.45f + 0.55f * near), 1);
		for (int i = 0; i + 1 < 48; i++) vLine(xs[i], ys[i], xs[i + 1], ys[i + 1], lc, (0.9f + near) * (k > 1 ? 1.4f : 1));
	}
	// trails of where the (warped) cycle has been, then the live cycle with glow
	blendAdd(1);
	for (int k = 1; k < gTrailN[o]; k++) {
		int idx = (gTrailHead[o] - k + TRAILS) % TRAILS;
		float a = 0.45f * (1 - k / (float)TRAILS);
		u32 c = rgba(cr, cg, cb, a);
		float px = 0, py = 0;
		for (int i = 0; i < TPTS; i++) {
			float sx, sy; proj(&P, i / (float)(TPTS - 1) * 2 - 1, gTrail[o][idx][i], gTrailPos[o][idx], &sx, &sy);
			if (i) vLine(px, py, sx, sy, c, 1.2f);
			px = sx; py = sy;
		}
	}
	blendAdd(0);
	float cyc[TPTS]; sampleCycle(os->cyc, cyc, NULL);
	float lx[TPTS], ly[TPTS];
	for (int i = 0; i < TPTS; i++) proj(&P, i / (float)(TPTS - 1) * 2 - 1, os->on ? cyc[i] : 0, gPosEff[o], &lx[i], &ly[i]);
	glowLine(lx, ly, TPTS, cr, cg, cb, 0.8f + gVisEnv * 0.6f);
}

// scope boxes for OSC A and B with phosphor-style trails
static void visScopes(float X0, float Y0, float W, float H) {
	float cr, cg, cb; hue(gCutoff * 0.7f + gVisEnv * 0.3f, &cr, &cg, &cb);
	for (int o = 0; o < 2; o++) {
		float h = (H - 4) / 2, y0 = Y0 + o * (h + 4), mid = y0 + h / 2;
		vRect(X0, y0, W, h, rgba(TH->pad[0] / 255.0f, TH->pad[1] / 255.0f, TH->pad[2] / 255.0f, 1));
		u32 grid = rgba(TH->grid[0] / 255.0f, TH->grid[1] / 255.0f, TH->grid[2] / 255.0f, 1);
		for (int g = 1; g < 6; g++) vLine(X0 + g * W / 6.0f, y0, X0 + g * W / 6.0f, y0 + h, grid, 0.6f);
		vLine(X0, mid, X0 + W, mid, grid, 0.8f);
		if (!gOsc[o].on) continue;
		float amp = (h / 2 - 6) * (0.75f + 0.25f * gVisEnv);
		blendAdd(1);
		for (int k = 1; k < gTrailN[o]; k++) {
			int idx = (gTrailHead[o] - k + TRAILS) % TRAILS;
			u32 c = rgba(cr, cg, cb, 0.35f * (1 - k / (float)TRAILS));
			for (int i = 0; i + 1 < TPTS; i++)
				vLine(X0 + i * W / (TPTS - 1), mid - gTrail[o][idx][i] * amp, X0 + (i + 1) * W / (TPTS - 1), mid - gTrail[o][idx][i + 1] * amp, c, 1.0f);
		}
		blendAdd(0);
		float s[TPTS], xs[TPTS], ys[TPTS]; sampleCycle(gOsc[o].cyc, s, NULL);
		for (int i = 0; i < TPTS; i++) { xs[i] = X0 + i * W / (TPTS - 1); ys[i] = mid - s[i] * amp; }
		glowLine(xs, ys, TPTS, cr, cg, cb, 0.75f + gVisEnv * 0.6f);
	}
}

// ---- full-screen visualizer (its own 800x480 target, scaled to 400x240)
#define ORB_T 14
static float gOrbX[ORB_T][TPTS], gOrbY[ORB_T][TPTS];
static int gOrbHead = 0, gOrbN = 0;

static void visOrbit(float cx, float cy, float R) {
	float cr, cg, cb; hue(gCutoff * 0.6f + gVisEnv * 0.4f, &cr, &cg, &cb);
	float a[TPTS], b[TPTS]; sampleCycle(gOsc[0].cyc, a, NULL);
	if (gOsc[1].on) sampleCycle(gOsc[1].cyc, b, NULL);
	else for (int i = 0; i < TPTS; i++) b[i] = a[(i + TPTS / 4) % TPTS];   // phase portrait when OSC B is off
	float ang = gVisT * 0.35f, cs = cosf(ang), sn = sinf(ang), r = R * (0.75f + 0.25f * gVisEnv);
	gOrbHead = (gOrbHead + 1) % ORB_T; if (gOrbN < ORB_T) gOrbN++;
	for (int i = 0; i < TPTS; i++) {
		float x = a[i] * r, y = b[i] * r;
		gOrbX[gOrbHead][i] = cx + x * cs - y * sn; gOrbY[gOrbHead][i] = cy + (x * sn + y * cs) * 0.85f;
	}
	blendAdd(1);
	for (int k = gOrbN - 1; k >= 1; k--) {   // older shapes fade out: spirograph trails
		int idx = (gOrbHead - k + ORB_T) % ORB_T;
		float t = 1 - k / (float)ORB_T, h2 = fmodf(gCutoff * 0.6f + k * 0.04f, 1.0f);
		float r2, g2, b2; hue(h2, &r2, &g2, &b2);
		u32 c = rgba(r2, g2, b2, 0.32f * t);
		for (int i = 0; i < TPTS; i++) { int j = (i + 1) % TPTS; vLine(gOrbX[idx][i], gOrbY[idx][i], gOrbX[idx][j], gOrbY[idx][j], c, 1.2f); }
	}
	blendAdd(0);
	float xs[TPTS + 1], ys[TPTS + 1];
	for (int i = 0; i <= TPTS; i++) { xs[i] = gOrbX[gOrbHead][i % TPTS]; ys[i] = gOrbY[gOrbHead][i % TPTS]; }
	glowLine(xs, ys, TPTS + 1, cr, cg, cb, 0.85f + gVisEnv * 0.6f);
}
static void visSpectrum(float X0, float Y0, float W, float H, int o) {
	Mips* m = &gOsc[o].mips;
	enum { NB = 48 };
	float mag[NB + 1], mx = 1e-6f;
	for (int k = 1; k <= NB; k++) { float re = m->spec[k].re, im = m->spec[k].im; mag[k] = sqrtf(re * re + im * im); if (mag[k] > mx) mx = mag[k]; }
	float base = Y0 + H * 0.72f, bw = W / NB;
	for (int k = 1; k <= NB; k++) {
		float db = 20 * log10f(mag[k] / mx + 1e-5f);   // 0 .. -100
		float h = (db + 60) / 60; if (h < 0) h = 0;
		h *= H * 0.68f * (0.8f + 0.2f * gVisEnv) * (gOsc[o].on ? 1 : 0);
		float r, g, b; hue((k - 1) / (float)NB * 0.9f + gCutoff * 0.2f, &r, &g, &b);
		float x = X0 + (k - 1) * bw;
		vRect(x + 1, base - h, bw - 2, h, rgba(r * 0.55f, g * 0.55f, b * 0.55f, 1));
		vRect(x + 1, base + 2, bw - 2, h * 0.3f, rgba(r * 0.18f, g * 0.18f, b * 0.18f, 1));   // reflection
		blendAdd(1);
		vRect(x, base - h - 2, bw, 3, rgba(r, g, b, 0.9f));
		vRect(x - 1, base - h - 4, bw + 2, 7, rgba(r, g, b, 0.18f));
		blendAdd(0);
	}
}
// sparks: bursts on new notes and on arp / sequencer steps
#define NPART 160
typedef struct { float x, y, vx, vy, life, col; } Part;
static Part gPart[NPART];
static void partSpawn(int n) {
	for (int i = 0; i < NPART && n > 0; i++) if (gPart[i].life <= 0) {
		float a = (rand() % 6283) / 1000.0f, sp = 40 + rand() % 120;
		gPart[i] = (Part){200 + (rand() % 120 - 60), 120 + (rand() % 60 - 30), cosf(a) * sp, sinf(a) * sp - 30, 1.0f, (rand() % 100) / 100.0f}; n--;
	}
}
static void partDraw(float dt) {
	blendAdd(1);
	for (int i = 0; i < NPART; i++) {
		Part* p = &gPart[i]; if (p->life <= 0) continue;
		p->x += p->vx * dt; p->y += p->vy * dt; p->vy += 40 * dt; p->vx *= 0.99f; p->life -= dt * 0.7f;
		float r, g, b; hue(p->col, &r, &g, &b); float sz = 1.5f + 3 * p->life;
		vRect(p->x - sz, p->y - sz, sz * 2, sz * 2, rgba(r, g, b, p->life * 0.35f));
		vRect(p->x - 1, p->y - 1, 2, 2, rgba(1, 1, 1, p->life));
	}
	blendAdd(0);
}
// ---- TRACKER: a Scream Tracker 3 pattern screen. The sequencer and the drum grid as tracker rows (note, volume),
// scrolling with the play position, the row being played in the bar, and the channel VU meters under it. The GPU
// draws the panels and meters; the text goes on with the CPU after the frame is copied (trackerText).
#define TRK_CH 6
#define TRK_ROWH 10
#define TRK_Y0 44
static float gTrkVu[TRK_CH];
static const char* TRK_CHN[TRK_CH] = {"SYN 1", "SYN 2", "KICK", "SNARE", "HAT", "CLAP"};
static int trkRows(int big) { return big ? 8 : 16; }
static int trkNow(void) { return gSeqPlaying && gSeqPos >= 0 ? gSeqPos % NSTEP : (gDrums && gDrumStep >= 0 ? gDrumStep % 16 : -1); }
static void trkMeters(void) {   // the meters: hits push them up, they fall back
	static int lastHit[4] = {-1, -1, -1, -1}; static int lastPos = -2; const int D[4] = {0, 1, 2, 4};   // kick, snare, hat, clap
	for (int c = 0; c < TRK_CH; c++) gTrkVu[c] *= 0.88f;
	for (int k = 0; k < 4; k++) { int n = gDrumHitN[D[k]]; if (lastHit[k] >= 0 && n != lastHit[k]) gTrkVu[2 + k] = 1; lastHit[k] = n; }
	int p = trkNow();
	if (p != lastPos && p >= 0 && gSeqPlaying) { const Step* st = &gSeq[p]; if (st->n > 0) gTrkVu[0] = 0.4f + 0.6f * st->vel / 255.0f; if (st->n > 1) gTrkVu[1] = 0.4f + 0.6f * st->vel / 255.0f; }
	lastPos = p;
	float lv = gAudioRms * 4; if (lv > 1) lv = 1; if (lv > gTrkVu[0]) gTrkVu[0] = lv;   // whatever you play live shows on SYNTH 1
}
static void visTracker(int big) {
	trkMeters();
	vRect(0, 0, 400, 240, rgba(0.0f, 0.0f, 0.09f, 1));
	vRect(0, 0, 400, 41, rgba(0.2f, 0.2f, 0.36f, 1)); vLine(0, 41, 400, 41, rgba(0.55f, 0.55f, 0.75f, 1), 1);
	int R = trkRows(big), mid = R / 2, now = trkNow(), top = (now < 0 ? 0 : now) - mid;
	for (int i = 0; i < R; i++) {
		int row = ((top + i) % NSTEP + NSTEP) % NSTEP; float y = TRK_Y0 + i * TRK_ROWH;
		if (row % 4 == 0) vRect(0, y - 1, 400, TRK_ROWH, rgba(0.05f, 0.05f, 0.16f, 1));
		if (i == mid && now >= 0) vRect(0, y - 1, 400, TRK_ROWH, rgba(0.13f, 0.27f, 0.62f, 1));
	}
	float yb = TRK_Y0 + R * TRK_ROWH + 2;
	for (int c = 0; c <= TRK_CH; c++) { float x = 22 + c * 58; vLine(x, 31, x, yb, rgba(0.3f, 0.3f, 0.5f, 1), 1); }
	vLine(0, yb, 400, yb, rgba(0.55f, 0.55f, 0.75f, 1), 1);
	float vh = 238 - yb - 14;   // the meters: green, yellow at the top, red past it, in blocks like the DOS ones
	for (int c = 0; c < TRK_CH; c++) {
		float x = 26 + c * 58, w = 50, lv = gTrkVu[c]; int nb = (int)(vh / 4), on = (int)(lv * nb + 0.5f);
		for (int k = 0; k < nb; k++) {
			float f = k / (float)nb, r = f > 0.85f ? 0.95f : f > 0.6f ? 0.95f : 0.2f, g = f > 0.85f ? 0.15f : f > 0.6f ? 0.85f : 0.85f, bb = 0.15f;
			float a = k < on ? 1.0f : 0.12f; vRect(x, 236 - (k + 1) * 4, w, 3, rgba(r, g, bb, a));
		}
	}
	if (big) {   // the big one: the output scope across the meters
		const s16* w = gOsc[gVisOsc].mips.lev[2]; int N = LEVN[2]; float px = 0, py = 0, cy = yb + vh / 2 + 6;
		blendAdd(1);
		for (int i = 0; i <= 96; i++) { float x = 4 + i * 392.0f / 96, y = cy - w[(i * N / 96) % N] / 32768.0f * vh * 0.4f * (0.3f + gTrkVu[0]); if (i) vLine(px, py, x, y, rgba(0.6f, 0.8f, 1, 0.7f), 1.5f); px = x; py = y; }
		blendAdd(0);
	}
}
static void trkNote(char* o, int n) { if (n < 0 || n > 127) { strcpy(o, "..."); return; } const char* nm = NOTE_NAMES[n % 12]; snprintf(o, 4, "%c%c%d", nm[0], nm[1] ? nm[1] : '-', n / 12 - 1 < 0 ? 0 : (n / 12 - 1) % 10); }
static void trackerText(int big) {   // CPU, straight onto the top screen after the GPU frame is copied there
	char s[64]; int R = trkRows(big), mid = R / 2, now = trkNow(), top = (now < 0 ? 0 : now) - mid;
	snprintf(s, sizeof(s), "PAT %c  ROW %02d  BPM %d", 'A' + gPatCur, now < 0 ? 0 : now, gBPM); text(396 - (int)strlen(s) * 8, 20, s, 1, 255, 255, 255);
	text(4, 20, "SYRUP TRACKER", 1, 150, 150, 200);
	for (int c = 0; c < TRK_CH; c++) text(26 + c * 58, 32, TRK_CHN[c], 1, 170, 170, 220);
	for (int i = 0; i < R; i++) {
		int row = ((top + i) % NSTEP + NSTEP) % NSTEP, y = TRK_Y0 + i * TRK_ROWH, hl = i == mid && now >= 0;
		snprintf(s, sizeof(s), "%02d", row); text(4, y, s, 1, hl ? 255 : 120, hl ? 255 : 120, hl ? 255 : 170);
		const Step* st = &gSeq[row]; int live = row < gSeqLen;
		for (int c = 0; c < 2; c++) {
			char nt[4]; int on = live && st->n > c; if (on) trkNote(nt, st->note[c] + gSeqTr); else strcpy(nt, "...");
			snprintf(s, sizeof(s), "%s %02X", nt, on ? st->vel >> 2 : 0); if (!on) { s[4] = s[5] = '.'; }
			text(26 + c * 58, y, s, 1, on ? (hl ? 255 : 230) : 70, on ? (hl ? 255 : 230) : 70, on ? (hl ? 255 : 120) : 110);
		}
		const u8 dv[4] = {gMyDrum[0][row], gMyDrum[1][row], gMyDrum[2][row], gDrumX[1][row]};
		for (int c = 0; c < 4; c++) {
			int on = gDrums && dv[c] > 0; snprintf(s, sizeof(s), on ? "C-5 %02X" : "... ..", dv[c] >> 2);
			text(26 + (c + 2) * 58, y, s, 1, on ? (hl ? 255 : 120) : 70, on ? 255 : 70, on ? (hl ? 255 : 200) : 110);
		}
	}
}
// ---- more full-screen modes: all geometry on the GPU, the CPU only works out a few hundred points
// TUNNEL: the waveform wrapped into rings that fly toward you; the tunnel bends with the LFO / wavetable position
static void visTunnel(void) {
	float s[TPTS]; sampleCycle(gOsc[gVisOsc].cyc, s, NULL);
	float spd = 0.35f + gVisEnv * 1.2f; static float z0 = 0; z0 += spd / 60.0f; if (z0 > 1) z0 -= 1;
	enum { NR = 14, NP = 48 };
	blendAdd(1);
	for (int k = NR - 1; k >= 0; k--) {
		float z = (k + 1 - z0) / NR;   // 1 far .. 0 near
		float rad = 18 + 260 * (1 - z) * (1 - z), a = 1 - z;
		float bx = 200 + sinf(gVisT * 0.7f + z * 3) * 40 * z + (gPosEff[gVisOsc] - 0.5f) * 80 * z, by = 120 + cosf(gVisT * 0.5f + z * 2.5f) * 26 * z;
		float r, g, b; hue(fmodf(z + gCutoff * 0.5f + gVisT * 0.05f, 1.0f), &r, &g, &b);
		u32 c = rgba(r, g, b, (0.15f + 0.6f * a) * (0.6f + 0.4f * gVisEnv));
		float rot = gVisT * 0.3f + z * 1.5f, px = 0, py = 0;
		for (int i = 0; i <= NP; i++) {
			float t = (i % NP) / (float)NP, ang = t * TWO_PI + rot, v = s[(int)(t * (TPTS - 1))];
			float rr = rad * (1 + v * 0.18f);
			float x = bx + cosf(ang) * rr, y = by + sinf(ang) * rr * 0.8f;
			if (i) vLine(px, py, x, y, c, 0.6f + 2.2f * a);
			px = x; py = y;
		}
	}
	blendAdd(0);
}
// KALEIDO: the live waveform (and OSC B against it) mirrored into 8 rotating wedges
static void visKaleido(void) {
	float a[TPTS], b[TPTS]; sampleCycle(gOsc[0].cyc, a, NULL);
	if (gOsc[1].on) sampleCycle(gOsc[1].cyc, b, NULL); else for (int i = 0; i < TPTS; i++) b[i] = a[(i + TPTS / 3) % TPTS];
	float R = 110 * (0.8f + 0.3f * gVisEnv), rot = gVisT * 0.25f;
	enum { NW = 8, NP = 40 };
	float xs[NP], ys[NP];
	for (int i = 0; i < NP; i++) {   // one wedge's curve in polar form: radius from A, angle wobble from B
		int j = i * (TPTS - 1) / (NP - 1); float t = i / (float)(NP - 1);
		float rr = R * (0.15f + 0.85f * t) * (1 + a[j] * 0.25f), an = (b[j] * 0.35f) * (TWO_PI / NW) * 0.5f;
		xs[i] = rr; ys[i] = an;
	}
	blendAdd(1);
	for (int w = 0; w < NW * 2; w++) {
		float base = rot + (w >> 1) * TWO_PI / NW, sg = (w & 1) ? -1 : 1;
		float r, g, bb; hue(fmodf(w / (float)(NW * 2) * 0.5f + gCutoff * 0.6f + gVisEnv * 0.2f, 1.0f), &r, &g, &bb);
		u32 c = rgba(r, g, bb, 0.55f + 0.35f * gVisEnv), cg = rgba(r, g, bb, 0.12f);
		float px = 0, py = 0;
		for (int i = 0; i < NP; i++) {
			float an = base + sg * (ys[i] + TWO_PI / NW * 0.25f);
			float x = 200 + cosf(an) * xs[i], y = 120 + sinf(an) * xs[i] * 0.9f;
			if (i) { vLine(px, py, x, y, cg, 5.0f); vLine(px, py, x, y, c, 1.3f); }
			px = x; py = y;
		}
	}
	blendAdd(0);
	float r, g, bb; hue(gVisEnv, &r, &g, &bb);
	blendAdd(1); vRect(196, 116, 8, 8, rgba(r, g, bb, 0.5f + gVisEnv * 0.5f)); blendAdd(0);
}
// RIPPLES: every note drops a ring shaped by the waveform, coloured by its pitch, spreading and fading
#define NRIP 24
typedef struct { float x, y, r, life, hue; } Rip;
static Rip gRip[NRIP]; static int gRipN = 0;
static void visRipples(void) {
	static unsigned lastId = 0;
	for (int i = 0; i < NVS; i++) {
		SVoice* v = &gV[i]; if (!v->active || v->isEcho || v->id <= lastId) continue;
		Rip* p = &gRip[gRipN++ % NRIP];
		float n = log2f(v->cur / 32.7f) / 7.0f; if (n < 0) n = 0; if (n > 1) n = 1;
		p->x = 40 + n * 320 + (rand() % 40 - 20); p->y = 70 + (rand() % 100); p->r = 4; p->life = 1; p->hue = n;
	}
	for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].id > lastId) lastId = gV[i].id;
	float s[TPTS]; sampleCycle(gOsc[gVisOsc].cyc, s, NULL);
	enum { NP = 40 };
	blendAdd(1);
	for (int k = 0; k < NRIP; k++) {
		Rip* p = &gRip[k]; if (p->life <= 0) continue;
		p->r += 1.6f + 1.2f * gVisEnv; p->life -= 1.0f / 110;
		float r, g, b; hue(p->hue, &r, &g, &b);
		for (int ring = 0; ring < 3; ring++) {
			float rr = p->r - ring * 9; if (rr <= 1) continue;
			u32 c = rgba(r, g, b, p->life * (0.7f - ring * 0.2f));
			float px = 0, py = 0;
			for (int i = 0; i <= NP; i++) {
				float t = (i % NP) / (float)NP, ang = t * TWO_PI;
				float q = rr * (1 + s[(int)(t * (TPTS - 1))] * 0.12f * p->life);
				float x = p->x + cosf(ang) * q, y = p->y + sinf(ang) * q * 0.55f;   // flattened: rings lying on water
				if (i) vLine(px, py, x, y, c, 1.4f - ring * 0.3f);
				px = x; py = y;
			}
		}
	}
	blendAdd(0);
}

// PLASMA: the PICA200's procedural-texture unit makes the picture by itself (noise-bent rings looked up in a
// 128-colour table); the CPU only rotates the colour table and nudges the noise phase each frame (palette cycling).
// Plasma: a grid of colour-blended triangles, the colours from the theme.
static float gPlasPh = 0, gPlasNz = 0, gPlasLvl = 0;
static void visPlasma(C3D_RenderTarget* tg, float dt) {
	(void)tg;   // drawn with plain citro2d triangles: the old ProcTex path could hang the GPU and take sound and controls with it
	float lvl = gAudioRms * 5.0f; if (lvl > 1) lvl = 1; if (gVisEnv * 0.8f > lvl) lvl = gVisEnv * 0.8f;
	gPlasLvl += (lvl - gPlasLvl) * (lvl > gPlasLvl ? 0.5f : 0.08f);
	gPlasPh += dt * (0.08f + gPlasLvl * 0.9f) + gVisBeat * 0.02f; gPlasPh -= floorf(gPlasPh);
	gPlasNz += dt * (0.25f + gPlasLvl * 1.5f);
	// palette: the theme's background, line, value and highlight colours, cycled
	u32 pal[64]; const u8* K[4] = {TH->bg, TH->line, TH->val, TH->keyOn};
	float glow = 0.55f + 0.45f * gPlasLvl;
	for (int i = 0; i < 64; i++) {
		float t = i / 64.0f + gPlasPh; t -= floorf(t); float f = t * 4; int a = (int)f % 4, b = (a + 1) % 4; float u = f - floorf(f); u = u * u * (3 - 2 * u);
		int r = (int)((K[a][0] + (K[b][0] - K[a][0]) * u) * glow), g = (int)((K[a][1] + (K[b][1] - K[a][1]) * u) * glow), bl = (int)((K[a][2] + (K[b][2] - K[a][2]) * u) * glow);
		pal[i] = (u32)(r > 255 ? 255 : r) | ((u32)(g > 255 ? 255 : g) << 8) | ((u32)(bl > 255 ? 255 : bl) << 16) | 0xFF000000u;
	}
	enum { PGX = 20, PGY = 12 };
	static u32 col[PGY + 1][PGX + 1];
	float T = gPlasNz, zoom = 1.6f + 0.25f * sinf(gVisT * 0.13f), ox = 0.15f * sinf(gVisT * 0.21f), oy = 0.12f * cosf(gVisT * 0.17f);
	for (int j = 0; j <= PGY; j++) for (int i = 0; i <= PGX; i++) {
		float x = ((float)i / PGX - 0.5f) * zoom * 1.66f * 6 + ox * 6, y = ((float)j / PGY - 0.5f) * zoom * 6 + oy * 6;
		float v = sinf(x + T) + sinf(y * 1.3f - T * 0.7f) + sinf((x + y) * 0.7f + T * 0.5f) + sinf(sqrtf(x * x + y * y + 1) * 1.5f - T * 1.2f);
		int k = (int)((v + 4) * 8 * (1 + 0.3f * gPlasLvl)) & 63; col[j][i] = pal[k];
	}
	float cw = 400.0f / PGX * VIS_SS, ch = 240.0f / PGY * VIS_SS;
	for (int j = 0; j < PGY; j++) for (int i = 0; i < PGX; i++) {
		float x0 = i * cw, y0 = j * ch, x1 = x0 + cw, y1 = y0 + ch;
		C2D_DrawTriangle(x0, y0, col[j][i], x1, y0, col[j][i + 1], x0, y1, col[j + 1][i], 0.5f);
		C2D_DrawTriangle(x1, y0, col[j][i + 1], x1, y1, col[j + 1][i + 1], x0, y1, col[j + 1][i], 0.5f);
	}
	// the live cycle of the oscillator glowing on top
	const s16* w = gOsc[gVisOsc].mips.lev[2]; int N = LEVN[2]; float px = 0, py = 0;
	blendAdd(1);
	for (int i = 0; i <= 96; i++) {
		float x = 20 + i * 360.0f / 96, y = 120 - w[(i * N / 96) % N] / 32768.0f * 70 * (0.4f + gPlasLvl);
		if (i) { vLine(px, py, x, y, rgba(1, 1, 1, 0.18f), 7); vLine(px, py, x, y, rgba(1, 1, 1, 0.8f), 2); }
		px = x; py = y;
	}
	blendAdd(0);
	static int was = 0; int hit = gAudioPeak > 0.6f; if (hit && !was) gVisBurst += 6; was = hit;   // transients throw sparks
}
static int visModeNow(void) { return gVisMode ? gVisMode : 1 + ((int)(gVisT / 10.0f)) % (NVISM - 1); }   // AUTO: change every 10 s
static void visFull(void) {
	int mode = visModeNow();
	if (mode == 10) visPlasma(gFullTgt, 1.0f / 60);
	if (gVisBeat > 0.01f) { float r, g, b; hue(0.35f, &r, &g, &b); blendAdd(1); vRect(0, 0, 400, 240, rgba(r, g, b, gVisBeat * 0.12f)); blendAdd(0); }
	if (mode == 1) visWavetable(gVisOsc, 200, 128, 1.6f);
	else if (mode == 2) visScopes(10, 20, 380, 210);
	else if (mode == 3) visOrbit(200, 125, 100);
	else if (mode == 4) visSpectrum(12, 20, 376, 210, gVisOsc);
	else if (mode == 5) visTracker(0);
	else if (mode == 6) visTracker(1);
	else if (mode == 7) visTunnel();
	else if (mode == 8) visKaleido();
	else if (mode == 9) visRipples();
	if (gVisParts) {
		static unsigned lastId = 0; if (gIdc != lastId) { gVisBurst += 14; lastId = gIdc; }
		if (gVisBurst > 0) { partSpawn(gVisBurst); gVisBurst = 0; }
		partDraw(1.0f / 60);
	} else gVisBurst = 0;
}

// LFOs on the GPU: shape with glow, fading phase trail, playhead, value dot
static void visLfos(void) {
	float cr, cg, cb; hue(gCutoff * 0.7f + gVisEnv * 0.3f, &cr, &cg, &cb);
	for (int i = 0; i < 2; i++) {
		const LFO* l = &gLfo[i]; float Y0 = 30 + i * 64, H = 60, mid = Y0 + 36, amp = 18;
		vRect(0, Y0, 196, H, rgba(TH->pad[0] / 255.0f, TH->pad[1] / 255.0f, TH->pad[2] / 255.0f, 1));
		u32 grid = rgba(TH->grid[0] / 255.0f, TH->grid[1] / 255.0f, TH->grid[2] / 255.0f, 1);
		for (int g = 1; g < 8; g++) vLine(6 + g * 23, Y0 + 12, 6 + g * 23, Y0 + H - 2, grid, 0.6f);
		vLine(6, mid, 190, mid, grid, 0.8f);
		float xs[93], ys[93];
		for (int k = 0; k <= 92; k++) { xs[k] = 6 + k * 2.0f; ys[k] = mid - lfoDrawAt(l, k / 92.0f) * amp; }
		blendAdd(1);   // area between the curve and the centre line, brighter just behind the playhead
		for (int k = 0; k < 92; k++) {
			float d = l->ph - k / 92.0f; if (d < 0) d += 1;
			float a = 0.08f + 0.35f * expf(-d * 6);
			u32 f = rgba(cr, cg, cb, a);
			vTri(xs[k], ys[k], xs[k + 1], ys[k + 1], xs[k + 1], mid, f); vTri(xs[k], ys[k], xs[k + 1], mid, xs[k], mid, f);
		}
		blendAdd(0);
		glowLine(xs, ys, 93, cr, cg, cb, lfoSelected(i) ? 1.0f : 0.6f);
		float hx = 6 + l->ph * 184, hy = mid - l->val * amp;
		vLine(hx, Y0 + 12, hx, Y0 + H - 2, rgba(cr, cg, cb, 0.5f), 0.8f);
		blendAdd(1); vRect(hx - 5, hy - 5, 10, 10, rgba(cr, cg, cb, 0.3f)); blendAdd(0);
		vRect(hx - 1.5f, hy - 1.5f, 3, 3, rgba(1, 1, 1, 1));
	}
}

// envelopes on the GPU: filled shapes, glowing outline, highlighted stage, live note dot
static void visEnvelopes(void) {
	float cr, cg, cb; hue(gCutoff * 0.7f + gVisEnv * 0.3f, &cr, &cg, &cb);
	for (int w = 0; w < 2; w++) {
		float Y0 = 30 + w * 64, H = 60;
		vRect(0, Y0, 196, H, rgba(TH->pad[0] / 255.0f, TH->pad[1] / 255.0f, TH->pad[2] / 255.0f, 1));
		EnvPts P; envShape(w ? &gEnv2 : &gEnv1, 10, Y0 + 13, 176, 33, &P);   // panel-local x (screen x - 4)
		u32 grid = rgba(TH->grid[0] / 255.0f, TH->grid[1] / 255.0f, TH->grid[2] / 255.0f, 1);
		for (int g = 1; g < 6; g++) vLine(g * 196 / 6.0f, Y0, g * 196 / 6.0f, Y0 + H, grid, 0.6f);
		blendAdd(1);
		int sel = envSelSeg(w);
		for (int k = 0; k < 5; k++) {   // translucent fill, brighter on the selected stage
			u32 f = rgba(cr, cg, cb, k == sel ? 0.30f : 0.13f);
			vTri(P.x[k], P.y[k], P.x[k + 1], P.y[k + 1], P.x[k + 1], P.y[0], f);
			vTri(P.x[k], P.y[k], P.x[k + 1], P.y[0], P.x[k], P.y[0], f);
		}
		for (float x = 4; x < 192; x += 6) vLine(x, P.y[3], x + 3, P.y[3], rgba(cr, cg, cb, 0.35f), 0.8f);   // sustain level
		blendAdd(0);
		glowLine(P.x, P.y, 6, cr, cg, cb, 0.8f);
		if (sel >= 0) { float xs[2] = {P.x[sel], P.x[sel + 1]}, ys[2] = {P.y[sel], P.y[sel + 1]}; glowLine(xs, ys, 2, 1, 1, 1, 0.9f); }
		for (int k = 1; k < 5; k++) vRect(P.x[k] - 1.5f, P.y[k] - 1.5f, 3, 3, rgba(1, 1, 1, 1));
		float dx, dy;
		if (envDot(w, &P, &dx, &dy)) {
			blendAdd(1);
			vRect(dx - 5, dy - 5, 10, 10, rgba(cr, cg, cb, 0.25f)); vRect(dx - 3, dy - 3, 6, 6, rgba(cr, cg, cb, 0.5f));
			blendAdd(0);
			vRect(dx - 1.5f, dy - 1.5f, 3, 3, rgba(1, 1, 1, 1));
			vLine(dx, P.y[0], dx, dy, rgba(cr, cg, cb, 0.6f), 0.8f);
		}
	}
}

// copy rows y0..y1 (screen) of the rendered panel into the top framebuffer
static void visBlit(int y0, int y1) {
	u8* fbp = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	GSPGPU_InvalidateDataCache(gVisLin, 240 * VIS_W * 3);
	int m0 = 239 - y1, m1 = 239 - y0;
	for (int x = 0; x < VIS_W; x++) memcpy(fbp + ((VIS_X + x) * 240 + m0) * 3, gVisLin + (x * 240 + m0) * 3, (m1 - m0 + 1) * 3);
}

static int visActive(int page) { return gGpuOK && !gGpuHung && ((gGpuFx && page >= 1 && page <= 6) || page == 8); }
static void visFrame(int page, float dt) {
	gVisT += dt;
	float e = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho) e += gV[i].env;
	if (e > 1) e = 1;
	gVisEnv += (e - gVisEnv) * (e > gVisEnv ? 0.5f : 0.08f);
	gVisBeat *= 0.82f;
	for (int o = 0; o < 2; o++) trailPush(o, gPosEff[o]);
	// pipelined: C3D_FrameBegin waits for the previous frame (its render + transfer), so that
	// result is copied in now and this frame's transfer is queued inside the frame (no PPF wait)
	// never block: if the GPU hasn't finished the last frame, skip; if it stays stuck, give up on it
	static int stuck = 0;
	char mk[40];
	if (gGpuFrames < 3) { snprintf(mk, sizeof(mk), "GPU F%d BEGIN", gGpuFrames + 1); crashMark(mk); }
	if (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
		if (++stuck > 90) { gGpuHung = 1; snprintf(mk, sizeof(mk), "GPU HUNG AFTER F%d", gGpuFrames); crashMark(mk); }
		return;
	}
	stuck = 0;
	static int lastPage = -1, have = 0;
	int full = page == 8;
	if (have && lastPage == page) {
		if (full) {
			u8* fbp = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
			// CPU copy: a GPU copy issued inside the frame lands after the menu text is drawn and wipes it
			GSPGPU_InvalidateDataCache(gFullLin, 240 * 400 * 3); memcpy(fbp, gFullLin, 240 * 400 * 3);
			{ int m = visModeNow(); if (m == 5 || m == 6) { u8* f0 = fb; int w0 = fbW; fb = fbp; fbW = 400; trackerText(m == 6); fb = f0; fbW = w0; } }   // TRACKER: its text on top
		}
		else if (page == 1 || page == 2) visBlit(30, 206);
		else visBlit(30, 154);
	}
	lastPage = page; have = 1;
	C3D_RenderTarget* tg = full ? gFullTgt : gVisTgt;
	int cols = full ? 400 : VIS_W;
	static int fullFresh = 0;
	int trails = full && gVisTrails && fullFresh;   // keep last frame and fade it: GPU persistence trails
	if (!trails) C2D_TargetClear(tg, C2D_Color32(TH->bg[0], TH->bg[1], TH->bg[2], 0xff));
	fullFresh = full;
	C2D_SceneBegin(tg);
	C2D_SceneSize(240 * VIS_SS, cols * VIS_SS, true);   // tilted like a real screen target
	if (trails) vRect(0, 0, 400, 240, rgba(TH->bg[0] / 255.0f, TH->bg[1] / 255.0f, TH->bg[2] / 255.0f, 0.3f));
	if (full) visFull();
	else if (page == 1 || page == 2) visWavetable(page - 1, 98, 122, 0.95f);
	else if (page == 4) visEnvelopes();
	else if (page == 5) visLfos();
	else visScopes(0, 30, 196, 124);
	C2D_Flush();
	if (gGpuFrames < 3) { snprintf(mk, sizeof(mk), "GPU F%d DRAWN", gGpuFrames + 1); crashMark(mk); }
	// with SCALE_XY the transfer engine takes the same (input) dimensions for both sides and halves them itself
	C3D_SyncDisplayTransfer((u32*)tg->frameBuf.colorBuf, GX_BUFFER_DIM(240 * VIS_SS, cols * VIS_SS), (u32*)(full ? gFullLin : gVisLin), GX_BUFFER_DIM(240 * VIS_SS, cols * VIS_SS),
		GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
		GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
	C3D_FrameEnd(0);
	gGpuFrames++;
	if (gGpuFrames <= 3) { snprintf(mk, sizeof(mk), "GPU F%d SUBMITTED", gGpuFrames); crashMark(mk); }
	if (gGpuFrames == 120) crashMark("GPU RUNNING 120 FRAMES");
}

// ---------------------------------------------------------------- second CPU core: waveform rebuild worker
// Morph + warp + FFT band-limiting run on core 1 (the system core; the app gets up to 80% of it via
// APT_SetAppCpuTimeLimit). The main core only posts jobs. gMipsLock guards the shared FFT buffers.
typedef struct { float pe[2], we[2]; int chg[2]; u32 used[2]; } RebuildJob;
static RebuildJob gJob, gMainJob; static volatile int gJobTaken = 1; static int gMainJobDue = 0, gTblDue = 0;
static int gJobBusy = 0, gWorkerQuit = 0, gJobDone = 0;   // accessed with __atomic ops (cross-core)
#define AGET(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define ASET(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static void doRebuild(const RebuildJob* j) {
	Osc* A = &gOsc[0]; Osc* B = &gOsc[1];
	oscMorph(A, j->pe[0]); oscMorph(B, j->pe[1]);
	float rA = powf(2.0f, (float)(B->oct - A->oct)), rB = powf(2.0f, (float)(A->oct - B->oct));
	if (rA < 1) rA = 1;
	if (rB < 1) rB = 1;
	for (int o = 0; o < 2; o++) if (j->chg[o]) {
		oscWarp(&gOsc[o], j->we[o], &gOsc[1 - o], o ? rB : rA);
		mipsSet(&gOsc[o].mips, gOsc[o].cyc, j->used[o]);
	}
}

static void workerMain(void* arg) {
	(void)arg;
	for (;;) {
		LightEvent_Wait(&gJobEv);
		if (AGET(gWorkerQuit)) break;
		fxService();   // effects frames first: they have a deadline
		if (AGET(gJobTaken)) continue;   // woken only for effects
		ASET(gJobTaken, 1);
		RebuildJob j = gJob;
		u64 t0 = svcGetSystemTick();
		LightLock_Lock(&gMipsLock);
		gInWorker = 1; doRebuild(&j); gInWorker = 0;
		LightLock_Unlock(&gMipsLock);
		__atomic_fetch_add(&gCore1Busy, svcGetSystemTick() - t0, __ATOMIC_RELAXED);
		ASET(gJobDone, 1);
		ASET(gJobBusy, 0);
		fxService();
	}
}

// ---------------------------------------------------------------- main
static u32 usedLevels(int o) {
	u32 m = 0x7Eu;   // levels 1-6 always (5 and 6 are tiny); level 0 only while a low note uses it
	for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started)
		for (int k = 0; k < gV[i].nch; k++) {
			int r = gV[i].role[k], to = gOsc[0].on ? 0 : 1;
			if ((o == 0 && r <= R_A2) || (o == 1 && r >= R_B0 && r <= R_B2) || (r == R_TAP && o == to)) m |= 1u << gV[i].lev[k];
		}
	return m;
}

// ---- sleep (lid closed / HOME menu sleep): the second sound chip, the camera, Wi-Fi and IR don't survive it cleanly,
// so they are quietened before the system sleeps and brought back on the main loop after it wakes
static aptHookCookie gSleepCookie;
static volatile int gSlept = 0, gWoke = 0, gNetResume = 0, gLinkResume = 0;
static void sleepHook(APT_HookType h, void* p) {
	(void)p;
	if (h == APTHOOK_ONSLEEP) {
		gSlept = 1;
		LightLock_Lock(&gVoiceLock);
		allOff();
		if (gCsndOK) {
			echoStop();
			for (int c = 0; c < NCS; c++) if (csndChannels & BIT(c)) CSND_SetPlayState(c, 0);
			csWait();
		}
		LightLock_Unlock(&gVoiceLock);
		if (!gSafeMode) {
			camStop();
			if (gMidiNet) { gMidiNet = 0; netMidiChanged(); gNetResume = 1; }
			if (gLink) { gLinkResume = gLink; linkStop(); gLink = gLinkPrev = 0; }
		}
	} else if (h == APTHOOK_ONWAKEUP) gWoke = 1;
}
static void wakeTick(void) {   // main loop, after a wake: Wi-Fi MIDI and the IR / local link back as they were
	if (!gWoke) return;
	gWoke = 0;
	if (gNetResume) { gNetResume = 0; gMidiNet = 1; netMidiChanged(); }
	if (gLinkResume) { gLink = gLinkResume; gLinkResume = 0; linkChanged(); }
}

// ---- SOUND SETUP, automatic: the DSP firmware (sdmc:/3ds/dspfirm.cdc) is Nintendo's, so Syrup can't ship it, but it
// sits inside the system's own Nintendo 3DS Sound app. On the first start without it, Syrup reads that app's code
// from NAND, unpacks it, finds the firmware ("DSP1", 0x100 bytes into it) and writes the file - what Rosalina's
// "Dump DSP firmware" and the DSP1 app do - so there's nothing to set up by hand. If it can't, the screen still
// shows the Rosalina steps.
static int gDspAuto = 0;   // 1 = made it this start, -1 = tried and couldn't
static int blzDecompress(const u8* in, u32 inSize, u8* out, u32 outSize) {   // the 3DS ExeFS code compression (backwards LZSS)
	if (inSize < 8) return 0;
	const u8* foot = in + inSize - 8;
	u32 tb = foot[0] | foot[1] << 8 | foot[2] << 16 | (u32)foot[3] << 24;
	u32 index = inSize - ((tb >> 24) & 0xFF), stop = inSize - (tb & 0xFFFFFF), o = outSize;
	if ((tb & 0xFFFFFF) > inSize || ((tb >> 24) & 0xFF) > inSize) return 0;
	memset(out, 0, outSize); memcpy(out, in, inSize);
	while (index > stop) {
		u8 ctl = in[--index];
		for (int j = 0; j < 8; j++) {
			if (index <= stop || o == 0) break;
			if (ctl & 0x80) {
				if (index < 2) return 0;
				index -= 2;
				u32 so = in[index] | in[index + 1] << 8, sz = ((so >> 12) & 15) + 3; so = (so & 0x0FFF) + 2;
				if (o < sz) return 0;
				for (u32 k = 0; k < sz; k++) { if (o + so >= outSize) return 0; u8 d = out[o + so]; out[--o] = d; }
			} else { if (o < 1) return 0; out[--o] = in[--index]; }
			ctl <<= 1;
		}
	}
	return 1;
}
static int dspFind(const u8* b, u32 n) {   // "DSP1" magic at +0x100 of the firmware: returns the firmware's offset, or -1
	for (u32 i = 0x100; i + 8 <= n; i += 4) if (b[i] == 'D' && b[i + 1] == 'S' && b[i + 2] == 'P' && b[i + 3] == '1') {
		u32 sz = b[i + 4] | b[i + 5] << 8 | b[i + 6] << 16 | (u32)b[i + 7] << 24;
		if (sz > 0x200 && sz < 0x100000 && i - 0x100 + sz <= n) return (int)(i - 0x100);
	}
	return -1;
}
static int dspAutoDump(void) {
#ifdef SYRUP_HOST
	return 0;
#else
	static const u32 SOUND[] = {0x00021500, 0x00022500, 0x00020500, 0x00026500, 0x00027500, 0x00028500};   // Nintendo 3DS Sound: USA EUR JPN CHN KOR TWN
	static const u32 CODE[5] = {0, 0, 2, 0x646F632E, 0x00000065};   // ExeFS ".code"
	for (unsigned t = 0; t < sizeof(SOUND) / sizeof(SOUND[0]); t++) {
		u32 ap[4] = {SOUND[t], 0x00040010, MEDIATYPE_NAND, 0};
		FS_Path a = {PATH_BINARY, sizeof(ap), ap}, f = {PATH_BINARY, sizeof(CODE), CODE};
		Handle h; if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, a, f, FS_OPEN_READ, 0))) continue;
		u64 sz = 0; FSFILE_GetSize(h, &sz);
		u8* code = sz > 16 && sz < 0x800000 ? (u8*)malloc((size_t)sz) : NULL; u32 rd = 0;
		if (code) FSFILE_Read(h, &rd, 0, code, (u32)sz);
		FSFILE_Close(h);
		if (!code || rd != sz) { free(code); continue; }
		u8* use = code; u32 n = (u32)sz; u8* big = NULL;
		int at = dspFind(code, n);
		if (at < 0) {   // compressed (the usual case): unpack, then look again
			u32 extra = code[n - 4] | code[n - 3] << 8 | code[n - 2] << 16 | (u32)code[n - 1] << 24, out = n + extra;
			if (extra && extra < 0x1000000 && (big = (u8*)malloc(out)) && blzDecompress(code, n, big, out)) { use = big; n = out; at = dspFind(big, n); }
		}
		int ok = 0;
		if (at >= 0) {
			u32 fsz = use[at + 0x104] | use[at + 0x105] << 8 | use[at + 0x106] << 16 | (u32)use[at + 0x107] << 24;
			mkdir("sdmc:/3ds", 0777);
			FILE* o = fopen("sdmc:/3ds/dspfirm.cdc.part", "wb");
			if (o) { ok = fwrite(use + at, 1, fsz, o) == fsz; fclose(o); if (ok) { remove("sdmc:/3ds/dspfirm.cdc"); ok = rename("sdmc:/3ds/dspfirm.cdc.part", "sdmc:/3ds/dspfirm.cdc") == 0; } else remove("sdmc:/3ds/dspfirm.cdc.part"); }
		}
		free(big); free(code);
		if (ok) return 1;
	}
	return -1;
#endif
}
static int dspAsk(void) {   // the first start without the firmware: say what Syrup will do, and let the user choose
#ifdef SYRUP_HOST
	return 0;
#else
	static const char* L[] = {"ONE-TIME SOUND SETUP", "", "THE 3DS SOUND CHIP NEEDS ITS FIRMWARE", "ON THE SD CARD. IT'S NINTENDO'S, SO", "SYRUP DOESN'T INCLUDE IT - BUT YOUR", "CONSOLE HAS IT IN ITS OWN SOUND APP.", "",
		"SYRUP CAN COPY IT FROM THERE TO", "SD:/3DS/DSPFIRM.CDC (IT STAYS ON", "THIS CONSOLE; NOTHING IS SENT).", "", "A: COPY IT NOW", "B: I'LL DO IT MYSELF (ROSALINA)"};
	int n = sizeof(L) / sizeof(L[0]);
	while (aptMainLoop()) {
		hidScanInput(); u32 k = hidKeysDown();
		if (k & KEY_A) return 1;
		if (k & (KEY_B | KEY_START)) return 0;
		for (int b = 0; b < 2; b++) {
			fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
			rect(0, 0, 400, 240, 10, 6, 2);
			int h = n * 13 + 16, y0 = 120 - h / 2;
			rect(30, y0, 340, h, 0, 0, 0); outline(30, y0, 340, h, 255, 140, 0);
			for (int i = 0; i < n; i++) text(200 - (int)strlen(L[i]) * 4, y0 + 8 + i * 13, L[i], 1, i == 0 || i >= n - 2 ? 255 : 220, i == 0 || i >= n - 2 ? 140 : 200, i == 0 || i >= n - 2 ? 0 : 160);
			fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320; rect(0, 0, 320, 240, 10, 6, 2);
			gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
		}
	}
	return 0;
#endif
}
int main(void) {
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, true);     // draw into the hidden buffer, swap on vblank: no tearing
	gfxSetDoubleBuffering(GFX_BOTTOM, true);
	crashCheck();
	crashMark("GPU INIT");
	if (!gSafeMode) gpuInit();
	crashMark("SENSORS INIT");
	if (!gSafeMode) { sensorsInit(); extrasBasicInit(); }
	if (gSafeMode) { gGpuFx = 0; gAudioClock = 0; }
	fontGfx = consoleGetDefault()->font.gfx;
	for (int k = 0; k < 2; k++) { splashDraw(); gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }   // both buffers
	gSplashT0 = osGetTime();
	fftInit();
	for (int o = 0; o < 2; o++) { gOsc[o].tb.frames = (float*)malloc(sizeof(float) * CYC * MAXFR); mipsInit(&gOsc[o].mips); }
	mipsInit(&gSubMips);
	gNoise = (s16*)linearAlloc(gNoiseLen * sizeof(s16));
	for (int c = 0; c < NCH; c++) gDspOwner[c] = -1;
	for (int c = 0; c < NCS; c++) gCsOwner[c] = -1;
	makeBusyTable();
	scanTables(); kitScanDirs();
	gTableCount = T_BUILTIN + gNFiles;

	// default patch: A = Basic Shapes, 3-voice unison; B = Harmonix an octave up (off)
	Osc* A = &gOsc[0]; Osc* B = &gOsc[1];
	A->on = 1; A->table = T_BASIC; A->pos = 0.6f; A->level = 0.8f; A->uni = 3; A->detune = 14; A->blend = 0.6f; A->width = 0.8f;
	B->on = 0; B->table = T_HARMONIX; B->pos = 0.3f; B->level = 0.5f; B->oct = 1; B->uni = 1; B->detune = 10; B->blend = 0.5f; B->width = 0.6f;
	tableSelect(&A->tb, A->table); tableSelect(&B->tb, B->table);
	LightLock_Init(&gMipsLock);
	LightLock_Init(&gVoiceLock); LightLock_Init(&gNetLock);
	userLoad(); learnLoad();   // MIDI LEARN bindings
	upCheckTook();
	seqDefault();
	seqLoad();
	presetLoad(gStyle, gPreset);   // start on PAD / SYRUP PAD
	lastLoad();                    // ... or where you left off (AUTOSAVE)
	LightEvent_Init(&gJobEv, RESET_ONESHOT);
	gCore2Rc = APT_SetAppCpuTimeLimit(80);
	if (R_FAILED(gCore2Rc)) gCore2Rc = APT_SetAppCpuTimeLimit(30);   // some systems refuse a high limit
	APT_GetAppCpuTimeLimit(&gCore2Lim);
	{
		s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		gWorker = threadCreate(workerMain, NULL, 64 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, 1, false);
		if (!gWorker) {   // try again with a smaller time slice, and find out why if it still fails
			APT_SetAppCpuTimeLimit(30); APT_GetAppCpuTimeLimit(&gCore2Lim);
			gWorker = threadCreate(workerMain, NULL, 64 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, 1, false);
		}
		if (!gWorker) { Handle h; static u8 probeStack[4096] __attribute__((aligned(8))); gCore2ThRc = svcCreateThread(&h, probeMain, 0, (u32*)(probeStack + sizeof(probeStack)), 0x30, 1); if (R_SUCCEEDED(gCore2ThRc)) { svcWaitSynchronization(h, 1000000000LL); svcCloseHandle(h); } }
		if (gWorker) fxCore2Start();   // the master effects on core 2 too
	}
	buildSub(); buildNoise();

	{ FILE* df = fopen("sdmc:/3ds/dspfirm.cdc", "rb"); if (df) fclose(df); else if (dspAsk()) { crashMark("DSP SETUP"); gDspAuto = dspAutoDump(); } }   // no firmware yet: ask, then make it
	gAudioOK = R_SUCCEEDED(ndspInit());
	if (gAudioOK && gDspAuto == 1) { say("SOUND SET UP: DSP FIRMWARE SAVED TO THE SD CARD"); gMsgT = 6; }
	if (gAudioOK) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspSetMasterVol(1.0f);
		for (int ch = 0; ch < NCH; ch++) { ndspChnReset(ch); ndspChnSetInterp(ch, NDSP_INTERP_POLYPHASE); ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16); }
		gCsndOK = R_SUCCEEDED(csndInit());   // second sound unit: sub + effect taps
		crashMark("FX BUS INIT");
		if (!gSafeMode) busInit();
		crashMark("OUTPUT INIT");
		outputApply();
		ndspSetCallback(audioTick, NULL);   // arp + sequencer clock, every 160 samples
	}

	int page = settingsLoad(), sel[NPAGES] = {0}, oct = 0, helpOn = 0, latch = 0, prevTouch = 0;
	int curKey = -1;
	int topDirty = 1, botDirty = 1, topAgain = 0, botAgain = 0;   // *Again: also refresh the other buffer next frame
	float repeatT = 0, lastPos[2] = {-1, -1}, lastWarp[2] = {-1, -1};
	u8 on[128], onPrev[128]; memset(onPrev, 0, sizeof(onPrev));
	u64 last = osGetTime();
	aptHook(&gSleepCookie, sleepHook, NULL);

#if defined(SYRUP_HOST) && !defined(FAKE_WEB)
	gSplash = 0;   // tests start straight on the instrument (the web build keeps the logo and a tip)
#endif
	if (gSplash) for (int k = 0; k < 2; k++) { splashBottomReady(); gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }
	while (aptMainLoop()) {
#ifndef SYRUP_HOST
		{   // DSP watchdog: if the DSP stops answering, turn SCREAM off and don't wait on it at exit (that wait never returned)
			extern u16 ndspFrameId; static u16 lastFid; static u64 lastFidT = 0; u64 nowT = osGetTime();
			static u64 lastLoopT = 0; if (nowT - lastLoopT > 300) lastFidT = 0;   // back from HOME / sleep: start counting again
			lastLoopT = nowT;
			if (ndspFrameId != lastFid || !lastFidT) { lastFid = ndspFrameId; lastFidT = nowT; }
			int st = gAudioOK && nowT - lastFidT > 800;
			if (st && !gDspStalled) { gQScream = 0; say("SOUND CHIP STOPPED ANSWERING: RESTART SYRUP"); topDirty = 1; }
			gDspStalled = st;
		}
#endif
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (gSplash) {   // the logo stays up a few seconds after loading, or until a tap / button
			if (kDown || osGetTime() - gSplashT0 > 4000) { gSplash = 0; kDown = 0; topDirty = botDirty = topAgain = botAgain = 1; }
			else { gspWaitForVBlank(); continue; }
		}
		if (kDown & KEY_START) { gStartMenu = !gStartMenu; gStartSel = 0; topDirty = topAgain = 1; kDown &= ~KEY_START; }
		gStickMenu = (gStartMenu || gThemePick || (page == 0 && gStickMode == SM_PAGE)) && !(kHeld & KEY_SELECT);
		if (gStickMenu) kDown |= stickDpad(); else stickDpad();
		if (gThemePick && !gStartMenu && (kDown & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A))) {   // the picker by buttons / circle pad: try them live, A keeps it
			int d = (kDown & KEY_DRIGHT) ? 1 : (kDown & KEY_DLEFT) ? -1 : (kDown & KEY_DDOWN) ? TP_COLS : (kDown & KEY_DUP) ? -TP_COLS : 0;
			if (d) { gTheme = (gTheme + NTHEMES + d) % NTHEMES; gBrightSaveDue = 1; }
			if (kDown & KEY_A) { gThemePick = 0; char m[32]; snprintf(m, sizeof(m), "THEME: %s", THEME_NAMES[gTheme]); say(m); }
			kDown &= ~(KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A); topDirty = topAgain = botDirty = botAgain = 1;
		}
		int startAct = -1;
		if (gStartMenu) {   // the menu has the buttons; the touch screen keeps playing
			if (kDown & KEY_DUP) { gStartSel = (gStartSel + NSTART - 1) % NSTART; topDirty = 1; }
			if (kDown & KEY_DDOWN) { gStartSel = (gStartSel + 1) % NSTART; topDirty = 1; }
			if (gStartSel == 4 && (kDown & (KEY_DLEFT | KEY_DRIGHT | KEY_L | KEY_R))) {   // THEME: < > or L / R try the themes right here, live
				int d = (kDown & (KEY_DRIGHT | KEY_R)) ? 1 : -1; gTheme = (gTheme + NTHEMES + d) % NTHEMES; gBrightSaveDue = 1; topDirty = topAgain = botDirty = botAgain = 1;
			}
			if (kDown & KEY_A) { startAct = gStartSel; gStartMenu = 0; }
			if (kDown & KEY_B) gStartMenu = 0;
			if (!gStartMenu) topDirty = topAgain = botDirty = 1;
			kDown &= KEY_TOUCH; kHeld &= KEY_TOUCH;
		}
		if (startAct == 6) break;   // QUIT
		wakeTick();
		LightLock_Lock(&gVoiceLock);   // audio state below; released before drawing
		if (!gSafeMode) { motionTick(1.0f / 60); camTick(); }
		if (gVoiceMode == 7 && !gSafeMode) {   // PERC KIT: shake the console, it plays the shaker
			static int armed = 1; static u64 lastHit = 0, lastTouch = 0; u64 tnow = osGetTime();
			if ((kHeld | kDown) & KEY_TOUCH) lastTouch = tnow;   // tapping the screen jolts the console: no shaker while touching, or just after
			static int over = 0; over = gShakeD > 0.6f ? over + 1 : 0;   // a real shake: hard, and two readings in a row
			if (armed && over >= 2 && tnow - lastHit > 60 && tnow - lastTouch > 250) {
				float v = 0.4f + 0.6f * (gShakeD > 1 ? 1 : gShakeD);
				if (gShakeQ && (gSeqPlaying || (gDrums && gDrumStep >= 0))) { if (v > gShakePend) gShakePend = v; }   // the audio clock plays it on the step
				else kitHitSound(0, v);
				armed = 0; lastHit = tnow; }
			if (gShakeD < 0.2f) armed = 1;
		}
		micRecTick(1.0f / 60); micPadTick(1.0f / 60); webUpTick(); if (gPanicClr > 0) { gPanicClr--; fxTailsClear(); } prerollTick(1.0f / 60); seqTapTick(1.0f / 60); tapeTick(); thruTick(); linkTick(1.0f / 60); dlTick(); upTick(); echoTick(); grainsTick(); revTypeApply(); netTabTick(); drumTapTick(1.0f / 60);
		if (midiTick()) topDirty = botDirty = 1;
		if (gOfType) {   // OUT FILTER cutoff mod: the mod matrix and ENV 2 of the newest note
			const SVoice* nv = newestVoice(); float m = modSum(DST_OFCUT, (SVoice*)nv);
			if (nv && gOfEnv != 0) m += gOfEnv * nv->env2;
			gOfMod = m;
		}
		gVidT += 1.0f / 60;
		if (gWtScan > 0) { A->pos += gWtScan / 60.0f / 8.0f; if (A->pos >= 1) A->pos -= 1; if (page == 1) topDirty = 1; }
		{ static int shown = 0; if (gMidiNet && gNetAddr[0] && !shown) { char m[48]; snprintf(m, sizeof(m), "ON YOUR PHONE: http://%s", gNetAddr); say(m); gMsgT = 8; shown = 1; topDirty = 1; } if (!gMidiNet) shown = 0; }
		if (gMsgT > 0) { gMsgT -= 1.0f / 60; topDirty = 1; }
		if (gCallT > 0) { gCallT -= 1.0f / 60; topDirty = 1; }
		u64 now = osGetTime(); float dt = (now - last) / 1000.0f; last = now; if (dt > 0.1f) dt = 0.1f;
		sensorsUpdate(dt);

		static int selSwallow = 0;
		if (helpOn && (kDown & (KEY_L | KEY_R))) { gHelpSlide = (gHelpSlide + ((kDown & KEY_R) ? 1 : NHELP - 1)) % NHELP; topDirty = 1; kDown &= KEY_TOUCH; }   // L / R: the other slides
		if (helpOn && (kDown & ~KEY_TOUCH)) { if (kDown & KEY_SELECT) selSwallow = 1; helpOn = 0; topDirty = botDirty = 1; kDown &= KEY_TOUCH; }   // any other button closes help
		{   // SELECT: tap = PLAY / STOP (sequencer and drums together). Held, it's a modifier:
			// SELECT+L undo, SELECT+R redo, SELECT+DOWN panic. The tap acts on release, only if nothing was combined.
			static u32 prevHeld = 0; static int selCombo = 0, drumsWas = 0; u32 kUp = prevHeld & ~kHeld; prevHeld = kHeld;
			static int qBar = -1, qIdx = 0, qHold = -1;   // qHold: the bar still lit after the pad went back to the middle
			if (kDown & KEY_SELECT) { selCombo = 0; qBar = -1; qHold = -1; }
			if (selSwallow) { selCombo = 1; if (!(kHeld & KEY_SELECT)) selSwallow = 0; }   // the press that closed HELP isn't PLAY
			gQuickBar = -1;
			if (kHeld & KEY_SELECT) {
				// QUICK SELECT feel: the first firm push picks a bar and LOCKS it until the pad comes back near the middle, so
				// leaning along it can't flip to the next bar. Along the page / transport rows the lean is a position with
				// hysteresis (an option has to be clearly passed before the next lights); along the long side lists it's a
				// speed (lean a little = slow, a lot = fast). The D-pad steps one at a time on any bar.
				// QUICK SELECT feel: the push's angle picks one of 16 spots round the ring, with hysteresis (a spot has to be
				// clearly passed before the next lights). Back near the middle, what's lit stays lit; the D-pad steps round.
				circlePosition qp; hidCircleRead(&qp); float qx = qp.dx / 150.0f, qy = qp.dy / 150.0f, mag = sqrtf(qx * qx + qy * qy);
				if (qBar >= 0 && mag < 0.25f) { qHold = qBar; qBar = -1; }
				if (mag > 0.55f) {
					float pos = atan2f(qx, qy) / (6.2831853f / NQR); if (pos < 0) pos += NQR;
					float d = pos - (float)qIdx; while (d > NQR / 2) d -= NQR; while (d < -NQR / 2) d += NQR;
					if (qBar < 0 && qHold < 0) qIdx = ((int)lroundf(pos)) % NQR;
					else if (fabsf(d) > 0.7f) qIdx = ((int)lroundf(pos)) % NQR;
					qBar = 0; qHold = -1; selCombo = 1;
				}
				{ int lit = qBar >= 0 ? qBar : qHold;   // the D-pad: one spot round the ring
				  if (lit >= 0 && (kDown & (KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN))) {
					if (kDown & (KEY_DLEFT | KEY_DUP)) qIdx = (qIdx + NQR - 1) % NQR; if (kDown & (KEY_DRIGHT | KEY_DDOWN)) qIdx = (qIdx + 1) % NQR; } }
				{ int lit = qBar >= 0 ? qBar : qHold; if (lit >= 0) { gQuickBar = lit; gQuickIdx = qIdx; topDirty = 1; kDown &= ~(KEY_A | KEY_B | KEY_X | KEY_Y | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT); } }
				if (kDown & KEY_L) { selCombo = 1; int r = histStep(1); say(r == 1 ? "UNDO: PATTERN" : r == 2 ? "UNDO: PATCH EDIT" : r == 3 ? "UNDO: DRUMS" : "NOTHING TO UNDO"); topDirty = botDirty = 1; }
				if (kDown & KEY_R) { selCombo = 1; int r = histStep(0); say(r == 1 ? "REDO: PATTERN" : r == 2 ? "REDO: PATCH EDIT" : r == 3 ? "REDO: DRUMS" : "NOTHING TO REDO"); topDirty = botDirty = 1; }
				if (kDown & KEY_DDOWN) { selCombo = 1; startAct = 2; }   // PANIC, below
				if (kDown & (KEY_DUP | KEY_DLEFT | KEY_DRIGHT)) selCombo = 1;
				kDown &= ~(KEY_L | KEY_R | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT); kHeld &= ~(KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
			}
			if ((kUp & KEY_SELECT) && (qBar >= 0 || qHold >= 0)) {   // take what's lit
				int d = QR_DEST[qIdx]; qBar = -1; qHold = -1; topDirty = botDirty = 1;
				if (d >= 0) { page = d; gPads = 0; if (d == 7 && gPatView != 3) gPatView = 0; }   // SEQ: the notes, or the drums if that's where you were
				else if (d == QA_TAPE) { page = 7; gPads = 0; gPatView = 2; }
				else if (d == QA_REC) liveRecToggle();
				else if (d == QA_TREC) { if (gTapeRec >= 0) tapeRecStop(0); else tapeRecStart(); }
				else selCombo = 0;   // PLAY / STOP: below
			}
			if ((kUp & KEY_SELECT) && !selCombo) {
				gSeqMode = gSeqMode == 1 ? 0 : 1; if (gSeqMode == 1 && gSeqBase < 0) gSeqBase = gridNote(0);
				if (gSeqMode == 0) { seqStop(); if (gLiveRec || gPreFor == 2) liveRecStop(); drumsWas = gDrums; if (gDrums) { gDrums = 0; drumsChanged(); } }
				else if (drumsWas && !gDrums) { gDrums = 1; drumsChanged(); }   // PLAY brings back the drums STOP stopped
				say(gSeqMode == 1 ? "PLAY" : "STOP"); topDirty = botDirty = 1;
			}
		}
#ifdef FAKE_WEB
		if (gQuickOpen) {   // the phone's QUICK button: the same ring on the top screen, picked with a tap (no two-thumb SELECT + pad)
			static int qw = 0, qi = -1;
			if (kDown & (KEY_B | KEY_SELECT | KEY_START)) { gQuickOpen = 0; qi = -1; topDirty = botDirty = 1; kDown &= KEY_TOUCH; }
			else if (TTOUCH) {
				float dx = (TTX - 200) / 158.0f, dy = (118 - TTY) / 94.0f;
				if (dx * dx + dy * dy > 0.3f) { float pos = atan2f(dx, dy) / (6.2831853f / NQR); if (pos < 0) pos += NQR; qi = ((int)lroundf(pos)) % NQR; } else qi = -1;
				gQuickBar = qi >= 0 ? 0 : -1; gQuickIdx = qi >= 0 ? qi : 0; topDirty = 1;
			} else if (qw) {   // let go: go there (a tap in the middle just closes it)
				gQuickOpen = 0; topDirty = botDirty = 1;
				if (qi >= 0) { int d = QR_DEST[qi];
					if (d >= 0) { page = d; gPads = 0; if (d == 7 && gPatView != 3) gPatView = 0; }
					else if (d == QA_TAPE) { page = 7; gPads = 0; gPatView = 2; }
					else if (d == QA_REC) liveRecToggle();
					else if (d == QA_TREC) { if (gTapeRec >= 0) tapeRecStop(0); else tapeRecStart(); }
					else { gSeqMode = gSeqMode == 1 ? 0 : 1; if (gSeqMode == 1 && gSeqBase < 0) gSeqBase = gridNote(0); if (gSeqMode == 0) { seqStop(); if (gLiveRec || gPreFor == 2) liveRecStop(); } say(gSeqMode == 1 ? "PLAY" : "STOP"); } }
				qi = -1;
			}
			if (gQuickOpen && qi >= 0) { gQuickBar = 0; gQuickIdx = qi; }
			qw = TTOUCH; TTOUCH = 0; kDown &= ~(KEY_A | KEY_X | KEY_Y | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
		}
#endif
		if (startAct == 3) { helpOn = 1; gHelpSlide = 0; topDirty = botDirty = 1; }
		if (startAct == 5) { learnArm(page, sel[page]); topDirty = 1; }
		{   // CIRCLE PAD in the editors (see curShown)
			int mode = 0;
			if ((gDrumOnB || gDrumInSeq) && !gPads) mode = gDgEd ? 3 : 2;
			else if (page == 7 && gGridOn && !gPatView && !gPads) mode = gStepEd >= 0 ? 1 : 4;
			if (kHeld & KEY_SELECT) mode = 0;   // SELECT held: the pad is the QUICK ring
			gEdStick = mode != 0;
			static float ax = 0, ay = 0; static int hx = 0, hy = 0; static u64 aT = 0; static int aHeld = 0, aDone = 0;
			if (mode) {
				circlePosition cp; hidCircleRead(&cp); float jx = cp.dx / 150.0f, jy = cp.dy / 150.0f;
				if (fabsf(jx) > fabsf(jy) * 1.3f) jy = 0; else if (fabsf(jy) > fabsf(jx) * 1.3f) jx = 0; else jx = jy = 0;   // one direction at a time: a diagonal push does nothing
				int ex = stickStep(jx, &ax, &hx, dt), ey = stickStep(jy, &ay, &hy, dt);   // ey: + = up
				if (ex || ey) { if (!curShown()) { ex = ey = 0; } gCurT = osGetTime(); botDirty = 1; }   // the first push only shows the cursor
				int aDown = (kDown & KEY_A) != 0, aUp = aHeld && !(kHeld & KEY_A);
				if (mode == 4) {   // piano roll: move, A = note, hold A = its step editor
					if (ex) { int L = (gSeqLen + 15) / 16 * 16; if (L < 16) L = 16; gRcS = (gRcS + ex + L) % L; gRollPg = gRcS / ROLL_VIEW; gRollFollow = 0; }
					if (ey) { if (gVoiceMode >= 3) { int r = rollRow(gRcN) + ey; if (r < 0) r = 0; if (r > 9) r = 9; gRcN = rollNote(r); }
						else if (rollFold()) { int nn = gRcN; do nn += ey; while (!inScale(nn) && nn > 0 && nn < 127); gRcN = nn; while (rollRow(gRcN) < 0 && gRcN < gRollLow) gRollLow--; while (rollRow(gRcN) < 0 && gRcN > gRollLow) gRollLow++; }
						else { gRcN += ey; if (gRcN < gRollLow) gRollLow = gRcN; if (gRcN >= gRollLow + ROLL_ROWS) gRollLow = gRcN - ROLL_ROWS + 1; } }   // (silent: moving the cursor doesn't play)
					if (rollRow(gRcN) < 0) gRcN = gVoiceMode >= 3 ? rollNote(0) : gRollLow + 5;
				} else if (mode == 2) {   // drum grid
					if (ex) { int L = drumLongest(); gDcI = (gDcI + ex + L) % L; gDgPg = gDcI / 16; gDgFollow = 0; }
					if (ey) { gDcR -= ey; if (gDcR < 0) gDcR = 0; if (gDcR > DG_ROWS - 1) gDcR = DG_ROWS - 1; gDgSel = gDcR; }   // (silent: moving the cursor doesn't play the drum)
				} else if (mode == 1) {   // piano roll step editor: up / down = setting, left / right = value, B = done
					if (ey) { int idx = 0, n = 0; for (int k = 0; k < 8; k++) if (SE_PAGE[gSeTab][k] >= 0) { if (SE_PAGE[gSeTab][k] == gSeSel) idx = n; n++; }
						idx = (idx - ey + n) % n; for (int k = 0, m = 0; k < 8; k++) if (SE_PAGE[gSeTab][k] >= 0) { if (m == idx) gSeSel = SE_PAGE[gSeTab][k]; m++; } }
					if (ex) seSet(gSeSel, seGet(gSeSel) + ex);
					if (kDown & KEY_B) { kDown &= ~KEY_B; gStepEd = -1; botDirty = 1; }
				} else {   // drum step / SOUND editor
					int nf = gDgEd == 1 ? 3 : 12, r = gDgEdR;
					if (ey) gDgSelF = (gDgSelF % nf - ey + nf) % nf;
					if (ex) {
						static int marked = 0; if (!marked || !curShown()) { drumGridMark(); marked = 1; }
						if (gDgEd == 1) { int i = gDgEdI; u8* vp = r < 3 ? &gMyDrum[r][i] : &gDrumX[r - 3][i];
							if (gDgSelF == 0) { int v = *vp + ex * 12; *vp = (u8)(v < 8 ? 8 : v > 255 ? 255 : v); }
							else if (gDgSelF == 1) { int v = gDrRat[r][i] + ex; gDrRat[r][i] = (u8)(v < 0 ? 0 : v > 3 ? 3 : v); }
							else gDrCond[r][i] = (u8)((gDrCond[r][i] + NCOND + ex) % NCOND); }
						else if (gDgSelF < 8) { int k = gDgSelF, st = (DV_MAX[k] - DV_MIN[k]) / 48; if (st < 1) st = 1; dvSet(&gDV[r], k, dvGet(&gDV[r], k) + ex * st);
							if (k != 0 && k != 6 && k != 7) gDvRebuildT = osGetTime() + 350; else gDvPlay = r + 1; }
						else if (gDgSelF == 8) gDrRate[r] = (u8)((gDrRate[r] + NDRATE + ex) % NDRATE);
						else if (gDgSelF == 9) { int v = gDrLen[r] + ex; gDrLen[r] = (u8)(v < 1 ? 1 : v > DSTEP ? DSTEP : v); }
						else if (gDgSelF == 10) drumEuclid(r, ex); else drumRotate(r, ex);
						gDrumSaveDue = 1;
					}
					if (kDown & KEY_B) { kDown &= ~KEY_B; gDgEd = 0; botDirty = 1; }
				}
				if (gDvRebuildT && osGetTime() > gDvRebuildT && !hx) { gDvRebuildT = 0; gDrumWant = gKit + 100; gDvPlay = gDgEdR + 1; }   // SOUND: rebuild once the pad lets go
				static u64 bT = 0; static int bHeld = 0, bDone = 0;
				if ((mode == 4 || mode == 2) && !curShown() && (kDown & (KEY_A | KEY_B))) { kDown &= ~(KEY_A | KEY_B); gCurT = osGetTime(); botDirty = 1; aDown = 0; }   // A / B with no cursor up: bring it up first
				if ((mode == 4 || mode == 2) && curShown()) {   // B: tap = take the note / hit away; hold on the drums = that drum's SOUND
					if (kDown & KEY_B) { kDown &= ~KEY_B; bHeld = 1; bDone = 0; bT = osGetTime(); }
					else if (bHeld && (kHeld & KEY_B)) { if (mode == 2 && !bDone && osGetTime() - bT > 450) { bDone = 1; gDgEd = 2; gDgEdR = gDcR; gDgSelF = 0; gCurT = osGetTime(); botDirty = 1; } }
					else if (bHeld) { bHeld = 0; if (!bDone) { gCurT = osGetTime(); botDirty = 1;
						if (mode == 4) { int h = noteCover(gRcS, gRcN, NULL); if (h >= 0) { undoMark(); stepSetNote(h, gRcN, 0); } }
						else if (drumCellOn(gDcR, gDcI)) { drumGridMark(); drumCellSet(gDcR, gDcI, 0); } } }
				}
				if ((mode == 4 || mode == 2) && curShown()) {   // A: tap = note / hit, hold = its editor
					if (aDown) { kDown &= ~KEY_A; aHeld = 1; aDone = 0; aT = osGetTime(); }
					else if (aHeld && (kHeld & KEY_A)) { kDown &= ~KEY_A;
						if (!aDone && osGetTime() - aT > 450) { aDone = 1; gCurT = osGetTime(); botDirty = 1;
							if (mode == 4) { undoMark(); if (noteCover(gRcS, gRcN, NULL) < 0 && gSeq[gRcS].n < 4) stepSetNote(gRcS, gRcN, 1); gStepEd = gRcS; gStepEdRow = gRcN; gSeDrag = -1; gSeSel = SE_PAGE[gSeTab][0]; }
							else { drumGridMark(); if (!drumCellOn(gDcR, gDcI)) drumCellSet(gDcR, gDcI, 1); gDgEd = 1; gDgEdR = gDcR; gDgEdI = gDcI; gDgSelF = 0; } } }
					if (aUp) { aHeld = 0; if (!aDone) { gCurT = osGetTime(); botDirty = 1;
						if (mode == 4) { undoMark(); int h = noteCover(gRcS, gRcN, NULL); if (h >= 0) stepSetNote(h, gRcN, 0); else if (gSeq[gRcS].n < 4) { stepSetNote(gRcS, gRcN, 1); audition(gRcN); } }
						else { drumGridMark(); drumCellSet(gDcR, gDcI, !drumCellOn(gDcR, gDcI)); if (drumCellOn(gDcR, gDcI) && gCsndOK && gDrumBuilt == gKit) { drumHit(gDcR, 0.85f); csndExecCmds(false); } } } }
				} else aHeld = 0;
			}
		}
		if (kDown & KEY_X) { page = navStep(page, 1); topDirty = 1; }
		if (kDown & KEY_Y) { page = navStep(page, -1); topDirty = 1; }
		if (kDown & KEY_A) { if (page != 0) gChord = (gChord + 1) % NCHORD; topDirty = 1; }   // (the browser saves with its SAVE button or START > SAVE PATCH, not A)
		if (gThemePick && (kDown & KEY_B)) { gThemePick = 0; kDown &= ~KEY_B; botDirty = 1; }   // B closes the theme picker
		if (kDown & KEY_B) { latch = !latch; if (!latch) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } topDirty = botDirty = 1; }
		if (gN3 > 0 && (kDown & KEY_ZR)) {   // ZR: the sequence plays / stops
			gSeqMode = gSeqMode == 1 ? 0 : 1; if (gSeqMode == 1 && gSeqBase < 0) gSeqBase = gridNote(0);
			if (gSeqMode == 0) { seqStop(); if (gLiveRec || gPreFor == 2) liveRecStop(); }
			say(gSeqMode ? "ZR: PLAY" : "ZR: STOP"); topDirty = botDirty = 1;
		}
		if (gN3 > 0 && (kDown & KEY_ZL)) { liveRecToggle(); topDirty = botDirty = 1; }   // ZL: live REC (with the count-in)
		if (kDown & (KEY_L | KEY_R)) {   // shoulders: the bottom screen's tabs, left / right (OSC FILT MOD VOICE FX SEQ PADS); octaves are the OCT buttons
			int d = (kDown & KEY_R) ? 1 : -1, cur = gPads ? 6 : chipOfPage(page), b = cur < 0 ? (d > 0 ? 0 : NCHIP - 1) : (cur + d + NCHIP) % NCHIP;
			if (b == 6) { gPads = 1; gPadSel = -1; if (page == 7) page = 1; if (!latch) playSet(NULL, 0, 0.8f); }
			else {
				gPads = 0;
				static const int PG[6] = {1, 4, 5, 6, 10, 7}; page = PG[b];
				if (b == 2) { gMxDrag = -1; gMxPick = -1; gLfPick = -1; }
			}
			static const char* TN[NCHIP] = {"OSC", "FILTER", "MOD", "VOICE", "FX", "SEQ", "PADS"}; say(TN[b]);
			topDirty = botDirty = 1;
		}
		if (startAct == 0) { browseSave(); if (gSaveArm >= 0) browseSave(); }   // one press from the menu: no "A again"
		if (startAct == 1) page = 0;
		if (startAct == 2) { latch = 0; if (gSeqMode == 1) gSeqMode = 0; seqStop(); playSet(NULL, 0, 0.8f); curKey = -1; gProgAuto = 0;
			panicAll();
			say("PANIC: EVERYTHING STOPPED"); }
		if (startAct == 4) { gThemePick = 1; botDirty = 1; }   // THEME: the picker, every theme at once
		if (startAct >= 0) topDirty = topAgain = botDirty = botAgain = 1;

		if (page == 0) {   // preset browser: the D-pad moves through the list itself
			int ds = 0, dp = 0, fresh = 0;
			if (kDown & KEY_DLEFT) { gBrFocus = 0; topDirty = 1; } else if (kDown & KEY_DRIGHT) { gBrFocus = 1; topDirty = 1; }   // < > : the STYLES list or the PATCHES list
			if (kDown & (KEY_DUP | KEY_DDOWN)) { dp = (kDown & KEY_DDOWN) ? 1 : -1; repeatT = 0.35f; }
			else if (kHeld & (KEY_DUP | KEY_DDOWN)) { repeatT -= dt; if (repeatT <= 0) { dp = (kHeld & KEY_DDOWN) ? 1 : -1; repeatT = gBrFocus ? 0.09f : 0.2f; } }
			if (!gBrFocus && dp) { ds = dp; dp = 0; }   // up / down in STYLES: the next style (its first patch plays)
			fresh = (kDown & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) != 0;
			gBrowseDefer = dp && !fresh;   // held: the list moves, the patch loads when it stops
			if (ds || dp) { if (browseStep(ds, dp, fresh)) { curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } topDirty = 1; }
			gBrowseDefer = 0;
			if (gBrowsePend && !(kHeld & (KEY_DUP | KEY_DDOWN))) { gBrowsePend = 0; presetLoad(gStyle, gPreset); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; topDirty = botDirty = 1; }
			kDown &= ~(KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT); kHeld &= ~(KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
		}
#ifdef FAKE_WEB
		{   // the web version's top screen is a touch screen too: tap a setting to pick it, drag along it to change it
			static int was = 0, ax = 0;
			if (TTOUCH && !was && page == 0 && TTY >= 32 && TTY < 222) {   // the PATCH page's lists: a style shows its patches, a patch loads
				if (TTX >= 6 && TTX < 84) { int k = (TTY - 32) / 10, i = k >= 0 && k < NVSTY ? VSTY[k] : -1; if (i >= 0 && i != gStyle) { gStyle = i; gPreset = 0; gBrStyle = -1; topDirty = botDirty = 1; } }
				else if (TTX >= 86 && TTX < 200) { int i = (TTY - 32) / 11, n = presetCount(gStyle) + (gStyle == NSTYLES - 1);
					if (i < n) { gPreset = i - 1; if (browseStep(0, 1, 1)) { curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } topDirty = botDirty = 1; } }
			}
			if (TTOUCH && page != 0 && TTX >= 204 && TTY >= 30 && TTY < 214) {
				int np = pageLen(page), first = sel[page] - 14; if (first < 0) first = 0;
				int r = first + (TTY - 31) / 12;
				if (!was) { if (r < np) { sel[page] = r; topDirty = 1; } ax = TTX; }
				else { int d = (TTX - ax) / 10; if (d && !gQuickDir) { gQuickDir = d > 0 ? 1 : -1; ax += gQuickDir * 10; } }
			}
			was = TTOUCH;
		}
#endif
		// parameter editing
		int n = pageLen(page);
		if (kDown & KEY_DUP)   { sel[page] = (sel[page] + n - 1) % n; topDirty = 1; }
		if (kDown & KEY_DDOWN) { sel[page] = (sel[page] + 1) % n; topDirty = 1; }
		int dir = 0, fast = 0;
		if (gQuickDir) { dir = gQuickDir; gQuickDir = 0; }   // QUICK SELECT picked a value: land on it through the normal path
		if (kDown & (KEY_DLEFT | KEY_DRIGHT)) { dir = (kDown & KEY_DRIGHT) ? 1 : -1; repeatT = 0.35f; }
		else if (kHeld & (KEY_DLEFT | KEY_DRIGHT)) { repeatT -= dt; if (repeatT <= 0) { dir = (kHeld & KEY_DRIGHT) ? 1 : -1; repeatT = 0.05f; fast = 1; } }
		else if (gStickDX != 0 && page != 0 && !gStartMenu) {   // circle pad DATA: the further it's pushed, the faster the value moves (2 to 30 steps a second)
			static float acc = 0; acc += gStickDX * fabsf(gStickDX) * dt * 30 + (gStickDX > 0 ? 2 : -2) * dt;
			if (acc >= 1) { dir = 1; fast = 1; acc -= 1; } else if (acc <= -1) { dir = -1; fast = 1; acc += 1; }
			if (acc > 1) acc = 1; if (acc < -1) acc = -1;
			if (dir && gStepEd >= 0) {   // step editor open: the pad moves the selected step setting one notch at a time
				int k = gSeSel; seSet(k, seGet(k) + dir); dir = 0; botDirty = 1;
			}
			if (dir && PAGES[page][sel[page]].p == &gBPM) {   // the circle pad never moves the tempo: too easy to knock while playing
				dir = 0; acc = 0;
				static float hintT = 0; if (hintT <= 0) { say("TEMPO: USE THE D-PAD"); hintT = 3; }
				hintT -= dt;
			}
		}
		if (dir && PAGES[page][sel[page]].p == &gTheme) { dir = 0; gThemePick = !gThemePick; botDirty = 1; }   // THEME: < or > opens the picker
		if (dir) {
			Param* p = &PAGES[page][sel[page]];
			if ((p->dirty & (D_LOOPTAB | D_MICTAB | D_CAMTAB | D_DLWT | D_UPDATE)) && dir > 0 && !fast) {
				if (p->dirty & D_UPDATE) upStart();
				else if ((p->dirty & D_DLWT) && !strcmp(p->name, "NET > TABLE")) netTabStart();
				else if ((p->dirty & D_DLWT) && !strcmp(p->name, "RULE > TABLE")) ruleTable();
				else if ((p->dirty & D_DLWT) && !strcmp(p->name, "KIT > TABLE")) kitToTable();
				else if ((p->dirty & D_DLWT) && !strcmp(p->name, "MIC > PAD")) micPadStart();
				else if ((p->dirty & D_DLWT) && !strcmp(p->name, "WEB UPLOAD")) webUpStart();
				else if (p->dirty & D_DLWT) dlStart();
				else if (p->dirty & D_LOOPTAB) loopToTable();
				else if (p->dirty & D_MICTAB) micRecStart();
				else { gCamWant = 1; say("CAMERA: HOLD STILL..."); }
				topDirty = 1;
			}
			else if ((p->dirty & D_TILT) && dir > 0 && !fast) { tiltRecenter(); topDirty = 1; }
			else if ((p->dirty & (D_REST | D_CLEAR)) && dir > 0 && !fast) {
				static u64 clrArm = 0;   // CLEAR takes two presses of RIGHT, so a slip doesn't wipe the pattern
				if (p->dirty & D_REST) seqRest();
				else if (osGetTime() - clrArm > 2500) { clrArm = osGetTime(); say("PRESS > AGAIN TO CLEAR THE PATTERN"); }
				else { clrArm = 0; undoMark(); seqClear(); say("PATTERN CLEARED (SELECT+L UNDOES)"); }
				topDirty = 1;
			} else if ((p->dirty & (D_SAVE | D_INIT)) && dir > 0 && !fast) {
				if (p->dirty & D_SAVE) { patchCapture(&gUser[gSaveSlot]); userSave(); gStyle = NSTYLES - 1; gPreset = gSaveSlot; }
				else { SPatch ip; patchInit(&ip); initWire(&ip); patchApply(&ip); }
				curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; topDirty = 1;
			} else if ((p->dirty & (D_PRESET | D_STYLE)) && gEdited && !gGuard && !fast) {
				gGuard = 1; say("PATCH EDITED: PRESS AGAIN TO LEAVE IT (SAVE IT IN USER)"); topDirty = 1;
			} else if ((page != 0 && p->names != PRESS_RIGHT ? patchMark(p) : (void)0), paramChange(p, dir, fast)) {
				if (page != 0 && p->names != PRESS_RIGHT) {   // VALUE HUD: the new value, big, for a moment
					snprintf(gCallCtx, sizeof(gCallCtx), "%s > %s", PAGE_NAMES[page], p->name);   // which control this is
					paramFmt(p, gCallTxt, sizeof(gCallTxt));   // (paramFmt knows the TABLE / PRESET name stand-ins; indexing names[] directly read past them)
					gCallT = p->type == PT_E ? 0.9f : 0.6f; gCallP = p;
				}
				if (page != 0 && page != 8 && page != 9) gEdited = 1;
				if ((p->dirty & D_STYLE) && styHidden(gStyle)) gStyle = dir > 0 ? 16 : 13;   // DRUMS / GROOVE aren't in the list
				if (p->p == &gKitDir) { gKit = KIT_SAMPLE; gDrumWant = KIT_SAMPLE + 100; }   // another SAMPLE folder: build it
				if (p->dirty & D_STYLE) { gPreset = 0; presetLoad(gStyle, 0); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
				if (p->dirty & D_OUT) outputApply();
				if ((p->dirty & (D_DRUMS | D_EUCLID | D_DREC)) || p->p == &gBeatSel || p->p == &gDrumSel[0] || p->p == &gDrumSel[1] || p->p == &gDrumSel[2] || p->p == &gDrumLevel) gBeatFromPatch = 0;   // you changed the beat: it stays when you change patch
				for (int r = 0; r < 3; r++) if (p->p == &gDrumSel[r]) drumPaste(r, gDrumSel[r], 1);
				if (p->p == &gBeatSel) beatPaste(gBeatSel);   // a factory pattern: pasted into the grid
				if ((p->dirty & D_DRUMS) && p->p == &gDrumRec) drumRecChanged();
				else if (p->dirty & D_DRUMS) drumsChanged();
				else if (p->dirty & D_EUCLID) {   // touching a row's HITS or ROTATE switches that row to EUCLID and starts the drums
					for (int r = 0; r < 3; r++) if (p->p == &gEuHits[r] || p->p == &gEuRot[r]) *DPAT[r] = EU_IDX[r];
					if (!gDrums) { gDrums = 1; drumsChanged(); }
				}
				if (p->dirty & D_CHIP) { float lv = gChipLevel; gChipLevel = 1; chipSet(lv); }
				if (p->dirty & D_GRAIN) grainsChanged();
				if ((p->dirty & D_DLYT) && p->p == &gOfType) ofReset();
				else if (p->dirty & D_DLYT) dlyTypeChanged();
				if ((p->dirty & D_CAMV) && p->p == &gAddEdit) { if (gAddEdit) { addApply(); say("DRAW HARMONICS ON THE BOTTOM SCREEN"); } else if (gAddFr) { wtInstall("ADD", gAddFr); } botDirty = 1; }
				else if (p->dirty & D_CAMV) vidChanged();
				if (p->dirty & D_NETMIDI) netMidiChanged();
				if (p->dirty & D_LINK) linkChanged();
				if (p->dirty & D_THEME) { botDirty = 1; gBrightSaveDue = 1; }   /* (saves the theme with the settings) */
				if (p->dirty & (D_SCALE | D_SEQ)) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; botDirty = 1; if (p->p == &gSeqMode && gSeqMode == 2) gRecPos = 0; if (p->p == &gSeqMode && gSeqMode != 1) gSeqTr = 0; }
				if (p->dirty & D_PRESET) { presetLoad(gStyle, gPreset); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
				if (p->dirty & (D_TBLA | D_TBLB)) LightLock_Lock(&gMipsLock);
				if (p->dirty & D_TBLA) gTblDue |= 1;   // loaded after the voice lock is let go: a user table comes off the SD card
				if (p->dirty & D_TBLB) gTblDue |= 2;
				if (p->dirty & (D_TBLA | D_TBLB)) LightLock_Unlock(&gMipsLock);
				if (p->dirty & (D_A | D_TBLA)) gForceBuild |= 1;
				if (p->dirty & (D_B | D_TBLB)) gForceBuild |= 2;
				if (p->dirty & D_SUB) { LightLock_Lock(&gMipsLock); buildSub(); LightLock_Unlock(&gMipsLock); }
				if (p->dirty & D_NOISE) buildNoise();
				if (p->p == &gVoiceMode || p->p == &gFx || p->p == &A->on || p->p == &B->on || p->p == &A->uni || p->p == &B->uni) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
				topDirty = 1;
			}
		}

		circlePosition cp; hidCircleRead(&cp);
		// circle pad: soft dead zone (no jump at its edge), clamp, then smooth (~35 ms) so bends and STICK mod glide
		static float sx = 0, sy = 0;
		float rx = cp.dx / 150.0f, ry = cp.dy / 150.0f;
		if (kHeld & KEY_SELECT) rx = ry = 0;   // held SELECT: the pad drives QUICK SELECT, not bend / data
		// wide centre dead zone on L/R (bend) and a curved response so small moves bend gently
		#define DZ(v, z) (fabsf(v) < (z) ? 0.0f : ((v) > 0 ? ((v) - (z)) : ((v) + (z))) / (1.0f - (z)))
		rx = DZ(rx, 0.30f); ry = DZ(ry, 0.15f);
		rx = rx * fabsf(rx);
		if (rx > 1) rx = 1;
		if (rx < -1) rx = -1;
		if (ry > 1) ry = 1;
		if (ry < -1) ry = -1;
		float k = 1.0f - expf(-dt / 0.035f);
		sx += (rx - sx) * k; sy += (ry - sy) * k;
		if (fabsf(sx) < 0.001f) sx = 0;
		if (fabsf(sy) < 0.001f) sy = 0;
		if (gEdStick || gStickMenu) { sx = 0; sy = 0; }   // an editor (or a list: START menu, themes, the browser) has the circle pad
		float cx = sx;
		gStick = sy;
		int stickRouted = 0; for (int m = 0; m < NMOD; m++) if (gMod[m].src == SRC_STICK && gMod[m].amt != 0) stickRouted = 1;
		if (gStickMode == SM_DATA || gStepEd >= 0) { gStickDX = fabsf(sx) > 0.12f ? sx : 0; cx = 0; }   // DATA (and the step editor, any mode): X edits the selected value (no bend), Y stays the STICK source
		else gStickDX = 0;
		if ((gStickMode == SM_PAGE || gStickMode == SM_MACROS) && !stickRouted && gStepEd < 0) {   // PAGE / MACROS (a patch that routes STICK keeps bend + STICK): the pad pushes two values up and down (hold it over to keep moving)
			float* px_ = NULL; float* py_ = NULL;
			if (gStickMode == SM_MACROS) { px_ = &gMacro[0]; py_ = &gMacro[1]; }
			else pagePair(page, &px_, &py_);
			// speed grows with how far the pad is pushed: a nudge is fine control, all the way over sweeps the range in under a second
			float rx = sx * fabsf(sx) * 1.2f * dt, ry = sy * fabsf(sy) * 1.2f * dt;
			int mx = stickNudge(px_, rx); const Param* qx = gStickP; int my = stickNudge(py_, ry); const Param* qy = gStickP;
			if (mx || my) {
				topDirty = 1; botDirty = 1;
				int useX = mx && (!my || fabsf(sx) >= fabsf(sy)); const Param* q = useX ? qx : qy; float* pv = useX ? px_ : py_;
				snprintf(gCallCtx, sizeof(gCallCtx), "%s", gStickMode == SM_MACROS ? "CIRCLE PAD: MACROS" : PAGE_NAMES[page]);
				if (q) { char v[16]; paramFmt(q, v, sizeof(v)); snprintf(gCallTxt, sizeof(gCallTxt), "%.10s %s", q->name, v); gCallP = q; }   // show what the pad is moving
				else { int k = pv == &gMacro[0] ? 1 : pv == &gMacro[1] ? 2 : 0; snprintf(gCallTxt, sizeof(gCallTxt), "MACRO %d %d%%", k, (int)lroundf(*pv * 100)); gCallP = NULL; }
				gCallT = 0.6f;
			}
			cx = 0; gStick = 0;
		}
		if (gN3 < 0) { bool n3 = false; APT_CheckNew3DS(&n3); gN3 = n3 ? 1 : 0; }
		if (gN3) {   // NEW 3DS CONTROLS: the C-stick is STICK 2 (a mod source); routed nowhere, it moves this page's two values
			circlePosition cs; hidCstickRead(&cs);
			float qx = DZ(cs.dx / 150.0f, 0.12f), qy = DZ(cs.dy / 150.0f, 0.12f); if (qx > 1) qx = 1; if (qx < -1) qx = -1; if (qy > 1) qy = 1; if (qy < -1) qy = -1;
			float kk = 1.0f - expf(-dt / 0.035f); gStick2X += (qx - gStick2X) * kk; gStick2Y += (qy - gStick2Y) * kk;
			if (fabsf(gStick2X) < 0.001f) gStick2X = 0; if (fabsf(gStick2Y) < 0.001f) gStick2Y = 0;
			int routed2 = 0; for (int m = 0; m < NMOD; m++) if ((gMod[m].src == SRC_STK2X || gMod[m].src == SRC_STK2Y) && gMod[m].amt != 0) routed2 = 1;
			if (!routed2 && (gStick2X || gStick2Y)) {
				float* px_ = NULL; float* py_ = NULL; pagePair(page, &px_, &py_);
				int mx = stickNudge(px_, gStick2X * fabsf(gStick2X) * 1.2f * dt); const Param* qxp = gStickP; int my = stickNudge(py_, gStick2Y * fabsf(gStick2Y) * 1.2f * dt); const Param* qyp = gStickP;
				if (mx || my) {
					topDirty = 1; botDirty = 1; const Param* q = mx && (!my || fabsf(gStick2X) >= fabsf(gStick2Y)) ? qxp : qyp;
					snprintf(gCallCtx, sizeof(gCallCtx), "C-STICK: %s", PAGE_NAMES[page]);
					if (q) { char v[16]; paramFmt(q, v, sizeof(v)); snprintf(gCallTxt, sizeof(gCallTxt), "%.10s %s", q->name, v); gCallP = q; }
					gCallT = 0.6f;
				}
			}
		} else gStick2X = gStick2Y = 0;

		// touch
		int base = 48 + 12 * oct, key = -1, touching = !!(kHeld & KEY_TOUCH);
		if (remPads(base, latch)) topDirty = botDirty = 1;   // the phone's PADS page
		float vel = 0.8f;
		if (touching) {
			touchPosition t; hidTouchRead(&t);
			if (gThemePick) { if (!prevTouch && themePickTouch(t.px, t.py)) topDirty = botDirty = topAgain = botAgain = 1; }
			else if (t.py < TAB_H) {
				if (!prevTouch) {
					int b = t.px * NCHIP / 320, again = !gPads && chipOfPage(page) == b;
					if (b == 6) { gPads = !gPads; gPadSel = -1; if (gPads && page == 7) page = 1; /* the piano roll owns the whole screen: leave it for the pads */ if (!latch) playSet(NULL, 0, 0.8f); if (gPads) say("PADS: LOW = TRIAD, MID = 7TH, TOP = 9TH; EDGES = INVERSION / SUS4"); }
					else {
						gPads = 0;
						if (b == 0) page = again ? (page % 3) + 1 : 1;   // OSC A -> OSC B -> SUB+NOISE
						if (b == 1) { if (again) gShapeView = (gShapeView + 1) % 3; page = 4; }
						if (b == 2) { if (again) gModView = (gModView + 1) % 5; page = 5; gMxDrag = -1; gMxPick = -1; gLfPick = -1; }
						if (b == 3) page = 6;
						if (b == 4) page = 10;
						if (b == 5) { page = 7; if (!again) say("PIANO ROLL: TAP A NOTE IN, DRAG FOR LENGTH, HOLD TO EDIT THE STEP"); }
					}
					topDirty = botDirty = 1;
				}
			} else if (page == 0 && !gPads && t.py < WORK_B) {   // PATCH page: the browser
				if (!prevTouch) { if (browserTouch(t.px, t.py)) { curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } topDirty = botDirty = 1; }
			} else if (gDrumOnB && !gPads) {   // DRUMS page: the drum sequencer has the whole screen
				if (drumGridTouch(t.px, t.py, !prevTouch)) topDirty = botDirty = 1;
			} else if (t.py >= WORK_B && t.py < KB_Y && !gModOn && !gGridOn && !(gPads && !gGridOn)) {
				if (!prevTouch) {
					int b = t.px * NPERF / 320;
					if (b == 0) { if (gScale) { gScaleLast = gScale; gScale = 0; } else gScale = gScaleLast; char m[40]; snprintf(m, sizeof(m), "SCALE: %s %s", NOTE_NAMES[gRoot], SCALE_NAMES[gScale]); say(m); }
					if (b == 1) { gSeqMode = gSeqMode == 1 ? 0 : 1; if (gSeqMode == 1 && gSeqBase < 0) gSeqBase = gridNote(0); if (gSeqMode == 0) seqStop(); }
					if (b == 2) { latch = 0; if (gSeqMode == 1) gSeqMode = 0; seqStop(); playSet(NULL, 0, 0.8f); curKey = -1; gProgAuto = 0; panicAll(); say("PANIC: EVERYTHING STOPPED"); }
					topDirty = botDirty = 1;
				}
			} else if (gModOn && !gPads) {
				gLastTX = t.px; gLastTY = t.py;
				if (modViewTouch(t.px, t.py, !prevTouch, 0)) { botDirty = 1; topDirty = 1; }
			} else if (gPads && !gGridOn) {
				float pv = 0.8f;
				if (padsTouch(base, t.px, t.py, !prevTouch, &pv)) { botDirty = 1; topDirty = 1; }
			} else if (gChainOn && t.py < WORK_B && t.py >= TAB_H && !gGridOn && !gAddEdit && !gPads) {
				gLastTX = t.px; gLastTY = t.py;
				if (chainTouch(t.px, t.py, !prevTouch, 0)) { botDirty = 1; topDirty = 1; }
			} else if (gShapeOn && t.py < WORK_B && t.py >= TAB_H && !gGridOn && !gAddEdit && !gPads) {
				gLastTX = t.px; gLastTY = t.py;
				if (shapeTouch(t.px, t.py, !prevTouch, 0)) { botDirty = 1; topDirty = 1; }
			} else if (t.py < WORK_B && !gGridOn && gAddEdit) {
				addTouch(t.px, t.py); botDirty = 1;
			} else if (gGridOn) {
				gLastTX = t.px; gLastTY = t.py;
				if (gridTouch(t.px, t.py, !prevTouch, 0)) topDirty = botDirty = 1;
			} else if (xyShown() && t.py < WORK_B && t.py >= XY_Y) {
				gLastTX = t.px; gLastTY = t.py;
				if (xyTouch(t.px, t.py, !prevTouch, 0)) { botDirty = 1; topDirty = 1; }
			} else if (gVisOnB && t.py < WORK_B && t.py >= TAB_H && !gGridOn && !gAddEdit) {
				if (!prevTouch && visTilesTouch(t.px, t.py)) { botDirty = 1; topDirty = 1; }
			} else if (gHwOnB && t.py < WORK_B && t.py >= TAB_H) {
				if (gGrains && grainEdTouch(t.px, t.py, !prevTouch, 0)) { botDirty = 1; gEdited = 1; }
			} else if (t.py >= KB_Y && t.px < KB_X && !gModOn && !gGridOn && !(gPads && !gGridOn)) {   // the column left of the keys
				if (!prevTouch) {
					int b = (t.py - KB_Y) / SIDE_H; if (b > 3) b = 3;
					if (b == 0 && oct < 3) { oct++; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
					if (b == 1 && oct > -3) { oct--; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
					if (b <= 1) gOctFlash[b == 0] = 1;
					if (b == 2) { if (gChord) { gChordLast = gChord; gChord = 0; } else gChord = gChordLast; char m[32]; snprintf(m, sizeof(m), "CHORD: %s", gChord ? CHORD_NAMES[gChord] : "OFF"); say(m); }
					if (b == 3) { latch = !latch; if (!latch) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } }
					topDirty = botDirty = 1;
				}
			} else if (t.py >= KB_Y) {
				int deg0 = gKeyDeg;   // (keyAt sets the scale degree; a kept key keeps its own)
				key = keyAt(t.px, t.py, base);
				if (prevTouch && curKey >= 0 && key != curKey && gVoiceMode < 3) {   // hysteresis: a small slip off the edge doesn't change the note
					for (int d = 0; d < 4; d++) { static const int DX[4] = {-KEY_HOLD, KEY_HOLD, 0, 0}, DY[4] = {0, 0, -KEY_HOLD, KEY_HOLD};
						int yy = t.py + DY[d]; if (yy >= KB_Y && yy < 240 && keyAt(t.px + DX[d], yy, base) == curKey) { key = curKey; break; } }
					if (key == curKey) gKeyDeg = deg0; else keyAt(t.px, t.py, base);
				}
				vel = 0.35f + 0.65f * (t.py - KB_Y) / (float)(240 - KB_Y);
				if (gVoiceMode >= 3) { int py = (t.py - KB_Y) % PAD_ROW; vel = 0.35f + 0.65f * py / (float)(PAD_ROW - 1); }
			}
		}
		if (gModOn && !gPads && prevTouch && !touching && modViewTouch(gLastTX, gLastTY, 0, 1)) { botDirty = 1; topDirty = 1; }
		if (gShapeOn && prevTouch && !touching && shapeTouch(gLastTX, gLastTY, 0, 1)) { botDirty = 1; topDirty = 1; }
		if (gChainOn && prevTouch && !touching && chainTouch(gLastTX, gLastTY, 0, 1)) { botDirty = 1; topDirty = 1; }
		if (gChainOn && !gSelReq) { int b = fxbOfParam(PAGES[page][sel[page]].name); if (b >= 0 && b != gChSel) { gChSel = b; botDirty = 1; } }   // and the other way: the top selection picks the block
		if (gSelReq) { int n = pageLen(page); for (int i = 0; i < n; i++) if (!strcmp(PAGES[page][i].name, gSelReq)) { sel[page] = i; break; } gSelReq = NULL; topDirty = 1; }   // a bottom-screen editor asked for this setting up top
		if (gGridOn && prevTouch && !touching && gridTouch(gLastTX, gLastTY, 0, 1)) { botDirty = 1; topDirty = 1; }
		if (gSlDrag >= 0 && prevTouch && !touching && xyTouch(gLastTX, gLastTY, 0, 1)) botDirty = 1;
		if (xyGlideTick(dt) && xyShown()) botDirty = 1;
		if (!touching) {   // FOLLOW: the piano roll and the drum grid turn the page with the playhead
			if (gRollFollow && gSeqPlaying && gSeqPos >= 0 && gSeqPos / ROLL_VIEW != gRollPg) { gRollPg = gSeqPos / ROLL_VIEW; if (gGridOn) botDirty = 1; }
			if (gDgFollow) { int pp = drumPlayPage(); if (pp >= 0 && pp != gDgPg) { gDgPg = pp; if (gDrumOnB || gDrumInSeq) botDirty = 1; } }
		}
		{ static int dT = 0; if ((page == 7 || page == 11) && !gStartMenu && ++dT % 4 == 0) topDirty = 1; }   // the gesture demo moves
		{ static int gT = 0; if (xyShown() && ++gT % 3 == 0 && xyModulated()) botDirty = 1; }   // the ghost follows the modulation
		if (prevTouch && !touching && grainEdTouch(0, 0, 0, 1)) botDirty = 1;
		if (gDrumOnB && prevTouch && !touching && drumGridRelease()) botDirty = 1;   // let go of the grain editor: SCAN carries on from there
		if (gPads && prevTouch && !touching) { gStrumZone = -1; if (gLiveRec) liveUp(); if (!latch && gPadSel >= 0) playSet(NULL, 0, 0.8f); botDirty = 1; }   // let go of the pad
		padsTick(dt); bassTick(dt); if (gProgDirty) { gProgDirty = 0; if (gPads) botDirty = 1; }
		int freshPress = touching && !prevTouch;   // a new tap restrikes the key even if it's the one still held (LATCH, drum pads)
		prevTouch = touching;
		{ static int f = 0; if (gAddDirty && (++f % 6 == 0 || !touching)) { gAddDirty = 0; addApply(); topDirty = 1; } }   // additive editor: rebuild a few times a second while drawing
		if ((key != curKey || (freshPress && key >= 0)) && !(latch && key < 0)) {
			int notes[4], n = key >= 0 ? chordNotes(key, notes) : 0;
			if (gLiveRec) { if (key >= 0) liveDown(notes, n, vel); else liveUp(); playSet(notes, n, vel); botDirty = 1; }   // live REC: the keys play and write
			else if (gSeqMode == 1 && gSeqKeys && gTapeRec < 0) { if (key >= 0 && gSeqBase >= 0) { gSeqTr = key - gSeqBase; topDirty = 1; } }   // SEQ KEYS TRANSPOSE: the keys move the pattern
			else {
				if (gSeqMode == 2 && key >= 0) { seqRecord(notes, n, vel); topDirty = 1; }
				playSet(notes, n, vel);
			}
			curKey = key;
		}
		if (gVoiceMode >= 3 && (gDrumBuilt != gVoiceMode - 3 || gDrumCh[3] < 0)) { kitEnsure(); botDirty = 1; }
		{ static float st = 0; if (gDrumBuilt == 5 && waveSig() != gWaveSig) { st += 1.0f / 60; if (st > 0.25f) { gDrumWant = 105; gWaveSig = waveSig(); st = 0; } } else st = 0; }   // WAVE kit: the drums follow OSC A's table and WT POS (rebuilt once it settles)
		if (page != gCurPage) { gCurPage = page; botDirty = 1; }
		{ int g = page == 7; if (g != gGridOn) { gGridOn = g; botDirty = 1; if (!g) { gStepEd = -1; gSeqSaveDue = 1; } } }
		{ int m = page == 5; if (m != gModOn) { gModOn = m; botDirty = 1; } if (gModOn && !gPads) botDirty = 1; }
		{ int m = page == 4; if (m != gShapeOn) { gShapeOn = m; gShDrag = -1; botDirty = 1; } }
		{ int m = page == 10; if (m != gChainOn) { gChainOn = m; gChDrag = -1; botDirty = 1; } }
		{ int m = page == 8; if (m != gVisOnB) { gVisOnB = m; botDirty = 1; } m = page == 9; if (m != gHwOnB) { gHwOnB = m; botDirty = 1; } m = page == 11; if (m != gDrumOnB) { gDrumOnB = m; botDirty = 1; if (!m) gDgEd = 0; if (!m && gTapRow >= 0) { gTapRow = -1; gDrumRec = 0; gDrumSaveDue = 1; } }
		  { static int ls = -2; int st = gSeqPlaying ? (gSeqPos & 15) : (gDrums ? gDrumStep : -1); if ((gDrumOnB || gDrumInSeq) && st != ls) { ls = st; botDirty = 1; } }
		  gDrumInSeq = page == 7 && gGridOn && gPatView == 3 && !gPads;
		  if (!gDrumOnB && !gDrumInSeq && (gDgEd || gTapRow >= 0)) { gDgEd = 0; if (gTapRow >= 0) { gTapRow = -1; gDrumRec = 0; } } }
		{ static int hwT = 0; if (gHwOnB && ++hwT % 6 == 0) botDirty = 1; }   // live sensor meters   // live matrix dots, drawn LFO / motion playheads
		motTick(dt);
		if (gAuditionT > 0 && (gAuditionT -= 1.0f / 60) <= 0 && !gSeqPlaying && curKey < 0) playSet(NULL, 0, 0.8f);
		{ static int lastPos = -2, lastMode = -1; if (gGridOn && (gSeqPos != lastPos || gSeqMode != lastMode)) { lastPos = gSeqPos; lastMode = gSeqMode; botDirty = 1; } }
		if (gBeatFlash) { gBeatFlash = 0; gVisBeat = 1; beatLightPulse(); gVisBurst += 8; if (page == 7) topDirty = 1; }

		// rebuild oscillator cycles when WT POS / WARP (incl. modulation) changed
		SVoice* newest = NULL;
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && (!newest || gV[i].age > newest->age)) newest = &gV[i];
		float pe[2], we[2];
		pe[0] = A->pos + modSum(DST_APOS, newest); pe[1] = B->pos + modSum(DST_BPOS, newest);
		we[0] = A->warpAmt + modSum(DST_AWARP, newest); we[1] = B->warpAmt + modSum(DST_BWARP, newest);
		for (int o = 0; o < 2; o++) {
			if (pe[o] < 0) pe[o] = 0;
			if (pe[o] > 1) pe[o] = 1;
			if (we[o] < 0) we[o] = 0;
			if (we[o] > 1) we[o] = 1;
		}
		{   // slew: a note's envelope can yank WT POS / WARP in one step (EP SATURDAY's FM attack); every ringing voice
			// shares the table, so that jump was a click in the other notes. Big moves now spread over a few frames.
			static float sp[2] = {-1, -1}, sw[2] = {-1, -1}; const float mx = 0.08f, mxw = 0.035f;   // WARP slower: a warp jump reshapes every ringing voice at once (STEEL PAN chords clicked)
			for (int o = 0; o < 2; o++) {
				if (sp[o] < 0 || gForceBuild) { sp[o] = pe[o]; sw[o] = we[o]; }
				sp[o] += fmaxf(-mx, fminf(mx, pe[o] - sp[o])); sw[o] += fmaxf(-mxw, fminf(mxw, we[o] - sw[o]));
				pe[o] = sp[o]; we[o] = sw[o];
			}
		}
		int chg[2];
		for (int o = 0; o < 2; o++) chg[o] = (gForceBuild & (1 << o)) || fabsf(pe[o] - lastPos[o]) > 0.002f || fabsf(we[o] - lastWarp[o]) > 0.002f;
		if (chg[1] && A->warp >= W_FM) chg[0] = 1;
		if (chg[0] && B->warp >= W_FM) chg[1] = 1;
		if ((chg[0] || chg[1]) && !AGET(gJobBusy)) {
			RebuildJob j;
			for (int o = 0; o < 2; o++) {
				j.pe[o] = pe[o]; j.we[o] = we[o]; j.chg[o] = chg[o]; j.used[o] = usedLevels(o);
				if (chg[o]) { lastPos[o] = pe[o]; lastWarp[o] = we[o]; gPosEff[o] = pe[o]; }
			}
			if (gWorker) { gJob = j; ASET(gJobTaken, 0); ASET(gJobBusy, 1); LightEvent_Signal(&gJobEv); }   // core 1
			else { gMainJob = j; gMainJobDue = 1; }   // no core 1: rebuilt below, after the voice lock is let go (it used to stall the clock)
			gForceBuild = 0;
		}
		if (AGET(gJobDone)) { ASET(gJobDone, 0); topDirty = 1; }

		if (!gAudioClock && !gMirror) { arpTick(dt); seqTick(dt); }
		{	// runtime milestones for the crash guard (written once each)
			static int m1 = 0, m2 = 0, m3 = 0, m4 = 0;
			if (!m1) { m1 = 1; crashMark("RUNNING"); }
			if (!m2 && gBusSeen) { m2 = 1; crashMark("FX BUS RUNNING"); }
			if (!m3 && (gArpOnN || gSeqPlaying)) { m3 = 1; crashMark("ARP/SEQ RUNNING"); }
			if (!m4 && visActive(page)) { m4 = 1; crashMark("GPU VISUALS"); }
			static int lastMarkPage = -1;
			if (page != lastMarkPage) { lastMarkPage = page; gPageMarkDue = 1; }   // written after the voice lock is released
		}
		gFxModVal = modSum(DST_FXP, newestVoice()); gFxMixMod = modSum(DST_FXMIX, newestVoice());
		if (gMirOn) mirSend(page, sel[page], helpOn, oct, latch, cx + gMidiBend);   // a mirror is listening (Wi-Fi)
		if (gMirror) {   // this is the mirror: the 3DS's page, its bend
			if (gMirUIn) { gMirUIn = 0; mirUiApply(); if (gMirUI.page >= 0 && gMirUI.page < NPAGES) { page = gMirUI.page; sel[page] = gMirUI.sel; } if (gMirVis) { page = 8; gQuickBar = -1; gStartMenu = 0; } helpOn = gMirUI.help; oct = gMirUI.oct; latch = gMirUI.latch; topDirty = botDirty = 1; }
			if (gMirVis && page != 8) { page = 8; topDirty = 1; }
			cx = gMirBend; gMidiBend = 0;
		}
		if (gAudioOK) voicesUpdate(dt, cx + gMidiBend);
		// effect-bus self check: if notes are sending but the DSP never delivers aux audio, fall back to taps
		if (gBusOK && !gBusFail && !gBusSeen && (masterActive() || busSend(0) > 0 || busSend(1) > 0)) {
			int any = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started && !gV[i].isEcho) any = 1;
			if (any) { gBusWait += dt; if (gBusWait > 1.5f) { gBusFail = 1; topDirty = 1; } }
		}
		LightLock_Unlock(&gVoiceLock);
		// SD writes wait until here, so a page press never holds the lock the audio tick needs
		if (gPageMarkDue) { gPageMarkDue = 0; settingsSave(page); char m[32]; snprintf(m, sizeof(m), "PAGE %s%s", PAGE_NAMES[page], visActive(page) ? " (GPU)" : ""); crashMark(m); }
		if (gTblDue) { int t = gTblDue; gTblDue = 0; LightLock_Lock(&gMipsLock); if (t & 1) tableSelect(&A->tb, A->table); if (t & 2) tableSelect(&B->tb, B->table); LightLock_Unlock(&gMipsLock); gForceBuild |= t; topDirty = 1; }
		if (gMainJobDue) { gMainJobDue = 0; LightLock_Lock(&gMipsLock); doRebuild(&gMainJob); LightLock_Unlock(&gMipsLock); ASET(gJobDone, 1); }
#ifndef SYRUP_NO_NET
		{   // LFO CC OUT: 30 times a second, only when the value moved
			static int div = 0, last[2] = {-1, -1};
			if (++div >= 2) { div = 0; for (int l = 0; l < 2; l++) if (gLfoCC[l] > 0 && gPeerOk) {
				int v = (int)lroundf((gLfo[l].val + 1) * 63.5f); v = v < 0 ? 0 : v > 127 ? 127 : v;
				if (v != last[l]) { last[l] = v; rtpSend(0xB0, (u8)gLfoCC[l], (u8)v); } } }
		}
#endif
		kitSync();
		drumService();
		if (gDrumSaveDue) { gDrumSaveDue = 0; myDrumSave(); }
		if (gLearnSaveDue) { gLearnSaveDue = 0; learnSave(); }
		{ static u64 at = 0; u64 tn = osGetTime(); if (tn - at > 5000) { at = tn; if (lastSnap()) lastWrite(); gSeqSaveDue = 1; } }   // AUTOSAVE: the sound and the sequences every 5 s
		if (gBrightSaveDue) { gBrightSaveDue = 0; settingsSave(page); }
		if (gSeqSaveDue) { gSeqSaveDue = 0; LightLock_Lock(&gVoiceLock); seqSnap(); LightLock_Unlock(&gVoiceLock); seqWriteSnap(); }
		{ static int sk = 0; int now = skipRecent(); if (now != sk) { sk = now; topDirty = 1; } }

		memset(on, 0, sizeof(on));
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4 && gV[i].note >= 0 && gV[i].note < 128) on[gV[i].note] = 1;
		for (int i = 0; i < 2; i++) if (gOctFlash[i] > 0) { gOctFlash[i] -= 5.0f / 60; botDirty = 1; }
		if (gVoiceMode >= 3) { static int was = 0; int any = 0; for (int d = 0; d < NDRUM; d++) if (gPadFlash[d] > 0) { gPadFlash[d] -= 6.0f / 60; any = 1; } if (any || was) botDirty = 1; was = any; }   // pad flashes fade over ~1/6 s
		{ int fading = 0;   // a key let go keeps a short glow: slides leave a trail
			for (int n = 0; n < 128; n++) { if (onPrev[n] && !on[n]) gKeyFade[n] = 1; if (on[n]) gKeyFade[n] = 0; if (gKeyFade[n] > 0) { gKeyFade[n] -= 1.0f / 60 * 3.5f; if (gKeyFade[n] < 0) gKeyFade[n] = 0; fading = 1; } }
			if (fading) botDirty = 1; }
		if (memcmp(on, onPrev, sizeof(on))) { memcpy(onPrev, on, sizeof(on)); botDirty = 1; topDirty = 1; }
		gChordKey = gChord && curKey >= 0 ? curKey : -1;
		{ static int lastStep = -1; if (gSeqPlaying && gSeqPos != lastStep && !gGridOn) { lastStep = gSeqPos; botDirty = 1; } if (!gSeqPlaying && lastStep != -1) { lastStep = -1; botDirty = 1; } }

		gSelName = PAGES[page][sel[page]].name;
		if (!helpOn && visActive(page)) topDirty = 1;   // animated panel: redraw every frame
		if (!helpOn && page == 4 && newestVoice()) topDirty = 1;   // moving envelope dot
		if (!helpOn && page == 5) topDirty = 1;                   // moving LFO playheads
		if (!helpOn && (page == 9 || page == 10 || page == 11)) topDirty = 1;   // live meters / step lights
		{ static int was = 0; int act = gGridOn && gPatView == 2 && (gTapeRec >= 0 || gTapePlay); if (act || was) botDirty = 1; was = act; }   // tape heads moving
		if (topDirty || topAgain) {
			if (helpOn) drawHelp();
			else {
				drawTop(page, sel[page], oct, latch);
				if (visActive(page)) {
					visFrame(page, dt);
					fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
					if (page == 8) {
						static float showT = 0; static int lastSel = -1, lastVal = -999;
						Param* P = &PAGES[8][sel[8]]; int v = *(int*)P->p;
						if (sel[8] != lastSel || v != lastVal) { showT = 2.5f; lastSel = sel[8]; lastVal = v; }
						if (showT > 0) showT -= dt;
						if (gVisOverlay || showT > 0) {
							char ov[48], vv[24]; paramFmt(P, vv, sizeof(vv)); snprintf(ov, sizeof(ov), "%s: %s", P->name, vv);
							text(6, 4, ov, 1, 255, 200, 130);
							text(394 - (int)strlen(gPatchName) * 8, 4, gPatchName, 1, 200, 150, 90);
						}
					} else if (page <= 2) { Osc* o = &gOsc[page - 1]; frame(4, 30, 196, 177); text(8, 34, o->on ? (page == 2 ? "OSC B" : "OSC A") : (page == 2 ? "OSC B OFF" : "OSC A OFF"), 1, DIM); }
					else if (page == 4) { frame(4, 30, 196, 60); frame(4, 94, 196, 60); envLabels(); drawFilterPlot(4, 158, 196, 66); }
					else if (page == 5) { frame(4, 30, 196, 60); frame(4, 94, 196, 60); lfoLabels(); }
					else { frame(4, 30, 196, 125); text(8, 34, "A", 1, DIM); text(8, 96, "B", 1, DIM); }
					scanlines();
				}
				if (gQuickBar >= 0 || gQuickOpen) drawQuick(page);
				else if (gStartMenu) drawStartMenu();
				else if (gCallT > 0 && page != 8) {   // big value pop-up, drawn last so the visuals don't cover it
					fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
					const Param* cp = (const Param*)gCallP; int bar = cp && cp->type != PT_E && cp->mx > cp->mn;
					int w = (int)strlen(gCallTxt) * 16 + 16; if (bar && w < 160) w = 160;
					int cw = (int)strlen(gCallCtx) * 8 + 16; if (cw > w) w = cw; if (w > 396) w = 396;
					int mdst = cp ? paramDst(cp->p) : -1, msl = -1; float mamt = 0;
					if (mdst >= 0) for (int m = 0; m < NMOD; m++) if (gMod[m].src != SRC_OFF && gMod[m].dst == mdst && fabsf(gMod[m].amt) > fabsf(mamt)) { mamt = gMod[m].amt; msl = m; }
					int mcount = 0; if (mdst >= 0) for (int m = 0; m < NMOD; m++) if (gMod[m].src != SRC_OFF && gMod[m].dst == mdst && gMod[m].amt != 0) mcount++;
					char mtx[32] = ""; if (msl >= 0) { if (mcount > 1) snprintf(mtx, sizeof(mtx), "< %s %+d%% +%d MORE", SRC_NAMES[gMod[msl].src], (int)lroundf(mamt * 100), mcount - 1); else snprintf(mtx, sizeof(mtx), "< %s %+d%%", SRC_NAMES[gMod[msl].src], (int)lroundf(mamt * 100)); if ((int)strlen(mtx) * 8 + 20 > w) w = (int)strlen(mtx) * 8 + 20; }
					int x = 200 - w / 2, h = (bar ? 52 : 40) + (msl >= 0 ? 12 : 0) + 12;
					rect(x, 84, w, h, RGB3(TH->tab)); outline(x, 84, w, h, RGB3(TH->line));
					text(200 - (int)strlen(gCallCtx) * 4, 88, gCallCtx, 1, RGB3(TH->dim));   // what you're adjusting
					h -= 12;
					if (msl >= 0) text(200 - (int)strlen(mtx) * 4, 96 + h - 12, mtx, 1, RGB3(TH->tabTxt));   // what's moving it
					text(200 - (int)strlen(gCallTxt) * 8, 106, gCallTxt, 2, RGB3(TH->tabTxt));
					if (bar) {   // where it sits in its range; bipolar values grow from the centre so the sign shows
						float v = cp->type == PT_F ? *(float*)cp->p : (float)*(int*)cp->p, t = (v - cp->mn) / (cp->mx - cp->mn);
						t = t < 0 ? 0 : t > 1 ? 1 : t;
						int bx = x + 10, bw = w - 20, by = 130; rect(bx, by, bw, 8, RGB3(TH->bg)); outline(bx, by, bw, 8, RGB3(TH->line));
						if (cp->mn < 0 && cp->mx > 0) {
							int z = bx + (int)((0 - cp->mn) / (cp->mx - cp->mn) * bw), e = bx + (int)(t * bw);
							rect(z, by - 2, 1, 12, RGB3(TH->tabTxt));
							if (e > z) rect(z + 1, by + 2, e - z, 4, RGB3(TH->keyOn)); else if (e < z) rect(e, by + 2, z - e, 4, RGB3(TH->warn));
						} else rect(bx + 2, by + 2, (int)(t * (bw - 4)), 4, RGB3(TH->keyOn));
						if (msl >= 0) {   // the modulation ghost: where the sound actually is right now
							float mv = modSum(mdst, newestVoice()), r = cp->mx - cp->mn, et = t + (mdst == DST_CUTOFF ? mv * 5.0f / 7.97f : mv) / (r > 0 ? r : 1) * (cp->mx - cp->mn);
							et = et < 0 ? 0 : et > 1 ? 1 : et; int gx = bx + 2 + (int)(et * (bw - 4));
							rect(gx - 1, by - 3, 3, 14, RGB3(TH->warn));
						}
					}
				}
			}
			topAgain = topDirty; topDirty = 0;
		}
		if (botDirty || botAgain) { drawBottom(base, on, helpOn, latch); botAgain = botDirty; botDirty = 0; }

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	aptUnhook(&gSleepCookie);
	// background threads first (network MIDI, IR, downloads, camera) so nothing is mid-call while services close
	if (!gSafeMode) {
		if (gMidiNet) { gMidiNet = 0; netMidiChanged(); }
		linkStop();
		if (gDlThread) { threadJoin(gDlThread, 5000000000ULL); threadFree(gDlThread); gDlThread = NULL; }
		camStop();
	}
	if (gWorker) { ASET(gWorkerQuit, 1); LightEvent_Signal(&gJobEv); threadJoin(gWorker, U64_MAX); threadFree(gWorker); }
	// shut the audio clock down first: the sound thread starts drum / arp / sequencer notes on the second
	// sound chip, so it must be stopped (and out of its tick) before that chip's command memory goes away
	if (gAudioOK) ndspSetCallback(NULL, NULL);
	LightLock_Lock(&gVoiceLock);   // waits for a tick that's still running
	gAudioClock = 0; gDrums = 0;
	if (gCsndOK) {
		echoStop();   // capture units off and released before the chip goes away
		for (int c = 0; c < NCS; c++) if (gCsndOK && (csndChannels & BIT(c))) CSND_SetPlayState(c, 0);
		csWait();   // bounded: after a sleep or a fault the chip may never report back
		gCsndOK = 0;
		csndExit();
	}
	LightLock_Unlock(&gVoiceLock);
	gFxCore2 = 0; svcSleepThread(20000000); fxCore2Stop();   // effects back on the DSP thread before it goes away
	if (gAudioOK && !gDspStalled) { ndspSetCapture(NULL); for (int ch = 0; ch < NCH; ch++) ndspChnWaveBufClear(ch); ndspExit(); }   // a stalled DSP never answers ndspExit: skip it so the app can still close
	if (!gSafeMode) { extrasExit(); sensorsExit(); }
	myDrumSave();
	seqSave();
	lastSnap(); lastWrite();
	settingsSave(page);
	gpuExit();
	remove(CRASH_PATH);   // clean exit
	gfxExit();
	return 0;
}
