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
//   A chord mode   B latch   L / R octave   SELECT help   START quit
//   Circle pad: left/right pitch bend, up/down = STICK mod source

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <stddef.h>
#ifndef SYRUP_VER
#define SYRUP_VER "dev"
#endif
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

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

static int gGpuOK = 0, gGpuFx = 1, gGpuHung = 0, gGpuFrames = 0;
#define NVISM 5
static const char* VISM_NAMES[NVISM] = {"AUTO", "WAVETABLE", "SCOPES", "ORBIT", "SPECTRUM"};
static const char* AB_NAMES[2] = {"OSC A", "OSC B"};
static int gVisMode = 0, gVisOsc = 0, gVisOverlay = 1;
static float gVisBeat = 0;
#define NTHEMES 5
static const char* THEME_NAMES[NTHEMES] = {"ORIGINAL", "PIP GREEN", "DOS BLUE", "AMBER EGA", "MAGENTA"};
static int gTheme = 0, gScan = 0;   // graphics-chip visuals (see visFrame); off by default while being tested on hardware
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
	FILE* f = fopen(CRASH_PATH, "r");
	if (!f) return;
	if (!fgets(gCrashStage, sizeof(gCrashStage), f)) snprintf(gCrashStage, sizeof(gCrashStage), "START");
	fclose(f);
	gSafeMode = 1;
}        // set while the audio thread runs the arp/seq clock (it holds gMipsLock)
static LightLock gMipsLock;   // guards the shared FFT buffers (worker on core 1 vs main core)
static Thread gWorker = NULL;

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
}

// new cycle -> spectrum, normalise (from the 512-harmonic peak), rebuild the levels in use
static void mipsSet(Mips* m, const float* cyc, u32 needMask) {
	for (int n = 0; n < CYC; n++) { m->spec[n].re = cyc[n]; m->spec[n].im = 0; }
	fft(m->spec, CYC, 0);
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
enum { T_BASIC, T_PWM, T_HARMONIX, T_VOWELS, T_SYNC, T_GROWL, T_DIGITAL, T_BUILTIN };
static const char* BUILTIN_NAMES[T_BUILTIN] = {"BASIC SHAPES", "PWM", "HARMONIX", "VOWELS", "SYNC SAW", "FM GROWL", "DIGITAL"};
#define MAXFILES 64
static char gFiles[MAXFILES][64];
static int gNFiles = 0;

static float hash01(int a, int b) {
	unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

typedef struct { float* frames; int nFrames; float peak; char name[40]; } Table;

static void genBuiltin(Table* tb, int t) {
	tb->nFrames = 64;
	for (int f = 0; f < 64; f++) {
		float* fr = tb->frames + f * CYC;
		float m = f / 63.0f;
		if (t == T_HARMONIX || t == T_VOWELS || t == T_DIGITAL) {
			static float amp[257];
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
			// sum of sines via one inverse FFT (was ~100k sinf calls per frame)
			static cpx gb[CYC];
			memset(gb, 0, sizeof(gb));
			for (int k = 1; k <= nh && k < CYC / 2; k++) if (amp[k] != 0) {
				float ph = t == T_DIGITAL ? k * 1.3f : 0, a = amp[k] * 0.5f;
				gb[k].re = a * sinf(ph); gb[k].im = -a * cosf(ph);
				gb[CYC - k].re = gb[k].re; gb[CYC - k].im = -gb[k].im;
			}
			fft(gb, CYC, 1);
			for (int n = 0; n < CYC; n++) fr[n] = gb[n].re;
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

static const char* SUB_SHAPES[4] = {"SINE", "TRIANGLE", "SAW", "SQUARE"};
static const char* NOISE_TYPES[3] = {"WHITE", "PINK", "CRACKLE"};
static const char* FILT_TYPES[5] = {"LOW PASS", "HIGH PASS", "BAND PASS", "NOTCH", "PEAK"};
#define NLFOSH 13
enum { LS_SINE, LS_TRI, LS_SAW, LS_SQR, LS_SH, LS_STEPS, LS_BOUNCE, LS_EXPUP, LS_EXPDN, LS_PULSE, LS_WTA, LS_WTB, LS_TABLE };
static const char* LFO_SHAPES[NLFOSH] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "S&H", "STEPS", "BOUNCE", "EXP RISE", "EXP FALL", "PULSE 25%", "WT OSC A", "WT OSC B", "WAVETABLE"};
#define NSYNC 10
static const char* SYNC_NAMES[NSYNC] = {"OFF", "4 BARS", "2 BARS", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/8T", "1/16T"};
static const float SYNC_BEATS[NSYNC] = {0, 16, 8, 4, 2, 1, 0.5f, 0.25f, 1.0f / 3, 1.0f / 6};
static const char* LFO_MODES[2] = {"FREE", "TRIG"};
enum { SRC_OFF, SRC_LFO1, SRC_LFO2, SRC_ENV2, SRC_VEL, SRC_STICK, SRC_XY, SRC_TILTX, SRC_TILTY, SRC_MIC, SRC_COUNT };
static const char* SRC_NAMES[SRC_COUNT] = {"OFF", "LFO 1", "LFO 2", "ENV 2", "VELOCITY", "STICK", "XY Y", "TILT L/R", "TILT F/B", "BREATH"};
static float gTiltX, gTiltY, gMicEnv;
enum { DST_APOS, DST_BPOS, DST_AWARP, DST_BWARP, DST_CUTOFF, DST_RES, DST_PITCH, DST_ALEV, DST_BLEV, DST_PAN, DST_COUNT };
static const char* DST_NAMES[DST_COUNT] = {"A WT POS", "B WT POS", "A WARP", "B WARP", "CUTOFF", "RES", "PITCH", "A LEVEL", "B LEVEL", "PAN"};
static const char* VOICE_MODES[3] = {"POLY", "MONO", "LEGATO"};
static const char* FX_NAMES[4] = {"OFF", "ECHO", "SPACE", "REVERB"};
static const char* ECHO_T_NAMES[3] = {"250MS", "375MS", "500MS"};
static const float ECHO_T[3] = {0.25f, 0.375f, 0.5f};
static const char* DRIVE_TYPES[5] = {"OFF", "TUBE", "HARD", "FOLD", "RECTIFY"};
static const char* CHORD_NAMES[5] = {"OFF", "MIN7", "MAJ7", "POWER", "SUS"};
static const int CHORDS[5][4] = {{0, -1, -1, -1}, {0, 3, 7, 10}, {0, 4, 7, 11}, {0, 7, 12, -1}, {0, 5, 7, 12}};

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
static ModSlot gMod[4] = {{SRC_ENV2, DST_CUTOFF, 0.35f}, {SRC_STICK, DST_APOS, 0.5f}, {SRC_LFO1, DST_BPOS, 0.0f}, {SRC_OFF, DST_PITCH, 0.0f}};
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
		}
		gTmpCyc[n] = s;
	}
	gSubMips.normInit = 0;
	mipsSet(&gSubMips, gTmpCyc, 0x7f);
}

static void buildNoise(void) {
	unsigned seed = 12345; float b0 = 0, b1 = 0, b2 = 0;
	for (int i = 0; i < gNoiseLen; i++) {
		seed = seed * 1664525u + 1013904223u;
		float w = ((seed >> 9) & 0xffff) / 32768.0f - 1.0f, s = w;
		if (gNoiseType == 1) { b0 = 0.99765f * b0 + w * 0.0990460f; b1 = 0.96300f * b1 + w * 0.2965164f; b2 = 0.57000f * b2 + w * 1.0526913f; s = (b0 + b1 + b2 + w * 0.1848f) * 0.3f; }
		if (gNoiseType == 2) {   // vinyl: soft hiss + sparse crackles
			s = w * 0.08f;
			if (((seed >> 3) & 1023) < 3) s += (((seed >> 13) & 1) ? 1.0f : -1.0f) * (0.4f + 0.6f * (((seed >> 17) & 255) / 255.0f));
		}
		if (s > 1) s = 1;
		if (s < -1) s = -1;
		gNoise[i] = (s16)(s * 30000);
	}
	DSP_FlushDataCache(gNoise, gNoiseLen * sizeof(s16));
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
} SVoice;
static SVoice gV[NVS];
static int gDspOwner[NCH], gCsOwner[NCS];
static unsigned gAge = 0, gIdc = 0;
static ndspWaveBuf gWB[NCH];
static int gAudioOK = 0, gCsndOK = 0, gNoiseOn = 0;
static float gStick = 0, gXYmod = 0, gChorusPh = 0;
static float gAutoGain = 1.0f;
static float gRevSize = 0.5f, gRevDamp = 0.5f, gFxMix = 0.5f;

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

static void stopCh(int bk, int ch) {
	if (bk == BK_DSP) { ndspChnWaveBufClear(ch); gDspOwner[ch] = -1; }
	else { CSND_SetPlayState(ch, 0); gCsOwner[ch] = -1; }
}

static void freeVoice(SVoice* v) {
	if (v->started) for (int i = 0; i < v->nch; i++) if (v->ch[i] >= 0) { stopCh(v->bk[i], v->ch[i]); v->ch[i] = -1; }
	v->active = 0; v->started = 0;
}

static int dspUsable(int c) { return !(gNoiseOn && c == NOISE_CH); }
static int csUsable(int c) { return gCsndOK && (csndChannels & BIT(c)); }
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
static void filterSet(int ch, float hz, float res) {
	if (hz < 40) hz = 40;
	if (hz > 15000) hz = 15000;
	float q = 0.707f + res * 9.0f;
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
	if (!(m->built & (1u << v->lev[i]))) mipsBuildLevel(m, v->lev[i]);
	if (!gInAudioTick) LightLock_Unlock(&gMipsLock);
	int N = LEVN[v->lev[i]];
	if (bk == BK_DSP) {
		float mix[12] = {0};
		ndspChnSetMix(ch, mix);
		ndspChnSetRate(ch, f * N);
		filterSet(ch, 2000, 0);
		memset(&gWB[ch], 0, sizeof(ndspWaveBuf));
		gWB[ch].data_vaddr = m->lev[v->lev[i]];
		gWB[ch].nsamples = N;
		gWB[ch].looping = true;
		ndspChnWaveBufAdd(ch, &gWB[ch]);
	} else {
		u32 pa = osConvertVirtToPhys(m->lev[v->lev[i]]);
		u32 timer = CSND_TIMER(f * N);
		if (timer < 0x42) timer = 0x42;
		if (timer > 0xFFFF) timer = 0xFFFF;
		u32 flags = SOUND_FORMAT_16BIT | SOUND_REPEAT | SOUND_LINEAR_INTERP | SOUND_ENABLE | SOUND_CHANNEL(ch) | (timer << 16);
		CSND_SetChnRegs(flags, pa, pa, N * 2, 0, 0);
	}
}

static void startVoice(int vi) {
	SVoice* v = &gV[vi];
	int roles[R_COUNT], bks[R_COUNT], nr = 0, nd = 0, nc = 0;
	if (v->isEcho) { if (!gOsc[0].on && !gOsc[1].on) { v->active = 0; return; } roles[nr] = R_TAP; bks[nr++] = gCsndOK ? BK_CSND : BK_DSP; }
	else {
		for (int o = 0; o < 2; o++) if (gOsc[o].on) for (int u = 0; u < gOsc[o].uni; u++) { roles[nr] = (o ? R_B0 : R_A0) + u; bks[nr++] = BK_DSP; }
		if (gSubLevel > 0.01f) { roles[nr] = R_SUB; bks[nr++] = gCsndOK ? BK_CSND : BK_DSP; }
	}
	for (int i = 0; i < nr; i++) { if (bks[i] == BK_DSP) nd++; else nc++; }
	if (nr == 0 || (nd && !stealFor(BK_DSP, nd, vi)) || (nc && !stealFor(BK_CSND, nc, vi))) { v->active = 0; return; }
	v->nch = nr;
	for (int i = 0; i < nr; i++) startLayer(v, vi, i, roles[i], bks[i]);
	v->started = 1;
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
	for (int i = 0; i < R_COUNT; i++) v->ch[i] = -1;
	v->active = 1; v->stage = 1; v->stage2 = 1; v->age = gAge++; v->id = ++gIdc; v->gate = -1;
	v->gain = 1; v->cutMul = 1; v->relMul = 1;
}

static void lfoTrig(void) { for (int i = 0; i < 2; i++) if (gLfo[i].mode == 1) gLfo[i].ph = 0; }

static int busActive(void);
static void addTaps(SVoice* v) {
	if (busActive() && (gFx == 1 || gFx == 3)) return;   // reverb / echo run on the DSP effect buses
	int ne = 0; float t[8], g[8], p[8], c[8], rm[8], dt[8];
	float mix = gFxMix * 2.0f;
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

static void voiceOn(int note, float vel) {
	if (!gAudioOK) return;
	if (gVoiceMode != 0) {   // mono / legato: one voice
		SVoice* m = NULL;
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4) { m = &gV[i]; break; }
		if (m && gVoiceMode == 2) { m->note = note; m->freq = mtof((float)note); return; }   // legato: just glide
		float from = m ? m->cur : 0;
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho) freeVoice(&gV[i]);
		SVoice* v = newVoice(); initVoice(v);
		v->note = note; v->freq = mtof((float)note); v->cur = (from > 0 && gGlide > 0) ? from : v->freq; v->vel = vel;
		lfoTrig(); startVoice(v - gV);
		if (v->active) addTaps(v);
		return;
	}
	SVoice* v = newVoice(); initVoice(v);
	v->note = note; v->freq = mtof((float)note); v->cur = v->freq; v->vel = vel;
	lfoTrig();
	startVoice(v - gV);
	if (v->active) addTaps(v);
}

static void voiceOff(int note) {
	for (int i = 0; i < NVS; i++) {
		SVoice* v = &gV[i];
		if (!v->active || v->isEcho || v->note != note || v->stage == 4) continue;
		v->stage = 4; v->stage2 = 4;
		for (int k = 0; k < NVS; k++) if (gV[k].active && gV[k].isEcho && gV[k].parent == v->id && gV[k].gate < 0) gV[k].gate = v->held;
	}
}
static void allOff(void) { for (int i = 0; i < NVS; i++) if (gV[i].active) { if (gV[i].isEcho && !gV[i].started) gV[i].active = 0; else gV[i].stage = gV[i].stage2 = 4; } }

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
	}
	return 0;
}

static float modSum(int dst, const SVoice* v) {
	float s = 0;
	for (int i = 0; i < 4; i++) if (gMod[i].src != SRC_OFF && gMod[i].dst == dst) s += srcValue(gMod[i].src, v) * gMod[i].amt;
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
		ndspChnWaveBufAdd(NOISE_CH, &gWB[NOISE_CH]);
	} else { ndspChnWaveBufClear(NOISE_CH); gNoiseOn = 0; }
}

static float busSend(int bus);
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
		v->cur += (v->freq - v->cur) * gl;

		float pitch = bend * gBendRange + modSum(DST_PITCH, v) * 12.0f;
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
		float s0 = busSend(0), s1 = busSend(1);   // per-channel sends to the DSP effect buses
		mix[4] = mix[0] * s0; mix[5] = mix[1] * s0;
		mix[8] = mix[0] * s1; mix[9] = mix[1] * s1;
		ndspChnSetMix(c, mix);
	}
	int any = 0;
	for (int c = 0; c < NCS; c++) if (cL[c] >= 0) {
		float l = cL[c] * gAutoGain, r = cR[c] * gAutoGain;
		if (l > 1) l = 1;
		if (r > 1) r = 1;
		u32 vol = (u32)(l * 0x8000) | ((u32)(r * 0x8000) << 16);
		CSND_SetVol(c, vol, vol); any = 1;
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
typedef struct { float* b; int n, i; float st; } Comb;
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
		gCbL[i].n = (int)(CL[i] * sc); gCbR[i].n = (int)((CL[i] + 23) * sc);
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
	if (++c->i >= c->n) c->i = 0;
	return y;
}
static inline float apass(APass* a, float in) {
	float b = a->b[a->i], y = b - in;
	a->b[a->i] = in + b * 0.5f;
	if (++a->i >= a->n) a->i = 0;
	return y;
}
static void auxReverb(void* data, int n, void* samples[4]) {
	(void)data;
	s32* L = (s32*)samples[0]; s32* R = (s32*)samples[1];
	float fb = 0.70f + 0.28f * gRevSize, damp = 0.15f + 0.6f * gRevDamp;
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
static void auxDelay(void* data, int n, void* samples[4]) {
	(void)data;
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
static void busInit(void) {
	if (!busFxInit()) return;
	ndspAuxSetCallback(0, auxReverb, NULL); ndspAuxSetCallback(1, auxDelay, NULL);
	for (int i = 0; i < 2; i++) { ndspAuxSetFrontBypass(i, false); ndspAuxSetVolume(i, 1.0f); ndspAuxSetEnable(i, true); }
	gBusOK = 1;
}
// sends for the current FX choice: REVERB -> bus 0, ECHO -> bus 1 (SPACE stays on taps)
static float busSend(int bus) {
	if (!busActive()) return 0;
	if (bus == 0 && gFx == 3) return gFxMix * 1.4f;
	if (bus == 1 && gFx == 1) return gFxMix * 1.2f;
	return 0;
}

// ---------------------------------------------------------------- output: surround + second filter stage
static int gOutMode = 0, gSurDepth = 3;   // STEREO / SURROUND
static const char* OUT_NAMES[2] = {"STEREO", "SURROUND"};
static const char* DEPTH_NAMES[4] = {"25%", "50%", "75%", "100%"};
static void outputApply(void) {
	if (!gAudioOK) return;
	ndspSetOutputMode(gOutMode ? NDSP_OUTPUT_SURROUND : NDSP_OUTPUT_STEREO);
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
		for (int i = 0; i < 512; i++) { float v = m[(end + N - 1 - i) % N] / 32768.0f; sum += v * v; }
		float rms = sqrtf(sum / 512) - 0.012f;   // small noise gate
		float lvl = rms / 0.12f; if (lvl < 0) lvl = 0; if (lvl > 1) lvl = 1;
		gMicEnv += (lvl - gMicEnv) * (lvl > gMicEnv ? 0.6f : 0.12f);
	}
}
static void tiltRecenter(void) { gTiltCal = 0; }

// ---------------------------------------------------------------- scales, arpeggiator, step sequencer
static const char* NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
#define NSCALES 14
static const char* SCALE_NAMES[NSCALES] = {"CHROMATIC", "MAJOR", "MINOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN",
	"HARM MINOR", "MAJ PENTA", "MIN PENTA", "BLUES", "WHOLE TONE", "JAPANESE", "ARABIC"};
static const s8 SCALES[NSCALES][13] = {
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1}, {0, 2, 4, 5, 7, 9, 11, -1}, {0, 2, 3, 5, 7, 8, 10, -1}, {0, 2, 3, 5, 7, 9, 10, -1},
	{0, 1, 3, 5, 7, 8, 10, -1}, {0, 2, 4, 6, 7, 9, 11, -1}, {0, 2, 4, 5, 7, 9, 10, -1}, {0, 2, 3, 5, 7, 8, 11, -1},
	{0, 2, 4, 7, 9, -1}, {0, 3, 5, 7, 10, -1}, {0, 3, 5, 6, 7, 10, -1}, {0, 2, 4, 6, 8, 10, -1},
	{0, 1, 5, 7, 8, -1}, {0, 1, 4, 5, 7, 8, 11, -1}};
static int gRoot = 0, gScale = 0;
static int scaleLen(void) { int n = 0; while (n < 12 && SCALES[gScale][n] >= 0) n++; return n; }
static int scaleNote(int rootNote, int deg) {   // degree -> MIDI note (any degree, negative ok)
	int n = scaleLen(), o = deg >= 0 ? deg / n : -((-deg + n - 1) / n);
	return rootNote + 12 * o + SCALES[gScale][deg - o * n];
}
static int inScale(int note) { int pc = ((note - gRoot) % 12 + 12) % 12; for (int i = 0; SCALES[gScale][i] >= 0; i++) if (SCALES[gScale][i] == pc) return 1; return 0; }
static int snapScale(int note) { if (!gScale) return note; for (int d = 0; d < 6; d++) { if (inScale(note - d)) return note - d; if (inScale(note + d)) return note + d; } return note; }
static int gKeyDeg = 0;   // degree of the last pad pressed in scale mode

// chord tones: chromatic chords, or chords stacked inside the scale when a scale is on
static int chordNotes(int key, int* out) {
	int n = 0;
	if (!gScale || gChord == 3) { for (int i = 0; i < 4; i++) if (CHORDS[gChord][i] >= 0) out[n++] = key + CHORDS[gChord][i]; return n; }
	static const s8 DEG[5][4] = {{0, -1, -1, -1}, {0, 2, 4, 6}, {0, 2, 4, -1}, {0, -1, -1, -1}, {0, 3, 4, -1}};
	int rootNote = key - scaleNote(0, gKeyDeg);   // = base + root
	for (int i = 0; i < 4; i++) if (DEG[gChord][i] >= 0) out[n++] = rootNote + scaleNote(0, gKeyDeg + DEG[gChord][i]);
	return n;
}

// ---- tempo + arpeggiator
#define NARPM 7
static const char* ARP_NAMES[NARPM] = {"OFF", "UP", "DOWN", "UP+DOWN", "RANDOM", "AS PLAYED", "CHORD"};
static const char* RATE_NAMES[6] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float RATE_BEATS[6] = {1.0f, 0.5f, 1.0f / 3, 0.25f, 1.0f / 6, 0.125f};
static int gArp = 0, gArpRate = 3, gArpOct = 1, gBPM = 120;
static float gArpGate = 0.5f;
static int gArpPool[8], gArpN = 0;   // notes the arp is cycling through (held keys or the current sequencer step)
static float gArpVel = 0.8f, gArpPh = 0;
static int gArpIdx = 0, gArpDir = 1, gArpFresh = 0;
static int gArpOn[8], gArpOnN = 0;
static int gBeatFlash = 0;   // set on each arp/seq step (UI blink)

static void arpRelease(void) { for (int i = 0; i < gArpOnN; i++) voiceOff(gArpOn[i]); gArpOnN = 0; }
static void arpSetPool(const int* notes, int n, float vel) {
	if (n > 8) n = 8;
	int was = gArpN;
	for (int i = 0; i < n; i++) gArpPool[i] = notes[i];
	gArpN = n; gArpVel = vel;
	if (!was && n) { gArpIdx = 0; gArpDir = 1; gArpFresh = 1; }   // start on the first press
	if (!n) arpRelease();
}
static void arpStep(void) {
	arpRelease();
	int s[8], n = gArpN;
	for (int i = 0; i < n; i++) s[i] = gArpPool[i];
	if (gArp == 6) {   // CHORD: retrigger the whole chord
		for (int i = 0; i < n; i++) { voiceOn(s[i], gArpVel); gArpOn[gArpOnN++] = s[i]; }
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
	if (!gArp || !gArpN) { if (gArpOnN) arpRelease(); gArpPh = 0; return; }
	float stepsPerSec = gBPM / 60.0f / RATE_BEATS[gArpRate];
	if (gArpFresh) { gArpFresh = 0; gArpPh = 0; arpStep(); gBeatFlash = 1; return; }
	float before = gArpPh;
	gArpPh += dt * stepsPerSec;
	if (before < gArpGate && gArpPh >= gArpGate && gArpGate < 0.99f) arpRelease();
	if (gArpPh >= 1) { gArpPh -= floorf(gArpPh); arpStep(); gBeatFlash = 1; }
}

// ---- clock: the arp and sequencer run in the audio thread, once per DSP frame (160 samples, ~4.9 ms)
static LightLock gVoiceLock;   // voices are touched by the main loop and by the audio thread
static float gClockDebt = 0;
static void arpTick(float dt);
static void seqTick(float dt);
static void audioTick(void* data) {
	(void)data;
	if (!gAudioClock) return;
	float dt = 160.0f / DSPRATE + gClockDebt;
	if (LightLock_TryLock(&gVoiceLock)) { gClockDebt = dt; return; }   // main loop busy: catch up next frame
	if (LightLock_TryLock(&gMipsLock)) { LightLock_Unlock(&gVoiceLock); gClockDebt = dt; return; }
	gClockDebt = 0;
	gInAudioTick = 1;
	arpTick(dt);
	seqTick(dt);
	gInAudioTick = 0;
	LightLock_Unlock(&gMipsLock);
	LightLock_Unlock(&gVoiceLock);
}

// ---- held notes (keys or sequencer) -> voices or arp
static int gHeld[8], gHeldN = 0;
static void playSet(const int* notes, int n, float vel) {
	for (int i = 0; i < gHeldN; i++) voiceOff(gHeld[i]);
	gHeldN = 0;
	if (gArp) { arpSetPool(notes, n, vel); return; }
	if (gArpN) arpSetPool(NULL, 0, vel);
	for (int i = 0; i < n && i < 8; i++) { gHeld[gHeldN++] = notes[i]; voiceOn(notes[i], vel); }
}

// ---- 16-step sequencer (step record, keys transpose while playing)
typedef struct { s8 n; u8 note[4]; u8 vel; } Step;
#define NSTEP 16
static const char* SEQ_NAMES[3] = {"OFF", "PLAY", "REC"};
static Step gSeq[NSTEP];
static int gSeqMode = 0, gSeqLen = 8, gSeqPos = -1, gRecPos = 0, gSeqBase = -1, gSeqTr = 0;
static float gSeqGate = 0.6f, gSwing = 0.0f, gSeqPh = 0;
static int gSeqPlaying = 0;

static void seqDefault(void) {   // a little pattern so PLAY makes sound straight away
	static const int pat[8] = {0, -1, 7, 12, -1, 10, 7, 3};
	memset(gSeq, 0, sizeof(gSeq));
	for (int i = 0; i < 8; i++) if (pat[i] >= 0) { gSeq[i].n = 1; gSeq[i].note[0] = 48 + pat[i]; gSeq[i].vel = 200; }
	gSeqLen = 8; gSeqBase = 48;
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
static void seqClear(void) { memset(gSeq, 0, sizeof(gSeq)); gRecPos = 0; gSeqLen = 1; gSeqBase = -1; gSeqTr = 0; }
static void seqStop(void) { if (gSeqPlaying) playSet(NULL, 0, 0.8f); gSeqPlaying = 0; gSeqPos = -1; gSeqPh = 0; }
static void seqFire(void) {
	Step* s = &gSeq[gSeqPos];
	int notes[4], n = 0;
	for (int i = 0; i < s->n; i++) notes[n++] = snapScale(s->note[i] + gSeqTr);
	playSet(notes, n, s->n ? s->vel / 255.0f : 0.8f);
	gBeatFlash = 1;
}
static void seqTick(float dt) {
	if (gSeqMode != 1) { if (gSeqPlaying) seqStop(); return; }
	if (!gSeqPlaying) { gSeqPlaying = 1; gSeqPos = 0; gSeqPh = 0; seqFire(); return; }
	float len = (gSeqPos & 1) ? 1.0f - gSwing : 1.0f + gSwing;   // in 16ths
	float before = gSeqPh;
	gSeqPh += dt * gBPM / 60.0f * 4.0f;
	if (!gArp && before < len * gSeqGate && gSeqPh >= len * gSeqGate) playSet(NULL, 0, 0.8f);
	if (gSeqPh >= len) { gSeqPh -= len; gSeqPos = (gSeqPos + 1) % (gSeqLen < 1 ? 1 : gSeqLen); seqFire(); }
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
} SPatch;
#define PATCH_MAGIC 0x53595233   // "SYR3"
#define PATCH_MAGIC2 0x53595232  // v2 (no LFO sync / table)
#define PATCH_V2_SIZE offsetof(SPatch, lfoSync)
#define PATCH_MAGIC1 0x53595231  // v1 (no arp fields)
#define PATCH_V1_SIZE offsetof(SPatch, arp)

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
	p->arp = 0; p->arpRate = 3; p->arpOct = 1; p->arpGate = 0.5f; p->bpm = 0;
	for (int i = 0; i < 2; i++) { p->lfoSync[i] = 0; p->lfoTable[i] = T_HARMONIX; p->lfoPos[i] = 0.5f; }
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

// ---- BASS
static void pr_subwob(SPatch* p) { p->master = 0.5f; OA.table = T_GROWL; OA.pos = 0.3f; OA.oct = -1; p->subLevel = 0.7f; p->cutoff = 0.35f; p->res = 0.35f;
	LFO(0, 0, 1, 4.0f); MOD(0, SRC_LFO1, DST_CUTOFF, 0.5f); ENV1(0.003f, 0, 0.3f, 0.9f, 0.15f); }
static void pr_reese(SPatch* p) { OA.oct = -1; OA.uni = 3; OA.detune = 22; OA.width = 0.4f; p->cutoff = 0.42f; p->res = 0.15f; p->chorus = 0.4f;
	p->subLevel = 0.4f; ENV1(0.005f, 0, 0.3f, 1.0f, 0.2f); LFO(0, 0, 0, 0.2f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.15f); }
static void pr_fmgrowl(SPatch* p) { p->master = 0.55f; OA.table = T_GROWL; OA.oct = -1; OA.warp = W_FM; OA.warpAmt = 0.4f; OB.table = T_BASIC; OB.pos = SINE; OB.oct = -1;
	LFO(0, 0, 1, 2.0f); MOD(2, SRC_LFO1, DST_AWARP, 0.6f); MOD(3, SRC_LFO1, DST_APOS, 0.3f); p->cutoff = 0.55f; p->drive = 1; p->driveAmt = 0.4f;
	ENV1(0.003f, 0, 0.3f, 0.9f, 0.15f); }
static void pr_808(SPatch* p) { OA.pos = SINE; OA.oct = -1; ENV1(0.002f, 0, 1.2f, 0.0f, 0.3f); ENV2(0.0f, 0, 0.06f, 0, 0.1f);
	MOD(0, SRC_ENV2, DST_PITCH, 0.1f); p->drive = 1; p->driveAmt = 0.3f; p->cutoff = 0.6f; }
static void pr_acid(SPatch* p) { OA.oct = -1; p->cutoff = 0.25f; p->res = 0.7f; p->keytrack = 0.5f; ENV2(0.0f, 0, 0.2f, 0, 0.2f);
	MOD(0, SRC_ENV2, DST_CUTOFF, 0.7f); p->voiceMode = 1; p->glide = 0.08f; p->drive = 2; p->driveAmt = 0.25f; ENV1(0.002f, 0, 0.3f, 0.8f, 0.08f); }
// ---- LEAD
static void pr_supersaw(SPatch* p) { p->master = 0.75f; OA.uni = 3; OA.detune = 25; OA.blend = 0.7f; OA.width = 0.9f; OB.on = 1; OB.oct = 1; OB.uni = 3; OB.detune = 18; OB.level = 0.4f;
	p->cutoff = 0.8f; p->chorus = 0.3f; p->fx = 3; p->revSize = 0.5f; p->fxMix = 0.3f; ENV1(0.01f, 0, 0.3f, 0.9f, 0.3f); }
static void pr_syncscream(SPatch* p) { OA.table = T_SYNC; OA.warp = W_SYNC; OA.warpAmt = 0.3f; LFO(0, 1, 0, 0.3f); MOD(2, SRC_LFO1, DST_AWARP, 0.5f);
	p->voiceMode = 2; p->glide = 0.06f; p->cutoff = 0.75f; p->res = 0.2f; p->fx = 1; p->echoT = 1; p->fxMix = 0.35f; }
static void pr_vowellead(SPatch* p) { OA.table = T_VOWELS; OA.uni = 2; OA.detune = 10; LFO(0, 1, 0, 0.7f); MOD(2, SRC_LFO1, DST_APOS, 0.45f);
	LFO(1, 0, 0, 5.5f); MOD(3, SRC_LFO2, DST_PITCH, 0.02f); p->fx = 1; p->fxMix = 0.3f; p->voiceMode = 2; p->glide = 0.05f; }
static void pr_chip(SPatch* p) { OA.table = T_PWM; OA.pos = 0; OA.warp = W_QUANT; OA.warpAmt = 0.5f; p->cutoff = 1.0f; p->voiceMode = 1;
	p->fx = 1; p->echoT = 0; p->fxMix = 0.3f; ENV1(0.002f, 0, 0.2f, 0.8f, 0.08f); p->chorus = 0; }
static void pr_digipipe(SPatch* p) { OA.table = T_DIGITAL; OA.pos = 0.2f; OA.warp = W_MIRROR; OA.warpAmt = 0.5f; OA.uni = 2; OA.detune = 6;
	LFO(1, 0, 0, 5.0f); MOD(3, SRC_LFO2, DST_PITCH, 0.015f); p->fx = 3; p->fxMix = 0.4f; }
// ---- PAD
static void pr_syruppad(SPatch* p) { OA.uni = 3; OB.on = 1; OB.table = T_HARMONIX; OB.pos = 0.4f; OB.oct = 1; OB.uni = 3; OB.detune = 10; OB.level = 0.5f;
	p->cutoff = 0.55f; ENV1(0.8f, 0, 1.0f, 0.85f, 1.5f); LFO(0, 0, 0, 0.15f); MOD(2, SRC_LFO1, DST_APOS, 0.25f); p->chorus = 0.5f;
	p->fx = 3; p->revSize = 0.8f; p->fxMix = 0.45f; }
static void pr_glass(SPatch* p) { p->master = 1.2f; OA.table = T_DIGITAL; OA.pos = 0.5f; OA.uni = 3; OA.detune = 8; OA.warp = W_BENDP; OA.warpAmt = 0.3f; p->cutoff = 0.85f;
	ENV1(0.5f, 0, 1.0f, 0.8f, 2.0f); LFO(0, 0, 0, 0.1f); MOD(2, SRC_LFO1, DST_APOS, 0.4f); p->fx = 3; p->revSize = 0.9f; p->revDamp = 0.2f; p->fxMix = 0.5f; }
static void pr_choir(SPatch* p) { p->master = 1.1f; OA.table = T_VOWELS; OA.pos = 0.2f; OA.uni = 3; OA.detune = 12; LFO(0, 0, 0, 0.08f); MOD(2, SRC_LFO1, DST_APOS, 0.5f);
	ENV1(0.9f, 0, 1.0f, 0.9f, 1.5f); p->fx = 3; p->revSize = 0.7f; p->fxMix = 0.45f; }
static void pr_strings(SPatch* p) { OA.uni = 3; OA.detune = 18; OA.width = 1.0f; OB.on = 1; OB.table = T_PWM; OB.pos = 0.3f; OB.uni = 2; OB.level = 0.4f;
	p->cutoff = 0.5f; p->keytrack = 0.5f; ENV1(0.6f, 0, 1.0f, 0.9f, 1.2f); p->chorus = 0.6f; p->fx = 3; p->revSize = 0.6f; p->fxMix = 0.4f; }
static void pr_drone(SPatch* p) { OA.table = T_GROWL; OA.pos = 0.2f; OA.uni = 3; OA.detune = 30; p->subShape = 1; p->subOct = -2; p->subLevel = 0.4f;
	p->cutoff = 0.3f; p->res = 0.3f; LFO(0, 0, 0, 0.05f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.4f); p->noiseType = 1; p->noiseLevel = 0.1f;
	ENV1(2.0f, 0, 1.0f, 1.0f, 3.0f); p->fx = 3; p->revSize = 1.0f; p->fxMix = 0.5f; }
// ---- PLUCK
static void pr_syruppluck(SPatch* p) { OA.table = T_HARMONIX; OA.pos = 0.5f; OA.uni = 2; OA.detune = 8; ENV1(0.002f, 0, 0.35f, 0, 0.3f);
	ENV2(0, 0, 0.25f, 0, 0.2f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.6f); p->cutoff = 0.3f; p->fx = 3; p->fxMix = 0.35f; }
static void pr_marimba(SPatch* p) { OA.pos = SINE; OB.on = 1; OB.pos = SINE; OB.oct = 2; OB.level = 0.3f; ENV1(0.001f, 0, 0.5f, 0, 0.25f);
	p->fx = 3; p->revSize = 0.3f; p->fxMix = 0.3f; p->chorus = 0; }
static void pr_kalimba(SPatch* p) { OA.table = T_HARMONIX; OA.pos = 0.1f; OA.warp = W_FM; OA.warpAmt = 0.2f; OB.pos = SINE; OB.oct = 1;
	ENV1(0.001f, 0, 0.8f, 0, 0.4f); p->fx = 2; p->fxMix = 0.5f; p->chorus = 0; }
static void pr_harp(SPatch* p) { OA.pos = TRI; p->cutoff = 0.7f; p->keytrack = 0.7f; ENV1(0.002f, 0, 1.2f, 0, 0.6f); p->fx = 3; p->revSize = 0.6f; p->fxMix = 0.4f; }
static void pr_dubstab(SPatch* p) { p->master = 0.72f; OA.uni = 3; OA.detune = 15; p->chord = 1; ENV1(0.003f, 0, 0.25f, 0, 0.2f); p->cutoff = 0.45f;
	ENV2(0, 0, 0.2f, 0, 0.2f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.5f); p->fx = 1; p->echoT = 1; p->fxMix = 0.5f; }
// ---- KEYS
static void pr_epiano(SPatch* p) { OA.pos = SINE; OA.warp = W_FM; OA.warpAmt = 0.35f; OB.pos = SINE; ENV1(0.002f, 0, 1.5f, 0.2f, 0.5f);
	p->chorus = 0.4f; LFO(1, 0, 0, 5.0f); MOD(3, SRC_LFO2, DST_PAN, 0.3f); p->fx = 3; p->revSize = 0.4f; p->fxMix = 0.3f; MOD(0, SRC_VEL, DST_CUTOFF, 0.3f); }
static void pr_organ(SPatch* p) { p->master = 0.68f; OA.table = T_HARMONIX; OA.pos = 0.1f; OB.on = 1; OB.table = T_HARMONIX; OB.pos = 0.05f; OB.oct = 1; OB.level = 0.5f;
	ENV1(0.005f, 0, 0.2f, 1.0f, 0.08f); p->chorus = 0.7f; LFO(1, 0, 0, 6.0f); MOD(3, SRC_LFO2, DST_PAN, 0.4f); }
static void pr_clav(SPatch* p) { OA.table = T_PWM; OA.pos = 0.7f; p->filtType = 2; p->res = 0.3f; p->cutoff = 0.55f; ENV1(0.002f, 0, 0.3f, 0.1f, 0.1f);
	p->drive = 2; p->driveAmt = 0.2f; p->chorus = 0; }
static void pr_acordeon(SPatch* p) { p->master = 0.62f; OA.table = T_PWM; OA.pos = 0.4f; OA.uni = 2; OA.detune = 12; OB.on = 1; OB.table = T_PWM; OB.pos = 0.4f; OB.oct = 1; OB.level = 0.5f;
	p->cutoff = 0.6f; ENV1(0.04f, 0, 0.3f, 0.9f, 0.12f); LFO(1, 0, 0, 6.0f); MOD(3, SRC_LFO2, DST_ALEV, 0.15f); p->chorus = 0.2f; }
static void pr_vinylkeys(SPatch* p) { pr_epiano(p); p->noiseType = 2; p->noiseLevel = 0.25f; p->cutoff = 0.45f; p->revSize = 0.5f; p->fxMix = 0.45f; }
// ---- FX
static void pr_riser(SPatch* p) { OA.table = T_SYNC; OA.pos = 0; OA.warp = W_SYNC; LFO(0, 2, 1, 0.25f); MOD(2, SRC_LFO1, DST_AWARP, 1.0f);
	MOD(3, SRC_LFO1, DST_PITCH, 0.25f); p->noiseLevel = 0.3f; p->fx = 3; p->revSize = 1.0f; p->fxMix = 0.5f; ENV1(0.5f, 0, 1, 1, 1.5f); }
static void pr_laser(SPatch* p) { OA.pos = SAW; ENV1(0.001f, 0, 0.3f, 0, 0.2f); ENV2(0, 0, 0.15f, 0, 0.1f); MOD(0, SRC_ENV2, DST_PITCH, 1.0f);
	p->fx = 1; p->echoT = 0; p->fxMix = 0.5f; }
static void pr_wind(SPatch* p) { OA.level = 0; p->noiseType = 0; p->noiseLevel = 1.0f; p->filtType = 2; p->res = 0.25f; p->cutoff = 0.55f; p->master = 1.5f;
	LFO(0, 0, 0, 0.2f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.4f); ENV1(1.0f, 0, 1, 1, 2.0f); }
static void pr_radio(SPatch* p) { OA.table = T_DIGITAL; OA.pos = 0.5f; LFO(1, 4, 0, 8.0f); MOD(3, SRC_LFO2, DST_APOS, 0.5f); p->filtType = 2; p->cutoff = 0.6f;
	p->res = 0.3f; p->noiseType = 2; p->noiseLevel = 0.3f; p->drive = 3; p->driveAmt = 0.5f; p->master = 1.4f; }
static void pr_robot(SPatch* p) { OA.table = T_VOWELS; LFO(1, 4, 0, 6.0f); MOD(3, SRC_LFO2, DST_APOS, 0.6f); OA.warp = W_QUANT; OA.warpAmt = 0.6f; p->voiceMode = 1; }
// ---- CHORDS
static void pr_houseorgan(SPatch* p) { p->master = 0.6f; p->chord = 1; OA.table = T_HARMONIX; OA.pos = 0.15f; ENV1(0.003f, 0, 0.25f, 0.3f, 0.15f); p->cutoff = 0.6f; p->fx = 2; p->fxMix = 0.4f; }
static void pr_neosoul(SPatch* p) { p->master = 0.7f; p->chord = 2; OA.pos = TRI; OB.on = 1; OB.pos = SINE; OB.oct = 1; OB.level = 0.4f; ENV1(0.01f, 0, 1.0f, 0.4f, 0.6f);
	p->chorus = 0.5f; p->fx = 3; p->revSize = 0.5f; p->fxMix = 0.4f; p->cutoff = 0.55f; }
static void pr_power(SPatch* p) { p->master = 0.55f; p->chord = 3; OA.uni = 3; p->drive = 1; p->driveAmt = 0.6f; p->cutoff = 0.6f; ENV1(0.005f, 0, 0.4f, 0.8f, 0.25f); }
static void pr_shimmer(SPatch* p) { p->chord = 4; OA.table = T_DIGITAL; OA.pos = 0.4f; OA.uni = 3; ENV1(0.2f, 0, 1, 0.8f, 2.0f); p->fx = 3; p->revSize = 1.0f; p->fxMix = 0.6f; }
static void pr_dubchord(SPatch* p) { p->master = 0.72f; p->chord = 1; OA.table = T_HARMONIX; OA.pos = 0.3f; OA.uni = 2; ENV1(0.002f, 0, 0.18f, 0, 0.15f); p->cutoff = 0.4f;
	ENV2(0, 0, 0.15f, 0, 0.1f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.4f); p->fx = 1; p->echoT = 2; p->fxMix = 0.6f; }

typedef struct { const char* name; void (*fn)(SPatch*); } PresetDef;
// ---- more BASS
static void pr_wob8(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; OA.uni = 2; OA.detune = 10; p->subLevel = 0.5f;
	p->cutoff = 0.3f; p->res = 0.3f; LFO(0, 1, 1, 6.0f); MOD(0, SRC_LFO1, DST_CUTOFF, 0.55f); ENV1(0.003f, 0, 0.3f, 0.9f, 0.12f); }
static void pr_digibass(SPatch* p) { OA.table = T_DIGITAL; OA.pos = 0.3f; OA.oct = -1; OA.warp = W_QUANT; OA.warpAmt = 0.45f; p->subLevel = 0.4f; p->cutoff = 0.6f;
	ENV1(0.002f, 0, 0.4f, 0.7f, 0.1f); p->chorus = 0; }
static void pr_sinebass(SPatch* p) { OA.pos = SINE; OA.oct = -1; p->voiceMode = 2; p->glide = 0.07f; p->drive = 1; p->driveAmt = 0.25f; p->cutoff = 0.7f;
	ENV1(0.003f, 0, 0.5f, 0.9f, 0.12f); p->chorus = 0; }
static void pr_pluckbass(SPatch* p) { OA.oct = -1; p->cutoff = 0.22f; p->res = 0.25f; ENV2(0, 0, 0.18f, 0, 0.1f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.65f);
	ENV1(0.002f, 0, 0.35f, 0.2f, 0.12f); p->subLevel = 0.35f; p->chorus = 0; }
static void pr_hoover(SPatch* p) { p->master = 0.65f; OA.table = T_PWM; OA.pos = 0.3f; OA.uni = 3; OA.detune = 40; OA.width = 1.0f; OA.oct = -1;
	p->voiceMode = 2; p->glide = 0.15f; LFO(0, 0, 1, 0.4f); MOD(2, SRC_LFO1, DST_APOS, 0.4f); p->chorus = 0.6f; p->cutoff = 0.7f; }
// ---- more LEAD
static void pr_brasslead(SPatch* p) { OA.uni = 2; OA.detune = 8; p->cutoff = 0.35f; ENV2(0.05f, 0, 0.4f, 0.5f, 0.3f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.55f);
	ENV1(0.03f, 0, 0.3f, 0.85f, 0.2f); p->fx = 3; p->fxMix = 0.25f; }
static void pr_whistle(SPatch* p) { OA.pos = SINE; LFO(1, 0, 0, 5.5f); MOD(3, SRC_LFO2, DST_PITCH, 0.025f); ENV1(0.06f, 0, 0.2f, 0.9f, 0.25f);
	p->voiceMode = 2; p->glide = 0.06f; p->fx = 3; p->revSize = 0.6f; p->fxMix = 0.45f; p->chorus = 0; }
static void pr_glidesq(SPatch* p) { OA.table = T_PWM; OA.pos = 0.15f; p->voiceMode = 2; p->glide = 0.2f; p->cutoff = 0.65f; p->fx = 1; p->echoT = 2; p->fxMix = 0.35f; }
static void pr_fuzzlead(SPatch* p) { p->master = 0.65f; OA.uni = 2; OA.detune = 12; p->drive = 2; p->driveAmt = 0.7f; p->cutoff = 0.6f; p->voiceMode = 1; p->fx = 1; p->fxMix = 0.3f; }
static void pr_fifths(SPatch* p) { OB.on = 1; OB.semi = 7; OB.level = 0.6f; OA.uni = 2; OB.uni = 2; p->cutoff = 0.7f; p->voiceMode = 2; p->glide = 0.05f; p->fx = 3; p->fxMix = 0.3f; }
// ---- motion / breath (accelerometer + microphone sources)
static void pr_breath(SPatch* p) { OA.table = T_VOWELS; OA.pos = 0.3f; p->voiceMode = 2; p->glide = 0.05f; p->cutoff = 0.25f; p->res = 0.2f;
	MOD(0, SRC_MIC, DST_CUTOFF, 0.7f); MOD(1, SRC_MIC, DST_APOS, 0.4f); MOD(2, SRC_TILTX, DST_PITCH, 0.02f); ENV1(0.03f, 0, 0.2f, 1.0f, 0.3f); p->fx = 3; p->fxMix = 0.35f; }
static void pr_tiltwob(SPatch* p) { p->master = 0.55f; OA.table = T_GROWL; OA.oct = -1; p->subLevel = 0.5f; p->cutoff = 0.35f; p->res = 0.35f;
	MOD(0, SRC_TILTX, DST_CUTOFF, 0.7f); MOD(1, SRC_TILTY, DST_APOS, 0.6f); ENV1(0.003f, 0, 0.3f, 0.9f, 0.15f); }
static void pr_tiltpad(SPatch* p) { pr_syruppad(p); MOD(1, SRC_TILTX, DST_APOS, 0.5f); MOD(3, SRC_TILTY, DST_CUTOFF, 0.5f); }
// ---- wavetable LFOs
static void pr_vowtalk(SPatch* p) { OA.table = T_GROWL; OA.pos = 0.4f; OA.uni = 2; OA.detune = 8; p->filtType = 2; p->cutoff = 0.5f; p->res = 0.35f;
	LFO(0, LS_TABLE, 1, 1.0f); p->lfoTable[0] = T_VOWELS; p->lfoPos[0] = 0.5f; p->lfoSync[0] = 5;
	MOD(2, SRC_LFO1, DST_CUTOFF, 0.45f); MOD(3, SRC_LFO1, DST_APOS, 0.35f); p->bpm = 0; p->master = 1.1f; }
static void pr_drift(SPatch* p) { pr_syruppad(p); LFO(0, LS_WTB, 0, 0.12f); MOD(2, SRC_LFO1, DST_APOS, 0.4f); LFO(1, LS_TABLE, 0, 0.3f);
	p->lfoTable[1] = T_DIGITAL; p->lfoPos[1] = 0.3f; MOD(3, SRC_LFO2, DST_CUTOFF, 0.25f); }
// ---- more PAD
static void pr_vapor(SPatch* p) { OA.pos = TRI; OA.uni = 3; OA.detune = 20; p->noiseType = 2; p->noiseLevel = 0.2f; p->cutoff = 0.45f; ENV1(1.0f, 0, 1, 0.9f, 2.0f);
	p->chorus = 0.8f; p->fx = 3; p->revSize = 0.9f; p->fxMix = 0.5f; }
static void pr_sweeppad(SPatch* p) { OA.uni = 3; OA.detune = 16; p->cutoff = 0.35f; p->res = 0.3f; LFO(0, 1, 0, 0.07f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.6f);
	ENV1(0.8f, 0, 1, 0.9f, 1.8f); p->fx = 3; p->revSize = 0.7f; p->fxMix = 0.4f; }
static void pr_formantpad(SPatch* p) { OA.table = T_VOWELS; OA.uni = 3; OA.detune = 10; LFO(1, 4, 0, 0.6f); MOD(3, SRC_LFO2, DST_APOS, 0.5f); ENV1(0.7f, 0, 1, 0.9f, 1.6f);
	p->fx = 3; p->revSize = 0.8f; p->fxMix = 0.45f; }
static void pr_shimmerpad(SPatch* p) { OA.table = T_DIGITAL; OA.pos = 0.35f; OA.uni = 3; OB.on = 1; OB.pos = SINE; OB.oct = 2; OB.level = 0.35f; ENV1(1.2f, 0, 1, 0.9f, 2.5f);
	p->fx = 3; p->revSize = 1.0f; p->revDamp = 0.1f; p->fxMix = 0.6f; }
static void pr_pwmpad(SPatch* p) { OA.table = T_PWM; OA.uni = 3; OA.detune = 12; LFO(0, 0, 0, 0.25f); MOD(2, SRC_LFO1, DST_APOS, 0.4f); p->cutoff = 0.55f;
	ENV1(0.5f, 0, 1, 0.85f, 1.2f); p->chorus = 0.5f; p->fx = 3; p->fxMix = 0.35f; }
// ---- more PLUCK
static void pr_bell(SPatch* p) { OA.pos = SINE; OA.warp = W_FM; OA.warpAmt = 0.6f; OB.pos = SINE; OB.oct = 2; ENV1(0.001f, 0, 1.8f, 0, 1.0f);
	p->fx = 3; p->revSize = 0.7f; p->fxMix = 0.45f; p->chorus = 0; }
static void pr_steeldrum(SPatch* p) { OA.table = T_HARMONIX; OA.pos = 0.2f; OA.warp = W_FM; OA.warpAmt = 0.3f; OB.pos = SINE; OB.oct = 1; ENV1(0.001f, 0, 0.6f, 0, 0.3f);
	p->fx = 2; p->fxMix = 0.4f; p->chorus = 0; }
static void pr_synthpluck(SPatch* p) { OA.uni = 2; OA.detune = 10; p->cutoff = 0.25f; ENV2(0, 0, 0.15f, 0, 0.1f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.7f);
	ENV1(0.002f, 0, 0.3f, 0, 0.25f); p->fx = 1; p->echoT = 0; p->fxMix = 0.35f; }
static void pr_muted(SPatch* p) { OA.table = T_PWM; OA.pos = 0.5f; p->cutoff = 0.35f; p->keytrack = 0.6f; ENV1(0.001f, 0, 0.2f, 0, 0.1f); p->chorus = 0; }
static void pr_pizz(SPatch* p) { OA.uni = 2; OA.detune = 14; p->cutoff = 0.5f; ENV1(0.002f, 0, 0.25f, 0, 0.3f); p->fx = 3; p->revSize = 0.5f; p->fxMix = 0.4f; }
// ---- more KEYS
static void pr_rhodesdark(SPatch* p) { pr_epiano(p); p->cutoff = 0.4f; p->chorus = 0.5f; }
static void pr_wurli(SPatch* p) { OA.table = T_PWM; OA.pos = 0.55f; ENV1(0.002f, 0, 0.9f, 0.3f, 0.3f); p->drive = 1; p->driveAmt = 0.3f; p->cutoff = 0.55f;
	LFO(1, 0, 0, 4.5f); MOD(3, SRC_LFO2, DST_ALEV, 0.15f); }
static void pr_church(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.pos = 0.2f; OB.on = 1; OB.table = T_HARMONIX; OB.pos = 0.1f; OB.oct = 1; OB.level = 0.5f;
	p->subLevel = 0.4f; ENV1(0.08f, 0, 0.3f, 1.0f, 0.6f); p->fx = 3; p->revSize = 1.0f; p->fxMix = 0.5f; }
static void pr_harpsi(SPatch* p) { OA.table = T_PWM; OA.pos = 0.85f; OB.on = 1; OB.table = T_PWM; OB.pos = 0.85f; OB.oct = 1; OB.level = 0.5f;
	ENV1(0.001f, 0, 0.8f, 0, 0.3f); p->cutoff = 0.8f; p->chorus = 0; }
static void pr_lofipiano(SPatch* p) { OA.pos = TRI; OA.uni = 2; OA.detune = 9; p->noiseType = 2; p->noiseLevel = 0.25f; p->cutoff = 0.4f; ENV1(0.002f, 0, 1.2f, 0.2f, 0.5f);
	p->chorus = 0.6f; p->fx = 3; p->fxMix = 0.3f; }
// ---- more FX
static void pr_siren(SPatch* p) { OA.pos = SAW; LFO(0, 1, 1, 0.5f); MOD(2, SRC_LFO1, DST_PITCH, 0.5f); p->cutoff = 0.7f; p->fx = 1; p->fxMix = 0.3f; }
static void pr_bubbles(SPatch* p) { OA.pos = SINE; LFO(1, 4, 0, 10.0f); MOD(3, SRC_LFO2, DST_PITCH, 0.3f); ENV1(0.002f, 0, 0.6f, 0.4f, 0.4f); p->fx = 2; p->fxMix = 0.5f; }
static void pr_noisesweep(SPatch* p) { OA.level = 0; p->noiseLevel = 1.0f; p->filtType = 2; p->res = 0.4f; LFO(0, 2, 1, 0.3f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.8f);
	p->master = 1.8f; ENV1(0.05f, 0, 1, 1, 1.0f); p->fx = 3; p->fxMix = 0.4f; }
static void pr_alarm(SPatch* p) { OA.table = T_PWM; OA.pos = 0; LFO(0, 3, 1, 4.0f); MOD(2, SRC_LFO1, DST_PITCH, 0.1f); p->cutoff = 0.75f; }
static void pr_spacedrone(SPatch* p) { p->master = 1.5f; OA.table = T_GROWL; OA.uni = 3; OA.detune = 25; LFO(0, 0, 0, 0.06f); MOD(2, SRC_LFO1, DST_APOS, 0.5f); p->cutoff = 0.4f;
	ENV1(2.0f, 0, 1, 1, 3.0f); p->fx = 3; p->revSize = 1.0f; p->fxMix = 0.6f; }
// ---- more CHORDS
static void pr_min7pad(SPatch* p) { pr_syruppad(p); p->chord = 1; p->master = 0.7f; }
static void pr_maj7keys(SPatch* p) { pr_epiano(p); p->chord = 2; p->master = 0.7f; }
static void pr_powerpluck(SPatch* p) { pr_synthpluck(p); p->chord = 3; p->master = 0.6f; }
static void pr_susorgan(SPatch* p) { pr_organ(p); p->chord = 4; p->master = 0.48f; }
static void pr_technostab(SPatch* p) { p->chord = 1; OA.uni = 2; OA.detune = 12; ENV1(0.002f, 0, 0.15f, 0, 0.1f); p->cutoff = 0.5f; p->res = 0.2f;
	p->fx = 1; p->echoT = 2; p->fxMix = 0.55f; p->master = 0.6f; }
// ---- RETRO
static void pr_8bitlead(SPatch* p) { OA.table = T_PWM; OA.pos = 0.5f; OA.warp = W_QUANT; OA.warpAmt = 0.55f; p->voiceMode = 1; p->cutoff = 1.0f; p->chorus = 0;
	ENV1(0.001f, 0, 0.2f, 0.8f, 0.05f); LFO(1, 0, 0, 6.0f); MOD(3, SRC_LFO2, DST_PITCH, 0.015f); }
static void pr_8bitbass(SPatch* p) { OA.table = T_PWM; OA.pos = 0.5f; OA.oct = -1; OA.warp = W_QUANT; OA.warpAmt = 0.6f; p->cutoff = 1.0f; p->chorus = 0;
	ENV1(0.001f, 0, 0.2f, 0.9f, 0.05f); }
static void pr_vgmbrass(SPatch* p) { OA.pos = SAW; OA.warp = W_FM; OA.warpAmt = 0.25f; OB.pos = SINE; ENV1(0.02f, 0, 0.3f, 0.8f, 0.15f); p->cutoff = 0.65f; p->chorus = 0.2f; }
static void pr_arcade(SPatch* p) { OA.table = T_PWM; OA.pos = 0.3f; OA.warp = W_QUANT; OA.warpAmt = 0.5f; LFO(0, 3, 1, 12.0f); MOD(2, SRC_LFO1, DST_PITCH, 0.5f);
	ENV1(0.001f, 0, 0.4f, 0.6f, 0.1f); p->chorus = 0; }
static void pr_80spoly(SPatch* p) { OA.uni = 2; OA.detune = 10; p->cutoff = 0.4f; ENV2(0.01f, 0, 0.5f, 0.3f, 0.4f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.4f);
	ENV1(0.01f, 0, 0.6f, 0.7f, 0.4f); p->chorus = 0.8f; p->fx = 3; p->fxMix = 0.3f; }
static void pr_synthwave(SPatch* p) { pr_supersaw(p); p->cutoff = 0.55f; LFO(0, 0, 0, 0.1f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.3f); p->revSize = 0.9f; p->fxMix = 0.45f; }
static void pr_chipbell(SPatch* p) { OA.table = T_PWM; OA.pos = 0.75f; OA.warp = W_QUANT; OA.warpAmt = 0.5f; ENV1(0.001f, 0, 0.6f, 0, 0.2f); p->fx = 1; p->echoT = 0; p->fxMix = 0.4f; p->chorus = 0; }
static void pr_gbwave(SPatch* p) { OA.table = T_DIGITAL; OA.pos = 0.6f; OA.warp = W_QUANT; OA.warpAmt = 0.7f; ENV1(0.001f, 0, 0.3f, 0.8f, 0.08f); p->voiceMode = 1; p->chorus = 0; }
// ---- AMBIENT
static void pr_ocean(SPatch* p) { OA.pos = TRI; OA.uni = 3; OA.level = 0.4f; p->noiseType = 1; p->noiseLevel = 0.6f; p->cutoff = 0.4f; LFO(0, 0, 0, 0.08f);
	MOD(2, SRC_LFO1, DST_CUTOFF, 0.5f); ENV1(1.2f, 0, 1, 1, 3.0f); p->fx = 3; p->revSize = 1.0f; p->fxMix = 0.5f; p->master = 1.6f; }
static void pr_aurora(SPatch* p) { OA.table = T_VOWELS; OA.uni = 3; OA.detune = 18; LFO(0, 1, 0, 0.05f); MOD(2, SRC_LFO1, DST_APOS, 0.5f); ENV1(1.0f, 0, 1, 0.9f, 3.0f);
	p->fx = 3; p->revSize = 1.0f; p->revDamp = 0.3f; p->fxMix = 0.6f; p->master = 1.3f; }
static void pr_dust(SPatch* p) { pr_glass(p); p->noiseType = 2; p->noiseLevel = 0.3f; p->cutoff = 0.6f; p->master = 1.3f; }
static void pr_bloom(SPatch* p) { OA.uni = 3; OA.detune = 12; p->cutoff = 0.15f; ENV2(3.0f, 0, 2.0f, 0.8f, 2.0f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.7f);
	ENV1(3.0f, 0, 1, 1, 3.0f); p->fx = 3; p->revSize = 0.9f; p->fxMix = 0.5f; }
static void pr_dreamkeys(SPatch* p) { pr_epiano(p); p->revSize = 1.0f; p->fxMix = 0.65f; p->chorus = 0.7f; }
static void pr_deepspace(SPatch* p) { OA.table = T_GROWL; OA.pos = 0.1f; OA.oct = -1; OA.uni = 3; OA.detune = 35; p->subShape = 0; p->subOct = -1; p->subLevel = 0.3f;
	p->cutoff = 0.3f; LFO(0, 0, 0, 0.03f); MOD(2, SRC_LFO1, DST_APOS, 0.4f); ENV1(3.0f, 0, 1, 1, 4.0f); p->fx = 3; p->revSize = 1.0f; p->fxMix = 0.6f; }
static void pr_rain(SPatch* p) { OA.pos = SINE; OA.level = 0.5f; p->noiseType = 2; p->noiseLevel = 0.6f; LFO(1, 4, 0, 7.0f); MOD(3, SRC_LFO2, DST_PITCH, 0.2f);
	ENV1(0.002f, 0, 0.5f, 0.3f, 0.8f); p->fx = 3; p->fxMix = 0.5f; }
static void pr_lullaby(SPatch* p) { pr_marimba(p); p->revSize = 1.0f; p->fxMix = 0.6f; p->chorus = 0.3f; }

// ---- ARP (patches that drive the arpeggiator; the chord sets which notes one key gives it)
#define ARPSET(m, r, o, g, b) do { p->arp = m; p->arpRate = r; p->arpOct = o; p->arpGate = g; p->bpm = b; } while (0)
static void pa_trance(SPatch* p) { pr_supersaw(p); p->chord = 1; p->master = 0.65f; ARPSET(6, 3, 1, 0.35f, 138); p->fx = 3; p->fxMix = 0.35f; }
static void pa_berlin(SPatch* p) { OA.uni = 2; OA.detune = 8; p->cutoff = 0.25f; p->res = 0.45f; ENV2(0, 0, 0.15f, 0, 0.1f); MOD(0, SRC_ENV2, DST_CUTOFF, 0.55f);
	LFO(0, 0, 0, 0.1f); MOD(2, SRC_LFO1, DST_CUTOFF, 0.25f); ENV1(0.002f, 0, 0.25f, 0.3f, 0.12f); p->chord = 1; ARPSET(1, 3, 2, 0.6f, 120); p->fx = 1; p->echoT = 1; p->fxMix = 0.3f; }
static void pa_crystal(SPatch* p) { pr_bell(p); p->chord = 2; ARPSET(3, 4, 3, 0.5f, 100); p->master = 0.75f; }
static void pa_glassrain(SPatch* p) { pr_glass(p); p->master = 1.7f; ENV1(0.01f, 0, 0.5f, 0.3f, 0.6f); p->chord = 4; ARPSET(4, 3, 2, 0.4f, 110); }
static void pa_chip(SPatch* p) { pr_8bitlead(p); p->voiceMode = 0; p->chord = 2; ARPSET(1, 5, 2, 0.8f, 140); p->master = 0.7f; }
static void pa_synthwave(SPatch* p) { pr_80spoly(p); p->chord = 1; ARPSET(1, 1, 2, 0.6f, 100); p->revSize = 0.8f; p->fxMix = 0.4f; }
static void pa_dubecho(SPatch* p) { pr_syruppluck(p); p->chord = 1; ARPSET(5, 1, 1, 0.4f, 90); p->fx = 1; p->echoT = 1; p->fxMix = 0.6f; }
static void pa_ripple(SPatch* p) { pr_syruppad(p); ENV1(0.05f, 0, 0.8f, 0.6f, 1.0f); p->chord = 4; ARPSET(4, 3, 3, 1.0f, 96); p->master = 0.7f; }
static void pa_bassoct(SPatch* p) { pr_pluckbass(p); p->chord = 0; ARPSET(1, 3, 2, 0.5f, 124); }
static void pa_marimba(SPatch* p) { pr_marimba(p); p->chord = 2; ARPSET(3, 3, 2, 0.6f, 112); }
#define MAXPRE 12
#define NSTYLES 11   // last style = USER
static const char* STYLE_NAMES[NSTYLES] = {"BASS", "LEAD", "PAD", "PLUCK", "KEYS", "CHORDS", "ARP", "RETRO", "AMBIENT", "FX", "USER"};
static const PresetDef PRESETS[NSTYLES - 1][MAXPRE] = {
	{{"SUB WOBBLE", pr_subwob}, {"REESE", pr_reese}, {"FM GROWL", pr_fmgrowl}, {"808 SUB", pr_808}, {"ACID", pr_acid},
	 {"WOBBLE 1/8", pr_wob8}, {"DIGI BASS", pr_digibass}, {"SINE BASS", pr_sinebass}, {"PLUCK BASS", pr_pluckbass}, {"HOOVER", pr_hoover}, {"TILT WOBBLE", pr_tiltwob}},
	{{"SUPERSAW", pr_supersaw}, {"SYNC SCREAM", pr_syncscream}, {"VOWEL LEAD", pr_vowellead}, {"CHIP SQUARE", pr_chip}, {"DIGI PIPE", pr_digipipe},
	 {"BRASS LEAD", pr_brasslead}, {"WHISTLE", pr_whistle}, {"GLIDE SQUARE", pr_glidesq}, {"FUZZ LEAD", pr_fuzzlead}, {"FIFTHS", pr_fifths}, {"BREATH LEAD", pr_breath}},
	{{"SYRUP PAD", pr_syruppad}, {"GLASS", pr_glass}, {"CHOIR", pr_choir}, {"WARM STRINGS", pr_strings}, {"DARK DRONE", pr_drone},
	 {"VAPOR PAD", pr_vapor}, {"SWEEP PAD", pr_sweeppad}, {"FORMANT PAD", pr_formantpad}, {"SHIMMER PAD", pr_shimmerpad}, {"PWM PAD", pr_pwmpad}, {"TILT PAD", pr_tiltpad}, {"TABLE DRIFT", pr_drift}},
	{{"SYRUP PLUCK", pr_syruppluck}, {"MARIMBA", pr_marimba}, {"KALIMBA", pr_kalimba}, {"HARP", pr_harp}, {"DUB STAB", pr_dubstab},
	 {"BELL", pr_bell}, {"STEEL DRUM", pr_steeldrum}, {"SYNTH PLUCK", pr_synthpluck}, {"MUTED", pr_muted}, {"PIZZ", pr_pizz}},
	{{"E PIANO", pr_epiano}, {"ORGAN", pr_organ}, {"CLAV", pr_clav}, {"ACORDEON", pr_acordeon}, {"VINYL KEYS", pr_vinylkeys},
	 {"DARK RHODES", pr_rhodesdark}, {"WURLI", pr_wurli}, {"CHURCH ORGAN", pr_church}, {"HARPSICHORD", pr_harpsi}, {"LOFI PIANO", pr_lofipiano}},
	{{"HOUSE ORGAN", pr_houseorgan}, {"NEO SOUL", pr_neosoul}, {"POWER", pr_power}, {"SUS SHIMMER", pr_shimmer}, {"DUB CHORDS", pr_dubchord},
	 {"MIN7 PAD", pr_min7pad}, {"MAJ7 KEYS", pr_maj7keys}, {"POWER PLUCK", pr_powerpluck}, {"SUS ORGAN", pr_susorgan}, {"TECHNO STAB", pr_technostab}},
	{{"TRANCE GATE", pa_trance}, {"BERLIN SEQ", pa_berlin}, {"CRYSTAL ARP", pa_crystal}, {"GLASS RAIN", pa_glassrain}, {"CHIP ARP", pa_chip},
	 {"SYNTHWAVE ARP", pa_synthwave}, {"DUB ECHO ARP", pa_dubecho}, {"PAD RIPPLE", pa_ripple}, {"BASS OCTAVES", pa_bassoct}, {"MARIMBA RUN", pa_marimba}},
	{{"8-BIT LEAD", pr_8bitlead}, {"8-BIT BASS", pr_8bitbass}, {"VGM BRASS", pr_vgmbrass}, {"ARCADE", pr_arcade},
	 {"80S POLY", pr_80spoly}, {"SYNTHWAVE", pr_synthwave}, {"CHIP BELL", pr_chipbell}, {"GB WAVE", pr_gbwave}},
	{{"OCEAN", pr_ocean}, {"AURORA", pr_aurora}, {"DUST", pr_dust}, {"SLOW BLOOM", pr_bloom},
	 {"DREAM KEYS", pr_dreamkeys}, {"DEEP SPACE", pr_deepspace}, {"RAIN", pr_rain}, {"LULLABY", pr_lullaby}},
	{{"RISER", pr_riser}, {"LASER", pr_laser}, {"WIND", pr_wind}, {"RADIO", pr_radio}, {"ROBOT", pr_robot},
	 {"SIREN", pr_siren}, {"BUBBLES", pr_bubbles}, {"NOISE SWEEP", pr_noisesweep}, {"ALARM", pr_alarm}, {"SPACE DRONE", pr_spacedrone}, {"VOWEL TALK", pr_vowtalk}},
};
#define NUSER 8
static SPatch gUser[NUSER];
static int gStyle = 2, gPreset = 0;      // style + patch index shown in the browser
static char gPatchName[24] = "INIT";
static int gSaveSlot = 0;
static const char* USER_PATH = "sdmc:/3ds/syrup/user_presets.bin";

static void userLoad(void) {
	for (int i = 0; i < NUSER; i++) { patchInit(&gUser[i]); snprintf(gUser[i].name, sizeof(gUser[i].name), "EMPTY %d", i + 1); gUser[i].magic = 0; }
	FILE* f = fopen(USER_PATH, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	if (len == (long)(sizeof(SPatch) * NUSER)) {
		SPatch tmp[NUSER];
		if (fread(tmp, sizeof(SPatch), NUSER, f) == NUSER) for (int i = 0; i < NUSER; i++) if (tmp[i].magic == PATCH_MAGIC) gUser[i] = tmp[i];
	} else if (len == (long)(PATCH_V1_SIZE * NUSER) || len == (long)(PATCH_V2_SIZE * NUSER)) {   // older save files: newer fields get defaults
		long rs = len / NUSER;
		for (int i = 0; i < NUSER; i++) {
			SPatch t; patchInit(&t);
			if (fread(&t, rs, 1, f) == 1 && (t.magic == PATCH_MAGIC1 || t.magic == PATCH_MAGIC2)) {
				if (t.magic == PATCH_MAGIC1) { t.arp = 0; t.arpRate = 3; t.arpOct = 1; t.arpGate = 0.5f; t.bpm = 0; }
				for (int k = 0; k < 2; k++) { t.lfoSync[k] = 0; t.lfoTable[k] = T_HARMONIX; t.lfoPos[k] = 0.5f; }
				t.magic = PATCH_MAGIC; gUser[i] = t;
			}
		}
	}
	fclose(f);
}
static int userSave(void) {
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/syrup", 0777);
	FILE* f = fopen(USER_PATH, "wb");
	if (!f) return 0;
	int ok = fwrite(gUser, sizeof(SPatch), NUSER, f) == NUSER;
	fclose(f);
	return ok;
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
	for (int i = 0; i < 4; i++) p->mod[i] = gMod[i];
	p->voiceMode = gVoiceMode; p->chord = gChord; p->fx = gFx; p->echoT = gEchoT; p->drive = gDrive; p->bendRange = gBendRange;
	p->glide = gGlide; p->chorus = gChorus; p->revSize = gRevSize; p->revDamp = gRevDamp; p->fxMix = gFxMix; p->driveAmt = gDriveAmt; p->master = gMaster;
	p->arp = gArp; p->arpRate = gArpRate; p->arpOct = gArpOct; p->arpGate = gArpGate; p->bpm = gBPM;
}

static int gForceBuild = 3;
static void patchApply(const SPatch* p) {
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
	gSubShape = p->subShape; gSubOct = p->subOct; gNoiseType = p->noiseType; gFiltType = p->filtType;
	gSubLevel = p->subLevel; gNoiseLevel = p->noiseLevel; gCutoff = p->cutoff; gRes = p->res; gKeytrack = p->keytrack;
	gEnv1 = p->env1; gEnv2 = p->env2;
	for (int i = 0; i < 2; i++) { gLfo[i].shape = p->lfoShape[i] < NLFOSH ? p->lfoShape[i] : 0; gLfo[i].mode = p->lfoMode[i]; gLfo[i].rate = p->lfoRate[i]; gLfo[i].sync = p->lfoSync[i] < NSYNC ? p->lfoSync[i] : 0; gLfo[i].table = p->lfoTable[i] < T_BUILTIN ? p->lfoTable[i] : 0; gLfo[i].pos = p->lfoPos[i]; gLfo[i].tabKey = -1; }
	for (int i = 0; i < 4; i++) gMod[i] = p->mod[i];
	gVoiceMode = p->voiceMode; gChord = p->chord; gFx = p->fx; gEchoT = p->echoT; gDrive = p->drive; gBendRange = p->bendRange;
	gGlide = p->glide; gChorus = p->chorus; gRevSize = p->revSize; gRevDamp = p->revDamp; gFxMix = p->fxMix; gDriveAmt = p->driveAmt; gMaster = p->master;
	gArp = p->arp; gArpRate = p->arpRate; gArpOct = p->arpOct < 1 ? 1 : p->arpOct; gArpGate = p->arpGate; if (p->bpm >= 40) gBPM = p->bpm;
	gArpN = 0; gArpOnN = 0; gHeldN = 0;
	if (subChg) buildSub();
	LightLock_Unlock(&gMipsLock);
	if (noiseChg) buildNoise();
	snprintf(gPatchName, sizeof(gPatchName), "%s", p->name);
	gForceBuild = 3;
}

static int presetCount(int style) {
	if (style == NSTYLES - 1) return NUSER;
	int n = 0; while (n < MAXPRE && PRESETS[style][n].name) n++;
	return n;
}
static void presetLoad(int style, int idx) {
	if (style == NSTYLES - 1) { if (gUser[idx].magic == PATCH_MAGIC) patchApply(&gUser[idx]); return; }
	SPatch p; patchInit(&p);
	PRESETS[style][idx].fn(&p);
	snprintf(p.name, sizeof(p.name), "%s", PRESETS[style][idx].name);
	patchApply(&p);
}
static const char* presetName(int style, int idx) { return style == NSTYLES - 1 ? gUser[idx].name : PRESETS[style][idx].name; }

// ---------------------------------------------------------------- parameter pages
enum { PT_F, PT_I, PT_E };
enum { D_A = 1, D_B = 2, D_SUB = 4, D_NOISE = 8, D_TBLA = 16, D_TBLB = 32, D_STYLE = 64, D_PRESET = 128, D_SAVE = 256, D_INIT = 512, D_REST = 1024, D_CLEAR = 2048, D_SEQ = 4096, D_SCALE = 8192, D_OUT = 16384, D_TILT = 32768, D_THEME = 65536 };
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
	PE("SUB SHAPE", &gSubShape, 4, SUB_SHAPES, D_SUB), PI_("SUB OCTAVE", &gSubOct, -2, 0, 0),
	PF("SUB LEVEL", &gSubLevel, 0, 1, 0.01f, "%", 0),
	PE("NOISE TYPE", &gNoiseType, 3, NOISE_TYPES, D_NOISE), PF("NOISE LEVEL", &gNoiseLevel, 0, 1, 0.01f, "%", 0),
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
static Param P_MOD[] = {
	PE("LFO1 SHAPE", &gLfo[0].shape, NLFOSH, LFO_SHAPES, 0), PF("LFO1 RATE", &gLfo[0].rate, 0.05f, 20, 0.05f, "HZ", 0), PE("LFO1 SYNC", &gLfo[0].sync, NSYNC, SYNC_NAMES, 0),
	PE("LFO1 MODE", &gLfo[0].mode, 2, LFO_MODES, 0), PE("LFO1 TABLE", &gLfo[0].table, T_BUILTIN, BUILTIN_NAMES, 0), PF("LFO1 WT POS", &gLfo[0].pos, 0, 1, 0.02f, "%", 0),
	PE("LFO2 SHAPE", &gLfo[1].shape, NLFOSH, LFO_SHAPES, 0), PF("LFO2 RATE", &gLfo[1].rate, 0.05f, 20, 0.05f, "HZ", 0), PE("LFO2 SYNC", &gLfo[1].sync, NSYNC, SYNC_NAMES, 0),
	PE("LFO2 MODE", &gLfo[1].mode, 2, LFO_MODES, 0), PE("LFO2 TABLE", &gLfo[1].table, T_BUILTIN, BUILTIN_NAMES, 0), PF("LFO2 WT POS", &gLfo[1].pos, 0, 1, 0.02f, "%", 0),
	PE("1 SOURCE", &gMod[0].src, SRC_COUNT, SRC_NAMES, 0), PE("1 DEST", &gMod[0].dst, DST_COUNT, DST_NAMES, 0), PF("1 AMOUNT", &gMod[0].amt, -1, 1, 0.02f, "%", 0),
	PE("2 SOURCE", &gMod[1].src, SRC_COUNT, SRC_NAMES, 0), PE("2 DEST", &gMod[1].dst, DST_COUNT, DST_NAMES, 0), PF("2 AMOUNT", &gMod[1].amt, -1, 1, 0.02f, "%", 0),
	PE("3 SOURCE", &gMod[2].src, SRC_COUNT, SRC_NAMES, 0), PE("3 DEST", &gMod[2].dst, DST_COUNT, DST_NAMES, 0), PF("3 AMOUNT", &gMod[2].amt, -1, 1, 0.02f, "%", 0),
	PE("4 SOURCE", &gMod[3].src, SRC_COUNT, SRC_NAMES, 0), PE("4 DEST", &gMod[3].dst, DST_COUNT, DST_NAMES, 0), PF("4 AMOUNT", &gMod[3].amt, -1, 1, 0.02f, "%", 0),
	PEND};
static const char* BEND_NAMES[13] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
static Param P_VOICE[] = {
	PE("MODE", &gVoiceMode, 3, VOICE_MODES, 0), PF("GLIDE", &gGlide, 0, 1, 0.01f, "S", 0),
	PE("BEND RANGE", &gBendRange, 13, BEND_NAMES, 0), PE("CHORD", &gChord, 5, CHORD_NAMES, 0),
	PF("CHORUS", &gChorus, 0, 1, 0.02f, "%", 0), PE("GPU VISUALS", &gGpuFx, 2, ONOFF, 0), PE("FX", &gFx, 4, FX_NAMES, 0), PE("ECHO TIME", &gEchoT, 3, ECHO_T_NAMES, 0),
	PF("ECHO FDBK", &gEchoFb, 0, 0.9f, 0.03f, "%", 0), PE("OUTPUT", &gOutMode, 2, OUT_NAMES, D_OUT), PE("SURROUND", &gSurDepth, 4, DEPTH_NAMES, D_OUT),
	{"TILT CENTER", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_TILT},
	PF("REV SIZE", &gRevSize, 0, 1, 0.02f, "%", 0), PF("REV DAMP", &gRevDamp, 0, 1, 0.02f, "%", 0), PF("FX MIX", &gFxMix, 0, 1, 0.02f, "%", 0),
	PE("DRIVE", &gDrive, 5, DRIVE_TYPES, D_A | D_B), PF("DRIVE AMT", &gDriveAmt, 0, 1, 0.02f, "%", D_A | D_B),
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
	PE("ROOT", &gRoot, 12, NOTE_NAMES, D_SCALE), PE("SCALE", &gScale, NSCALES, SCALE_NAMES, D_SCALE),
	PE("ARP", &gArp, NARPM, ARP_NAMES, D_SEQ), PE("ARP RATE", &gArpRate, 6, RATE_NAMES, 0), PI_("ARP OCTAVES", &gArpOct, 1, 4, 0),
	PF("ARP GATE", &gArpGate, 0.1f, 1, 0.05f, "%", 0), PI_("BPM", &gBPM, 40, 240, 0),
	PE("SEQ", &gSeqMode, 3, SEQ_NAMES, D_SEQ), PI_("SEQ LENGTH", &gSeqLen, 1, NSTEP, 0), PF("SEQ GATE", &gSeqGate, 0.1f, 1, 0.05f, "%", 0),
	PF("SWING", &gSwing, 0, 0.5f, 0.02f, "%", 0),
	{"ADD REST", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_REST},
	{"CLEAR SEQ", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_CLEAR},
	PEND};
static Param P_VIS[] = {
	PE("VIS MODE", &gVisMode, NVISM, VISM_NAMES, 0), PE("VIS OSC", &gVisOsc, 2, AB_NAMES, 0), PE("OVERLAY", &gVisOverlay, 2, ONOFF, 0),
	PE("THEME", &gTheme, NTHEMES, THEME_NAMES, D_THEME), PE("SCANLINES", &gScan, 2, ONOFF, D_THEME),
	PEND};
#define NPAGES 9
static Param* PAGES[NPAGES] = {P_PRESET, P_OSCA, P_OSCB, P_SUBN, P_FILT, P_MOD, P_VOICE, P_PLAY, P_VIS};
static const char* PAGE_NAMES[NPAGES] = {"PRESETS", "OSC A", "OSC B", "SUB+NOISE", "FILT+ENV", "LFO+MOD", "VOICE+FX", "ARP+SEQ", "VISUALIZER"};
static int pageLen(int pg) { int n = 0; while (PAGES[pg][n].name) n++; return n; }

static void paramFmt(const Param* p, char* out, int len) {
	if (p->type == PT_E) {
		int v = *(int*)p->p;
		if (p->names == TABLE_NAME_DUMMY) snprintf(out, len, "%.16s", p->p == &gOsc[0].table ? gOsc[0].tb.name : gOsc[1].tb.name);
		else if (p->names == PRESET_DUMMY) snprintf(out, len, "%.16s", presetName(gStyle, gPreset));
		else if (p->names == PRESS_RIGHT) snprintf(out, len, "PRESS >");
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
};
#define TH (&THEMES[gTheme])
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
static void frame(int x, int y, int w, int h) {
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
		{"CHORD", "CHORD PLAYED BY ONE KEY (ALSO A)"}, {"CHORUS", "STEREO CHORUS AMOUNT"}, {"GPU VISUALS", "GRAPHICS CHIP VISUALS ON THE LEFT"},
		{"FX", "ECHO, SPACE OR REVERB"}, {"ECHO TIME", "DELAY TIME"}, {"ECHO FDBK", "NUMBER OF REPEATS"}, {"OUTPUT", "STEREO OR VIRTUAL SURROUND"},
		{"SURROUND", "SURROUND DEPTH"}, {"TILT CENTER", "PRESS > TO RE-CENTRE THE TILT"}, {"REV SIZE", "REVERB ROOM SIZE"},
		{"REV DAMP", "DARKER REVERB TAIL"}, {"FX MIX", "HOW MUCH EFFECT"}, {"DRIVE", "DISTORTION TYPE"}, {"DRIVE AMT", "HOW MUCH DISTORTION"},
		{"MASTER", "OUTPUT VOLUME"}, {"ROOT", "KEY OF THE SCALE"}, {"SCALE", "LOCK THE KEYS TO A SCALE"}, {"ARP", "ARPEGGIATOR PATTERN"},
		{"ARP RATE", "ARPEGGIATOR SPEED"}, {"ARP OCTAVES", "OCTAVES THE ARP CLIMBS"}, {"ARP GATE", "ARP NOTE LENGTH"}, {"BPM", "TEMPO"},
		{"SEQ", "SEQUENCER: OFF, PLAY OR RECORD"}, {"SEQ LENGTH", "STEPS IN THE PATTERN"}, {"SEQ GATE", "STEP NOTE LENGTH"},
		{"SWING", "SHUFFLE FEEL"}, {"ADD REST", "PRESS > TO RECORD A REST"}, {"CLEAR SEQ", "PRESS > TO ERASE THE PATTERN"},
		{"VIS MODE", "VISUALIZER STYLE"}, {"VIS OSC", "OSCILLATOR SHOWN"}, {"OVERLAY", "SHOW TEXT OVER THE VISUALIZER"},
		{"THEME", "COLOUR SCHEME"}, {"SCANLINES", "CRT SCANLINES ON THE TOP SCREEN"},
	};
	for (unsigned i = 0; i < sizeof(H) / sizeof(H[0]); i++) if (!strcmp(H[i][0], n)) return H[i][1];
	return "";
}

// ---- Pip-Boy tabs: main sections with sub-pages
static const char* SECT_NAMES[6] = {"PATCH", "OSC", "SHAPE", "FX", "PLAY", "VIS"};
static const int PAGE_SECT[NPAGES] = {0, 1, 1, 1, 2, 2, 3, 4, 5};
static const char* SUB_NAMES[NPAGES] = {"PRESETS", "OSC A", "OSC B", "SUB+NOISE", "FILTER+ENV", "LFO+MOD", "VOICE+FX", "ARP+SEQ", "VISUALIZER"};

static float gPosEff[2];
static int visActive(int page);
static void drawHeader(int page) {
	text(4, 3, "SYRUP", 1, TXT);
	rect(0, 13, 400, 1, LINE);
	int x = 58, sect = PAGE_SECT[page];
	for (int i = 0; i < 6; i++) {
		int w = strlen(SECT_NAMES[i]) * 8;
		if (i == sect) {   // the active tab breaks the line, Pip-Boy style
			rect(x - 4, 13, w + 8, 1, BG);
			rect(x - 4, 7, 1, 7, LINE); rect(x + w + 3, 7, 1, 7, LINE);
			rect(x - 4, 7, 3, 1, LINE); rect(x + w + 1, 7, 3, 1, LINE);
			text(x, 3, SECT_NAMES[i], 1, TXT);
		} else text(x, 3, SECT_NAMES[i], 1, DIM);
		x += w + 14;
	}
	char pn[16]; snprintf(pn, sizeof(pn), "%.12s", gPatchName);
	text(396 - (int)strlen(pn) * 8, 3, pn, 1, VAL);
	// sub-pages of this section
	int sx = 8;
	for (int p = 0; p < NPAGES; p++) if (PAGE_SECT[p] == sect) {
		int w = strlen(SUB_NAMES[p]) * 8;
		if (p == page) { rect(sx - 3, 16, w + 6, 11, HI); text(sx, 18, SUB_NAMES[p], 1, HITXT); }
		else text(sx, 18, SUB_NAMES[p], 1, DIM);
		sx += w + 16;
	}
}
static void drawFooter(int page, int sel) {
	rect(0, 226, 400, 1, LINE);
	if (!gAudioOK) { text(4, 230, "NO AUDIO - RUN DSP1 ONCE (DUMPS DSP FIRMWARE)", 1, WARN); return; }
	if (gSafeMode) { char m[60]; snprintf(m, sizeof(m), "SAFE MODE - LAST RUN CRASHED AT %.24s", gCrashStage); text(4, 230, m, 1, WARN); return; }
	const char* h = paramHelp(PAGES[page][sel].name);
	char m[52]; snprintf(m, sizeof(m), "%.49s", *h ? h : "D-PAD: PICK / CHANGE   X/Y: PAGES");
	text(4, 230, m, 1, DIM);
}

static void drawTop(int page, int sel, int oct, int latch) {
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
	if (page == 0) {   // preset browser: styles | patches
		frame(4, 30, 196, 194);
		for (int i = 0; i < NSTYLES; i++) {
			int y = 35 + i * 13;
			if (i == gStyle) { rect(7, y - 2, 74, 11, HI); text(10, y, STYLE_NAMES[i], 1, HITXT); }
			else text(10, y, STYLE_NAMES[i], 1, DIM);
		}
		rect(84, 33, 1, 186, LINE);
		for (int i = 0; i < presetCount(gStyle); i++) {
			int y = 35 + i * 13, cur = i == gPreset;
			char nm[16]; snprintf(nm, sizeof(nm), "%.13s", presetName(gStyle, i));
			if (cur) { rect(87, y - 2, 110, 11, HI); text(90, y, nm, 1, HITXT); }
			else text(90, y, nm, 1, TXT);
		}
		char cnt[32]; snprintf(cnt, sizeof(cnt), "%d PATCHES  BUILD " SYRUP_VER, presetCount(gStyle)); text(10, 212, cnt, 1, DIM);
	} else if (page == 7) {   // scale / arp / sequencer
		char s[40];
		frame(4, 30, 196, 194);
		snprintf(s, sizeof(s), "%s %s", NOTE_NAMES[gRoot], SCALE_NAMES[gScale]); text(10, 36, s, 1, VAL);
		snprintf(s, sizeof(s), "ARP %s %s X%d", ARP_NAMES[gArp], RATE_NAMES[gArpRate], gArpOct); text(10, 48, s, 1, TXT);
		snprintf(s, sizeof(s), "%d BPM  SEQ %s", gBPM, SEQ_NAMES[gSeqMode]); text(10, 60, s, 1, TXT);
		for (int i = 0; i < NSTEP; i++) {   // 2 rows of 8 steps
			int x = 8 + (i % 8) * 24, y = 76 + (i / 8) * 40, act = i < gSeqLen;
			int playing = gSeqPlaying && i == gSeqPos, rec = gSeqMode == 2 && i == gRecPos;
			if (playing) rect(x, y, 22, 34, HI);
			else if (rec) rect(x, y, 22, 34, WARN);
			else { rect(x, y, 22, 34, BG); if (act) { rect(x, y, 22, 1, LINE); rect(x, y + 33, 22, 1, LINE); rect(x, y, 1, 34, LINE); rect(x + 21, y, 1, 34, LINE); } else for (int k = 0; k < 22; k += 2) px(x + k, y + 33, DIM); }
			if (act && gSeq[i].n) {
				int nt = gSeq[i].note[0];
				if (playing) text(x + 11 - (int)strlen(NOTE_NAMES[nt % 12]) * 4, y + 6, NOTE_NAMES[nt % 12], 1, HITXT);
				else text(x + 11 - (int)strlen(NOTE_NAMES[nt % 12]) * 4, y + 6, NOTE_NAMES[nt % 12], 1, TXT);
				char o[4]; snprintf(o, sizeof(o), "%d", nt / 12 - 1);
				if (playing) text(x + 7, y + 18, o, 1, HITXT); else text(x + 7, y + 18, o, 1, DIM);
				if (gSeq[i].n > 1) rect(x + 16, y + 27, 3, 3, VAL);
			} else if (act) text(x + 7, y + 12, "-", 1, DIM);
		}
		text(10, 162, gSeqMode == 2 ? "REC: KEYS WRITE STEPS" : (gSeqMode == 1 ? "PLAY: KEYS TRANSPOSE" : "SEQ OFF"), 1, DIM);
		if (gSeqMode == 1 && gSeqTr) { snprintf(s, sizeof(s), "TRANSPOSE %+d", gSeqTr); text(10, 174, s, 1, DIM); }
		drawWave(gOsc[0].mips.lev[2], LEVN[2], 8, 186, 188, 34, gOsc[0].on);
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
		char s[40];
		snprintf(s, sizeof(s), "%s  OCT %+d%s", VOICE_MODES[gVoiceMode], oct, latch ? "  LATCH" : ""); text(6, 160, s, 1, TXT);
		snprintf(s, sizeof(s), "CHORD %s  FX %s", CHORD_NAMES[gChord], FX_NAMES[gFx]); text(6, 172, s, 1, TXT);
		int voices = 0, dsp = 0, cs = 0, csTot = 0;
		for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started && !gV[i].isEcho) voices++;
		for (int c = 0; c < NCH; c++) if (gDspOwner[c] >= 0 || (gNoiseOn && c == NOISE_CH)) dsp++;
		for (int c = 0; c < NCS; c++) if (csUsable(c)) { csTot++; if (gCsOwner[c] >= 0) cs++; }
		snprintf(s, sizeof(s), "NOTES %d  FX BUS %s", voices, busActive() ? (gBusSeen ? "OK" : "ON") : (gBusFail ? "FALLBK" : "OFF")); text(6, 186, s, 1, DIM);
		snprintf(s, sizeof(s), "DSP %d/24  CSND %d/%d", dsp, cs, csTot); text(6, 198, s, 1, DIM);
		if (gGpuHung) { snprintf(s, sizeof(s), "GPU HUNG AFTER %d FRAMES", gGpuFrames); text(6, 210, s, 1, WARN); }
		else if (gGpuFx && gGpuOK) { snprintf(s, sizeof(s), "GPU OK  CORE2 %s", gWorker ? "ON" : "OFF"); text(6, 210, s, 1, DIM); }
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
		if (P[i].type == PT_F && TH->ega) {   // 8-block meter
			float t = (*(float*)P[i].p - P[i].mn) / (P[i].mx - P[i].mn + 1e-9f);
			int blocks = (int)(t * 8 + 0.5f);
			for (int b = 0; b < 8; b++) {
				int bx = 300 + b * 4;
				if (b < blocks) { if (cur) rect(bx, y, 3, 7, HITXT); else rect(bx, y, 3, 7, TXT); }
				else { if (cur) rect(bx, y + 6, 3, 1, HITXT); else rect(bx, y + 6, 3, 1, DIM); }
			}
			if (w > 54) w = 54;
		}
		if (cur && TH->ega && P[i].type == PT_E && P[i].names != PRESS_RIGHT) { char b2[28]; snprintf(b2, sizeof(b2), "<%s>", v); text(394 - (int)strlen(b2) * 8, y, b2, 1, HITXT); }
		else if (cur && TH->ega) text(394 - w, y, v, 1, HITXT);
		else text(394 - w, y, v, 1, VAL);
	}
	if (first > 0) text(388, 30, "^", 1, TXT);
	if (first + rows < n) text(388, 216, "v", 1, TXT);
	drawFooter(page, sel);
	if (!visActive(page)) scanlines();
}

static void drawHelp(void) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 0, 400, 240, BG);
	text(4, 3, "SYRUP", 1, TXT); text(58, 3, "HELP", 1, TXT); rect(0, 13, 400, 1, LINE);
	text(200, 3, "SELECT / HELP TO CLOSE", 1, DIM);
	static const char* L[] = {
		"PAGES",
		"  X / Y ........ NEXT / PREVIOUS PAGE",
		"  D-PAD U/D .... PICK A CONTROL",
		"  D-PAD L/R .... CHANGE IT (HOLD = FAST)",
		"TOUCH",
		"  XY PAD ....... X = OSC A WT POS, Y = CUTOFF",
		"  KEYS ......... PLAY; LOWER ON A KEY = LOUDER",
		"  TABS ......... HELP, PAGE, OCT-/OCT+, LATCH",
		"BUTTONS",
		"  A ............ CHORD MODE",
		"  B ............ LATCH (NOTES KEEP RINGING)",
		"  L / R ........ OCTAVE",
		"  CIRCLE PAD ... L/R BEND, U/D = STICK SOURCE",
		"  START ........ QUIT",
		"NOTES",
		"  MOD SOURCES: TILT L/R, TILT F/B, BREATH (MIC)",
		"  SCALE ON: KEYS BECOME SCALE PADS",
		"  SEQ REC: KEYS WRITE STEPS; PLAY: KEYS TRANSPOSE",
		"  OWN WAVETABLES: SERUM .WAV IN /3DS/SYRUP/",
	};
	int y = 22;
	for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++, y += 11) {
		if (L[i][0] != ' ') text(8, y, L[i], 1, VAL); else text(8, y, L[i], 1, TXT);
	}
	scanlines();
}

#define TAB_H 20
#define XY_Y  22
#define XY_H  94
#define KB_Y  118
static int keyAt(int x, int y, int base) {
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};
	float ww = 320.0f / 14;
	if (gScale) { int i = (int)(x / ww); if (i > 13) i = 13; if (i < 0) i = 0; gKeyDeg = i; return scaleNote(base + gRoot, i); }
	if (y < KB_Y + 70)
		for (int i = 0; i < 14; i++) {
			if (!BL[i % 7]) continue;
			int bx = (int)((i + 1) * ww - ww * 0.3f);
			if (x >= bx && x < bx + (int)(ww * 0.6f)) return base + (i / 7) * 12 + WN[i % 7] + 1;
		}
	int i = (int)(x / ww); if (i > 13) i = 13;
	return base + (i / 7) * 12 + WN[i % 7];
}

static void outline(int x, int y, int w, int h, u8 r, u8 g, u8 b) { rect(x, y, w, 1, r, g, b); rect(x, y + h - 1, w, 1, r, g, b); rect(x, y, 1, h, r, g, b); rect(x + w - 1, y, 1, h, r, g, b); }
static void drawBottom(int base, const u8* on, int helpOn, int latch) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, KB_Y, BG);
	static const char* TABS[6] = {"HELP", "<PAGE", "PAGE>", "OCT-", "OCT+", "LATCH"};
	for (int i = 0; i < 6; i++) {
		int x = i * 320 / 6, lit = (i == 0 && helpOn) || (i == 5 && latch);
		int w = strlen(TABS[i]) * 8, bw = 320 / 6 - 4;
		if (TH->ega) {
			if (lit) { rect(x + 2, 1, bw, TAB_H - 3, HI); text(x + 320 / 12 - w / 2, 6, TABS[i], 1, HITXT); }
			else { outline(x + 2, 1, bw, TAB_H - 3, LINE); text(x + 320 / 12 - w / 2, 6, TABS[i], 1, TXT); }
		} else {
			if (lit) { rect(x + 2, 1, bw, TAB_H - 2, 90, 56, 16); text(x + 320 / 12 - w / 2, 7, TABS[i], 1, 255, 200, 120); }
			else { rect(x + 2, 1, bw, TAB_H - 2, RGB3(TH->tab)); text(x + 320 / 12 - w / 2, 7, TABS[i], 1, RGB3(TH->tabTxt)); }
		}
	}
	// XY pad: dotted EGA grid, crosshair
	int cx = 4 + (int)(gOsc[0].pos * 311), cy = XY_Y + (int)((1 - gCutoff) * (XY_H - 1));
	if (TH->ega) {
		for (int gx = 0; gx <= 8; gx++) for (int y = XY_Y; y < XY_Y + XY_H; y += 2) px(4 + gx * 39, y, DIM);
		for (int gy = 0; gy <= 4; gy++) for (int x = 4; x < 316; x += 2) px(x, XY_Y + gy * (XY_H - 1) / 4, DIM);
		rect(cx, XY_Y, 1, XY_H, LINE); rect(4, cy, 312, 1, LINE);
		rect(cx - 4, cy - 4, 9, 9, TXT); rect(cx - 2, cy - 2, 5, 5, BG);
	} else {
		rect(4, XY_Y, 312, XY_H, RGB3(TH->pad));
		for (int gx = 1; gx < 8; gx++) rect(4 + gx * 39, XY_Y, 1, XY_H, RGB3(TH->grid));
		for (int gy = 1; gy < 4; gy++) rect(4, XY_Y + gy * XY_H / 4, 312, 1, RGB3(TH->grid));
		rect(cx, XY_Y, 1, XY_H, LINE); rect(4, cy, 312, 1, LINE); rect(cx - 4, cy - 4, 9, 9, TXT);
		frame(4, XY_Y, 312, XY_H);
	}
	text(8, XY_Y + 4, "WT POS >", 1, DIM); text(248, XY_Y + XY_H - 12, "^ CUTOFF", 1, DIM);
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};
	float ww = 320.0f / 14;
	rect(0, KB_Y, 320, 240 - KB_Y, RGB3(TH->kbBg));
	if (TH->ega) rect(0, KB_Y, 320, 1, LINE);
	if (gScale) {   // scale mode: 14 pads, one per scale degree
		int n = scaleLen();
		for (int i = 0; i < 14; i++) {
			int note = scaleNote(base + gRoot, i), x = (int)(i * ww), root = i % n == 0;
			int lit = note < 128 && on[note];
			if (lit) rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyOn));
			else if (root) rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->dim));
			else { rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyW)); if (TH->ega) outline(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyLine)); }
			if (lit || root) text(x + (int)ww / 2 - (int)strlen(NOTE_NAMES[note % 12]) * 4, 226, NOTE_NAMES[note % 12], 1, EC(0));
			else text(x + (int)ww / 2 - (int)strlen(NOTE_NAMES[note % 12]) * 4, 226, NOTE_NAMES[note % 12], 1, TH->ega ? TH->keyLine[0] : 60, TH->ega ? TH->keyLine[1] : 40, TH->ega ? TH->keyLine[2] : 20);
		}
		char c[24]; snprintf(c, sizeof(c), "%s %s", NOTE_NAMES[gRoot], SCALE_NAMES[gScale]); text(4, KB_Y + 5, c, 1, VAL);
		return;
	}
	for (int i = 0; i < 14; i++) {
		int n = base + (i / 7) * 12 + WN[i % 7], x = (int)(i * ww);
		if (on[n]) rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyOn));
		else { rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyW)); if (TH->ega) outline(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, RGB3(TH->keyLine)); }
	}
	for (int i = 0; i < 14; i++) {
		if (!BL[i % 7]) continue;
		int n = base + (i / 7) * 12 + WN[i % 7] + 1, bx = (int)((i + 1) * ww - ww * 0.3f);
		if (on[n]) rect(bx, KB_Y + 2, (int)(ww * 0.6f), 68, RGB3(TH->keyOnB));
		else { rect(bx, KB_Y + 2, (int)(ww * 0.6f), 68, RGB3(TH->keyB)); if (TH->ega) outline(bx, KB_Y + 2, (int)(ww * 0.6f), 68, RGB3(TH->keyLine)); }
	}
	char c[8]; snprintf(c, sizeof(c), "C%d", base / 12 - 1);
	if (on[base] || !TH->ega) text(4, 228, c, 1, TH->ega ? 0 : 120, TH->ega ? 0 : 90, TH->ega ? 0 : 50); else text(4, 228, c, 1, RGB3(TH->keyLine));
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
static float gVisT = 0, gVisEnv = 0;
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
static void visFull(void) {
	int mode = gVisMode ? gVisMode : 1 + ((int)(gVisT / 10.0f)) % 4;   // AUTO: change every 10 s
	if (gVisBeat > 0.01f) { float r, g, b; hue(0.35f, &r, &g, &b); blendAdd(1); vRect(0, 0, 400, 240, rgba(r, g, b, gVisBeat * 0.12f)); blendAdd(0); }
	if (mode == 1) visWavetable(gVisOsc, 200, 128, 1.6f);
	else if (mode == 2) visScopes(10, 20, 380, 210);
	else if (mode == 3) visOrbit(200, 125, 100);
	else visSpectrum(12, 20, 376, 210, gVisOsc);
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
		if (full) { u8* fbp = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); GSPGPU_InvalidateDataCache(gFullLin, 240 * 400 * 3); memcpy(fbp, gFullLin, 240 * 400 * 3); }
		else if (page == 1 || page == 2) visBlit(30, 206);
		else visBlit(30, 154);
	}
	lastPage = page; have = 1;
	C3D_RenderTarget* tg = full ? gFullTgt : gVisTgt;
	int cols = full ? 400 : VIS_W;
	C2D_TargetClear(tg, C2D_Color32(TH->bg[0], TH->bg[1], TH->bg[2], 0xff));
	C2D_SceneBegin(tg);
	C2D_SceneSize(240 * VIS_SS, cols * VIS_SS, true);   // tilted like a real screen target
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
static LightEvent gJobEv;
static RebuildJob gJob;
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
		RebuildJob j = gJob;
		LightLock_Lock(&gMipsLock);
		doRebuild(&j);
		LightLock_Unlock(&gMipsLock);
		ASET(gJobDone, 1);
		ASET(gJobBusy, 0);
	}
}

// ---------------------------------------------------------------- main
static u32 usedLevels(int o) {
	u32 m = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);   // display + common note ranges
	for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started)
		for (int k = 0; k < gV[i].nch; k++) {
			int r = gV[i].role[k], to = gOsc[0].on ? 0 : 1;
			if ((o == 0 && r <= R_A2) || (o == 1 && r >= R_B0 && r <= R_B2) || (r == R_TAP && o == to)) m |= 1u << gV[i].lev[k];
		}
	return m;
}

int main(void) {
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, true);     // draw into the hidden buffer, swap on vblank: no tearing
	gfxSetDoubleBuffering(GFX_BOTTOM, true);
	crashCheck();
	crashMark("GPU INIT");
	if (!gSafeMode) gpuInit();
	crashMark("SENSORS INIT");
	if (!gSafeMode) sensorsInit();
	if (gSafeMode) { gGpuFx = 0; gAudioClock = 0; }
	fontGfx = consoleGetDefault()->font.gfx;
	fftInit();
	for (int o = 0; o < 2; o++) { gOsc[o].tb.frames = (float*)malloc(sizeof(float) * CYC * MAXFR); mipsInit(&gOsc[o].mips); }
	mipsInit(&gSubMips);
	gNoise = (s16*)linearAlloc(gNoiseLen * sizeof(s16));
	for (int c = 0; c < NCH; c++) gDspOwner[c] = -1;
	for (int c = 0; c < NCS; c++) gCsOwner[c] = -1;
	scanTables();
	gTableCount = T_BUILTIN + gNFiles;

	// default patch: A = Basic Shapes, 3-voice unison; B = Harmonix an octave up (off)
	Osc* A = &gOsc[0]; Osc* B = &gOsc[1];
	A->on = 1; A->table = T_BASIC; A->pos = 0.6f; A->level = 0.8f; A->uni = 3; A->detune = 14; A->blend = 0.6f; A->width = 0.8f;
	B->on = 0; B->table = T_HARMONIX; B->pos = 0.3f; B->level = 0.5f; B->oct = 1; B->uni = 1; B->detune = 10; B->blend = 0.5f; B->width = 0.6f;
	tableSelect(&A->tb, A->table); tableSelect(&B->tb, B->table);
	LightLock_Init(&gMipsLock);
	LightLock_Init(&gVoiceLock);
	userLoad();
	seqDefault();
	presetLoad(gStyle, gPreset);   // start on PAD / SYRUP PAD
	LightEvent_Init(&gJobEv, RESET_ONESHOT);
	APT_SetAppCpuTimeLimit(80);
	{
		s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		gWorker = threadCreate(workerMain, NULL, 64 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, 1, false);
	}
	buildSub(); buildNoise();

	gAudioOK = R_SUCCEEDED(ndspInit());
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

	int page = 0, sel[NPAGES] = {0}, oct = 0, helpOn = 0, latch = 0, prevTouch = 0;
	int curKey = -1;
	int topDirty = 1, botDirty = 1, topAgain = 0, botAgain = 0;   // *Again: also refresh the other buffer next frame
	float repeatT = 0, lastPos[2] = {-1, -1}, lastWarp[2] = {-1, -1};
	u8 on[128], onPrev[128]; memset(onPrev, 0, sizeof(onPrev));
	u64 last = osGetTime();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		LightLock_Lock(&gVoiceLock);   // audio state below; released before drawing
		u64 now = osGetTime(); float dt = (now - last) / 1000.0f; last = now; if (dt > 0.1f) dt = 0.1f;
		sensorsUpdate(dt);

		if (helpOn && (kDown & ~(KEY_TOUCH | KEY_SELECT))) { helpOn = 0; topDirty = botDirty = 1; }
		if (kDown & KEY_SELECT) { helpOn = !helpOn; topDirty = botDirty = 1; }
		if (kDown & KEY_X) { page = (page + 1) % NPAGES; topDirty = 1; }
		if (kDown & KEY_Y) { page = (page + NPAGES - 1) % NPAGES; topDirty = 1; }
		if (kDown & KEY_A) { gChord = (gChord + 1) % 5; topDirty = 1; }
		if (kDown & KEY_B) { latch = !latch; if (!latch) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } topDirty = botDirty = 1; }
		if (kDown & KEY_L) { if (oct > -3) oct--; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; topDirty = botDirty = 1; }
		if (kDown & KEY_R) { if (oct < 3) oct++; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; topDirty = botDirty = 1; }

		// parameter editing
		int n = pageLen(page);
		if (kDown & KEY_DUP)   { sel[page] = (sel[page] + n - 1) % n; topDirty = 1; }
		if (kDown & KEY_DDOWN) { sel[page] = (sel[page] + 1) % n; topDirty = 1; }
		int dir = 0, fast = 0;
		if (kDown & (KEY_DLEFT | KEY_DRIGHT)) { dir = (kDown & KEY_DRIGHT) ? 1 : -1; repeatT = 0.35f; }
		else if (kHeld & (KEY_DLEFT | KEY_DRIGHT)) { repeatT -= dt; if (repeatT <= 0) { dir = (kHeld & KEY_DRIGHT) ? 1 : -1; repeatT = 0.05f; fast = 1; } }
		if (dir) {
			Param* p = &PAGES[page][sel[page]];
			if ((p->dirty & D_TILT) && dir > 0 && !fast) { tiltRecenter(); topDirty = 1; }
			else if ((p->dirty & (D_REST | D_CLEAR)) && dir > 0 && !fast) {
				if (p->dirty & D_REST) seqRest(); else seqClear();
				topDirty = 1;
			} else if ((p->dirty & (D_SAVE | D_INIT)) && dir > 0 && !fast) {
				if (p->dirty & D_SAVE) { patchCapture(&gUser[gSaveSlot]); userSave(); gStyle = NSTYLES - 1; gPreset = gSaveSlot; }
				else { SPatch ip; patchInit(&ip); patchApply(&ip); }
				curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; topDirty = 1;
			} else if (paramChange(p, dir, fast)) {
				if (p->dirty & D_STYLE) { gPreset = 0; presetLoad(gStyle, 0); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
				if (p->dirty & D_OUT) outputApply();
				if (p->dirty & D_THEME) botDirty = 1;
				if (p->dirty & (D_SCALE | D_SEQ)) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; botDirty = 1; if (p->p == &gSeqMode && gSeqMode == 2) gRecPos = 0; if (p->p == &gSeqMode && gSeqMode != 1) gSeqTr = 0; }
				if (p->dirty & D_PRESET) { presetLoad(gStyle, gPreset); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
				if (p->dirty & (D_TBLA | D_TBLB)) LightLock_Lock(&gMipsLock);
				if (p->dirty & D_TBLA) tableSelect(&A->tb, A->table);
				if (p->dirty & D_TBLB) tableSelect(&B->tb, B->table);
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
		float cx = sx;
		gStick = sy;

		// touch
		int base = 48 + 12 * oct, key = -1, touching = !!(kHeld & KEY_TOUCH);
		float vel = 0.8f;
		if (touching) {
			touchPosition t; hidTouchRead(&t);
			if (t.py < TAB_H) {
				if (!prevTouch) {
					int b = t.px * 6 / 320;
					if (b == 0) helpOn = !helpOn;
					if (b == 1) page = (page + NPAGES - 1) % NPAGES;
					if (b == 2) page = (page + 1) % NPAGES;
					if (b == 3 && oct > -3) { oct--; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
					if (b == 4 && oct < 3) { oct++; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
					if (b == 5) { latch = !latch; if (!latch) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } }
					topDirty = botDirty = 1;
				}
			} else if (t.py < XY_Y + XY_H) {
				float np = (t.px - 4) / 311.0f, nc = 1.0f - (t.py - XY_Y) / (float)(XY_H - 1);
				if (np < 0) np = 0;
				if (np > 1) np = 1;
				if (nc < 0) nc = 0;
				if (nc > 1) nc = 1;
				A->pos = np; gCutoff = nc; gXYmod = nc * 2 - 1;
				topDirty = botDirty = 1;
			} else if (t.py >= KB_Y) {
				key = keyAt(t.px, t.py, base);
				vel = 0.35f + 0.65f * (t.py - KB_Y) / (float)(240 - KB_Y);
			}
		}
		prevTouch = touching;
		if (key != curKey && !(latch && key < 0)) {
			int notes[4], n = key >= 0 ? chordNotes(key, notes) : 0;
			if (gSeqMode == 1) { if (key >= 0 && gSeqBase >= 0) { gSeqTr = key - gSeqBase; topDirty = 1; } }   // transpose the pattern
			else {
				if (gSeqMode == 2 && key >= 0) { seqRecord(notes, n, vel); topDirty = 1; }
				playSet(notes, n, vel);
			}
			curKey = key;
		}
		if (gBeatFlash) { gBeatFlash = 0; gVisBeat = 1; if (page == 7) topDirty = 1; }

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
			if (gWorker) { gJob = j; ASET(gJobBusy, 1); LightEvent_Signal(&gJobEv); }   // core 1
			else { LightLock_Lock(&gMipsLock); doRebuild(&j); LightLock_Unlock(&gMipsLock); ASET(gJobDone, 1); }
			gForceBuild = 0;
		}
		if (AGET(gJobDone)) { ASET(gJobDone, 0); topDirty = 1; }

		if (!gAudioClock) { arpTick(dt); seqTick(dt); }
		{	// runtime milestones for the crash guard (written once each)
			static int m1 = 0, m2 = 0, m3 = 0, m4 = 0;
			if (!m1) { m1 = 1; crashMark("RUNNING"); }
			if (!m2 && gBusSeen) { m2 = 1; crashMark("FX BUS RUNNING"); }
			if (!m3 && (gArpOnN || gSeqPlaying)) { m3 = 1; crashMark("ARP/SEQ RUNNING"); }
			if (!m4 && visActive(page)) { m4 = 1; crashMark("GPU VISUALS"); }
			static int lastMarkPage = -1;
			if (page != lastMarkPage) { lastMarkPage = page; char m[32]; snprintf(m, sizeof(m), "PAGE %s%s", PAGE_NAMES[page], visActive(page) ? " (GPU)" : ""); crashMark(m); }
		}
		if (gAudioOK) voicesUpdate(dt, cx);
		// effect-bus self check: if notes are sending but the DSP never delivers aux audio, fall back to taps
		if (gBusOK && !gBusFail && !gBusSeen && (busSend(0) > 0 || busSend(1) > 0)) {
			int any = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started && !gV[i].isEcho) any = 1;
			if (any) { gBusWait += dt; if (gBusWait > 1.5f) { gBusFail = 1; topDirty = 1; } }
		}
		LightLock_Unlock(&gVoiceLock);

		memset(on, 0, sizeof(on));
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4 && gV[i].note < 128) on[gV[i].note] = 1;
		if (memcmp(on, onPrev, sizeof(on))) { memcpy(onPrev, on, sizeof(on)); botDirty = 1; topDirty = 1; }

		gSelName = PAGES[page][sel[page]].name;
		if (!helpOn && visActive(page)) topDirty = 1;   // animated panel: redraw every frame
		if (!helpOn && page == 4 && newestVoice()) topDirty = 1;   // moving envelope dot
		if (!helpOn && page == 5) topDirty = 1;                   // moving LFO playheads
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
					else if (page == 4) { frame(4, 30, 196, 60); frame(4, 94, 196, 60); envLabels(); }
					else if (page == 5) { frame(4, 30, 196, 60); frame(4, 94, 196, 60); lfoLabels(); }
					else { frame(4, 30, 196, 125); text(8, 34, "A", 1, DIM); text(8, 96, "B", 1, DIM); }
					scanlines();
				}
			}
			topAgain = topDirty; topDirty = 0;
		}
		if (botDirty || botAgain) { drawBottom(base, on, helpOn, latch); botAgain = botDirty; botDirty = 0; }

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	if (gWorker) { ASET(gWorkerQuit, 1); LightEvent_Signal(&gJobEv); threadJoin(gWorker, U64_MAX); threadFree(gWorker); }
	if (gCsndOK) { for (int c = 0; c < NCS; c++) if (csUsable(c)) CSND_SetPlayState(c, 0); csndExecCmds(true); csndExit(); }
	if (gAudioOK) { for (int ch = 0; ch < NCH; ch++) ndspChnWaveBufClear(ch); ndspExit(); }
	if (!gSafeMode) sensorsExit();
	gpuExit();
	remove(CRASH_PATH);   // clean exit
	gfxExit();
	return 0;
}
