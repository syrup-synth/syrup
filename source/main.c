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
// Controls:
//   D-pad U/D select parameter, L/R change it (hold to repeat)   X / Y  next / previous page
//   Bottom tabs: HELP, < PAGE, PAGE >, LATCH   XY pad: X = OSC A WT POS, Y = filter cutoff
//   Keyboard: play (lower on a key = louder), slide to glide
//   A chord mode   B latch   L / R octave   SELECT help   START quit
//   Circle pad: left/right pitch bend, up/down = STICK mod source

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>

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
			for (int n = 0; n < CYC; n++) {
				float p = n / (float)CYC, s = 0;
				for (int k = 1; k <= nh; k++) if (amp[k] != 0) s += amp[k] * sinf(TWO_PI * k * p + (t == T_DIGITAL ? k * 1.3f : 0));
				fr[n] = s;
			}
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

static void tableSelect(Table* tb, int idx) {
	if (idx < T_BUILTIN) genBuiltin(tb, idx);
	else {
		char path[128]; snprintf(path, sizeof(path), "sdmc:/3ds/syrup/%s", gFiles[idx - T_BUILTIN]);
		if (loadWav(tb, path)) {
			char nm[64]; snprintf(nm, sizeof(nm), "%s", gFiles[idx - T_BUILTIN]);
			nm[strlen(nm) - 4] = 0;
			for (char* c = nm; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
			snprintf(tb->name, sizeof(tb->name), "%.22s", nm);
		} else { genBuiltin(tb, T_BASIC); snprintf(tb->name, sizeof(tb->name), "LOAD FAILED"); }
	}
	float pk = 0;
	for (int i = 0; i < tb->nFrames * CYC; i += 4) { float v = fabsf(tb->frames[i]); if (v > pk) pk = v; }
	tb->peak = pk > 1e-4f ? pk : 1.0f;
}

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
static const char* LFO_SHAPES[5] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "S&H"};
static const char* LFO_MODES[2] = {"FREE", "TRIG"};
enum { SRC_OFF, SRC_LFO1, SRC_LFO2, SRC_ENV2, SRC_VEL, SRC_STICK, SRC_XY, SRC_COUNT };
static const char* SRC_NAMES[SRC_COUNT] = {"OFF", "LFO 1", "LFO 2", "ENV 2", "VELOCITY", "STICK", "XY Y"};
enum { DST_APOS, DST_BPOS, DST_AWARP, DST_BWARP, DST_CUTOFF, DST_RES, DST_PITCH, DST_ALEV, DST_BLEV, DST_PAN, DST_COUNT };
static const char* DST_NAMES[DST_COUNT] = {"A WT POS", "B WT POS", "A WARP", "B WARP", "CUTOFF", "RES", "PITCH", "A LEVEL", "B LEVEL", "PAN"};
static const char* VOICE_MODES[3] = {"POLY", "MONO", "LEGATO"};
static const char* FX_NAMES[3] = {"OFF", "ECHO", "SPACE"};
static const char* ECHO_T_NAMES[3] = {"250MS", "375MS", "500MS"};
static const float ECHO_T[3] = {0.25f, 0.375f, 0.5f};
static const char* DRIVE_TYPES[5] = {"OFF", "TUBE", "HARD", "FOLD", "RECTIFY"};
static const char* CHORD_NAMES[5] = {"OFF", "MIN7", "MAJ7", "POWER", "SUS"};
static const int CHORDS[5][4] = {{0, -1, -1, -1}, {0, 3, 7, 10}, {0, 4, 7, 11}, {0, 7, 12, -1}, {0, 5, 7, 12}};

typedef struct { float a, h, d, s, r; } AHDSR;
typedef struct { int shape, mode; float rate; float ph, val, sh; } LFO;
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

// ---------------------------------------------------------------- voices on DSP channels
#define NVS 32
enum { R_A0, R_A1, R_A2, R_B0, R_B1, R_B2, R_SUB, R_NOISE, R_COUNT };
typedef struct {
	int active, started, note, stage, stage2, isEcho, nch;
	int ch[R_COUNT], role[R_COUNT], lev[R_COUNT];
	float env, env2, freq, cur, gain, pan, cutMul, startDelay, gate, held, vel, hold1, hold2;
	unsigned parent, id, age;
} SVoice;
static SVoice gV[NVS];
static int gChOwner[NCH];
static unsigned gAge = 0, gIdc = 0;
static ndspWaveBuf gWB[NCH];
static int gAudioOK = 0;
static float gStick = 0, gXYmod = 0, gChorusPh = 0;
static float gAutoGain = 1.0f;

static float mtof(float n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }
static int levelFor(float f) { for (int L = 0; L < NLEV; L++) if (f * LEVN[L] <= 2.0f * DSPRATE) return L; return NLEV - 1; }
static float oscTune(int o) { return powf(2.0f, gOsc[o].oct + gOsc[o].semi / 12.0f + gOsc[o].fine / 1200.0f); }

static void uniParams(const Osc* o, int u, float* det, float* pan, float* gain) {
	int n = o->uni;
	if (n == 1) { *det = 0; *pan = 0.5f; *gain = 1; return; }
	if (n == 2) { *det = (u == 0 ? -0.5f : 0.5f) * o->detune; *pan = 0.5f + (u == 0 ? -0.5f : 0.5f) * o->width; *gain = 0.71f; return; }
	if (u == 0) { *det = 0; *pan = 0.5f; *gain = 1.0f - 0.7f * o->blend; }
	else { *det = (u == 1 ? -1.0f : 1.0f) * o->detune; *pan = 0.5f + (u == 1 ? -0.5f : 0.5f) * o->width; *gain = 0.35f + 0.5f * o->blend; }
}

static void freeVoice(SVoice* v) {
	if (v->started) for (int i = 0; i < v->nch; i++) if (v->ch[i] >= 0) { ndspChnWaveBufClear(v->ch[i]); gChOwner[v->ch[i]] = -1; v->ch[i] = -1; }
	v->active = 0; v->started = 0;
}
static int freeChannels(void) { int n = 0; for (int c = 0; c < NCH; c++) if (gChOwner[c] < 0) n++; return n; }
static int stealFor(int n, int self) {
	while (freeChannels() < n) {
		int vi = -1; unsigned best = 0xffffffff;
		for (int pass = 0; pass < 3 && vi < 0; pass++)
			for (int i = 0; i < NVS; i++) {
				SVoice* v = &gV[i];
				if (i == self || !v->active || !v->started) continue;
				if (pass == 0 && !v->isEcho) continue;
				if (pass == 1 && v->stage != 4) continue;
				if (v->age < best) { best = v->age; vi = i; }
			}
		if (vi < 0) return 0;
		freeVoice(&gV[vi]);
	}
	return 1;
}

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
}

static const s16* roleBuf(int role, int lev, int* N) {
	if (role == R_NOISE) { *N = gNoiseLen; return gNoise; }
	Mips* m = role == R_SUB ? &gSubMips : &gOsc[role >= R_B0].mips;
	if (!(m->built & (1u << lev))) mipsBuildLevel(m, lev);
	*N = LEVN[lev];
	return m->lev[lev];
}

static void startVoice(int vi) {
	SVoice* v = &gV[vi];
	int roles[R_COUNT], nr = 0;
	if (v->isEcho) { if (gOsc[0].on) roles[nr++] = R_A0; else if (gOsc[1].on) roles[nr++] = R_B0; }
	else {
		for (int o = 0; o < 2; o++) if (gOsc[o].on) for (int u = 0; u < gOsc[o].uni; u++) roles[nr++] = (o ? R_B0 : R_A0) + u;
		if (gSubLevel > 0.01f) roles[nr++] = R_SUB;
		if (gNoiseLevel > 0.01f) roles[nr++] = R_NOISE;
	}
	if (nr == 0 || !stealFor(nr, vi)) { v->active = 0; return; }
	v->nch = nr;
	for (int i = 0; i < nr; i++) {
		int ch = -1; for (int c = 0; c < NCH; c++) if (gChOwner[c] < 0) { ch = c; break; }
		gChOwner[ch] = vi; v->ch[i] = ch; v->role[i] = roles[i];
		int role = roles[i];
		float f = v->freq;
		if (role <= R_A2) f *= oscTune(0);
		else if (role <= R_B2) f *= oscTune(1);
		else if (role == R_SUB) f *= powf(2.0f, (float)gSubOct);
		v->lev[i] = role == R_NOISE ? 0 : levelFor(f * 1.03f);
		int N; const s16* buf = roleBuf(role, v->lev[i], &N);
		float mix[12] = {0};
		ndspChnSetMix(ch, mix);
		ndspChnSetRate(ch, role == R_NOISE ? DSPRATE : f * N);
		filterSet(ch, 2000, 0);
		memset(&gWB[ch], 0, sizeof(ndspWaveBuf));
		gWB[ch].data_vaddr = buf;
		gWB[ch].nsamples = N;
		gWB[ch].looping = true;
		ndspChnWaveBufAdd(ch, &gWB[ch]);
	}
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
	v->gain = 1; v->cutMul = 1;
}

static void lfoTrig(void) { for (int i = 0; i < 2; i++) if (gLfo[i].mode == 1) gLfo[i].ph = 0; }

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
		return;
	}
	SVoice* v = newVoice(); initVoice(v);
	v->note = note; v->freq = mtof((float)note); v->cur = v->freq; v->vel = vel;
	lfoTrig();
	startVoice(v - gV);
	int ne = 0; float t[6], g[6], p[6], c[6];
	if (gFx == 1) { float d = ECHO_T[gEchoT]; for (int k = 0; k < 3; k++) { t[k] = d * (k + 1); g[k] = 0.6f * powf(0.55f, (float)k); p[k] = (k & 1) ? 0.45f : -0.45f; c[k] = powf(0.7f, (float)(k + 1)); } ne = 3; }
	if (gFx == 2) { static const float T[6] = {0.041f, 0.067f, 0.109f, 0.157f, 0.233f, 0.331f};
		for (int k = 0; k < 6; k++) { t[k] = T[k]; g[k] = 0.5f * powf(0.8f, (float)k); p[k] = (k & 1) ? 0.4f : -0.4f; c[k] = powf(0.82f, (float)(k + 1)); } ne = 6; }
	for (int k = 0; k < ne; k++) {
		SVoice* e = newVoice(); initVoice(e);
		e->note = note; e->freq = v->freq; e->cur = v->freq; e->vel = vel;
		e->gain = g[k]; e->pan = p[k]; e->cutMul = c[k]; e->startDelay = t[k]; e->isEcho = 1; e->parent = v->id;
	}
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

static void envStep(float* e, int* st, const AHDSR* E, float dt, float* hold) {
	switch (*st) {
	case 1: *e += dt / (E->a + 0.0005f); if (*e >= 1) { *e = 1; *st = E->h > 0 ? 5 : 2; *hold = 0; } break;
	case 5: *hold += dt; if (*hold >= E->h) *st = 2; break;
	case 2: *e -= dt * (1 - E->s) / (E->d + 0.0005f); if (*e <= E->s) { *e = E->s; *st = 3; } break;
	case 4: *e -= dt / (E->r + 0.0005f); if (*e < 0) *e = 0; break;
	}
}

static float lfoValue(LFO* l) {
	float p = l->ph;
	switch (l->shape) {
	case 0: return sinf(TWO_PI * p);
	case 1: return 1.0f - 4.0f * fabsf(p - 0.5f);
	case 2: return 2 * p - 1;
	case 3: return p < 0.5f ? 1 : -1;
	default: return l->sh;
	}
}

static float srcValue(int src, const SVoice* v) {
	switch (src) {
	case SRC_LFO1: return gLfo[0].val;
	case SRC_LFO2: return gLfo[1].val;
	case SRC_ENV2: return v ? v->env2 : 0;
	case SRC_VEL: return v ? v->vel : 0;
	case SRC_STICK: return gStick;
	case SRC_XY: return gXYmod;
	}
	return 0;
}

static float modSum(int dst, const SVoice* v) {
	float s = 0;
	for (int i = 0; i < 4; i++) if (gMod[i].src != SRC_OFF && gMod[i].dst == dst) s += srcValue(gMod[i].src, v) * gMod[i].amt;
	return s;
}

static void voicesUpdate(float dt, float bend) {
	for (int i = 0; i < 2; i++) {
		LFO* l = &gLfo[i];
		float prev = l->ph; l->ph += dt * l->rate; if (l->ph >= 1) l->ph -= floorf(l->ph);
		if (l->ph < prev) l->sh = hash01((int)(gAge + i * 77), (int)(l->rate * 100 + gIdc)) * 2 - 1;
		l->val = lfoValue(l);
	}
	gChorusPh += dt * 0.7f * TWO_PI;
	static float mixL[NCH], mixR[NCH];
	for (int c = 0; c < NCH; c++) mixL[c] = mixR[c] = -1;
	float gl = gGlide > 0 ? 1.0f - expf(-dt / (gGlide * 0.35f)) : 1.0f;

	for (int i = 0; i < NVS; i++) {
		SVoice* v = &gV[i];
		if (!v->active) continue;
		if (!v->started) { v->startDelay -= dt; if (v->startDelay <= 0) startVoice(i); if (!v->started) continue; }
		if (v->stage != 4) v->held += dt;
		if (v->gate >= 0 && v->stage != 4) { v->gate -= dt; if (v->gate <= 0) v->stage = v->stage2 = 4; }
		envStep(&v->env, &v->stage, &gEnv1, dt, &v->hold1);
		envStep(&v->env2, &v->stage2, &gEnv2, dt, &v->hold2);
		if (v->stage == 4 && v->env <= 0) { freeVoice(v); continue; }
		if (v->stage == 3 && gEnv1.s <= 0.001f) { freeVoice(v); continue; }
		v->cur += (v->freq - v->cur) * gl;

		float pitch = bend * gBendRange + modSum(DST_PITCH, v) * 12.0f;
		float pm = powf(2.0f, pitch / 12.0f);
		float cutoff = 60.0f * powf(15000.0f / 60.0f, gCutoff) * powf(2.0f, modSum(DST_CUTOFF, v) * 5.0f);
		cutoff *= powf(v->cur / 261.6f, gKeytrack) * v->cutMul;
		float res = gRes + modSum(DST_RES, v);
		if (res < 0) res = 0;
		if (res > 1) res = 1;
		float panMod = modSum(DST_PAN, v) * 0.5f;
		float aLev = gOsc[0].level * (1 + modSum(DST_ALEV, v)); if (aLev < 0) aLev = 0;
		float bLev = gOsc[1].level * (1 + modSum(DST_BLEV, v)); if (bLev < 0) bLev = 0;
		float vg = v->env * v->env * v->gain * gMaster * (0.35f + 0.65f * v->vel) * 1.5f;

		for (int k = 0; k < v->nch; k++) {
			int ch = v->ch[k], role = v->role[k]; if (ch < 0) continue;
			float det = 0, pan = 0.5f, ug = 1, lv = 1, f = v->cur;
			if (role <= R_B2) {
				int o = role >= R_B0, u = role - (o ? R_B0 : R_A0);
				if (!v->isEcho) uniParams(&gOsc[o], u, &det, &pan, &ug);
				lv = o ? bLev : aLev;
				f *= oscTune(o);
				ug /= sqrtf((float)gOsc[o].uni);
				if (gChorus > 0 && gOsc[o].uni > 1 && u > 0) det += gChorus * 12.0f * sinf(gChorusPh + u * 2.1f + o);
			} else if (role == R_SUB) { lv = gSubLevel; f *= powf(2.0f, (float)gSubOct); }
			else lv = gNoiseLevel * 0.5f;
			pan += v->pan + panMod;
			if (pan < 0) pan = 0;
			if (pan > 1) pan = 1;
			float g = vg * ug * lv;
			mixL[ch] = g * sqrtf(1 - pan) * 1.41f; mixR[ch] = g * sqrtf(pan) * 1.41f;
			if (role != R_NOISE) ndspChnSetRate(ch, f * pm * powf(2.0f, det / 1200.0f) * LEVN[v->lev[k]]);
			filterSet(ch, role == R_SUB ? cutoff * 0.6f + 300 : cutoff, res);
		}
	}
	// limiter across all channels: estimate the summed peak and scale everything down if needed
	float sl = 0, sr = 0, al = 0, ar = 0;
	for (int c = 0; c < NCH; c++) if (mixL[c] >= 0) { sl += mixL[c] * mixL[c]; sr += mixR[c] * mixR[c]; al += mixL[c]; ar += mixR[c]; }
	float estL = sqrtf(sl) * 0.9f + al * 0.5f, estR = sqrtf(sr) * 0.9f + ar * 0.5f;
	float est = estL > estR ? estL : estR;
	float target = est > 0.95f ? 0.95f / est : 1.0f;
	gAutoGain += (target - gAutoGain) * (target < gAutoGain ? 0.6f : 0.05f);
	for (int c = 0; c < NCH; c++) if (mixL[c] >= 0) {
		float mix[12] = {0};
		mix[0] = mixL[c] * gAutoGain; mix[1] = mixR[c] * gAutoGain;
		ndspChnSetMix(c, mix);
	}
}

// ---------------------------------------------------------------- parameter pages
enum { PT_F, PT_I, PT_E };
enum { D_A = 1, D_B = 2, D_SUB = 4, D_NOISE = 8, D_TBLA = 16, D_TBLB = 32 };
typedef struct { const char* name; int type; void* p; float mn, mx, step; const char** names; const char* unit; int dirty; } Param;

#define PF(n, ptr, mn, mx, st, unit, d) {n, PT_F, ptr, mn, mx, st, NULL, unit, d}
#define PI_(n, ptr, mn, mx, d) {n, PT_I, ptr, mn, mx, 1, NULL, "", d}
#define PE(n, ptr, cnt, names, d) {n, PT_E, ptr, 0, (cnt) - 1, 1, names, "", d}
#define PEND {NULL, 0, NULL, 0, 0, 0, NULL, NULL, 0}

static const char* ONOFF[2] = {"OFF", "ON"};
static int gTableCount = T_BUILTIN;
static const char* TABLE_NAME_DUMMY[1] = {""};

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
	PF("RES", &gRes, 0, 1, 0.01f, "%", 0), PF("KEYTRACK", &gKeytrack, 0, 1, 0.05f, "%", 0),
	PF("ENV1 ATK", &gEnv1.a, 0, 4, 0.005f, "S", 0), PF("ENV1 HOLD", &gEnv1.h, 0, 2, 0.01f, "S", 0),
	PF("ENV1 DEC", &gEnv1.d, 0.01f, 4, 0.02f, "S", 0), PF("ENV1 SUS", &gEnv1.s, 0, 1, 0.01f, "%", 0),
	PF("ENV1 REL", &gEnv1.r, 0.01f, 5, 0.02f, "S", 0),
	PF("ENV2 ATK", &gEnv2.a, 0, 4, 0.005f, "S", 0), PF("ENV2 DEC", &gEnv2.d, 0.01f, 4, 0.02f, "S", 0),
	PF("ENV2 SUS", &gEnv2.s, 0, 1, 0.01f, "%", 0), PF("ENV2 REL", &gEnv2.r, 0.01f, 5, 0.02f, "S", 0),
	PEND};
static Param P_MOD[] = {
	PE("LFO1 SHAPE", &gLfo[0].shape, 5, LFO_SHAPES, 0), PF("LFO1 RATE", &gLfo[0].rate, 0.05f, 20, 0.05f, "HZ", 0),
	PE("LFO1 MODE", &gLfo[0].mode, 2, LFO_MODES, 0),
	PE("LFO2 SHAPE", &gLfo[1].shape, 5, LFO_SHAPES, 0), PF("LFO2 RATE", &gLfo[1].rate, 0.05f, 20, 0.05f, "HZ", 0),
	PE("LFO2 MODE", &gLfo[1].mode, 2, LFO_MODES, 0),
	PE("1 SOURCE", &gMod[0].src, SRC_COUNT, SRC_NAMES, 0), PE("1 DEST", &gMod[0].dst, DST_COUNT, DST_NAMES, 0), PF("1 AMOUNT", &gMod[0].amt, -1, 1, 0.02f, "%", 0),
	PE("2 SOURCE", &gMod[1].src, SRC_COUNT, SRC_NAMES, 0), PE("2 DEST", &gMod[1].dst, DST_COUNT, DST_NAMES, 0), PF("2 AMOUNT", &gMod[1].amt, -1, 1, 0.02f, "%", 0),
	PE("3 SOURCE", &gMod[2].src, SRC_COUNT, SRC_NAMES, 0), PE("3 DEST", &gMod[2].dst, DST_COUNT, DST_NAMES, 0), PF("3 AMOUNT", &gMod[2].amt, -1, 1, 0.02f, "%", 0),
	PE("4 SOURCE", &gMod[3].src, SRC_COUNT, SRC_NAMES, 0), PE("4 DEST", &gMod[3].dst, DST_COUNT, DST_NAMES, 0), PF("4 AMOUNT", &gMod[3].amt, -1, 1, 0.02f, "%", 0),
	PEND};
static const char* BEND_NAMES[13] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
static Param P_VOICE[] = {
	PE("MODE", &gVoiceMode, 3, VOICE_MODES, 0), PF("GLIDE", &gGlide, 0, 1, 0.01f, "S", 0),
	PE("BEND RANGE", &gBendRange, 13, BEND_NAMES, 0), PE("CHORD", &gChord, 5, CHORD_NAMES, 0),
	PF("CHORUS", &gChorus, 0, 1, 0.02f, "%", 0), PE("FX", &gFx, 3, FX_NAMES, 0), PE("ECHO TIME", &gEchoT, 3, ECHO_T_NAMES, 0),
	PE("DRIVE", &gDrive, 5, DRIVE_TYPES, D_A | D_B), PF("DRIVE AMT", &gDriveAmt, 0, 1, 0.02f, "%", D_A | D_B),
	PF("MASTER", &gMaster, 0, 1.5f, 0.02f, "%", 0),
	PEND};

#define NPAGES 6
static Param* PAGES[NPAGES] = {P_OSCA, P_OSCB, P_SUBN, P_FILT, P_MOD, P_VOICE};
static const char* PAGE_NAMES[NPAGES] = {"OSC A", "OSC B", "SUB+NOISE", "FILT+ENV", "LFO+MOD", "VOICE+FX"};
static int pageLen(int pg) { int n = 0; while (PAGES[pg][n].name) n++; return n; }

static void paramFmt(const Param* p, char* out, int len) {
	if (p->type == PT_E) {
		int v = *(int*)p->p;
		if (p->names == TABLE_NAME_DUMMY) snprintf(out, len, "%.16s", p->p == &gOsc[0].table ? gOsc[0].tb.name : gOsc[1].tb.name);
		else snprintf(out, len, "%s", p->names[v]);
		return;
	}
	if (p->type == PT_I) { if (!strcmp(p->name, "UNISON")) snprintf(out, len, "%d", *(int*)p->p); else snprintf(out, len, "%+d", *(int*)p->p); return; }
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
	int nv = *v + dir;
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

#define BG    14, 9, 4
#define PANEL 30, 19, 7
#define AMB   255, 170, 40
#define AMB2  200, 110, 20
#define DIM   130, 85, 30
#define TXT   255, 225, 170

static void drawWave(const s16* lv, int N, int x0, int y0, int w, int h, int bright) {
	rect(x0, y0, w, h, PANEL);
	rect(x0, y0 + h / 2, w, 1, 70, 44, 16);
	int px0 = 0, py0 = 0;
	for (int i = 0; i <= w - 6; i++) {
		int n = i * N / (w - 6); if (n >= N) n = N - 1;
		int x = x0 + 3 + i, y = y0 + h / 2 - (int)(lv[n] / 32768.0f * (h / 2 - 4));
		if (i) { if (bright) { line(px0, py0, x, y, AMB); line(px0, py0 + 1, x, y + 1, AMB); } else line(px0, py0, x, y, 90, 55, 20); }
		px0 = x; py0 = y;
	}
}

static void drawStack(const Osc* o, float pos, int ox, int oy, int W, int H) {
	const Table* tb = &o->tb; int steps = 18;
	float ds = 1.0f / tb->peak;
	int curK = (int)(pos * (steps - 1) + 0.5f);
	for (int k = steps - 1; k >= 0; k--) {
		int fi = (int)((float)k / (steps - 1) * (tb->nFrames - 1));
		const float* fr = tb->frames + fi * CYC;
		int bx = ox + k * 3, by = oy - k * 4, cur = k == curK;
		u8 r = cur ? 255 : 80, g = cur ? 170 : 50, b = cur ? 40 : 18;
		int px0 = bx, py0 = by - (int)(fr[0] * ds * H * 0.5f);
		for (int n = 1; n <= 40; n++) {
			int x = bx + n * W / 40, y = by - (int)(fr[(n * CYC / 40) & (CYC - 1)] * ds * H * 0.5f);
			line(px0, py0, x, y, r, g, b); px0 = x; py0 = y;
		}
	}
}

static float gPosEff[2];
static void drawTop(int page, int sel, int oct, int latch) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 0, 400, 240, BG);
	text(4, 3, "SYRUP", 1, AMB);
	char pg[48]; snprintf(pg, sizeof(pg), "< %s >", PAGE_NAMES[page]);
	text(58, 3, pg, 1, 255, 200, 120);
	char pn[8]; snprintf(pn, sizeof(pn), "%d/%d", page + 1, NPAGES); text(170, 3, pn, 1, DIM);
	int focus = (page == 1) ? 1 : 0;
	if (page == 0 || page == 1) {
		Osc* o = &gOsc[focus];
		drawWave(o->mips.lev[2], LEVN[2], 4, 16, 196, 100, o->on);
		text(8, 19, o->on ? (focus ? "OSC B" : "OSC A") : (focus ? "OSC B OFF" : "OSC A OFF"), 1, DIM);
		drawStack(o, gPosEff[focus], 4, 218, 150, 20);
		text(4, 228, o->tb.name, 1, DIM);
	} else {
		drawWave(gOsc[0].mips.lev[2], LEVN[2], 4, 16, 196, 66, gOsc[0].on); text(8, 19, "A", 1, DIM);
		drawWave(gOsc[1].mips.lev[2], LEVN[2], 4, 86, 196, 66, gOsc[1].on); text(8, 89, "B", 1, DIM);
		char s[40];
		snprintf(s, sizeof(s), "%s  OCT %+d%s", VOICE_MODES[gVoiceMode], oct, latch ? "  LATCH" : "");
		text(6, 166, s, 1, TXT);
		snprintf(s, sizeof(s), "CHORD %s  FX %s", CHORD_NAMES[gChord], FX_NAMES[gFx]); text(6, 180, s, 1, TXT);
		int voices = 0, chans = 0; for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started) { voices++; chans += gV[i].nch; }
		snprintf(s, sizeof(s), "VOICES %d  DSP CH %d/24", voices, chans); text(6, 194, s, 1, DIM);
	}
	Param* P = PAGES[page]; int n = pageLen(page);
	int rows = 17, first = sel - rows + 1; if (first < 0) first = 0;
	int y = 18;
	for (int i = first; i < n && i < first + rows; i++, y += 13) {
		char v[24]; paramFmt(&P[i], v, sizeof(v));
		if (i == sel) rect(204, y - 2, 194, 12, 70, 44, 14);
		text(208, y, P[i].name, 1, i == sel ? 255 : 150, i == sel ? 220 : 100, i == sel ? 160 : 40);
		int w = strlen(v) * 8; text(396 - w, y, v, 1, TXT);
	}
	if (!gAudioOK) text(208, 230, "NO AUDIO: RUN DSP1", 1, 255, 80, 60);
}

static void drawHelp(void) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 0, 400, 240, BG);
	text(8, 6, "SYRUP HELP", 2, AMB); text(200, 10, "SELECT or HELP to close", 1, DIM);
	static const char* L[] = {
		"PAGES  OSC A, OSC B, SUB+NOISE, FILT+ENV,",
		"       LFO+MOD, VOICE+FX",
		"  X / Y ........ next / previous page",
		"  D-pad U/D .... pick a parameter",
		"  D-pad L/R .... change it (hold = fast)",
		"TOUCH",
		"  XY pad ....... X = OSC A WT POS, Y = cutoff",
		"  Keys ......... play; lower on key = louder",
		"  Tabs ......... HELP, < PAGE, PAGE >, LATCH",
		"BUTTONS",
		"  A ............ chord mode",
		"  B ............ latch (notes keep ringing)",
		"  L / R ........ octave",
		"  Circle pad ... L/R bend, U/D = STICK source",
		"  START ........ quit",
		"NOTES",
		"  4 mod slots: source > destination x amount",
		"  WT POS / WARP mod is shared by all notes",
		"  FM/AM/RM <OSC uses whole-octave ratios",
		"  Own wavetables: Serum .wav in /3ds/syrup/",
	};
	int y = 30;
	for (unsigned i = 0; i < sizeof(L) / sizeof(L[0]); i++, y += 10) {
		int hdr = L[i][0] != ' ';
		text(8, y, L[i], 1, 255, hdr ? 170 : 225, hdr ? 40 : 170);
	}
}

#define TAB_H 20
#define XY_Y  22
#define XY_H  94
#define KB_Y  118
static int keyAt(int x, int y, int base) {
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};
	float ww = 320.0f / 14;
	if (y < KB_Y + 70)
		for (int i = 0; i < 14; i++) {
			if (!BL[i % 7]) continue;
			int bx = (int)((i + 1) * ww - ww * 0.3f);
			if (x >= bx && x < bx + (int)(ww * 0.6f)) return base + (i / 7) * 12 + WN[i % 7] + 1;
		}
	int i = (int)(x / ww); if (i > 13) i = 13;
	return base + (i / 7) * 12 + WN[i % 7];
}

static void drawBottom(int base, const u8* on, int helpOn, int latch) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, KB_Y, BG);
	static const char* TABS[4] = {"HELP", "< PAGE", "PAGE >", "LATCH"};
	for (int i = 0; i < 4; i++) {
		int x = i * 80, lit = (i == 0 && helpOn) || (i == 3 && latch);
		rect(x + 2, 1, 76, TAB_H - 2, lit ? 90 : 40, lit ? 56 : 26, lit ? 16 : 9);
		int w = strlen(TABS[i]) * 8;
		text(x + 40 - w / 2, 7, TABS[i], 1, lit ? 255 : 200, lit ? 200 : 150, lit ? 120 : 90);
	}
	rect(4, XY_Y, 312, XY_H, 28, 18, 8);
	for (int gx = 1; gx < 8; gx++) rect(4 + gx * 39, XY_Y, 1, XY_H, 44, 28, 12);
	for (int gy = 1; gy < 4; gy++) rect(4, XY_Y + gy * XY_H / 4, 312, 1, 44, 28, 12);
	int cx = 4 + (int)(gOsc[0].pos * 311), cy = XY_Y + (int)((1 - gCutoff) * (XY_H - 1));
	rect(cx, XY_Y, 1, XY_H, AMB2); rect(4, cy, 312, 1, AMB2); rect(cx - 4, cy - 4, 9, 9, AMB);
	text(8, XY_Y + 4, "A WT POS >", 1, DIM); text(240, XY_Y + XY_H - 12, "^ CUTOFF", 1, DIM);
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};
	float ww = 320.0f / 14;
	rect(0, KB_Y, 320, 240 - KB_Y, 30, 20, 10);
	for (int i = 0; i < 14; i++) {
		int n = base + (i / 7) * 12 + WN[i % 7], x = (int)(i * ww);
		if (on[n]) rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, AMB);
		else rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, 240, 228, 205);
	}
	for (int i = 0; i < 14; i++) {
		if (!BL[i % 7]) continue;
		int n = base + (i / 7) * 12 + WN[i % 7] + 1, bx = (int)((i + 1) * ww - ww * 0.3f);
		if (on[n]) rect(bx, KB_Y + 2, (int)(ww * 0.6f), 68, AMB2);
		else rect(bx, KB_Y + 2, (int)(ww * 0.6f), 68, 26, 16, 8);
	}
	char c[8]; snprintf(c, sizeof(c), "C%d", base / 12 - 1); text(3, 228, c, 1, 120, 90, 50);
}

// ---------------------------------------------------------------- main
static u32 usedLevels(int o) {
	u32 m = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);   // display + common note ranges
	for (int i = 0; i < NVS; i++) if (gV[i].active && gV[i].started)
		for (int k = 0; k < gV[i].nch; k++) { int r = gV[i].role[k]; if ((o == 0 && r <= R_A2) || (o == 1 && r >= R_B0 && r <= R_B2)) m |= 1u << gV[i].lev[k]; }
	return m;
}

int main(void) {
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, false);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	fontGfx = consoleGetDefault()->font.gfx;
	fftInit();
	for (int o = 0; o < 2; o++) { gOsc[o].tb.frames = (float*)malloc(sizeof(float) * CYC * MAXFR); mipsInit(&gOsc[o].mips); }
	mipsInit(&gSubMips);
	gNoise = (s16*)linearAlloc(gNoiseLen * sizeof(s16));
	for (int c = 0; c < NCH; c++) gChOwner[c] = -1;
	scanTables();
	gTableCount = T_BUILTIN + gNFiles;

	// default patch: A = Basic Shapes, 3-voice unison; B = Harmonix an octave up (off)
	Osc* A = &gOsc[0]; Osc* B = &gOsc[1];
	A->on = 1; A->table = T_BASIC; A->pos = 0.6f; A->level = 0.8f; A->uni = 3; A->detune = 14; A->blend = 0.6f; A->width = 0.8f;
	B->on = 0; B->table = T_HARMONIX; B->pos = 0.3f; B->level = 0.5f; B->oct = 1; B->uni = 1; B->detune = 10; B->blend = 0.5f; B->width = 0.6f;
	tableSelect(&A->tb, A->table); tableSelect(&B->tb, B->table);
	buildSub(); buildNoise();

	gAudioOK = R_SUCCEEDED(ndspInit());
	if (gAudioOK) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspSetMasterVol(1.0f);
		for (int ch = 0; ch < NCH; ch++) { ndspChnReset(ch); ndspChnSetInterp(ch, NDSP_INTERP_POLYPHASE); ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16); }
	}

	int page = 0, sel[NPAGES] = {0}, oct = 0, helpOn = 0, latch = 0, prevTouch = 0;
	int held[4] = {-1, -1, -1, -1}, heldN = 0, curKey = -1;
	int topDirty = 1, botDirty = 1;
	float repeatT = 0, lastPos[2] = {-1, -1}, lastWarp[2] = {-1, -1};
	int forceBuild = 3;
	u8 on[128], onPrev[128]; memset(onPrev, 0, sizeof(onPrev));
	u64 last = osGetTime();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		u64 now = osGetTime(); float dt = (now - last) / 1000.0f; last = now; if (dt > 0.1f) dt = 0.1f;

		if (helpOn && (kDown & ~(KEY_TOUCH | KEY_SELECT))) { helpOn = 0; topDirty = botDirty = 1; }
		if (kDown & KEY_SELECT) { helpOn = !helpOn; topDirty = botDirty = 1; }
		if (kDown & KEY_X) { page = (page + 1) % NPAGES; topDirty = 1; }
		if (kDown & KEY_Y) { page = (page + NPAGES - 1) % NPAGES; topDirty = 1; }
		if (kDown & KEY_A) { gChord = (gChord + 1) % 5; topDirty = 1; }
		if (kDown & KEY_B) { latch = !latch; if (!latch) { allOff(); curKey = -1; heldN = 0; } topDirty = botDirty = 1; }
		if (kDown & KEY_L) { if (oct > -2) oct--; allOff(); curKey = -1; heldN = 0; topDirty = botDirty = 1; }
		if (kDown & KEY_R) { if (oct < 2) oct++; allOff(); curKey = -1; heldN = 0; topDirty = botDirty = 1; }

		// parameter editing
		int n = pageLen(page);
		if (kDown & KEY_DUP)   { sel[page] = (sel[page] + n - 1) % n; topDirty = 1; }
		if (kDown & KEY_DDOWN) { sel[page] = (sel[page] + 1) % n; topDirty = 1; }
		int dir = 0, fast = 0;
		if (kDown & (KEY_DLEFT | KEY_DRIGHT)) { dir = (kDown & KEY_DRIGHT) ? 1 : -1; repeatT = 0.35f; }
		else if (kHeld & (KEY_DLEFT | KEY_DRIGHT)) { repeatT -= dt; if (repeatT <= 0) { dir = (kHeld & KEY_DRIGHT) ? 1 : -1; repeatT = 0.05f; fast = 1; } }
		if (dir) {
			Param* p = &PAGES[page][sel[page]];
			if (paramChange(p, dir, fast)) {
				if (p->dirty & D_TBLA) tableSelect(&A->tb, A->table);
				if (p->dirty & D_TBLB) tableSelect(&B->tb, B->table);
				if (p->dirty & (D_A | D_TBLA)) forceBuild |= 1;
				if (p->dirty & (D_B | D_TBLB)) forceBuild |= 2;
				if (p->dirty & D_SUB) buildSub();
				if (p->dirty & D_NOISE) buildNoise();
				if (p->p == &gVoiceMode || p->p == &gFx || p->p == &A->on || p->p == &B->on || p->p == &A->uni || p->p == &B->uni) { allOff(); curKey = -1; heldN = 0; }
				topDirty = 1;
			}
		}

		circlePosition cp; hidCircleRead(&cp);
		float cx = cp.dx / 156.0f, cy = cp.dy / 156.0f;
		if (fabsf(cx) < 0.1f) cx = 0;
		if (fabsf(cy) < 0.1f) cy = 0;
		gStick = cy > 1 ? 1 : (cy < -1 ? -1 : cy);

		// touch
		int base = 48 + 12 * oct, key = -1, touching = !!(kHeld & KEY_TOUCH);
		float vel = 0.8f;
		if (touching) {
			touchPosition t; hidTouchRead(&t);
			if (t.py < TAB_H) {
				if (!prevTouch) {
					int b = t.px / 80;
					if (b == 0) helpOn = !helpOn;
					if (b == 1) page = (page + NPAGES - 1) % NPAGES;
					if (b == 2) page = (page + 1) % NPAGES;
					if (b == 3) { latch = !latch; if (!latch) { allOff(); curKey = -1; heldN = 0; } }
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
			for (int i = 0; i < heldN; i++) voiceOff(held[i]);
			heldN = 0;
			if (key >= 0) for (int i = 0; i < 4; i++) if (CHORDS[gChord][i] >= 0) { held[heldN++] = key + CHORDS[gChord][i]; voiceOn(key + CHORDS[gChord][i], vel); }
			curKey = key;
		}

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
		for (int o = 0; o < 2; o++) chg[o] = (forceBuild & (1 << o)) || fabsf(pe[o] - lastPos[o]) > 0.002f || fabsf(we[o] - lastWarp[o]) > 0.002f;
		if (chg[1] && A->warp >= W_FM) chg[0] = 1;
		if (chg[0] && B->warp >= W_FM) chg[1] = 1;
		if (chg[0] || chg[1]) {
			oscMorph(A, pe[0]); oscMorph(B, pe[1]);
			float rA = powf(2.0f, (float)(B->oct - A->oct)), rB = powf(2.0f, (float)(A->oct - B->oct));
			if (rA < 1) rA = 1;
			if (rB < 1) rB = 1;
			for (int o = 0; o < 2; o++) if (chg[o]) {
				oscWarp(&gOsc[o], we[o], &gOsc[1 - o], o ? rB : rA);
				mipsSet(&gOsc[o].mips, gOsc[o].cyc, usedLevels(o));
				lastPos[o] = pe[o]; lastWarp[o] = we[o]; gPosEff[o] = pe[o];
			}
			forceBuild = 0;
			topDirty = 1;
		}

		if (gAudioOK) voicesUpdate(dt, cx);

		memset(on, 0, sizeof(on));
		for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4 && gV[i].note < 128) on[gV[i].note] = 1;
		if (memcmp(on, onPrev, sizeof(on))) { memcpy(onPrev, on, sizeof(on)); botDirty = 1; topDirty = 1; }

		if (topDirty) { if (helpOn) drawHelp(); else drawTop(page, sel[page], oct, latch); topDirty = 0; }
		if (botDirty) { drawBottom(base, on, helpOn, latch); botDirty = 0; }

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	if (gAudioOK) { for (int ch = 0; ch < NCH; ch++) ndspChnWaveBufClear(ch); ndspExit(); }
	gfxExit();
	return 0;
}
