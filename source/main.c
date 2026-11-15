// SYRUP - Serum-style wavetable synth proof of concept for Nintendo 2DS / 3DS (homebrew, libctru)
//
// The DSP does the oscillators: every voice is a DSP channel looping one single-cycle waveform.
// The ARM builds that cycle ~60x a second: it morphs between wavetable frames (POSITION), applies
// a warp, FFTs it and writes band-limited copies (mip levels) so high notes don't alias. Pitch is
// the channel's playback rate, unison is extra detuned channels, and each channel's biquad
// low-pass is the filter. The ARM is left almost idle.
//
// Controls:
//   Bottom screen top half: XY pad (X = wavetable POSITION, Y = filter CUTOFF)
//   Bottom screen bottom half: 2-octave keyboard (slide to glide between notes)
//   D-pad L/R ..... wavetable            D-pad U/D .... warp amount
//   X ............. warp mode            A ............ unison 1 / 2 / 3
//   Y ............. envelope             B ............ LFO on position
//   L / R ......... octave down / up     SELECT ....... chord (off/min7/maj7/power/sus)
//   Circle pad .... left/right pitch bend, up vibrato, down filter resonance
//   START ......... quit
// Serum-format wavetables (.wav, 2048-sample frames) are loaded from sdmc:/3ds/syrup/

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
#define NV       8
#define NCH      24
#define DSPRATE  32728.0f

// ---------------------------------------------------------------- wavetables
static float* gFrames;      // nFrames x CYC
static int gNFrames = 64;
static char gTableName[40] = "";
static float gTablePeak = 1.0f;   // for display scaling

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

static void genBuiltin(int t) {
	gNFrames = 64;
	for (int f = 0; f < 64; f++) {
		float* fr = gFrames + f * CYC;
		float m = f / 63.0f;
		for (int n = 0; n < CYC; n++) {
			float p = n / (float)CYC, s = 0;
			switch (t) {
			case T_BASIC: {
				float sine = sinf(TWO_PI * p);
				float tri = 1.0f - 4.0f * fabsf(p - 0.5f); tri = -tri;
				float saw = 2.0f * p - 1.0f;
				float sq = p < 0.5f ? 1.0f : -1.0f;
				float seg = m * 3.0f; int si = (int)seg; float u = seg - si;
				float a[4] = {sine, tri, saw, sq};
				if (si >= 3) { si = 2; u = 1; }
				s = a[si] * (1 - u) + a[si + 1] * u;
				break; }
			case T_PWM: { float w = 0.5f - 0.47f * m; s = p < w ? 1.0f : -1.0f; break; }
			case T_SYNC: { float r = 1.0f + 7.0f * m; float q = p * r; q -= floorf(q); s = (2 * q - 1) * (1.0f - p * 0.3f); break; }
			case T_GROWL: { float I = 6.0f * m; s = sinf(TWO_PI * p + I * sinf(TWO_PI * 2 * p) + 0.5f * I * sinf(TWO_PI * 3 * p)); break; }
			default: break;
			}
			fr[n] = s;
		}
		if (t == T_HARMONIX || t == T_VOWELS || t == T_DIGITAL) {
			// additive frames
			float amp[257] = {0};
			int nh = 256;
			if (t == T_HARMONIX) { int cnt = 1 + f; for (int k = 1; k <= cnt; k++) amp[k] = 1.0f / k; }
			if (t == T_VOWELS) {
				static const float V[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
				float seg = m * 4.0f; int vi = (int)seg; float u = seg - vi; if (vi >= 4) { vi = 3; u = 1; }
				float F[3]; for (int j = 0; j < 3; j++) F[j] = V[vi][j] * (1 - u) + V[vi + 1][j] * u;
				for (int k = 1; k <= 64; k++) {
					float hz = k * 110.0f, a = 0;
					static const float G[3] = {1.0f, 0.6f, 0.35f}, BW[3] = {110, 160, 220};
					for (int j = 0; j < 3; j++) { float d = (hz - F[j]) / BW[j]; a += G[j] * expf(-d * d); }
					amp[k] = a * powf(k, -0.3f) + 0.02f / k;
				}
				nh = 64;
			}
			if (t == T_DIGITAL) {
				float seg = m * 7.0f; int a0 = (int)seg; float u = seg - a0;
				for (int k = 1; k <= 96; k++) {
					float h0 = hash01(k, a0), h1 = hash01(k, a0 + 1);
					float h = h0 * (1 - u) + h1 * u;
					amp[k] = (h > 0.55f ? h : 0.0f) / sqrtf((float)k);
				}
				amp[1] = 0.6f; nh = 96;
			}
			for (int n = 0; n < CYC; n++) {
				float p = n / (float)CYC, s = 0;
				for (int k = 1; k <= nh; k++) if (amp[k] != 0) s += amp[k] * sinf(TWO_PI * k * p + (t == T_DIGITAL ? k * 1.3f : 0));
				fr[n] = s;
			}
		}
	}
	snprintf(gTableName, sizeof(gTableName), "%s", BUILTIN_NAMES[t]);
}

static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }

// Serum wavetable: RIFF WAV (PCM16/24/32f), frame size from the 'clm ' chunk ("<!>2048 ..."), default 2048
static int loadWav(const char* path) {
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
				float* fr = gFrames + fi * CYC;
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
			gNFrames = nf; ok = 1;
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
		if (l > 4 && (!strcasecmp(e->d_name + l - 4, ".wav"))) snprintf(gFiles[gNFiles++], 64, "%s", e->d_name);
	}
	closedir(d);
}

static void calcPeak(void) {
	float pk = 0;
	for (int i = 0; i < gNFrames * CYC; i += 4) { float v = fabsf(gFrames[i]); if (v > pk) pk = v; }
	gTablePeak = pk > 1e-4f ? pk : 1.0f;
}

static void selectTable(int idx) {
	if (idx < T_BUILTIN) { genBuiltin(idx); calcPeak(); return; }
	char path[128]; snprintf(path, sizeof(path), "sdmc:/3ds/syrup/%s", gFiles[idx - T_BUILTIN]);
	if (loadWav(path)) {
		char nm[64]; snprintf(nm, sizeof(nm), "%s", gFiles[idx - T_BUILTIN]);
		nm[strlen(nm) - 4] = 0;
		for (char* c = nm; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
		snprintf(gTableName, sizeof(gTableName), "%.24s", nm);
	} else { genBuiltin(T_BASIC); snprintf(gTableName, sizeof(gTableName), "LOAD FAILED"); }
	calcPeak();
}

// ---------------------------------------------------------------- morph + warp + band-limit
enum { W_OFF, W_SYNC, W_BENDP, W_BENDM, W_PWM, W_MIRROR, W_FM, W_QUANT, W_COUNT };
static const char* WARP_NAMES[W_COUNT] = {"OFF", "SYNC", "BEND+", "BEND-", "PWM", "MIRROR", "FM", "QUANTIZE"};

static float gCycle[CYC];      // morphed + warped (pre band-limit), for display
typedef struct { float re, im; } cpx;
static cpx gSpec[CYC], gTmp[CYC];
static float twRe[CYC / 2], twIm[CYC / 2];
static const int LEVN[NLEV] = {2048, 1024, 512, 256, 128, 64, 32};
static s16* gLev[NLEV];
static float gNorm = 1.0f;

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

static inline float frameAt(const float* a, const float* b, float t, float p) {
	p -= floorf(p);
	float x = p * CYC; int i = (int)x; float u = x - i; int j = (i + 1) & (CYC - 1); i &= CYC - 1;
	float va = a[i] + (a[j] - a[i]) * u, vb = b[i] + (b[j] - b[i]) * u;
	return va + (vb - va) * t;
}

static void rebuild(float pos, int warp, float amt) {
	float fp = pos * (gNFrames - 1); int f0 = (int)fp; if (f0 > gNFrames - 2) f0 = gNFrames > 1 ? gNFrames - 2 : 0;
	float t = gNFrames > 1 ? fp - f0 : 0;
	const float* A = gFrames + f0 * CYC; const float* B = gFrames + (gNFrames > 1 ? f0 + 1 : f0) * CYC;
	for (int n = 0; n < CYC; n++) {
		float p = n / (float)CYC, q = p, s;
		switch (warp) {
		case W_SYNC: q = p * (1.0f + 7.0f * amt); break;
		case W_BENDP: q = powf(p, 1.0f / (1.0f + 3.0f * amt)); break;
		case W_BENDM: q = powf(p, 1.0f + 3.0f * amt); break;
		case W_PWM: { float w = 0.5f - 0.45f * amt; q = p < w ? p * 0.5f / w : 0.5f + (p - w) * 0.5f / (1.0f - w); break; }
		case W_MIRROR: q = p < 0.5f ? 2 * p : 2 - 2 * p; break;
		case W_FM: q = p + amt * 0.35f * sinf(TWO_PI * 2.0f * p); break;
		default: break;
		}
		s = frameAt(A, B, t, q);
		if (warp == W_MIRROR) s = s * amt + frameAt(A, B, t, p) * (1 - amt);
		if (warp == W_QUANT && amt > 0) { float lv = powf(2.0f, 7.0f - 6.0f * amt); s = roundf(s * lv) / lv; }
		gCycle[n] = s;
	}
	for (int n = 0; n < CYC; n++) { gSpec[n].re = gCycle[n]; gSpec[n].im = 0; }
	fft(gSpec, CYC, 0);
	gSpec[0].re = gSpec[0].im = 0;
	float peak = 0;
	for (int L = 0; L < NLEV; L++) {
		int N = LEVN[L], H = N / 4;
		memset(gTmp, 0, sizeof(cpx) * N);
		for (int k = 1; k <= H; k++) { gTmp[k] = gSpec[k]; gTmp[N - k].re = gSpec[k].re; gTmp[N - k].im = -gSpec[k].im; }
		fft(gTmp, N, 1);
		if (L == 0) {
			for (int n = 0; n < N; n++) { float v = fabsf(gTmp[n].re); if (v > peak) peak = v; }
			float target = peak > 1e-3f ? 0.92f * CYC / peak : 1.0f;
			static int first = 1;
			if (first) { gNorm = target; first = 0; }
			else gNorm += (target - gNorm) * 0.5f;   // smooth level changes while morphing
		}
		float sc = gNorm / CYC * 32767.0f;
		for (int n = 0; n < N; n++) {
			float v = gTmp[n].re * sc;
			if (v > 32767) v = 32767;
			if (v < -32768) v = -32768;
			gLev[L][n] = (s16)v;
		}
		DSP_FlushDataCache(gLev[L], N * sizeof(s16));
	}
}

// ---------------------------------------------------------------- voices on DSP channels
typedef struct { const char* name; float a, d, s, r; } Env;
static const Env ENVS[] = {{"PLUCK", 0.003f, 0.35f, 0.15f, 0.25f}, {"KEYS", 0.005f, 0.8f, 0.5f, 0.4f},
                           {"PAD", 0.40f, 0.5f, 0.85f, 1.0f}, {"LEAD", 0.01f, 0.2f, 0.9f, 0.15f}};
#define NENV 4

typedef struct {
	int active, note, stage, level, nu;   // stage: 1 att 2 dec 3 sus 4 rel
	float env, freq;
	unsigned age;
} SVoice;
static SVoice gV[NV];
static unsigned gAge = 0;
static ndspWaveBuf gWB[NCH];
static int gUnison = 3, gEnv = 0;
static float gCutoff = 0.7f, gReso = 0.0f;
static int gAudioOK = 0;
static const float UNI_DET[3] = {0.0f, -0.14f, 0.14f};   // semitones
static const float UNI_PAN[3] = {0.5f, 0.15f, 0.85f};

static float mtof(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

static int levelFor(float f) {
	for (int L = 0; L < NLEV; L++) if (f * LEVN[L] <= 2.0f * DSPRATE) return L;
	return NLEV - 1;
}

static float cutoffHz(void) { return 80.0f * powf(14000.0f / 80.0f, gCutoff); }

static void applyFilter(int ch) {
	float q = 0.707f + gReso * 5.0f;
	ndspChnIirBiquadSetParamsLowPassFilter(ch, cutoffHz(), q);
	ndspChnIirBiquadSetEnable(ch, true);
}

static void voiceOn(int note) {
	if (!gAudioOK) return;
	int vi = -1; unsigned oldest = 0xffffffff;
	for (int i = 0; i < NV; i++) if (!gV[i].active) { vi = i; break; }
	if (vi < 0) for (int i = 0; i < NV; i++) if (gV[i].age < oldest) { oldest = gV[i].age; vi = i; }
	SVoice* v = &gV[vi];
	for (int u = 0; u < 3; u++) ndspChnWaveBufClear(vi * 3 + u);
	v->active = 1; v->note = note; v->stage = 1; v->env = 0; v->age = gAge++;
	v->freq = mtof(note); v->level = levelFor(v->freq * 1.02f); v->nu = gUnison;
	int N = LEVN[v->level];
	for (int u = 0; u < v->nu; u++) {
		int ch = vi * 3 + u;
		float mix[12] = {0};
		ndspChnSetMix(ch, mix);
		ndspChnSetRate(ch, v->freq * N * powf(2.0f, UNI_DET[u] / 12.0f));
		applyFilter(ch);
		memset(&gWB[ch], 0, sizeof(ndspWaveBuf));
		gWB[ch].data_vaddr = gLev[v->level];
		gWB[ch].nsamples = N;
		gWB[ch].looping = true;
		ndspChnWaveBufAdd(ch, &gWB[ch]);
	}
}

static void voiceOff(int note) {
	for (int i = 0; i < NV; i++) if (gV[i].active && gV[i].note == note && gV[i].stage != 4) gV[i].stage = 4;
}
static void allOff(void) { for (int i = 0; i < NV; i++) if (gV[i].active) gV[i].stage = 4; }

// runs once per video frame (~60 Hz): envelopes, mix levels, pitch, filter
static void voicesUpdate(float dt, float bend, float vib, int filterDirty) {
	const Env* E = &ENVS[gEnv];
	static float vibPh = 0; vibPh += dt * 5.5f * TWO_PI;
	float pm = powf(2.0f, (bend + vib * 0.5f * sinf(vibPh)) / 12.0f);
	for (int i = 0; i < NV; i++) {
		SVoice* v = &gV[i];
		if (!v->active) continue;
		switch (v->stage) {
		case 1: v->env += dt / E->a; if (v->env >= 1) { v->env = 1; v->stage = 2; } break;
		case 2: v->env -= dt * (1 - E->s) / E->d; if (v->env <= E->s) { v->env = E->s; v->stage = 3; } break;
		case 4: v->env -= dt / E->r; if (v->env <= 0) { v->env = 0; v->active = 0; } break;
		}
		if (v->stage == 3 && E->s <= 0.001f) { v->active = 0; v->env = 0; }
		float g = v->env * v->env * 0.20f / sqrtf((float)v->nu);
		int N = LEVN[v->level];
		for (int u = 0; u < v->nu; u++) {
			int ch = i * 3 + u;
			if (!v->active) { ndspChnWaveBufClear(ch); continue; }
			float mix[12] = {0};
			mix[0] = g * (1.0f - UNI_PAN[u]) * 1.4f; mix[1] = g * UNI_PAN[u] * 1.4f;
			ndspChnSetMix(ch, mix);
			ndspChnSetRate(ch, v->freq * N * pm * powf(2.0f, UNI_DET[u] / 12.0f));
			if (filterDirty) applyFilter(ch);
		}
	}
}

// ---------------------------------------------------------------- drawing
static u8* fb; static int fbW; static u8* fontGfx;
static inline void px(int x, int y, u8 r, u8 g, u8 b) {
	if (x < 0 || x >= fbW || y < 0 || y >= 240) return;
	u8* p = fb + (x * 240 + (239 - y)) * 3; p[0] = b; p[1] = g; p[2] = r;
}
static void rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b) {
	for (int x = x0; x < x0 + w; x++) for (int y = y0; y < y0 + h; y++) px(x, y, r, g, b);
}
static void line(int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b) {
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
	for (;;) { px(x0, y0, r, g, b); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}
static void drawChar(int x, int y, char c, int sc, u8 r, u8 g, u8 b) {
	const u8* gl = fontGfx + ((u8)c) * 8;
	for (int row = 0; row < 8; row++) for (int col = 0; col < 8; col++)
		if (gl[row] & (0x80 >> col)) rect(x + col * sc, y + row * sc, sc, sc, r, g, b);
}
static void text(int x, int y, const char* s, int sc, u8 r, u8 g, u8 b) { for (; *s; s++, x += 8 * sc) drawChar(x, y, *s, sc, r, g, b); }

// syrup palette
#define BG   14, 9, 4
#define AMB  255, 170, 40
#define AMB2 200, 110, 20
#define DIM  110, 70, 25
#define TXT  255, 225, 170

static const char* CHORD_NAMES[5] = {"OFF", "MIN7", "MAJ7", "POWER", "SUS"};
static const int CHORDS[5][4] = {{0, -1, -1, -1}, {0, 3, 7, 10}, {0, 4, 7, 11}, {0, 7, 12, -1}, {0, 5, 7, 12}};

static void drawTop(float pos, int warp, float amt, int oct, int chord, int lfo, int tableIdx, int nTables) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 0, 400, 240, BG);
	text(8, 6, "SYRUP", 2, AMB);
	text(96, 10, "wavetable synth", 1, DIM);
	// 3D stacked wavetable view (Serum style)
	int ox = 14, oy = 200, W = 190, H = 40, steps = 24;
	float ds = 1.0f / gTablePeak;
	int curF = (int)(pos * (gNFrames - 1) + 0.5f);
	for (int k = steps - 1; k >= 0; k--) {
		int fi = (int)((float)k / (steps - 1) * (gNFrames - 1));
		const float* fr = gFrames + fi * CYC;
		int bx = ox + k * 3, by = oy - k * 5;
		int near = abs(fi - curF) <= (gNFrames / steps);
		u8 r = near ? 150 : 70, g = near ? 90 : 42, b = near ? 30 : 14;
		int px0 = bx, py0 = by - (int)(fr[0] * ds * H * 0.5f);
		for (int n = 1; n <= 64; n++) {
			int x = bx + n * W / 64;
			int y = by - (int)(fr[(n * CYC / 64) & (CYC - 1)] * ds * H * 0.5f);
			line(px0, py0, x, y, r, g, b); px0 = x; py0 = y;
		}
	}
	// current morphed+warped cycle, bright, at its depth
	{
		int k = (int)(pos * (steps - 1) + 0.5f);
		int bx = ox + k * 3, by = oy - k * 5;
		float pk = 0; for (int n = 0; n < CYC; n += 8) if (fabsf(gCycle[n]) > pk) pk = fabsf(gCycle[n]);
		float sc = pk > 1e-3f ? 1.0f / pk : 1.0f;
		int px0 = bx, py0 = by - (int)(gCycle[0] * sc * H * 0.5f);
		for (int n = 1; n <= 128; n++) {
			int x = bx + n * W / 128;
			int y = by - (int)(gCycle[(n * CYC / 128) & (CYC - 1)] * sc * H * 0.5f);
			line(px0, py0, x, y, AMB); line(px0, py0 + 1, x, y + 1, AMB2); px0 = x; py0 = y;
		}
	}
	// parameter panel
	char s[48]; int x = 300, y = 40;
	text(x, y, "TABLE", 1, DIM); snprintf(s, sizeof(s), "%d/%d", tableIdx + 1, nTables); text(x + 56, y, s, 1, DIM); y += 11;
	text(x - 40, y, gTableName, 1, TXT); y += 18;
	text(x, y, "POS", 1, DIM); snprintf(s, sizeof(s), "%3d%%%s", (int)(pos * 100 + 0.5f), lfo ? " LFO" : ""); text(x + 40, y, s, 1, TXT); y += 13;
	text(x, y, "WARP", 1, DIM); text(x + 40, y, WARP_NAMES[warp], 1, TXT); y += 11;
	snprintf(s, sizeof(s), "%3d%%", (int)(amt * 100 + 0.5f)); text(x + 40, y, s, 1, TXT); y += 13;
	text(x, y, "UNI", 1, DIM); snprintf(s, sizeof(s), "%d", gUnison); text(x + 40, y, s, 1, TXT); y += 13;
	text(x, y, "ENV", 1, DIM); text(x + 40, y, ENVS[gEnv].name, 1, TXT); y += 13;
	text(x, y, "CUT", 1, DIM); snprintf(s, sizeof(s), "%dHz", (int)cutoffHz()); text(x + 40, y, s, 1, TXT); y += 13;
	text(x, y, "RES", 1, DIM); snprintf(s, sizeof(s), "%d%%", (int)(gReso * 100)); text(x + 40, y, s, 1, TXT); y += 13;
	text(x, y, "OCT", 1, DIM); snprintf(s, sizeof(s), "%+d", oct); text(x + 40, y, s, 1, TXT); y += 13;
	text(x, y, "CHORD", 1, DIM); text(x + 48, y, CHORD_NAMES[chord], 1, TXT); y += 13;
	if (!gAudioOK) text(8, 226, "NO AUDIO - run DSP1 once (dspfirm.cdc)", 1, 255, 80, 60);
	else text(8, 226, "DSP: 24 ch wavetable osc + biquad", 1, DIM);
}

#define XY_H 112
#define KB_Y 118
static int keyAt(int x, int y, int base) {   // 2 octaves, 14 white keys
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	float ww = 320.0f / 14;
	if (y < KB_Y + 70) {   // black key zone
		static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};   // black key after white i?
		for (int i = 0; i < 14; i++) {
			if (!BL[i % 7]) continue;
			int bx = (int)((i + 1) * ww - ww * 0.3f);
			if (x >= bx && x < bx + (int)(ww * 0.6f)) return base + (i / 7) * 12 + WN[i % 7] + 1;
		}
	}
	int i = (int)(x / ww); if (i > 13) i = 13;
	return base + (i / 7) * 12 + WN[i % 7];
}

static void drawBottom(float pos, int base, const u8* on) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, KB_Y, BG);
	// XY pad
	rect(4, 4, 312, XY_H - 4, 28, 18, 8);
	for (int gx = 1; gx < 8; gx++) rect(4 + gx * 39, 4, 1, XY_H - 4, 44, 28, 12);
	for (int gy = 1; gy < 4; gy++) rect(4, 4 + gy * 27, 312, 1, 44, 28, 12);
	int cx = 4 + (int)(pos * 311), cy = 4 + (int)((1 - gCutoff) * (XY_H - 5));
	rect(cx, 4, 1, XY_H - 4, AMB2); rect(4, cy, 312, 1, AMB2);
	rect(cx - 4, cy - 4, 9, 9, AMB);
	text(8, 8, "POSITION >", 1, DIM); text(240, XY_H - 12, "^ CUTOFF", 1, DIM);
	// keyboard
	static const int WN[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int BL[7] = {1, 1, 0, 1, 1, 1, 0};
	float ww = 320.0f / 14;
	rect(0, KB_Y, 320, 240 - KB_Y, 30, 20, 10);
	for (int i = 0; i < 14; i++) {
		int n = base + (i / 7) * 12 + WN[i % 7];
		int x = (int)(i * ww);
		if (on[n]) rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, AMB);
		else rect(x + 1, KB_Y + 2, (int)ww - 2, 240 - KB_Y - 4, 240, 228, 205);
	}
	for (int i = 0; i < 14; i++) {
		if (!BL[i % 7]) continue;
		int n = base + (i / 7) * 12 + WN[i % 7] + 1;
		int bx = (int)((i + 1) * ww - ww * 0.3f);
		if (on[n]) rect(bx, KB_Y + 2, (int)(ww * 0.6f), 68, AMB2);
		else rect(bx, KB_Y + 2, (int)(ww * 0.6f), 68, 26, 16, 8);
	}
	char c[8]; snprintf(c, sizeof(c), "C%d", base / 12 - 1); text(3, 228, c, 1, 120, 90, 50);
}

// ---------------------------------------------------------------- main
int main(void) {
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, false);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	fontGfx = consoleGetDefault()->font.gfx;
	fftInit();
	gFrames = (float*)malloc(sizeof(float) * CYC * MAXFR);
	int levTotal = 0; for (int L = 0; L < NLEV; L++) levTotal += LEVN[L];
	s16* levMem = (s16*)linearAlloc(levTotal * sizeof(s16));
	for (int L = 0, o = 0; L < NLEV; L++) { gLev[L] = levMem + o; o += LEVN[L]; }
	memset(levMem, 0, levTotal * sizeof(s16));
	scanTables();

	gAudioOK = R_SUCCEEDED(ndspInit());
	if (gAudioOK) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		for (int ch = 0; ch < NCH; ch++) {
			ndspChnReset(ch);
			ndspChnSetInterp(ch, NDSP_INTERP_POLYPHASE);
			ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
		}
	}

	int tableIdx = T_BASIC, nTables = T_BUILTIN + gNFiles;
	selectTable(tableIdx);
	float pos = 0.35f, amt = 0.0f; int warp = W_OFF;
	int oct = 0, chord = 0, lfo = 0;
	float lfoPh = 0;
	int held[4] = {-1, -1, -1, -1}, heldN = 0, curKey = -1;
	int topDirty = 1, botDirty = 1, rebuildDirty = 1, filterDirty = 1;
	u8 on[128], onPrev[128]; memset(onPrev, 0, sizeof(onPrev));
	u64 last = osGetTime();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		u64 now = osGetTime(); float dt = (now - last) / 1000.0f; last = now; if (dt > 0.1f) dt = 0.1f;

		if (kDown & KEY_DRIGHT) { tableIdx = (tableIdx + 1) % nTables; selectTable(tableIdx); rebuildDirty = topDirty = 1; }
		if (kDown & KEY_DLEFT)  { tableIdx = (tableIdx + nTables - 1) % nTables; selectTable(tableIdx); rebuildDirty = topDirty = 1; }
		if (kHeld & KEY_DUP)    { amt += dt * 0.6f; if (amt > 1) amt = 1; rebuildDirty = topDirty = 1; }
		if (kHeld & KEY_DDOWN)  { amt -= dt * 0.6f; if (amt < 0) amt = 0; rebuildDirty = topDirty = 1; }
		if (kDown & KEY_X) { warp = (warp + 1) % W_COUNT; if (warp != W_OFF && amt < 0.05f) amt = 0.5f; rebuildDirty = topDirty = 1; }
		if (kDown & KEY_A) { gUnison = gUnison % 3 + 1; topDirty = 1; }
		if (kDown & KEY_Y) { gEnv = (gEnv + 1) % NENV; topDirty = 1; }
		if (kDown & KEY_B) { lfo = !lfo; topDirty = 1; }
		if (kDown & KEY_L) { if (oct > -2) oct--; allOff(); topDirty = botDirty = 1; }
		if (kDown & KEY_R) { if (oct < 2) oct++; allOff(); topDirty = botDirty = 1; }
		if (kDown & KEY_SELECT) { chord = (chord + 1) % 5; topDirty = 1; }

		circlePosition cp; hidCircleRead(&cp);
		float cx = cp.dx / 156.0f, cy = cp.dy / 156.0f;
		if (fabsf(cx) < 0.1f) cx = 0;
		if (fabsf(cy) < 0.1f) cy = 0;
		float bend = cx * 2.0f, vib = cy > 0 ? cy : 0;
		float reso = cy < 0 ? -cy : 0;
		if (fabsf(reso - gReso) > 0.02f) { gReso = reso; filterDirty = topDirty = 1; }

		// touch
		int base = 48 + 12 * oct;
		int key = -1;
		if (kHeld & KEY_TOUCH) {
			touchPosition t; hidTouchRead(&t);
			if (t.py < XY_H) {
				float np = (t.px - 4) / 311.0f, nc = 1.0f - (t.py - 4) / (float)(XY_H - 5);
				if (np < 0) np = 0;
				if (np > 1) np = 1;
				if (nc < 0) nc = 0;
				if (nc > 1) nc = 1;
				if (np != pos) { pos = np; rebuildDirty = 1; }
				if (nc != gCutoff) { gCutoff = nc; filterDirty = 1; }
				topDirty = botDirty = 1;
			} else if (t.py >= KB_Y) key = keyAt(t.px, t.py, base);
		}
		if (key != curKey) {
			for (int i = 0; i < heldN; i++) voiceOff(held[i]);
			heldN = 0;
			if (key >= 0) for (int i = 0; i < 4; i++) if (CHORDS[chord][i] >= 0) { held[heldN++] = key + CHORDS[chord][i]; voiceOn(key + CHORDS[chord][i]); }
			curKey = key;
		}

		float p = pos;
		if (lfo) { lfoPh += dt * 0.25f * TWO_PI; p = pos + 0.35f * sinf(lfoPh); if (p < 0) p = 0; if (p > 1) p = 1; rebuildDirty = topDirty = 1; }
		if (rebuildDirty) { rebuild(p, warp, amt); rebuildDirty = 0; }
		if (gAudioOK) voicesUpdate(dt, bend, vib, filterDirty);
		filterDirty = 0;

		memset(on, 0, sizeof(on));
		for (int i = 0; i < NV; i++) if (gV[i].active && gV[i].stage != 4 && gV[i].note < 128) on[gV[i].note] = 1;
		if (memcmp(on, onPrev, sizeof(on))) { memcpy(onPrev, on, sizeof(on)); botDirty = 1; }

		if (topDirty) { drawTop(p, warp, amt, oct, chord, lfo, tableIdx, nTables); topDirty = 0; }
		if (botDirty) { drawBottom(pos, base, on); botDirty = 0; }

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	if (gAudioOK) { for (int ch = 0; ch < NCH; ch++) ndspChnWaveBufClear(ch); ndspExit(); }
	linearFree(levMem); free(gFrames);
	gfxExit();
	return 0;
}
