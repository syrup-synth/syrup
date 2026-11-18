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
// chip quirks: DSP GRIT (the DSP's interpolation off), FILTER SCREAM (its biquad pushed past stable), MIX OVERDRIVE (its
// channel volumes past full, clipped at the 16-bit ceiling), ECHO PITCH (the chip echo replayed at another rate than it records)
static int gQGrit = 0, gCsEchoPitch = 0; static float gQScream = 0, gQMixDrive = 0; static const char* GRIT_NAMES[3] = {"OFF", "LINEAR", "RAW"};
static float gChipLevel, gCsEcho, gCsEchoFb, gOrbit, gOrbitRate, gGrainSize, gGrainScan, gGrainSpray, gGrainPitch, gGrainLevel; static int gChipDuty, gChipOct, gCsEchoT, gGrains, gGrainSrc; static const char* GRAIN_MODES[3]; static const char* GRAIN_SRCS[3];
static const char* DUTY_NAMES[8]; static const char* CSECHO_T_NAMES[4];
static u8 gMot[4][64]; static int gMotTgt[4], gMotOn[4], gMotRec, gMotBars; static float gMotPh;
static int gCamVid, gMidiNet, gAddEdit, gOfType; static float gOfCut, gOfRes, gOfDrive, gOfEnv; static const char* OFT_NAMES[14]; static float gWtScan; static const char* VID_NAMES[3];
static void chipSet(float lv);
static void grainsChanged(void);
static const char* KIT_NAMES[4]; static const char* KICKP_NAMES[9]; static const char* SNAREP_NAMES[8]; static const char* HATP_NAMES[8]; static int gEuHits[3], gEuRot[3], gEuLen; static int gDrumRec; static const char* DREC_NAMES[5];
#define NVISM 11
static const char* VISM_NAMES[NVISM] = {"AUTO", "WAVETABLE", "SCOPES", "ORBIT", "SPECTRUM", "VOXELS", "VOXEL TABLE", "TUNNEL", "KALEIDO", "SYNTHWAVE", "RIPPLES"};
static const char* AB_NAMES[2] = {"OSC A", "OSC B"};
static int gVisMode = 0, gVisOsc = 0, gVisOverlay = 1;
static float gVisBeat = 0;
static int gVisTrails = 0, gVisParts = 1, gVisBurst = 0;
#define NTHEMES 7
static const char* THEME_NAMES[NTHEMES] = {"ORIGINAL", "PIP GREEN", "DOS BLUE", "AMBER EGA", "MAGENTA", "STICKER", "IMPULSE"};
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
	FILE* nc = fopen("sdmc:/3ds/syrup/nogpucopy", "r"); if (nc) { gGpuCopy = 0; fclose(nc); }
	FILE* f = fopen(CRASH_PATH, "r");
	if (!f) return;
	if (!fgets(gCrashStage, sizeof(gCrashStage), f)) snprintf(gCrashStage, sizeof(gCrashStage), "START");
	if (!strncmp(gCrashStage, "GPU COPY", 8) && strncmp(gCrashStage, "GPU COPY OK", 11)) { FILE* n = fopen("sdmc:/3ds/syrup/nogpucopy", "w"); if (n) fclose(n); }   // hung in the GPU copy: CPU copies from now on
	fclose(f);
	gSafeMode = 1;
}        // set while the audio thread runs the arp/seq clock (it holds gMipsLock)
static LightLock gMipsLock;   // guards the shared FFT buffers (worker on core 1 vs main core)
static Thread gWorker = NULL;
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
enum { SRC_OFF, SRC_LFO1, SRC_LFO2, SRC_ENV2, SRC_VEL, SRC_STICK, SRC_XY, SRC_TILTX, SRC_TILTY, SRC_MIC, SRC_TWIST, SRC_SHAKE, SRC_CAML, SRC_CAMC, SRC_M1, SRC_M2, SRC_M3, SRC_M4, SRC_CHAOSX, SRC_CHAOSY, SRC_RAND, SRC_NOTE, SRC_LFO3, SRC_LFO4, SRC_COUNT };
static const char* SRC_NAMES[SRC_COUNT] = {"OFF", "LFO 1", "LFO 2", "ENV 2", "VELOCITY", "STICK", "XY Y", "TILT L/R", "TILT F/B", "BREATH", "TWIST", "SHAKE", "CAM LIGHT", "CAM COLOR", "MACRO 1", "MACRO 2", "MACRO 3", "MACRO 4", "CHAOS X", "CHAOS Y", "RANDOM", "NOTE", "LFO 3", "LFO 4"};
// LFO 3 and 4 are drawn: 64 points you paint with the stylus on the MOD page
static float gDl[2][64]; static float gDlRate[2] = {1.0f, 0.5f}, gDlPh[2], gDlVal[2]; static int gDlSync[2] = {0, 0};
static float gChaosX = 0, gChaosY = 0, gChaosRate = 0.3f;   // a Lorenz attractor, slowly wandering, never repeating
static float gMacro[4] = {0, 0, 0, 0};
#define NMOD 8
static int gModCurve[NMOD];
static const char* CURVE_NAMES[4] = {"LINEAR", "EXP", "LOG", "S-CURVE"};
static float gTiltX, gTiltY, gMicEnv, gTwist, gShake, gCamLight, gCamColor;
static int gHeadphones = 0;
enum { DST_APOS, DST_BPOS, DST_AWARP, DST_BWARP, DST_CUTOFF, DST_RES, DST_PITCH, DST_ALEV, DST_BLEV, DST_PAN, DST_FXP, DST_AMP, DST_OFCUT, DST_COUNT };
static const char* DST_NAMES[DST_COUNT] = {"A WT POS", "B WT POS", "A WARP", "B WARP", "CUTOFF", "RES", "PITCH", "A LEVEL", "B LEVEL", "PAN", "FX COLOR", "VOLUME", "OUT CUTOFF"};   // VOLUME: when routed, the source IS the loudness (breath control)
static const char* VOICE_MODES[3] = {"POLY", "MONO", "LEGATO"};
static const char* FX_NAMES[4] = {"OFF", "ECHO", "SPACE", "REVERB"};
static const char* ECHO_T_NAMES[3] = {"250MS", "375MS", "500MS"};
static const float ECHO_T[3] = {0.25f, 0.375f, 0.5f};
static const char* DRIVE_TYPES[15] = {"OFF", "TANH", "HARD CLIP", "SINE FOLD", "RECTIFY", "TUBE", "DIODE", "ASYM", "LIN FOLD", "ZERO SQUARE", "HALF RECT", "BITCRUSH", "CUBIC", "X-SHAPE", "OVERDRIVE"};
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
	else { if (gCsndOK) CSND_SetPlayState(ch, 0); gCsOwner[ch] = -1; }
}

static void chipStop(SVoice* v);
static void freeVoice(SVoice* v) {
	chipStop(v);
	if (v->started) for (int i = 0; i < v->nch; i++) if (v->ch[i] >= 0) { stopCh(v->bk[i], v->ch[i]); v->ch[i] = -1; }
	v->active = 0; v->started = 0;
}

static int dspUsable(int c) { return !(gNoiseOn && c == NOISE_CH) && !(gGrains && (c == 21 || c == 22)); }
static int loopReserved(int c);
static int drumReserved(int c);
static int chipReserved(int c);
static int echoReserved(int c);
static int csUsable(int c) { return gCsndOK && (csndChannels & BIT(c)) && !loopReserved(c) && !drumReserved(c) && !chipReserved(c) && !echoReserved(c); }
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
static u8 gScreamFlush[24];   // CONTAINMENT: one tick of a zero filter clears a voice's stale self-oscillation before its next note
static void filterSet(int ch, float hz, float res) {
	if (hz < 40) hz = 40;
	if (hz > 15000) hz = 15000;
	float q = 0.707f + res * 9.0f;
	if (gQScream > 0.001f && gFiltType == 0 && ch != NOISE_CH) {
		// FILTER SCREAM: poles at radius r around the cutoff. Past r = 1 the DSP's fixed-point filter rings by itself and
		// saturates: a screaming self-oscillation the stock filter types never allow.
		if (ch < 24 && gScreamFlush[ch]) {   // b = a = 0: output 0 and both state words 0 after one sample
			gScreamFlush[ch] = 0;
			ndspChnIirBiquadSetParamsCustomFilter(ch, 1.0f, 0, 0, 0, 0, 0); ndspChnIirBiquadSetEnable(ch, true); ndspChnIirMonoSetEnable(ch, false);
			return;
		}
		float w = TWO_PI * hz / DSPRATE, cw = cosf(w), r = 0.985f + gQScream * 0.03f + res * 0.004f;
		float rmax = 0.9995f / (cw > 0.5f ? cw : 0.5f);   // ceiling: keep a1 inside the DSP's Q14 range (past it the coefficient wraps and the filter latches)
		if (r > rmax) r = rmax;
		float a1 = -2.0f * r * cw, a2 = r * r;
		float b0 = (1.0f + a1 + a2) * 0.6f + 0.002f;
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
	if (!(m->built & (1u << v->lev[i]))) mipsBuildLevel(m, v->lev[i]);
	if (!gInAudioTick) LightLock_Unlock(&gMipsLock);
	int N = LEVN[v->lev[i]];
	if (bk == BK_DSP) {
		float mix[12] = {0};
		ndspChnSetMix(ch, mix);
		ndspChnSetRate(ch, f * N);
		ndspChnSetInterp(ch, gQGrit == 2 ? NDSP_INTERP_NONE : gQGrit == 1 ? NDSP_INTERP_LINEAR : NDSP_INTERP_POLYPHASE);
		if (gQScream > 0.001f && ch < 24) gScreamFlush[ch] = 1;
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
	v->note = note; v->freq = mtof((float)note); v->cur = v->freq; v->vel = vel; v->rnd = (rand() % 2001) / 1000.0f - 1;
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
	}
	return 0;
}

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
		v->cur += (v->freq - v->cur) * (v->slide ? 1.0f - expf(-dt / 0.03f) : gl);

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
		if (v->chipCh >= 0) {   // CHIP layer: hardware square, pitch and level from the same voice
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
		ndspChnSetMix(c, mix);
	}
	int any = 0;
	for (int c = 0; c < NCS; c++) if (cL[c] >= 0) {
		float l = cL[c] * gAutoGain, r = cR[c] * gAutoGain;
		if (l > 1) l = 1;
		if (r > 1) r = 1;
		u32 vol = (u32)(l * 0x8000) | ((u32)(r * 0x8000) << 16);
		CSND_SetVol(c, vol, csSend(l, r)); any = 1;
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
static s32 gFxIn[2][160], gFxOut[2][160], gFxZ[2][160];
static volatile u64 gCore1Busy;
static void fxMain(void* a) {
	(void)a;
	for (;;) {
		LightEvent_Wait(&gFxGo);
		if (gFxQuit) break;
		u64 t0 = svcGetSystemTick();
		void* smp[4] = {gFxIn[0], gFxIn[1], gFxZ[0], gFxZ[1]};
		auxMaster(NULL, gFxN, smp);
		memcpy(gFxOut[0], gFxIn[0], gFxN * 4); memcpy(gFxOut[1], gFxIn[1], gFxN * 4);
		gCore1Busy += svcGetSystemTick() - t0;
		LightEvent_Signal(&gFxDone);
	}
}
static void fxCore2Start(void) {
	if (gFxThread) return;
	LightEvent_Init(&gFxGo, RESET_ONESHOT); LightEvent_Init(&gFxDone, RESET_ONESHOT);
	LightEvent_Signal(&gFxDone);   // "the last frame is done" (silence) so the first hand-over doesn't wait
	memset(gFxOut, 0, sizeof(gFxOut));
	gFxThread = threadCreate(fxMain, NULL, 0x4000, 0x18, 1, false);
}
static void fxCore2Stop(void) {
	if (!gFxThread) return;
	gFxQuit = 1; LightEvent_Signal(&gFxGo); threadJoin(gFxThread, 1000000000ULL); threadFree(gFxThread); gFxThread = NULL; gFxQuit = 0;
}
static void aux0(void* d, int n, void* s[4]) {
	if (!gMasterFX) { auxReverb(d, n, s); return; }
	if (!gFxCore2) gFxC2Fail = 0;   // switching it off and on again retries core 2
	if (!gFxCore2 || !gFxThread || gFxC2Fail || n > 160) { auxMaster(d, n, s); return; }
	if (LightEvent_WaitTimeout(&gFxDone, 4000000LL) != 0) {   // core 2 missed the frame: run it here from now on
		gFxC2Fail = 1; auxMaster(d, n, s); return;
	}
	s32* L = (s32*)s[0]; s32* R = (s32*)s[1];
	int prevN = gFxN;
	memcpy(gFxIn[0], L, n * 4); memcpy(gFxIn[1], R, n * 4);
	if (prevN == n) { memcpy(L, gFxOut[0], n * 4); memcpy(R, gFxOut[1], n * 4); }
	else { memset(L, 0, n * 4); memset(R, 0, n * 4); }
	memset(s[2], 0, n * 4); memset(s[3], 0, n * 4);
	gFxN = n;
	LightEvent_Signal(&gFxGo);
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
	if (!busActive() || (gMasterFX && bus == 1)) return 0;
	if (bus == 0 && gFx == 3) return gFxMix * 1.4f;
	if (bus == 1 && gFx == 1) return gFxMix * 1.2f;
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
static const char* PUMP_NAMES[3] = {"1/2", "1/4", "1/8"};
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
	for (int i = 0; i < n; i++) {
		float x[2] = {L[i] / 32768.0f, R[i] / 32768.0f};
		for (int c = 0; c < 2; c++) {
			float in = x[c] * drv, y = in; float* s4 = gOfS[c];
			switch (gOfType) {
			case 1: case 2: case 5: {   // ladder: four one-pole stages with saturated resonance feedback
				float k = gOfType == 5 ? res * 2.6f : res * 4.1f;
				float u = ftanh(in - k * (gOfType == 2 ? s4[1] : s4[3]) + (gOfType == 5 ? 0 : 0.0f));
				s4[0] += g * (u - s4[0]); s4[1] += g * (ftanh(s4[0]) - s4[1]); s4[2] += g * (s4[1] - s4[2]); s4[3] += g * (s4[2] - s4[3]);
				y = (gOfType == 2 ? s4[1] : s4[3]) * comp;
				if (gOfType == 5) y = (s4[3] * 0.85f + s4[2] * 0.15f) * (1 + res);   // creamy: softer knee, rounder peak
				break; }
			case 3: {   // acid: three stages, asymmetric squelch
				float u = ftanh(in - res * 3.3f * s4[2]) + 0.02f * s4[2] * s4[2];
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
static void auxMaster(void* data, int n, void* samples[4]) {
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
	float dSmp = dSec * DSPRATE, dMix = (gDlyMix + (gFx == 1 ? gFxMix : 0)) * 1.2f, rMix = (gRevMix + (gFx == 3 ? gFxMix : 0)) * 1.4f;
	float fb, damp; revParams(&fb, &damp); if (gFreeze) { fb = 1.0f; damp = 0; }
	int bits = gLofiBits ? 14 - gLofiBits * 2 : 0, hold = 1 + (int)(gLofiRate * 15);
	float pumpRate = gPumpRate == 0 ? 0.5f : (gPumpRate == 1 ? 1.0f : 2.0f), beatInc = gBPM / 60.0f / DSPRATE;
	float combHz = 60 * powf(2, color * 6), combD = DSPRATE / combHz;
	for (int s = 0; s < n; s++) {
		if (L[s] | R[s]) seen = 1;
		float x[2] = {L[s] / 32768.0f, R[s] / 32768.0f};
		// lofi
		if (bits || hold > 1) {
			if (gLofiCnt++ % hold == 0) for (int c = 0; c < 2; c++) gLofiHold[c] = bits ? roundf(x[c] * (1 << (bits - 1))) / (1 << (bits - 1)) : x[c];
			x[0] = gLofiHold[0]; x[1] = gLofiHold[1];
		}
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
		// EQ
		if (gEqLow != 0 || gEqMid != 0 || gEqHigh != 0) for (int c = 0; c < 2; c++) x[c] = bq(&gEq[2], c, bq(&gEq[1], c, bq(&gEq[0], c, x[c])));
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
		// sidechain pump: duck on each beat, recover through it
		if (gPump > 0.001f) {
			float ph = (gBeatPh + s * beatInc) * pumpRate; ph -= floorf(ph);
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
static const char* OUT_NAMES[3] = {"STEREO", "SURROUND", "AUTO"};   // AUTO: surround on headphones
static const char* DEPTH_NAMES[4] = {"25%", "50%", "75%", "100%"};
static void outputApply(void) {
	if (!gAudioOK) return;
	int sur = gOutMode == 1 || (gOutMode == 2 && gHeadphones);
	ndspSetOutputMode(sur ? NDSP_OUTPUT_SURROUND : NDSP_OUTPUT_STEREO); gSurActive = sur;
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
static void drumTick(float dt);
static int tqDrain(int inTick); static volatile int gTqUi;
static void audioTick(void* data) {
	(void)data;
	if (!gAudioClock) return;
	float dt = 160.0f / DSPRATE + gClockDebt;
	if (LightLock_TryLock(&gVoiceLock)) { gClockDebt = dt; return; }   // main loop busy: catch up next frame
	if (LightLock_TryLock(&gMipsLock)) { LightLock_Unlock(&gVoiceLock); gClockDebt = dt; return; }
	gClockDebt = 0;
	gInAudioTick = 1;
	if (tqDrain(1)) gTqUi = 1;   // FM-1 LINK's timed notes, on time to the tick
	arpTick(dt);
	seqTick(dt);
	drumTick(dt);
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
typedef struct { s8 n; u8 note[4]; u8 vel; u8 fl; } Step;   // fl: 1 accent, 2 slide into the next step
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
static int gSlideOn;
static void seqStop(void) { if (gSeqPlaying) playSet(NULL, 0, 0.8f); gSlideOn = 0; gSeqPlaying = 0; gSeqPos = -1; gSeqPh = 0; }
static int gSlideOn;   // the step before asked to slide into this one
static void seqFire(void) {
	Step* s = &gSeq[gSeqPos];
	int notes[4], n = 0;
	for (int i = 0; i < s->n; i++) notes[n++] = snapScale(s->note[i] + gSeqTr);
	float vel = s->n ? s->vel / 255.0f : 0.8f;
	if (s->fl & 1) vel = 1.0f;   // accent
	if (gSlideOn && n == 1 && gHeldN == 1 && !gArp) {   // slide: keep the voice, bend it to the new note
		int done = 0;
		for (int i = 0; i < NVS; i++) { SVoice* v = &gV[i]; if (v->active && !v->isEcho && v->note == gHeld[0] && v->stage < 4) { v->note = notes[0]; v->freq = mtof((float)notes[0]); v->slide = 1; done = 1; } }
		if (done) { gHeld[0] = notes[0]; gSlideOn = (s->fl & 2) != 0; gBeatFlash = 1; return; }
	}
	playSet(notes, n, vel);
	gSlideOn = n && (s->fl & 2);
	gBeatFlash = 1;
}
static void seqTick(float dt) {
	if (gSeqMode != 1) { if (gSeqPlaying) seqStop(); return; }
	if (!gSeqPlaying) { gSeqPlaying = 1; gSeqPos = 0; gSeqPh = 0; seqFire(); return; }
	float len = (gSeqPos & 1) ? 1.0f - gSwing : 1.0f + gSwing;   // in 16ths
	float before = gSeqPh;
	gSeqPh += dt * gBPM / 60.0f * 4.0f;
	if (!gArp && !gSlideOn && before < len * gSeqGate && gSeqPh >= len * gSeqGate) playSet(NULL, 0, 0.8f);
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
} SPatch;
#define PATCH_MAGIC 0x5359523B   // "SYR;" (v11)
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
	p->motTgt[l] = tgt; p->motOn[l] = 1; p->motBars = bars;
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
	ENV1(0.01f, 0, 0.5f, 0.9f, 0.2f); OUTF(OF_L24, 0.42f, 0.2f, 0.35f, 0); MFX(FX_PHASER, 0.12f, 0.6f, 0.4f); p->ott = 0.35f; LFO(0, LS_SINE, 0, 0.1f); MOD(2, SRC_LFO1, DST_OFCUT, 0.15f); }
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
static void l_bigunison(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 7; OA.detune = 26; OA.width = 1; ENV1(0.01f, 0, 0.4f, 0.85f, 0.4f);
	MFX(FX_HYPER, 0.4f, 0.5f, 0.35f); DLYS(0.2f, S8D, DL_PING); REVB(0.2f, RV_HALL); OUTF(OF_LP, 0.75f, 0.1f, 0, 0); }
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
static void p_strings77(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 5; OA.detune = 18; OA.width = 1; ENV1(0.35f, 0, 1.0f, 0.9f, 0.9f);
	OUTF(OF_L12, 0.5f, 0.1f, 0, 0); MFX(FX_CHORUS, 0.6f, 0.8f, 0.6f); REVB(0.25f, RV_HALL); p->eqHigh = -1; }
static void p_drawnswell(SPatch* p) { p->master = 0.65f; OA.table = T_SYNC; OA.uni = 3; OA.detune = 12; dshape(p, 0, 4, NULL); p->dlSync[0] = LY_1BAR - 1; p->dlRate[0] = 0.25f;
	MOD(2, SRC_LFO3, DST_OFCUT, 0.4f); OUTF(OF_L24, 0.35f, 0.35f, 0.2f, 0); ENV1(0.5f, 0, 1.0f, 1.0f, 1.5f); REVB(0.35f, RV_HALL); }
static void p_haze(SPatch* p) { p->master = 0.7f; OA.table = T_VOWELS; OA.uni = 3; OA.detune = 14; OB.on = 1; OB.table = T_HARMONIX; OB.oct = 1; OB.level = 0.25f; ENV1(0.6f, 0, 1.0f, 0.9f, 1.8f);
	lane(p, 0, 0, 2, 0.1f, 0.9f, 3); lane(p, 1, 4, 3, 0.5f, 0.85f, 2); OUTF(OF_CREAMY, 0.65f, 0.25f, 0, 0); REVB(0.35f, RV_HALL); }
static void p_reverse(SPatch* p) { p->master = 0.7f; OA.table = T_HARMONIX; OA.uni = 4; OA.detune = 14; ENV1(0.4f, 0, 1.0f, 0.9f, 1.2f); REVB(0.45f, RV_SWELL); DLYS(0.3f, S4, DL_REV); }
static void p_sonar(SPatch* p) { p->master = 0.75f; OA.table = T_HARMONIX; OA.oct = -1; OA.uni = 3; OA.detune = 8; ENV1(1.0f, 0, 1.0f, 1.0f, 2.5f); OUTF(OF_PHASER, 0.4f, 0.6f, 0, 0);
	LFO(0, LS_SINE, 0, 0.05f); MOD(2, SRC_LFO1, DST_OFCUT, 0.4f); ORBIT(0.5f, 0.06f); REVB(0.4f, RV_HALL); }
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
static void c_omni(SPatch* p) { p->master = 0.6f; OA.table = T_BASIC; OA.pos = TRI; OB.on = 1; OB.table = T_BASIC; OB.pos = SINE; OB.oct = 1; OB.level = 0.3f; ENV1(0.002f, 0, 2.0f, 0.0f, 1.5f);
	MFX(FX_CHORUS, 0.5f, 0.6f, 0.45f); REVB(0.35f, RV_HALL); }
static void c_pump(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 4; OA.detune = 16; ENV1(0.01f, 0, 0.6f, 0.8f, 0.3f); OUTF(OF_L12, 0.5f, 0.2f, 0, 0); p->pump = 0.7f; REVB(0.2f, RV_HALL); }
static void c_gospel(SPatch* p) { p->master = 0.5f; OA.table = T_HARMONIX; OA.pos = 0.6f; OB.on = 1; OB.table = T_HARMONIX; OB.pos = 0.3f; OB.oct = -1; OB.level = 0.3f; ENV1(0.01f, 0, 0.1f, 1.0f, 0.15f);
	MFX(FX_CHORUS, 5.0f, 0.5f, 0.5f); DRV(1, 0.3f); REVB(0.25f, RV_HALL); }
static void c_shoegaze(SPatch* p) { p->master = 0.45f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 6; OA.detune = 30; OA.width = 1; DRV(14, 0.5f); ENV1(0.15f, 0, 1.0f, 0.9f, 1.5f);
	MFX(FX_HYPER, 0.3f, 0.7f, 0.5f); REVB(0.5f, RV_HALL); p->shimmer = 1; OUTF(OF_LP, 0.6f, 0.1f, 0, 0); }

// ---- SEQUENCE (arps and movement)
static void q_berlin(SPatch* p) { p->master = 0.55f; OA.table = T_BASIC; OA.pos = SAW; ARP(1, 3, 2, 0.45f); ENV1(0.002f, 0, 0.2f, 0.3f, 0.1f); ENV2(0.0f, 0, 0.15f, 0, 0.1f);
	OUTF(OF_L24, 0.35f, 0.5f, 0.3f, 0.35f); lane(p, 0, 4, 3, 0.25f, 0.6f, 3); DLYS(0.2f, S8D, DL_TAPE); }
static void q_acidcircle(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.oct = -1; ARP(3, 3, 2, 0.6f); p->voiceMode = 0; ENV1(0.002f, 0, 0.2f, 0.4f, 0.08f);
	ENV2(0.0f, 0, 0.18f, 0, 0.1f); OUTF(OF_ACID, 0.25f, 0.8f, 0.5f, 0.5f); lane(p, 0, 5, 2, 0.55f, 0.92f, 2); lane(p, 1, 4, 3, 0.15f, 0.45f, 1); DLYS(0.2f, S8D, DL_DUB); }
static void q_gate(SPatch* p) { p->master = 0.5f; OA.table = T_BASIC; OA.pos = SAW; OA.uni = 5; OA.detune = 20; ENV1(0.01f, 0, 1.0f, 1.0f, 0.4f); dshape(p, 0, 3, NULL); p->dlSync[0] = LY_16;
	MOD(2, SRC_LFO3, DST_AMP, 0.5f); MOD(3, SRC_OFF, DST_AMP, 0); OUTF(OF_L12, 0.55f, 0.2f, 0, 0); REVB(0.25f, RV_HALL); DLYS(0.15f, S8D, DL_PING); }
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
static void m_tidal(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.uni = 3; OA.detune = 12; OB.on = 1; OB.table = T_VOWELS; OB.level = 0.4f; ENV1(0.4f, 0, 1.0f, 0.9f, 1.4f);
	lane(p, 0, 0, 2, 0.0f, 1.0f, 2); lane(p, 1, 1, 3, 1.0f, 0.0f, 3); lane(p, 2, 4, 5, 0.45f, 0.85f, 2); OUTF(OF_CREAMY, 0.7f, 0.2f, 0, 0); MFX(FX_DIM, 0.3f, 0.6f, 0.5f); REVB(0.35f, RV_HALL); }
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
static void m_graintide(SPatch* p) { p->master = 0.65f; OA.table = T_HARMONIX; OA.level = 0.25f; GRN(0.45f, 0.5f, 0.35f, 0, 0.9f); ENV1(0.3f, 0, 1.0f, 0.9f, 1.5f);
	lane(p, 0, 11, 2, 0.0f, 1.0f, 3); lane(p, 1, 0, 3, 0.2f, 0.9f, 2); lane(p, 2, 10, 6, 0.15f, 0.5f, 3); ORBIT(0.5f, 0.1f); REVB(0.3f, RV_HALL); }


typedef struct { const char* name; void (*fn)(SPatch*); } PresetDef;
#define MAXPRE 15
#define NSTYLES 13   // last style = USER
static const char* STYLE_NAMES[NSTYLES] = {"BASS", "LEAD", "PAD", "KEYS", "PLUCK", "CHORD", "SEQUENCE", "TEXTURE", "CHIP", "SPACE", "CHIPSET", "MOTION", "USER"};
#define STYLE_SPACE 9
static const PresetDef PRESETS[NSTYLES - 1][MAXPRE] = {
	{{"SUB FLOOR", b_subfloor}, {"303 ON 3RD AVE", b_303}, {"REESE CRATE", b_reese}, {"WOBBLE TIDE", b_wobble}, {"FM KNOCK", b_fmknock}, {"DIODE BUZZ", b_diode},
	 {"SP-12 DROP", b_sp12}, {"CUMBIA BAJO", b_cumbia}, {"ROLLING NOTCH", b_dnb}, {"STICK GROWL", b_tiltgrowl}, {"UPRIGHT THUMB", b_upright}, {"CHIP BASS", b_chip}},
	{{"GULF SYNC", l_gulfsync}, {"TALK BOX", l_talkbox}, {"WIDE UNISON", l_bigunison}, {"WHISTLE", l_whistle}, {"FOLD SCREAM", l_scream}, {"LADDER TAPE", l_moogtape},
	 {"CHIP HERO", l_chiphero}, {"G-FUNK SINE", l_glidesine}, {"RINGER", l_ringer}, {"ACID SCREAM", l_acidscream}, {"PAD BENDER", l_bender},
	 {"FLUTE", l_flute}, {"STICK THEREMIN", l_theremin}, {"XY SCULPT", l_xysculpt}, {"BUSY BOY", l_busyboy}},
	{{"WARM FELT", p_felt}, {"GLASS HOUR", p_glass}, {"VOWEL DUSK", p_vowel}, {"COMB WEATHER", p_combweather}, {"CLOUD BANK", p_cloudbank}, {"STRINGS 77", p_strings77},
	 {"DRAWN SWELL", p_drawnswell}, {"MOTION HAZE", p_haze}, {"SWELL BACKWARD", p_reverse}, {"DEEP SONAR", p_sonar}, {"GRAIN CHOIR", p_hallchoir}},
	{{"EP SATURDAY", k_epsat}, {"DRAWBAR", k_drawbar}, {"LOFI UPRIGHT", k_lofiup}, {"CLAV FUNK", k_clav}, {"WURLI BARK", k_wurli}, {"VIBES", k_vibes},
	 {"TOY KEYS", k_toy}, {"HARPSI", k_harpsi}, {"ROTARY", k_rotary}, {"CUMBIA ORGANO", k_cumbiaorg}},
	{{"NYLON", u_nylon}, {"MARIMBA WOOD", u_marimba}, {"KALIMBA", u_kalimba}, {"DIGI HARP", u_digiharp}, {"STAB 1999", u_stab99}, {"KOTO SPRING", u_koto},
	 {"CHIP PLINK", u_chipplink}, {"GRAIN PLUCK", u_grain}, {"STEEL PAN", u_steel}, {"RAIN ON TIN", u_raintin}},
	{{"ORGAN STAB", c_organstab}, {"NEO SOUL 9", c_neosoul}, {"DUB CHORD", c_dub}, {"TECHNO STAB", c_techno}, {"STRUM GUITAR", c_strum}, {"OMNI HARP", c_omni},
	 {"PUMP CHORDS", c_pump}, {"GOSPEL ORGAN", c_gospel}, {"SHOEGAZE WALL", c_shoegaze}},
	{{"BERLIN 16", q_berlin}, {"ACID CIRCLE", q_acidcircle}, {"TRANCE GATE", q_gate}, {"PING ARP", q_pingarp}, {"CHAOS ARP", q_chaosarp}, {"MOTION BELLS", q_motionbells},
	 {"OCTAVE BOUNCE", q_octbounce}, {"DRAWN STEPS", q_drawnsteps}, {"GRAIN RUN", q_grainrun}, {"POLYRHYTHM", q_polyrhythm}},
	{{"TIDE POOL", x_tidepool}, {"FROZEN TABLE", x_frozen}, {"NET STATIC", x_netstatic}, {"TAPE ROOM", x_taperoom}, {"BROWN SEA", x_sea}, {"GEIGER", x_geiger},
	 {"DRONE 60", x_drone60}, {"SHIMMER CLOUD", x_shimmercloud}, {"SONAR PING", x_sonarping}, {"STORM FRONT", x_storm}},
	{{"GB WAVE", h_gbwave}, {"NES PULSE", h_nes}, {"C64 PWM", h_c64}, {"ARCADE ARP", h_arcade}, {"POWER UP", h_powerup}, {"LASER", h_laser},
	 {"8-BIT BASS", h_bass8}, {"BIT ORGAN", h_bitorgan}, {"SQUARE CHOIR", h_squarechoir}},
	{{"ORBIT PAD", s_orbitpad}, {"HELICOPTER", s_heli}, {"CIRCLE ARP", s_circlearp}, {"AROUND YOU", s_aroundyou}, {"3D BELLS", s_bells}, {"SPIRAL PLUCK", s_spiral},
	 {"PASSING CAR", s_passing}, {"STEER ORBIT", s_tilt}},
	{{"PSG STACK", c_psgstack}, {"ECHO CANYON", c_canyon}, {"CRUSH BELL", c_crushbell}, {"8-BIT CHOIR", c_choir8}, {"CAPTURE DUB", c_capturedub},
	 {"TWO CHIPS", c_twochips}, {"QUANT GRIT", c_quantgrit}, {"GB ORBIT", c_gborbit}, {"DOUBLE ECHO", c_doubleecho}, {"SUB CHIP", c_subchip},
	 {"SCREAM LEAD", c_screamlead}, {"RAW SAMPLER", c_rawsampler}, {"HOT ORGAN", c_hotorgan}, {"GLITCH STORM", c_glitchstorm}, {"PITCH CANYON", c_pitchcanyon}},
	{{"KITCHEN SINK", m_busyboy}, {"TIDAL TABLE", m_tidal}, {"STEP SCULPT", m_stepsculpt}, {"GLASS BREATH", m_breathglass}, {"CHAOS GARDEN", m_chaosgarden},
	 {"ACID MORPH", m_acidmorph}, {"VOWEL WALK", m_vowelwalk}, {"LIQUID SYNC", m_liquidsync}, {"DRAWN DANCE", m_drawndance}, {"GRAIN TIDE", m_graintide}},
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
	} else if (len == (long)(PATCH_V1_SIZE * NUSER) || len == (long)(PATCH_V2_SIZE * NUSER) || len == (long)(PATCH_V3_SIZE * NUSER) || len == (long)(PATCH_V4_SIZE * NUSER) || len == (long)(PATCH_V5_SIZE * NUSER) || len == (long)(PATCH_V6_SIZE * NUSER) || len == (long)(PATCH_V7_SIZE * NUSER) || len == (long)(PATCH_V8_SIZE * NUSER) || len == (long)(PATCH_V9_SIZE * NUSER) || len == (long)(PATCH_V10_SIZE * NUSER)) {   // older save files: newer fields get defaults
		long rs = len / NUSER;
		for (int i = 0; i < NUSER; i++) {
			SPatch t; patchInit(&t);
			SPatch d; patchInit(&d);
			if (fread(&t, rs, 1, f) == 1 && (t.magic == PATCH_MAGIC1 || t.magic == PATCH_MAGIC2 || t.magic == PATCH_MAGIC3 || t.magic == PATCH_MAGIC4 || t.magic == PATCH_MAGIC5 || t.magic == PATCH_MAGIC6 || t.magic == PATCH_MAGIC7 || t.magic == PATCH_MAGIC8 || t.magic == PATCH_MAGIC9 || t.magic == PATCH_MAGIC10)) {
				memcpy((char*)&t + rs, (char*)&d + rs, sizeof(SPatch) - rs);   // fields this save didn't have: defaults
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
	for (int i = 0; i < 4; i++) { p->mod[i] = gMod[i]; p->mod2[i] = gMod[i + 4]; p->macro[i] = gMacro[i]; }
	for (int i = 0; i < 8; i++) p->modCurve[i] = gModCurve[i];
	p->modFx = gModFx; p->dlySync = gDlySync; p->shimmer = gShimmer; p->freeze = gFreeze; p->pumpRate = gPumpRate; p->lofiBits = gLofiBits;
	p->modRate = gModRate; p->modDepth = gModDepth; p->modMix = gModMix; p->fxColor = gFxColor; p->eqLow = gEqLow; p->eqMid = gEqMid; p->eqHigh = gEqHigh;
	p->dlyMix = gDlyMix; p->dlyWow = gDlyWow; p->dlySat = gDlySat; p->revMix = gRevMix; p->ott = gOtt; p->pump = gPump; p->lofiRate = gLofiRate;
	p->chipLevel = gChipLevel; p->chipDuty = gChipDuty; p->chipOct = gChipOct; p->csEcho = gCsEcho; p->csEchoFb = gCsEchoFb; p->csEchoT = gCsEchoT; p->orbit = gOrbit; p->orbitRate = gOrbitRate; p->revType = gRevType; p->dlyType = gDlyType; p->grainMode = gGrains; p->grainSrc = gGrainSrc; p->grainSize = gGrainSize; p->grainScan = gGrainScan; p->grainSpray = gGrainSpray; p->grainPitch = gGrainPitch; p->grainLevel = gGrainLevel; p->ofType = gOfType; p->ofCut = gOfCut; p->ofRes = gOfRes; p->ofDrive = gOfDrive; p->ofEnv = gOfEnv;
	p->qGrit = gQGrit; p->csEchoPitch = gCsEchoPitch; p->qScream = gQScream; p->qMixDrive = gQMixDrive;
	for (int k = 0; k < 2; k++) { for (int i = 0; i < 64; i++) p->dl[k][i] = (s8)(gDl[k][i] * 127); p->dlRate[k] = gDlRate[k]; p->dlSync[k] = gDlSync[k]; }
	memcpy(p->mot, gMot, sizeof(gMot)); memcpy(p->motTgt, gMotTgt, sizeof(gMotTgt)); memcpy(p->motOn, gMotOn, sizeof(gMotOn)); p->motBars = gMotBars;
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
	gSubShape = p->subShape; gSubOct = p->subOct; gNoiseType = p->noiseType % 6; gFiltType = p->filtType;
	gSubLevel = p->subLevel; gNoiseLevel = p->noiseLevel; gCutoff = p->cutoff; gRes = p->res; gKeytrack = p->keytrack;
	gEnv1 = p->env1; gEnv2 = p->env2;
	for (int i = 0; i < 2; i++) { gLfo[i].shape = p->lfoShape[i] < NLFOSH ? p->lfoShape[i] : 0; gLfo[i].mode = p->lfoMode[i]; gLfo[i].rate = p->lfoRate[i]; gLfo[i].sync = p->lfoSync[i] < NSYNC ? p->lfoSync[i] : 0; gLfo[i].table = p->lfoTable[i] < T_BUILTIN ? p->lfoTable[i] : 0; gLfo[i].pos = p->lfoPos[i]; gLfo[i].tabKey = -1; }
	for (int i = 0; i < 4; i++) { gMod[i] = p->mod[i]; gMod[i + 4] = p->mod2[i]; gMacro[i] = p->macro[i]; }
	for (int i = 0; i < 8; i++) gModCurve[i] = p->modCurve[i] & 3;
	gModFx = p->modFx % 8; gDlySync = p->dlySync % 7; gShimmer = p->shimmer & 1; gFreeze = p->freeze & 1; gPumpRate = p->pumpRate % 3; gLofiBits = p->lofiBits % 6;
	gModRate = p->modRate; gModDepth = p->modDepth; gModMix = p->modMix; gFxColor = p->fxColor; gEqLow = p->eqLow; gEqMid = p->eqMid; gEqHigh = p->eqHigh;
	gDlyMix = p->dlyMix; gDlyWow = p->dlyWow; gDlySat = p->dlySat; gRevMix = p->revMix; gOtt = p->ott; gPump = p->pump; gLofiRate = p->lofiRate;
	chipSet(p->chipLevel); gChipDuty = p->chipDuty; gChipOct = p->chipOct; gCsEcho = p->csEcho; gCsEchoFb = p->csEchoFb; gCsEchoT = p->csEchoT; gOrbit = p->orbit; gOrbitRate = p->orbitRate; gRevType = p->revType % NREVT; gDlyType = p->dlyType % NDLYT; dlyTypeChanged();
	gQGrit = p->qGrit % 3; gCsEchoPitch = p->csEchoPitch; gQScream = p->qScream; gQMixDrive = p->qMixDrive;
	if (p->ofType % NOFT != gOfType) { gOfType = p->ofType % NOFT; ofReset(); } gOfCut = p->ofCut; gOfRes = p->ofRes; gOfDrive = p->ofDrive; gOfEnv = p->ofEnv;
	for (int k = 0; k < 2; k++) { for (int i = 0; i < 64; i++) gDl[k][i] = p->dl[k][i] / 127.0f; gDlRate[k] = p->dlRate[k]; gDlSync[k] = p->dlSync[k] % NSYNC; }
	memcpy(gMot, p->mot, sizeof(gMot)); for (int l = 0; l < 4; l++) { gMotTgt[l] = p->motTgt[l] % 16; gMotOn[l] = p->motOn[l] & 1; } gMotBars = p->motBars & 3; gMotRec = -1; gMotPh = 0;
	gVoiceMode = p->voiceMode; gChord = p->chord; gFx = p->fx; gEchoT = p->echoT; gDrive = p->drive; gBendRange = p->bendRange;
	gGlide = p->glide; gChorus = p->chorus; gRevSize = p->revSize; gRevDamp = p->revDamp; gFxMix = p->fxMix; gDriveAmt = p->driveAmt; gMaster = p->master;
	gArp = p->arp; gArpRate = p->arpRate; gArpOct = p->arpOct < 1 ? 1 : p->arpOct; gArpGate = p->arpGate; if (p->bpm >= 40) gBPM = p->bpm;
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
	{0.49f, 0.79f, 0.91f, 0.81f, 0.59f, 0.60f, 0.82f, 1.42f, 1.42f, 1.32f, 1.28f, 0.68f, 1.00f, 1.00f, 1.00f},
	{2.57f, 1.93f, 3.31f, 0.67f, 0.86f, 0.65f, 0.68f, 0.59f, 1.03f, 0.80f, 2.27f, 0.60f, 0.47f, 6.00f, 1.00f},
	{6.00f, 1.00f, 3.07f, 3.77f, 4.25f, 3.46f, 1.76f, 6.00f, 0.84f, 3.48f, 3.53f, 1.00f, 1.00f, 1.00f, 1.00f},
	{0.59f, 0.60f, 0.68f, 0.91f, 0.72f, 0.61f, 1.77f, 2.05f, 0.75f, 0.75f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{0.79f, 1.50f, 0.64f, 1.21f, 2.44f, 0.93f, 1.07f, 2.21f, 0.59f, 1.63f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{2.33f, 1.34f, 4.24f, 2.18f, 2.20f, 1.12f, 2.30f, 0.67f, 2.08f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{1.32f, 1.05f, 4.46f, 1.10f, 3.54f, 1.82f, 2.40f, 6.00f, 1.34f, 2.41f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{6.00f, 1.51f, 3.91f, 1.19f, 3.50f, 4.82f, 6.00f, 4.32f, 0.62f, 1.36f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{0.90f, 1.24f, 1.14f, 1.50f, 1.37f, 1.28f, 0.91f, 0.63f, 1.18f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{1.30f, 0.81f, 1.34f, 2.99f, 0.90f, 1.37f, 3.46f, 1.10f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
	{0.89f, 1.18f, 0.84f, 1.09f, 1.20f, 1.20f, 1.20f, 1.00f, 1.20f, 0.89f, 1.00f, 1.00f, 0.60f, 1.20f, 1.20f},   // CHIPSET
	{2.66f, 2.19f, 0.62f, 4.29f, 2.26f, 0.85f, 2.35f, 6.00f, 1.43f, 1.13f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},   // MOTION
};
static int presetCount(int style) {
	if (style == NSTYLES - 1) return NUSER;
	int n = 0; while (n < MAXPRE && PRESETS[style][n].name) n++;
	return n;
}
static void say(const char* m);
// edit guard: after you change a patch, the first step to another patch only warns (the second one goes)
static int gEdited = 0, gGuard = 0, gStyleCur = 0, gPresetCur = 0;
static char gCallTxt[24]; static float gCallT = 0;   // big pop-up of a stepped value (Shrub-style)
static void presetLoad(int style, int idx) {
	gEdited = 0; gGuard = 0; gStyleCur = style; gPresetCur = idx;
	if (style == NSTYLES - 1) { if (gUser[idx].magic == PATCH_MAGIC) patchApply(&gUser[idx]); return; }
	SPatch p; patchInit(&p);
	gPackMissing = 0;
	PRESETS[style][idx].fn(&p);
	p.master *= PGAIN[style][idx];
	snprintf(p.name, sizeof(p.name), "%s", PRESETS[style][idx].name);
	patchApply(&p);
	if (style == STYLE_SPACE && gOutMode == 0) { gOutMode = 1; outputApply(); say("3D PATCH: OUTPUT SET TO SURROUND (HEADPHONES BEST)"); }
	else if (gPackMissing) say("GET WAVETABLES (HW PAGE) FOR THE FULL SOUND");
}
static const char* presetName(int style, int idx) { return style == NSTYLES - 1 ? gUser[idx].name : PRESETS[style][idx].name; }

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
static const char* XY_NAMES[3] = {"WT + CUTOFF", "MACRO 1+2", "MACRO 3+4"};
static Param P_MOD[] = {
	PF("MACRO 1", &gMacro[0], 0, 1, 0.02f, "%", 0), PF("MACRO 2", &gMacro[1], 0, 1, 0.02f, "%", 0), PF("MACRO 3", &gMacro[2], 0, 1, 0.02f, "%", 0), PF("MACRO 4", &gMacro[3], 0, 1, 0.02f, "%", 0),
	PE("XY PAD", &gXyMode, 3, XY_NAMES, D_THEME), PF("CHAOS RATE", &gChaosRate, 0, 1, 0.02f, "%", 0),
	PF("LFO3 RATE", &gDlRate[0], 0.05f, 20, 0.05f, "HZ", 0), PE("LFO3 SYNC", &gDlSync[0], NSYNC, SYNC_NAMES, 0), PF("LFO4 RATE", &gDlRate[1], 0.05f, 20, 0.05f, "HZ", 0), PE("LFO4 SYNC", &gDlSync[1], NSYNC, SYNC_NAMES, 0),
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
	PE("MODE", &gVoiceMode, 3, VOICE_MODES, 0), PF("GLIDE", &gGlide, 0, 1, 0.01f, "S", 0),
	PE("BEND RANGE", &gBendRange, 13, BEND_NAMES, 0), PE("CHORD", &gChord, 5, CHORD_NAMES, 0),
	PF("CHORUS", &gChorus, 0, 1, 0.02f, "%", 0), PE("GPU VISUALS", &gGpuFx, 2, ONOFF, 0), PE("FX", &gFx, 4, FX_NAMES, 0), PE("ECHO TIME", &gEchoT, 3, ECHO_T_NAMES, 0),
	PF("ECHO FDBK", &gEchoFb, 0, 0.9f, 0.03f, "%", 0), PE("OUTPUT", &gOutMode, 3, OUT_NAMES, D_OUT), PE("SURROUND", &gSurDepth, 4, DEPTH_NAMES, D_OUT),
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
	PE("TRAILS", &gVisTrails, 2, ONOFF, 0), PE("PARTICLES", &gVisParts, 2, ONOFF, 0),
	PE("THEME", &gTheme, NTHEMES, THEME_NAMES, D_THEME), PE("SCANLINES", &gScan, 2, ONOFF, D_THEME),
	PEND};
static const char* LOOP_NAMES_P[3] = {"OFF", "REC", "PLAY"};
static const char* BARS_NAMES_P[5] = {"1 BAR", "2 BARS", "4 BARS", "8 BARS", "16 BARS"};
static const char* LINK_NAMES_P[6] = {"OFF", "WIFI HOST", "WIFI JOIN", "IR SEND", "IR RECEIVE", "IR MIDI IN"};
static int gLoopMode, gLoopBars, gCamOn, gLink, gBeatLight;
static float gNetLat = 20;   // ms: FM-1 LINK's timing buffer (steady delay instead of wobble)
static float gLoopLevel;
static Param P_HW[] = {
	PE("LOOPER", &gLoopMode, 3, LOOP_NAMES_P, D_LOOP), PE("LOOP BARS", &gLoopBars, 5, BARS_NAMES_P, 0), PF("LOOP LEVEL", &gLoopLevel, 0, 1, 0.05f, "%", 0),
	PE("GRAINS", &gGrains, 3, GRAIN_MODES, D_GRAIN), PE("GRAIN SOURCE", &gGrainSrc, 3, GRAIN_SRCS, D_GRAIN), PF("GRAIN SIZE", &gGrainSize, 0, 1, 0.02f, "%", 0), PF("GRAIN SCAN", &gGrainScan, 0, 1, 0.02f, "%", 0),
	PF("GRAIN SPRAY", &gGrainSpray, 0, 1, 0.02f, "%", 0), PF("GRAIN PITCH", &gGrainPitch, -24, 24, 1, "ST", 0), PF("GRAIN LEVEL", &gGrainLevel, 0, 1, 0.05f, "%", 0),
	{"LOOP > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_LOOPTAB},
	{"MIC > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_MICTAB},
	PE("CAMERA", &gCamOn, 2, ONOFF, D_CAM),
	{"CAM > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_CAMTAB},
	PE("ADD EDITOR", &gAddEdit, 2, ONOFF, D_ADDED), {"RULE > TABLE", PT_E, &gSaveSlot, 0, 0, 1, PRESS_RIGHT, "", D_DLWT},
	PE("CAM WAVE", &gCamVid, 3, VID_NAMES, D_CAMV), PF("WT SCAN", &gWtScan, 0, 4, 0.05f, "X", 0),
	PE("WI-FI MIDI", &gMidiNet, 2, ONOFF, D_NETMIDI), PF("NET BUFFER", &gNetLat, 0, 60, 1, "MS", 0),
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
	PF("OTT", &gOtt, 0, 1, 0.02f, "%", 0), PF("PUMP", &gPump, 0, 1, 0.02f, "%", 0), PE("PUMP RATE", &gPumpRate, 3, PUMP_NAMES, 0),
	PE("LOFI BITS", &gLofiBits, 6, BITS_NAMES, 0), PF("LOFI RATE", &gLofiRate, 0, 1, 0.02f, "%", 0),
	PF("3D ORBIT", &gOrbit, 0, 1, 0.02f, "%", 0), PF("ORBIT RATE", &gOrbitRate, 0, 2, 0.02f, "HZ", 0),
	PE("FX ON CORE 2", &gFxCore2, 2, ONOFF2, 0), PE("DSP GRIT", &gQGrit, 3, GRIT_NAMES, 0), PF("FILTER SCREAM", &gQScream, 0, 1, 0.02f, "%", 0), PF("MIX OVERDRIVE", &gQMixDrive, 0, 1, 0.02f, "%", 0),
	PEND};
static Param P_DRUMS[] = {
	PE("DRUMS", &gDrums, 2, ONOFF2, D_DRUMS), PE("KIT", &gKit, 4, KIT_NAMES, D_DRUMS), PE("KICK", &gKickPat, 9, KICKP_NAMES, 0),
	PE("SNARE", &gSnarePat, 8, SNAREP_NAMES, 0), PE("HAT", &gHatPat, 8, HATP_NAMES, 0), PE("MIC TAP", &gDrumRec, 5, DREC_NAMES, D_DREC), PF("DRUM LEVEL", &gDrumLevel, 0, 1, 0.05f, "%", 0),
	PI_("BPM", &gBPM, 40, 240, 0), PF("SWING", &gSwing, 0, 0.5f, 0.02f, "%", 0),
	PI_("EUCLID STEPS", &gEuLen, 2, 16, D_EUCLID), PI_("KICK HITS", &gEuHits[0], 0, 16, D_EUCLID), PI_("KICK ROTATE", &gEuRot[0], 0, 15, D_EUCLID),
	PI_("SNARE HITS", &gEuHits[1], 0, 16, D_EUCLID), PI_("SNARE ROTATE", &gEuRot[1], 0, 15, D_EUCLID), PI_("HAT HITS", &gEuHits[2], 0, 16, D_EUCLID), PI_("HAT ROTATE", &gEuRot[2], 0, 15, D_EUCLID),
	PF("CHIP ECHO", &gCsEcho, 0, 1, 0.02f, "%", 0), PE("ECHO TIME", &gCsEchoT, 4, CSECHO_T_NAMES, 0), PF("ECHO REPEATS", &gCsEchoFb, 0, 0.85f, 0.03f, "%", 0), PI_("ECHO PITCH", &gCsEchoPitch, -12, 12, 0),
	PEND};
#define NPAGES 12
static Param* PAGES[NPAGES] = {P_PRESET, P_OSCA, P_OSCB, P_SUBN, P_FILT, P_MOD, P_VOICE, P_PLAY, P_VIS, P_HW, P_MASTER, P_DRUMS};
static const int NAV[NPAGES] = {0, 1, 2, 3, 4, 5, 6, 10, 7, 11, 8, 9};   // X / Y order: tabs left to right
static int navStep(int page, int dir) { int i = 0; while (NAV[i] != page) i++; return NAV[(i + dir + NPAGES) % NPAGES]; }
static const char* PAGE_NAMES[NPAGES] = {"PRESETS", "OSC A", "OSC B", "SUB+NOISE", "FILT+ENV", "LFO+MOD", "VOICE+FX", "ARP+SEQ", "VISUALIZER", "HARDWARE", "MASTER FX", "DRUMS"};
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
	int rampOn; u8 ramp[3][3];   // visual colour ramp (GPU visuals) for non-EGA themes; ORIGINAL uses amber > magenta > cyan
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
	{"IMPULSE", {0, 0, 0}, {168, 164, 152}, {112, 108, 100}, {196, 192, 180}, {44, 44, 132}, {255, 255, 255}, {236, 232, 220}, {220, 76, 64},
	 {216, 212, 200}, {28, 28, 28}, {72, 72, 196}, {100, 100, 216}, {100, 96, 88}, {132, 128, 120}, {168, 164, 152}, {0, 0, 0}, {14, 14, 22}, {44, 44, 56}, {0, 0, 0, 0}, 0,
	 1, {{168, 164, 152}, {72, 72, 210}, {236, 232, 220}}},
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
		{"THEME", "COLOUR SCHEME"},
		{"LOOPER", "REC RECORDS THE SOUND, THEN LOOPS IT"}, {"LOOP BARS", "LOOP LENGTH AT THE CURRENT BPM"}, {"LOOP LEVEL", "LOOP VOLUME"},
		{"LOOP > TABLE", "PRESS > TO TURN THE LOOP INTO A WAVETABLE"}, {"MIC > TABLE", "PRESS >, THEN SING 1 SECOND INTO THE MIC"},
		{"CAMERA", "CAMERA AS MOD SOURCES (CAM LIGHT / COLOR)"}, {"CAM > TABLE", "PRESS > TO TURN A PHOTO INTO A WAVETABLE"},
		{"LINK", "SYNC TEMPO WITH ANOTHER SYRUP"},
		{"MACRO 1", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"}, {"MACRO 2", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"},
		{"MACRO 3", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"}, {"MACRO 4", "MACRO: USE AS A MOD SOURCE, OR ON THE XY PAD"},
		{"XY PAD", "WHAT THE TOUCH PAD CONTROLS"},
		{"5 SOURCE", "MOD SLOT 5: WHAT MOVES IT"}, {"5 DEST", "MOD SLOT 5: WHAT IT MOVES"}, {"5 AMOUNT", "MOD SLOT 5: HOW MUCH"}, {"6 SOURCE", "MOD SLOT 6: WHAT MOVES IT"}, {"6 DEST", "MOD SLOT 6: WHAT IT MOVES"}, {"6 AMOUNT", "MOD SLOT 6: HOW MUCH"}, {"7 SOURCE", "MOD SLOT 7: WHAT MOVES IT"}, {"7 DEST", "MOD SLOT 7: WHAT IT MOVES"}, {"7 AMOUNT", "MOD SLOT 7: HOW MUCH"}, {"8 SOURCE", "MOD SLOT 8: WHAT MOVES IT"}, {"8 DEST", "MOD SLOT 8: WHAT IT MOVES"}, {"8 AMOUNT", "MOD SLOT 8: HOW MUCH"}, {"1 CURVE", "MOD SLOT 1: RESPONSE CURVE"}, {"2 CURVE", "MOD SLOT 2: RESPONSE CURVE"}, {"3 CURVE", "MOD SLOT 3: RESPONSE CURVE"}, {"4 CURVE", "MOD SLOT 4: RESPONSE CURVE"}, {"5 CURVE", "MOD SLOT 5: RESPONSE CURVE"}, {"6 CURVE", "MOD SLOT 6: RESPONSE CURVE"}, {"7 CURVE", "MOD SLOT 7: RESPONSE CURVE"}, {"8 CURVE", "MOD SLOT 8: RESPONSE CURVE"}, 
		{"MASTER FX", "WHOLE SOUND THROUGH THE EFFECT CHAIN"}, {"MOD FX", "CHORUS, FLANGER, PHASER, COMB, FORMANT"}, {"MOD RATE", "MOD FX SPEED"},
		{"MOD DEPTH", "MOD FX DEPTH / FEEDBACK"}, {"MOD MIX", "MOD FX AMOUNT"}, {"FX COLOR", "COMB PITCH / FORMANT VOWEL (MOD DEST)"},
		{"EQ LOW", "BASS BOOST / CUT"}, {"EQ MID", "MIDS BOOST / CUT"}, {"EQ HIGH", "TREBLE BOOST / CUT"},
		{"DELAY MIX", "TAPE DELAY AMOUNT (ADDS TO FX ECHO)"}, {"DELAY SYNC", "DELAY TIME LOCKED TO THE BPM"}, {"TAPE WOW", "TAPE PITCH WOBBLE"},
		{"TAPE SAT", "TAPE SATURATION ON REPEATS"}, {"REVERB MIX", "REVERB AMOUNT (ADDS TO FX REVERB)"}, {"SHIMMER", "OCTAVE-UP REVERB TAIL"},
		{"FREEZE", "HOLD THE REVERB FOREVER"}, {"OTT", "MULTIBAND UP+DOWN COMPRESSION"}, {"PUMP", "SIDECHAIN DUCKING ON THE BEAT"},
		{"PUMP RATE", "HOW OFTEN IT PUMPS"}, {"LOFI BITS", "BIT CRUSH THE WHOLE SOUND"}, {"LOFI RATE", "SAMPLE RATE REDUCTION"},
		{"DRUMS", "DRUM MACHINE ON / OFF"}, {"KIT", "DRUM SOUNDS"}, {"KICK", "KICK PATTERN"}, {"SNARE", "SNARE PATTERN"}, {"HAT", "HI-HAT PATTERN"},
		{"DRUM LEVEL", "DRUM VOLUME"}, {"GET WAVETABLES", "PRESS > TO DOWNLOAD THE SYRUP PACK (WI-FI)"}, {"UPDATE SYRUP", "PRESS > TO INSTALL THE NEWEST BUILD (WI-FI)"}, {"NET > TABLE", "PRESS >: RANDOM WEB PAGE BYTES -> WAVETABLE"}, {"GRAINS", "LOOP: PLAY THE SOURCE  KEYS: GRAIN OSCILLATOR"}, {"GRAIN SOURCE", "LOOP RECORDING, OSC A TABLE OR LAST SECOND OF MIC"}, {"CAM WAVE", "LIVE CAMERA TABLE, OR RECORD AN 8 S VIDEO TABLE"}, {"WT SCAN", "SWEEPS OSC A WT POS (1X = TABLE IN 8 S)"}, {"WI-FI MIDI", "PHONE REMOTE, NETWORK MIDI 5004, FM-1 LINK 5008"}, {"NET BUFFER", "FM-1 LINK TIMING: HIGHER = STEADIER, LOWER = SOONER"}, {"BEAT LIGHT", "SCREENS FLASH ON ARP / SEQ STEPS"}, {"FX ON CORE 2", "MASTER EFFECTS ON THE SECOND CPU CORE (+5 MS)"}, {"DSP GRIT", "DSP INTERPOLATION: LINEAR = DULLER, RAW = ALIASED CRUNCH"}, {"FILTER SCREAM", "LOW PASS PUSHED PAST STABLE: IT RINGS BY ITSELF"}, {"MIX OVERDRIVE", "DSP CHANNELS PAST FULL, CLIPPED AT 16 BITS"}, {"ECHO PITCH", "CHIP ECHO REPLAYED FASTER / SLOWER: REPEATS SHIFT"}, {"EUCLID STEPS", "LENGTH OF EUCLID ROWS: UNDER 16 = POLYRHYTHM"}, {"KICK HITS", "HITS SPREAD EVENLY - SETS KICK TO EUCLID"}, {"KICK ROTATE", "SHIFTS THE KICK EUCLID PATTERN"}, {"SNARE HITS", "HITS SPREAD EVENLY - SETS SNARE TO EUCLID"}, {"SNARE ROTATE", "SHIFTS THE SNARE EUCLID PATTERN"}, {"HAT HITS", "HITS SPREAD EVENLY - SETS HAT TO EUCLID"}, {"HAT ROTATE", "SHIFTS THE HAT EUCLID PATTERN"},
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
}
static void drawFooter(int page, int sel) {
	rect(0, 226, 400, 1, LINE);
	if (!gAudioOK) { text(4, 230, "NO AUDIO - RUN DSP1 ONCE (DUMPS DSP FIRMWARE)", 1, WARN); return; }
	if (gSafeMode) { char m[60]; snprintf(m, sizeof(m), "SAFE MODE - LAST RUN CRASHED AT %.24s", gCrashStage); text(4, 230, m, 1, WARN); return; }
	if (gMsgT > 0) { text(4, 230, gMsg, 1, VAL); return; }
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
	if (page == 9) drawHardwarePanel();
	else if (page == 10) drawMasterPanel();
	else if (page == 11) drawDrumsPanel();
	else if (page == 0) {   // preset browser: styles | patches
		frame(4, 30, 196, 194);
		for (int i = 0; i < NSTYLES; i++) {
			int y = 35 + i * 12;
			if (i == gStyle) { rect(7, y - 2, 74, 11, HI); text(10, y, STYLE_NAMES[i], 1, HITXT); }
			else text(10, y, STYLE_NAMES[i], 1, DIM);
		}
		rect(84, 33, 1, 186, LINE);
		for (int i = 0; i < presetCount(gStyle); i++) {
			int y = 35 + i * 12, cur = i == gPreset;
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
		else {
			static u64 lb = 0, lt = 0; static int pct = 0; u64 nt = svcGetSystemTick();
			if (nt - lt > SYSCLOCK_ARM11) { u64 b = gCore1Busy; pct = lt ? (int)((b - lb) * 100 / (nt - lt)) : 0; lb = b; lt = nt; }
			if (gWorker) snprintf(s, sizeof(s), "CORE 2 %d%%  FX %s", pct, !gFxCore2 ? "CORE 1" : (!gFxThread || gFxC2Fail) ? "FELL BACK" : "ON CORE 2");
			else snprintf(s, sizeof(s), "CORE 2 OFF %s %08lX %08lX L%lu", envIsHomebrew() ? "3DSX" : "CIA", (unsigned long)gCore2Rc, (unsigned long)gCore2ThRc, (unsigned long)gCore2Lim);
			if (gWorker) text(6, 210, s, 1, DIM); else text(6, 210, s, 1, WARN);
		}
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
#define GRID_BTNS 7
static void drawGrid(void) {
	rectP(0, XY_Y, 320, KB_Y - XY_Y, TH->bg);
	const char* B[GRID_BTNS] = {gSeqMode == 1 ? "STOP" : "PLAY", "CLEAR", "<LEN", "LEN>", "ROW-", "ROW+", "ACID"};
	for (int i = 0; i < GRID_BTNS; i++) {
		int x = i * 320 / GRID_BTNS, lit = (i == 0 && gSeqMode == 1) || i == 6;
		rectP(x + 2, XY_Y, 320 / GRID_BTNS - 4, 14, lit ? TH->keyOn : TH->tab);
		const u8* tc = lit ? EGA[0] : TH->tabTxt;
		text(x + 160 / GRID_BTNS - (int)strlen(B[i]) * 4, XY_Y + 3, B[i], 1, tc[0], tc[1], tc[2]);
	}
	float cw = 320.0f / NSTEP;
	for (int st = 0; st < NSTEP; st++) {
		int x = (int)(st * cw), w = (int)((st + 1) * cw) - x - 1, act = st < gSeqLen, head = gSeqPlaying && st == gSeqPos;
		int hit = 0;
		for (int r = 0; r < 8; r++) {
			int y = GRID_TOP + (7 - r) * GRID_ROWH, nt = gridNote(r), on = 0;
			for (int k = 0; k < gSeq[st].n; k++) if (gSeq[st].note[k] == nt) on = 1;
			hit += on;
			const u8* c = on ? (head ? TH->keyOn : (act ? TH->val : TH->dim)) : (head ? TH->grid : (act ? ((st & 3) == 0 ? TH->tab : TH->pad) : TH->bg));
			rectP(x + 1, y + 1, w - 1, GRID_ROWH - 2, c);
			if (on && (gSeq[st].fl & 1)) rectP(x + w / 2 - 2, y + 3, 4, GRID_ROWH - 6, TH->bg);   // accent: dark dot
			if (on && (gSeq[st].fl & 2)) rectP(x + w - 2, y + GRID_ROWH / 2 - 1, 4, 3, TH->keyOn);   // slide: tail into the next step
		}
		if (gSeq[st].n > hit) rectP(x + 1, GRID_TOP - 2, w - 1, 1, TH->val);   // notes off this grid (played on the keys)
	}
}
// 303-style line: 16ths, root-heavy, octave jumps, rests, accents, slides; notes from the grid's rows
static void acidGen(void) {
	static const int W[8] = {34, 6, 12, 8, 10, 6, 8, 16};   // weights per grid row (row 7 = octave up)
	memset(gSeq, 0, sizeof(gSeq));
	int prev = 0;
	for (int st = 0; st < 16; st++) {
		int restP = (st % 4 == 0) ? 8 : 26;
		if (rand() % 100 < restP) continue;
		int r, t = rand() % 100;
		if (st % 4 == 0 && rand() % 100 < 55) r = 0;
		else if (prev && t < 18) r = prev;
		else { int sum = 0, k = rand() % 100; for (r = 0; r < 8; r++) { sum += W[r]; if (k < sum) break; } if (r > 7) r = 0; }
		Step* sp = &gSeq[st]; sp->n = 1; sp->note[0] = (u8)gridNote(r); sp->vel = 190;
		if (rand() % 100 < (st % 4 == 2 ? 45 : 25)) sp->fl |= 1;
		if (rand() % 100 < 22) sp->fl |= 2;
		prev = r;
	}
	for (int st = 0; st < 16; st++) if ((gSeq[st].fl & 2) && !gSeq[(st + 1) % 16].n) gSeq[st].fl &= ~2;   // no slide into a rest
	if (!gSeq[0].n) { gSeq[0].n = 1; gSeq[0].note[0] = (u8)gridNote(0); gSeq[0].vel = 190; gSeq[0].fl = 1; }
	gSeqLen = 16; gSeqBase = gridNote(0); gSeqTr = 0;
}
static void gridTouch(int x, int y) {   // on press only
	if (y < GRID_TOP) {
		int b = x * GRID_BTNS / 320;
		if (b == 0) { gSeqMode = gSeqMode == 1 ? 0 : 1; if (gSeqMode == 1 && gSeqBase < 0) gSeqBase = gridNote(0); if (gSeqMode == 0) seqStop(); }
		if (b == 1) { seqClear(); gSeqLen = 16; gSeqBase = gridNote(0); }
		if (b == 2 && gSeqLen > 1) gSeqLen--;
		if (b == 3 && gSeqLen < NSTEP) gSeqLen++;
		if (b == 4 && gGridOct > -3) gGridOct--;
		if (b == 5 && gGridOct < 2) gGridOct++;
		if (b == 4 || b == 5) { char m[32]; int n0 = gridNote(0); snprintf(m, sizeof(m), "GRID BOTTOM ROW: %s%d", NOTE_NAMES[n0 % 12], n0 / 12 - 1); say(m); }
		if (b == 6) { if (gGridOct > -1) gGridOct = -1; acidGen(); say("ACID LINE: TAP ACID AGAIN FOR A NEW ONE"); }
		return;
	}
	int st = (int)(x * NSTEP / 320.0f), r = 7 - (y - GRID_TOP) / GRID_ROWH;
	if (st < 0 || st >= NSTEP || r < 0 || r > 7) return;
	int nt = gridNote(r); Step* sp = &gSeq[st];
	for (int k = 0; k < sp->n; k++) if (sp->note[k] == nt) {   // tap a note again: accent, slide, both, then off
		if (sp->fl < 3) { sp->fl++; return; }
		for (int j = k; j + 1 < sp->n; j++) sp->note[j] = sp->note[j + 1];
		sp->n--; sp->fl = 0; return;
	}
	if (sp->n >= 4) return;
	sp->note[sp->n++] = (u8)nt; sp->vel = 190;
	if (gSeqBase < 0) gSeqBase = gridNote(0);
	if (st >= gSeqLen) { gSeqLen = (st / 4 + 1) * 4; if (gSeqLen > NSTEP) gSeqLen = NSTEP; }
	if (!gSeqPlaying) { int n1 = nt; playSet(&n1, 1, 0.8f); gAuditionT = 0.18f; }   // hear it
}

static void outline(int x, int y, int w, int h, u8 r, u8 g, u8 b) { rect(x, y, w, 1, r, g, b); rect(x, y + h - 1, w, 1, r, g, b); rect(x, y, 1, h, r, g, b); rect(x + w - 1, y, 1, h, r, g, b); }

// ---- PADS: a touch chord instrument on the bottom screen (in place of the keys and XY pad). One pad per chord of
// the key; where you touch a pad picks the chord: low = triad, middle = 7th, high = 9th; left edge = inversion,
// right edge = sus4. Slide across the STRUM strip to strum the last chord like an autoharp.
static int gPads = 0, gPadSel = -1, gPadN = 0, gPadNotes[4], gStrumZone = -1;
static struct { int note; float t; } gStrumOff[24]; static int gStrumOffN = 0;
#define PAD_Y (TAB_H + 2)
#define PAD_H 120
#define STRUM_Y (PAD_Y + PAD_H + 4)
static const int MAJ7[7] = {0, 2, 4, 5, 7, 9, 11};
static int padDeg(int base, int d) {   // scale degree -> note, using the scale when it has 7 notes, else major
	if (gScale && scaleLen() == 7) return scaleNote(base + gRoot, d);
	int o = d / 7; return base + gRoot + 12 * o + MAJ7[d % 7];
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
static void drawPads(int base) {
	rect(0, TAB_H, 320, 240 - TAB_H, RGB3(TH->bg));
	for (int i = 0; i < 7; i++) {
		int x = 2 + i * 45, w = 43, lit = i == gPadSel;
		rect(x, PAD_Y, w, PAD_H, lit ? TH->keyOn[0] : TH->pad[0], lit ? TH->keyOn[1] : TH->pad[1], lit ? TH->keyOn[2] : TH->pad[2]);
		outline(x, PAD_Y, w, PAD_H, RGB3(TH->line));
		for (int k = 1; k < 3; k++) rect(x + 3, PAD_Y + k * PAD_H / 3, w - 6, 1, RGB3(TH->grid));
		char nm[24]; padName(base, i, nm, sizeof(nm)); char* bar = strchr(nm, '|'); *bar = 0;
		const u8* tc = lit ? EGA[0] : TH->val;
		text(x + w / 2 - (int)strlen(nm) * 4, PAD_Y + 4, nm, 1, tc[0], tc[1], tc[2]);
		text(x + w / 2 - (int)strlen(bar + 1) * 4, PAD_Y + PAD_H - 14, bar + 1, 1, tc[0], tc[1], tc[2]);
		text(x + 4, PAD_Y + PAD_H / 6 - 4, "9", 1, RGB3(TH->dim)); text(x + 4, PAD_Y + PAD_H / 2 - 4, "7", 1, RGB3(TH->dim));
	}
	int sh = 240 - STRUM_Y - 2;
	rect(2, STRUM_Y, 316, sh, RGB3(TH->kbBg));
	for (int z = 0; z < 24; z++) { int x = 2 + z * 316 / 24; rect(x + 6, STRUM_Y + 2, 1, sh - 4, z == gStrumZone ? TH->keyOn[0] : TH->line[0], z == gStrumZone ? TH->keyOn[1] : TH->line[1], z == gStrumZone ? TH->keyOn[2] : TH->line[2]); }
	text(6, STRUM_Y + 3, "STRUM", 1, RGB3(TH->dim));
}
static int padsTouch(int base, int x, int y, int press, float* velOut) {   // 1 = screen changed
	if (y < STRUM_Y - 2) {
		if (!press) return 0;
		int i = (x - 2) / 45; if (i < 0 || i > 6) return 0;
		int ly = y - PAD_Y, band = ly < PAD_H / 3 ? 2 : ly < 2 * PAD_H / 3 ? 1 : 0, lx = x - 2 - i * 45, side = lx < 10 ? 0 : lx > 33 ? 2 : 1;
		gPadSel = i; gPadN = padChord(base, i, band, side, gPadNotes);
		*velOut = 0.55f + 0.45f * (1 - ly / (float)PAD_H) * 0.5f + 0.2f;
		playSet(gPadNotes, gPadN, *velOut);
		return 1;
	}
	if (gPadSel < 0) { gPadSel = 0; gPadN = padChord(base, 0, 0, 1, gPadNotes); }
	int z = (x - 2) * 24 / 316; if (z < 0) z = 0; if (z > 23) z = 23;
	if (z == gStrumZone) return 0;
	gStrumZone = z;
	int nt = gPadNotes[z % gPadN] + 12 * (z / gPadN) - 12;   // walk up the chord tones, octave by octave
	float v = 0.4f + 0.6f * (y - STRUM_Y) / (float)(240 - STRUM_Y);
	voiceOn(nt, v);
	if (gStrumOffN < 24) { gStrumOff[gStrumOffN].note = nt; gStrumOff[gStrumOffN].t = 0.35f; gStrumOffN++; }
	return 1;
}
static void padsTick(float dt) {   // strummed notes ring for a moment, then let go
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
	if (press && y >= 40 && y < 56 && x < 96) { gMotBars = (gMotBars + 1) % 4; return 1; }
	int l = (y - MOT_Y) / MOT_LH; if (y < MOT_Y || l < 0 || l >= NMOT) { gMotPrevI = -1; return 0; }
	int ly = y - MOT_Y - l * MOT_LH, h = MOT_LH - 4;
	if (x < 76) {
		if (!press) return 0;
		if (ly < 16) gMotTgt[l] = (gMotTgt[l] + 1) % NMTGT;
		else if (x < 38) gMotOn[l] = !gMotOn[l];
		else { if (gMotRec == l) gMotRec = -1; else { gMotRec = l; gMotRecArmed = 1; say("MOTION: REC STARTS AT THE NEXT LOOP"); } }
		return 1;
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
	return 1;
}
static int gModView = 0, gModOn = 0, gLastTX = 0, gLastTY = 0;   // view: 0 route, 1 draw LFO 3, 2 draw LFO 4, 3 motion
// ROUTE view, destination first: pick what should move, then add what moves it. Touch + buttons sources are the core
// grid; mic, tilt and camera live behind the SENSORS tab as extras.
static const int RT_CORE[16] = {SRC_LFO1, SRC_LFO2, SRC_LFO3, SRC_LFO4, SRC_ENV2, SRC_VEL, SRC_NOTE, SRC_RAND, SRC_STICK, SRC_XY, SRC_CHAOSX, SRC_CHAOSY, SRC_M1, SRC_M2, SRC_M3, SRC_M4};
static const int RT_SENS[7] = {SRC_TILTX, SRC_TILTY, SRC_MIC, SRC_TWIST, SRC_SHAKE, SRC_CAML, SRC_CAMC};
#define RT_ROWY 60
#define RT_ROWH 18
#define RT_GRIDY 154
static int gRtDst = -1, gRtSens = 0, gRtDrag = -1, gDrawPrevX = -1;
static int rtRows(int dst, int* slots) { int n = 0; for (int m = 0; m < NMOD; m++) if (gMod[m].src != SRC_OFF && gMod[m].dst == dst && n < 5) slots[n++] = m; return n; }
static int rtSlot(int src, int dst) { for (int m = 0; m < NMOD; m++) if (gMod[m].src == src && gMod[m].dst == dst) return m; return -1; }
static void drawRoute(void) {
	char t[40];
	if (gRtDst < 0) {   // destination grid
		for (int d = 0; d < DST_COUNT; d++) {
			int x = 4 + (d % 3) * 105, y = 40 + (d / 3) * 39, n = 0; char srcs[24] = "";
			for (int m = 0; m < NMOD; m++) if (gMod[m].src != SRC_OFF && gMod[m].dst == d) {
				if (!n) snprintf(srcs, sizeof(srcs), "%s", SRC_NAMES[gMod[m].src]);
				n++;
			}
			const u8* bc = n ? TH->tab : TH->pad; rect(x, y, 101, 35, bc[0], bc[1], bc[2]);
			text(x + 50 - (int)strlen(DST_NAMES[d]) * 4, y + 6, DST_NAMES[d], 1, n ? TH->tabTxt[0] : TH->txt[0], n ? TH->tabTxt[1] : TH->txt[1], n ? TH->tabTxt[2] : TH->txt[2]);
			if (n > 1) snprintf(t, sizeof(t), "%d SOURCES", n); else snprintf(t, sizeof(t), "%s", n ? srcs : "-");
			text(x + 50 - (int)strlen(t) * 4, y + 21, t, 1, n ? TH->keyOn[0] : TH->dim[0], n ? TH->keyOn[1] : TH->dim[1], n ? TH->keyOn[2] : TH->dim[2]);
		}
		int used = 0; for (int m = 0; m < NMOD; m++) used += gMod[m].src != SRC_OFF;
		text(114, 200, "TAP WHAT SHOULD MOVE,", 1, RGB3(TH->dim)); text(114, 212, "THEN ADD WHAT MOVES IT", 1, RGB3(TH->dim));
		snprintf(t, sizeof(t), "%d/%d ROUTES USED", used, NMOD); text(114, 224, t, 1, RGB3(TH->dim));
		return;
	}
	rect(4, 40, 52, 16, RGB3(TH->tab)); text(10, 44, "< ALL", 1, RGB3(TH->tabTxt));
	text(62, 44, DST_NAMES[gRtDst], 1, RGB3(TH->val));
	const u8* sc = gRtSens ? TH->keyOn : TH->tab; rect(236, 40, 80, 16, sc[0], sc[1], sc[2]);
	text(244, 44, "SENSORS", 1, gRtSens ? EGA[0][0] : TH->tabTxt[0], gRtSens ? EGA[0][1] : TH->tabTxt[1], gRtSens ? EGA[0][2] : TH->tabTxt[2]);
	int sl[5], n = rtRows(gRtDst, sl);
	if (!n) text(8, RT_ROWY + 30, "NOTHING MOVES THIS YET - ADD A SOURCE BELOW", 1, RGB3(TH->dim));
	for (int r = 0; r < n; r++) {
		int y = RT_ROWY + r * RT_ROWH, m = sl[r]; float a = gMod[m].amt;
		rect(4, y, 74, RT_ROWH - 2, RGB3(TH->pad)); text(8, y + 5, SRC_NAMES[gMod[m].src], 1, RGB3(TH->val));
		rect(82, y, 196, RT_ROWH - 2, RGB3(TH->pad)); rect(180, y, 1, RT_ROWH - 2, RGB3(TH->grid));
		const u8* c = a >= 0 ? TH->keyOn : TH->warn; int w = (int)(a * 98);
		if (w >= 0) rect(180, y + 3, w + 1, RT_ROWH - 8, c[0], c[1], c[2]); else rect(180 + w, y + 3, -w + 1, RT_ROWH - 8, c[0], c[1], c[2]);
		snprintf(t, sizeof(t), "%+d", (int)roundf(a * 100)); text(a >= 0 ? 90 : 240, y + 5, t, 1, RGB3(TH->txt));
		rect(282, y, 34, RT_ROWH - 2, RGB3(TH->tab)); text(295, y + 5, "X", 1, RGB3(TH->tabTxt));
	}
	text(8, RT_GRIDY - 12 + 2, gRtSens ? "ADD A SENSOR (EXTRA)" : "ADD A SOURCE", 1, RGB3(TH->dim));
	int cnt = gRtSens ? 7 : 16;
	for (int i = 0; i < cnt; i++) {
		int src = gRtSens ? RT_SENS[i] : RT_CORE[i], x = 4 + (i % 4) * 79, y = RT_GRIDY + (i / 4) * 21, on = rtSlot(src, gRtDst) >= 0;
		const u8* bc = on ? TH->keyOn : TH->tab; rect(x, y, 75, 18, bc[0], bc[1], bc[2]);
		const u8* tc = on ? EGA[0] : TH->tabTxt; text(x + 37 - (int)strlen(SRC_NAMES[src]) * 4, y + 5, SRC_NAMES[src], 1, tc[0], tc[1], tc[2]);
	}
}
static int routeTouch(int x, int y, int press, int release) {
	if (release) { gRtDrag = -1; return 0; }
	if (gRtDst < 0) {
		if (!press || y < 40) return 0;
		int c = (x - 4) / 105, r = (y - 40) / 39, d = r * 3 + c;
		if (c >= 0 && c < 3 && d >= 0 && d < DST_COUNT) { gRtDst = d; gRtSens = 0; return 1; }
		return 0;
	}
	if (press && y >= 40 && y < 56) {
		if (x < 58) { gRtDst = -1; return 1; }
		if (x > 234) { gRtSens = !gRtSens; return 1; }
		return 0;
	}
	int sl[5], n = rtRows(gRtDst, sl);
	if (y >= RT_ROWY && y < RT_ROWY + 5 * RT_ROWH) {
		int r = (y - RT_ROWY) / RT_ROWH;
		if (press) gRtDrag = -1;
		if (press && r < n && x >= 282) { gMod[sl[r]].src = SRC_OFF; gMod[sl[r]].amt = 0; say("REMOVED"); return 1; }
		if (press && r < n && x >= 82 && x < 280) gRtDrag = sl[r];
	}
	if (gRtDrag >= 0) {   // drag along the bar: centre = 0, right = +100, left = -100
		float a = (x - 180) / 98.0f; if (a > 1) a = 1; if (a < -1) a = -1;
		gMod[gRtDrag].amt = roundf(a * 50) / 50; return 1;
	}
	if (press && y >= RT_GRIDY) {
		int c = (x - 4) / 79, r = (y - RT_GRIDY) / 21, i = r * 4 + c, cnt = gRtSens ? 7 : 16;
		if (c < 0 || c > 3 || i >= cnt) return 0;
		int src = gRtSens ? RT_SENS[i] : RT_CORE[i];
		if (rtSlot(src, gRtDst) >= 0) { say("ALREADY ROUTED - DRAG ITS BAR, OR X TO REMOVE"); return 0; }
		if (n >= 5) { say("5 SOURCES ON ONE DESTINATION IS THE LIMIT"); return 0; }
		int m = -1; for (int k = 0; k < NMOD; k++) if (gMod[k].src == SRC_OFF) { m = k; break; }
		if (m < 0) { say("ALL 8 ROUTES IN USE - REMOVE ONE FIRST"); return 0; }
		gMod[m].src = src; gMod[m].dst = gRtDst; gMod[m].amt = 0.5f; gModCurve[m] = 0; return 1;
	}
	return 0;
}
static void drawModView(void) {
	rect(0, TAB_H, 320, 240 - TAB_H, RGB3(TH->bg));
	static const char* VT[4] = {"ROUTE", "LFO 3", "LFO 4", "MOTION"};
	for (int i = 0; i < 4; i++) {
		int x = 4 + i * 79, lit = i == gModView; rect(x, TAB_H + 2, 75, 14, lit ? TH->keyOn[0] : TH->tab[0], lit ? TH->keyOn[1] : TH->tab[1], lit ? TH->keyOn[2] : TH->tab[2]);
		const u8* c = lit ? EGA[0] : TH->tabTxt; text(x + 37 - (int)strlen(VT[i]) * 4, TAB_H + 5, VT[i], 1, c[0], c[1], c[2]);
	}
	if (gModView == 3) { drawMotion(); return; }
	if (gModView == 0) { drawRoute(); return; }
	{
		int k = gModView - 1, x0 = 8, y0 = 46, w = 304, h = 150;
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
	if (press && y < TAB_H + 18) { int v = (x - 4) / 79; if (v >= 0 && v < 4) { gModView = v; gRtDrag = -1; } return 1; }
	if (gModView == 3) return motionTouch(x, y, press, release);
	if (gModView == 0) return routeTouch(x, y, press, release);
	int k = gModView - 1;
	if (press && y >= 202 && y < 218) {   // shape presets
		int b = (x - 8) / 61;
		for (int i = 0; i < 64; i++) {
			float v = b == 0 ? sinf(TWO_PI * i / 64) : b == 1 ? 1 - i / 31.5f : b == 2 ? ((i / 8) % 2 ? 0.8f : -0.8f) * ((i / 16) % 2 ? 0.5f : 1) : b == 3 ? (rand() % 2001) / 1000.0f - 1 : 0;
			if (b == 3 && i % 4) v = gDl[k][i - 1];
			gDl[k][i] = v;
		}
		return 1;
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
	return 1;
}
static void drawBottom(int base, const u8* on, int helpOn, int latch) {
	(void)helpOn;
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, KB_Y, BG);
	static const char* TABS[7] = {"PADS", "<PAGE", "PAGE>", "OCT-", "OCT+", "LATCH", "SEQ"};
	for (int i = 0; i < 7; i++) {
		int x = i * 320 / 7, lit = (i == 0 && gPads) || (i == 5 && latch) || (i == 6 && gGridOn);
		int w = strlen(TABS[i]) * 8, bw = 320 / 7 - 4;
		if (TH->ega) {
			if (lit) { rect(x + 2, 1, bw, TAB_H - 3, HI); text(x + 160 / 7 - w / 2, 6, TABS[i], 1, HITXT); }
			else { outline(x + 2, 1, bw, TAB_H - 3, LINE); text(x + 160 / 7 - w / 2, 6, TABS[i], 1, TXT); }
		} else {
			if (lit) { rect(x + 2, 1, bw, TAB_H - 2, HI); text(x + 160 / 7 - w / 2, 7, TABS[i], 1, HITXT); }
			else { rect(x + 2, 1, bw, TAB_H - 2, RGB3(TH->tab)); text(x + 160 / 7 - w / 2, 7, TABS[i], 1, RGB3(TH->tabTxt)); }
		}
	}
	if (gPads && !gGridOn) { drawPads(base); return; }   // chord pads take the whole screen under the tabs
	if (gModOn) { drawModView(); return; }               // MOD page: route / drawn LFOs / motion
	if (gGridOn) drawGrid(); else if (gAddEdit) drawAdd(); else {
	// XY pad: dotted EGA grid, crosshair
	float xv = gXyMode ? gMacro[gXyMode * 2 - 2] : gOsc[0].pos, yv = gXyMode ? gMacro[gXyMode * 2 - 1] : gCutoff;
	int cx = 4 + (int)(xv * 311), cy = XY_Y + (int)((1 - yv) * (XY_H - 1));
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
	static const char* XL[3] = {"WT POS >", "MACRO 1 >", "MACRO 3 >"}; static const char* YL[3] = {"^ CUTOFF", "^ MACRO 2", "^ MACRO 4"};
	text(8, XY_Y + 4, XL[gXyMode], 1, DIM); text(312 - (int)strlen(YL[gXyMode]) * 8, XY_Y + XY_H - 12, YL[gXyMode], 1, DIM);
	}
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
static void beatLightTick(float dt) {   // brightness jump on each arp / sequencer step, then back
	if (!gLcdOK) return;
	if (gLcdFlash && (gLightT -= dt) <= 0) { GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, gBrightTop); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, gBrightBot); gLcdFlash = 0; }
}
static void beatLightPulse(void) {
	if (!gLcdOK || !gBeatLight) return;
	u32 t = gBrightTop <= 3 ? gBrightTop + 2 : gBrightTop - 2, b = gBrightBot <= 3 ? gBrightBot + 2 : gBrightBot - 2;
	GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, t); GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM, b);
	gLcdFlash = 1; gLightT = 0.07f;
}
static void motionTick(float dt) {
	angularRate g; hidGyroRead(&g);
	float tw = g.z / 1800.0f; if (fabsf(tw) < 0.04f) tw = 0;
	if (tw > 1) tw = 1;
	if (tw < -1) tw = -1;
	float k = 1.0f - expf(-dt / 0.06f); gTwist += (tw - gTwist) * k;
	static accelVector pa; static int have = 0;
	accelVector a; hidAccelRead(&a);
	if (have) {
		float d = (fabsf((float)a.x - pa.x) + fabsf((float)a.y - pa.y) + fabsf((float)a.z - pa.z)) / 600.0f;
		if (d > 1) d = 1;
		gShake += (d - gShake) * (d > gShake ? 0.7f : 0.05f);
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
	if (v->chipCh < 0) return;
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
static float gCsEcho = 0, gCsEchoFb = 0.45f; static int gCsEchoT = 2;
static const char* CSECHO_T_NAMES[4] = {"1/8", "3/16", "1/4", "3/8"};
static const float CSECHO_BEATS[4] = {0.5f, 0.75f, 1.0f, 1.5f};
static int gEchoUnits[2] = {-1, -1}, gEchoCh[2] = {-1, -1}, gEchoOn = 0, gEchoLen = 0, gEchoBpm = 0;
static s16* gEchoBuf[2];
static int echoReserved(int c) { return gEchoOn && (c == gEchoCh[0] || c == gEchoCh[1]); }
static void csWait(void) { csndExecCmds(false); svcSleepThread(6000000); }   // never spin on the chip's done flag: a wedged chip would hang Syrup forever
static u32 csSend(float l, float r) {   // capture volumes for sounds that feed the chip echo
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
	for (int c = NCS - 3; c > 18 && n < 2; c--) if (csndChannels & BIT(c)) { if (gCsOwner[c] >= 0) stopCh(BK_CSND, c); gEchoCh[n++] = c; }   // below the looper's two, above the drums
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
	int want = gCsEcho > 0.001f && gCsndOK;
	static int pitchWas = 0;
	if (want && (!gEchoOn || gEchoBpm != gBPM || pitchWas != gCsEchoPitch)) { pitchWas = gCsEchoPitch; echoStart(); }
	else if (!want && gEchoOn) echoStop();
	static float lastFb = -1;
	if (gEchoOn && lastFb != gCsEchoFb) {
		lastFb = gCsEchoFb; u32 fb = (u32)(gCsEchoFb * 0x7FFF);
		CSND_SetVol(gEchoCh[0], 0x5000, fb << 16); CSND_SetVol(gEchoCh[1], 0x5000u << 16, fb); csndExecCmds(false);
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
#define AD_MAX (32728 * 48)
#define AD_BLK 1024
static u8* gAd[2]; static Ima gAdBlk[2][AD_MAX / AD_BLK + 1];   // state at each 1024-sample block (random access for grains)
static Ima gAdEnc[2]; static int gAdPos = 0;
static int adpcmReady(void) {
	for (int k = 0; k < 2; k++) if (!gAd[k] && !(gAd[k] = (u8*)linearAlloc(AD_MAX / 2 + 16))) return 0;
	return 1;
}
static void adPush(int k, const s16* in, int stride, int n) {   // encode n samples onto loop channel k
	for (int i = 0; i < n; i++) {
		int p = gAdPos + i; if (p >= AD_MAX) return;
		if (p % AD_BLK == 0) gAdBlk[k][p / AD_BLK] = gAdEnc[k];
		int c = imaEnc(&gAdEnc[k], in[i * stride]);
		if (p & 1) gAd[k][p >> 1] |= c << 4; else gAd[k][p >> 1] = c;
	}
}
static float adSample(int k, int pos) {   // random access (grains): decode from the nearest block start
	static int cacheP[2] = {-1, -1}; static Ima cs[2];   // sequential reads decode one nibble each
	if (pos < cacheP[k] || cacheP[k] < 0 || pos - cacheP[k] > AD_BLK) { cs[k] = gAdBlk[k][pos / AD_BLK]; cacheP[k] = pos - pos % AD_BLK; }
	while (cacheP[k] < pos) { int b = gAd[k][cacheP[k] >> 1]; imaDec(&cs[k], (cacheP[k] & 1) ? b >> 4 : b & 15); cacheP[k]++; }
	int b = gAd[k][pos >> 1]; Ima t = cs[k]; return imaDec(&t, (pos & 1) ? b >> 4 : b & 15) / 32768.0f;
}

static void echoTick(void) { echoTickL(); }   // called inside the main loop's voice lock, so CSND commands never overlap the audio thread

// ---- looper: the DSP's own output tap (ndsp capture of the final mix) streams into a small ring; the main
// thread packs it to IMA-ADPCM as it arrives, and two second-chip channels play the ADPCM back (decoded in hardware)
static const char* LOOP_NAMES[3] = {"OFF", "REC", "PLAY"};
static const char* BARS_NAMES[5] = {"1 BAR", "2 BARS", "4 BARS", "8 BARS", "16 BARS"};
static int gLoopMode = 0, gLoopBars = 0, gLoopLen = 0, gLoopHave = 0, gLoopCh[2] = {-1, -1};
static float gLoopLevel = 0.8f;
#define CAPRING (32728 * 2 - (32728 * 2) % 160)
static ndspWaveBuf gCapWB;
static s16* gCapBuf = NULL;
static u32 gCapLast = 0;
static int loopReserved(int c) { return gLoopHave && gLoopMode == 2 && (c == gLoopCh[0] || c == gLoopCh[1]); }
static void loopStopPlay(void) {
	if (!gCsndOK) return;
	for (int k = 0; k < 2; k++) if (gLoopCh[k] >= 0) CSND_SetPlayState(gLoopCh[k], 0);
	csndExecCmds(false);
}
static void loopStartPlay(void) {
	if (!gCsndOK || !gLoopHave) { say("LOOPER NEEDS THE SECOND SOUND CHIP"); return; }
	if (gLoopCh[0] < 0) { int n = 0; for (int c = NCS - 1; c >= 0 && n < 2; c--) if (gCsndOK && (csndChannels & BIT(c))) gLoopCh[n++] = c; }
	if (gLoopCh[1] < 0) return;
	for (int k = 0; k < 2; k++) {
		int c = gLoopCh[k]; stopCh(BK_CSND, c);
		u32 flags = SOUND_FORMAT_ADPCM | SOUND_REPEAT | SOUND_LINEAR_INTERP | SOUND_ENABLE | SOUND_CHANNEL(c) | (CSND_TIMER(32728) << 16);
		u32 l = (u32)(gLoopLevel * 0x8000), vol = k ? (l << 16) : l;   // L buffer hard left, R buffer hard right
		CSND_SetAdpcmState(c, 0, gAdBlk[k][0].pred, gAdBlk[k][0].idx);
		CSND_SetAdpcmState(c, 1, gAdBlk[k][0].pred, gAdBlk[k][0].idx);
		CSND_SetAdpcmReload(c, true);
		CSND_SetChnRegs(flags, osConvertVirtToPhys(gAd[k]), osConvertVirtToPhys(gAd[k]), gLoopLen / 2, vol, 0);
	}
	csndExecCmds(false);
}
static void loopRecStart(void) {
	if (!gCapBuf) gCapBuf = (s16*)linearAlloc(CAPRING * 4);
	if (!gCapBuf || !adpcmReady()) { say("NOT ENOUGH MEMORY FOR THE LOOPER"); gLoopMode = 0; return; }
	loopStopPlay();
	int bars = 1 << gLoopBars;
	int len = (int)(bars * 4 * 60.0f / gBPM * 32728.0f); if (len > AD_MAX) len = AD_MAX;
	len -= len % 160; if (len < 160 * 8) len = 160 * 8;
	memset(&gCapWB, 0, sizeof(gCapWB));
	gCapWB.data_pcm16 = gCapBuf; gCapWB.nsamples = CAPRING; gCapWB.offset = 0;
	gLoopLen = len; gCapLast = 0; gAdPos = 0; gLoopHave = 0;
	for (int k = 0; k < 2; k++) { gAdEnc[k].pred = 0; gAdEnc[k].idx = 0; }
	ndspSetCapture(&gCapWB);
	char m[40]; snprintf(m, sizeof(m), "LOOPER RECORDING %d S", len / 32728); say(m);
}
static void loopTick(void) {   // main thread: pack whatever the capture wrote since last frame
	if (gLoopMode != 1) return;
	u32 off = gCapWB.offset;
	int n = (int)off - (int)gCapLast; if (n < 0) n += CAPRING;
	if (n <= 0) return;
	DSP_InvalidateDataCache(gCapBuf, CAPRING * 4);
	while (n > 0 && gAdPos < gLoopLen) {
		int run = CAPRING - gCapLast; if (run > n) run = n;
		if (run > gLoopLen - gAdPos) run = gLoopLen - gAdPos;
		for (int k = 0; k < 2; k++) adPush(k, gCapBuf + gCapLast * 2 + k, 2, run);
		gAdPos += run; gCapLast = (gCapLast + run) % CAPRING; n -= run;
	}
	gCapLast = off;
	if (gAdPos >= gLoopLen) {
		ndspSetCapture(NULL);
		if (gCsndOK) for (int k = 0; k < 2; k++) CSND_FlushDataCache(gAd[k], gLoopLen / 2);
		gLoopHave = 1; gLoopMode = 2; loopStartPlay(); say("LOOP PLAYING");
	}
}
static void loopLevelTick(void) {   // keep the loop volume in step with LOOP LEVEL
	static float last = -1;
	if (!gCsndOK || gLoopMode != 2 || !gLoopHave || gLoopCh[1] < 0 || last == gLoopLevel) return;
	last = gLoopLevel; u32 l = (u32)(gLoopLevel * 0x8000);
	CSND_SetVol(gLoopCh[0], l, 0); CSND_SetVol(gLoopCh[1], l << 16, 0); csndExecCmds(false);
}
static void loopModeChanged(void) {
	if (gLoopMode == 1) loopRecStart();
	else if (gLoopMode == 2) { if (gLoopHave) loopStartPlay(); else { say("RECORD A LOOP FIRST"); gLoopMode = 0; } }
	else { ndspSetCapture(NULL); loopStopPlay(); }
}
static void loopToTable(void) {
	if (!gLoopHave) { say("RECORD A LOOP FIRST"); return; }
	int n = gLoopLen > 32728 * 12 ? 32728 * 12 : gLoopLen;
	float* in = (float*)malloc(sizeof(float) * n); float* fr = (float*)malloc(sizeof(float) * CYC * WT_FR);
	if (in && fr) { for (int i = 0; i < n; i++) in[i] = adSample(0, i) * 0.5f; for (int i = 0; i < n; i++) in[i] += adSample(1, i) * 0.5f; audioToFrames(in, n, 32728, fr); wtInstall("LOOP", fr); }
	free(in); free(fr);
}

// ---- grain engine: the DSP plays short windowed slices from a source, two channels offset by half a grain;
// the CPU only refills a slice once the DSP has finished with it (so the DSP sets the pace).
// GRAINS LOOP plays the source by itself; GRAINS KEYS makes it an oscillator: pitch from the keys, loudness
// from the voice envelope, and with the TABLE source the position follows OSC A's WT POS (and its mods).
static int gGrains = 0, gGrainSrc = 1; static float gGrainSize = 0.35f, gGrainScan = 0.5f, gGrainSpray = 0.2f, gGrainPitch = 0, gGrainLevel = 0.8f;
static const char* GRAIN_MODES[3] = {"OFF", "LOOP", "KEYS"};
static const char* GRAIN_SRCS[3] = {"LOOP", "TABLE", "MIC"};
#define GR_CH0 21
#define GR_N 4
#define GR_MAX 8192
static s16* gGrBuf[GR_N]; static ndspWaveBuf gGrWB[GR_N]; static s16* gGrPad = NULL; static ndspWaveBuf gGrPadWB;
static float gGrPos = 0;
static s16* gGrSrcBuf = NULL; static int gGrSrcLen = 0, gGrSrcRate = 32728, gGrSrcKind = -1; static char gGrSrcTab[40] = "";
#define GR_SRCMAX (32728 * 16)
static int grainReserved(int c) { return gGrains && (c == GR_CH0 || c == GR_CH0 + 1); }
static int grainBuild(void) {   // (re)make the source buffer for the chosen source; 0 = nothing to play yet
	if (!gGrSrcBuf && !(gGrSrcBuf = (s16*)linearAlloc(GR_SRCMAX * 2))) return 0;
	if (gGrainSrc == 0) {
		if (!gLoopHave) return 0;
		int n = gLoopLen > GR_SRCMAX ? GR_SRCMAX : gLoopLen;
		for (int i = 0; i < n; i++) { float v = (adSample(0, i) + adSample(1, i)) * 0.5f * 32000; gGrSrcBuf[i] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
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
	gGrSrcKind = gGrainSrc;
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
		if (gGrains == 2 && gGrainSrc == 1) {   // TABLE in KEYS: position = OSC A's WT POS (with mods) plus SCAN drift
			static float drift = 0; drift += len * 0.5f * (gGrainScan * 2 - 1); while (drift < 0) drift += L; while (drift >= L) drift -= L;
			gGrPos = gPosEff[0] * (L - len) + (gGrainScan != 0.5f ? drift : 0);
		} else gGrPos += len * 0.5f * (gGrainScan * 2 - 1);   // SCAN: 50% = frozen, above = forwards, below = backwards
		while (gGrPos < 0) gGrPos += L;
		while (gGrPos >= L) gGrPos -= L;
		int st = (int)gGrPos + (int)((rand() % 1000 / 1000.0f - 0.5f) * gGrainSpray * gGrSrcRate);
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
	if (!grainBuild()) { say(gGrainSrc == 0 ? "GRAIN SOURCE LOOP: RECORD A LOOP FIRST" : "GRAIN SOURCE MIC: MIC NOT AVAILABLE"); return; }
	if (!gGrPad && (gGrPad = (s16*)linearAlloc(GR_MAX))) { memset(gGrPad, 0, GR_MAX); DSP_FlushDataCache(gGrPad, GR_MAX); }
	if (gGrPad) {   // start the second stream half a grain late so the two overlap evenly
		memset(&gGrPadWB, 0, sizeof(gGrPadWB)); gGrPadWB.data_vaddr = gGrPad; gGrPadWB.nsamples = (int)(512 + gGrainSize * 7000) / 2;
		ndspChnSetRate(GR_CH0 + 1, gGrSrcRate); ndspChnWaveBufAdd(GR_CH0 + 1, &gGrPadWB);
	}
	if (gGrainSrc == 2) say("GRAINS: GRABBED THE LAST SECOND OF MIC (PICK MIC AGAIN TO REGRAB)");
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
	rect(0, XY_Y, 320, KB_Y - XY_Y, RGB3(TH->bg));
	text(4, XY_Y + 2, "DRAW HARMONICS 1-24  (OSC A, LIVE)", 1, DIM);
	for (int h = 0; h < 24; h++) {
		int x = 4 + h * 13, top = XY_Y + 14, hgt = KB_Y - 4 - top, v = (int)(gAddH[h] * hgt);
		rect(x, top, 11, hgt, RGB3(TH->pad));
		if (v > 0) rect(x, top + hgt - v, 11, v, (h & 3) == 0 ? TH->val[0] : TH->line[0], (h & 3) == 0 ? TH->val[1] : TH->line[1], (h & 3) == 0 ? TH->val[2] : TH->line[2]);
	}
}
static void addTouch(int x, int y) {
	int h = (x - 4) / 13; if (h < 0 || h > 23) return;
	int top = XY_Y + 14, hgt = KB_Y - 4 - top; float v = 1 - (y - top) / (float)hgt; if (v < 0) v = 0; if (v > 1) v = 1;
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
static void midiParse(const u8* p, int n, u8* run);
static volatile int gIrMidiBytes = 0;
static void irMidiMain(void* a) {
	(void)a; u8 run = 0;
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
static void midiPush(u8 a, u8 b, u8 c) { int n = (gMqW + 1) % MQ; if (n == gMqR) return; gMq[gMqW][0] = a; gMq[gMqW][1] = b; gMq[gMqW][2] = c; __sync_synchronize(); gMqW = n; }
static int midiLen(u8 st) {   // data bytes after a status byte
	if (st >= 0xF8) return 0;
	switch (st & 0xF0) { case 0xC0: case 0xD0: return 1; case 0xF0: return st == 0xF2 ? 2 : (st == 0xF1 || st == 0xF3) ? 1 : 0; default: return 2; }
}
// raw MIDI byte stream (WebSocket frames); running status allowed, sysex skipped
static void midiParse(const u8* p, int n, u8* run) {
	int i = 0;
	while (i < n) {
		u8 st = p[i];
		if (st == 0xF0) { while (i < n && p[i] != 0xF7) i++; i++; continue; }
		if (st & 0x80) { i++; if (st < 0xF0) *run = st; } else st = *run;
		if (!st) { i++; continue; }
		int k = midiLen(st); if (i + k > n) return;
		midiPush(st, k > 0 ? p[i] : 0, k > 1 ? p[i + 1] : 0); i += k;
	}
}

// FM-1 LINK timed packets: "SL" 1, then records of a 4-byte sender time (24 MHz ticks) + one MIDI message. Each
// message is played at its sender time + a learned clock offset + NET BUFFER, from the audio tick: the network's
// wobble becomes one steady delay.
#define TQN 512
static struct { u64 due; u8 st, a, b; } gTq[TQN];
static volatile int gTqW = 0, gTqR = 0;
static inline u64 nowUs(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000); }
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
typedef struct { int s, ws; char buf[2048]; int n; u8 run, closing; u64 t; } Cli;   // t: last activity (ms)
static void cliClose(Cli* c) { if (c->s >= 0) close(c->s); c->s = -1; c->ws = 0; c->n = 0; c->closing = 0; }
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
		return;
	}
	if (!strncmp(c->buf, "GET /favicon", 12)) {   // browsers ask for an icon: answer quickly with nothing
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
		if (op == 9) { u8 hd[2] = {0x8A, (u8)len}; if (len < 126) { sendAll(c->s, hd, 2); sendAll(c->s, d, len); } }
		if (op == 1 || op == 2) midiParse(d, len, &c->run);
		int used = hl + len; memmove(c->buf, c->buf + used, c->n - used); c->n -= used;
	}
}
// ---- AppleMIDI / RTP-MIDI (Network MIDI on Mac, iOS apps, rtpMIDI on Windows, many DAWs)
static u32 gSsrc = 0x53595255;
static u64 netNow(void) { return osGetTime() * 10; }   // 100 us units
static void amReply(int s, struct sockaddr_in* a, const u8* pk, int n) {
	if (n < 4) return;
	if (pk[2] == 'I' && pk[3] == 'N' && n >= 16) {   // invitation -> accept
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
	} else if (pk[2] == 'B' && pk[3] == 'Y') gNetPeer[0] = 0;
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
	int uc = udpOpen(5004), ud = udpOpen(5005), ur = udpOpen(5008);   // 5008: plain MIDI bytes (FM-1 LINK and friends)
	int ub = socket(AF_INET, SOCK_DGRAM, 0); u64 beaconT = 0; u8 rawRun = 0;
	if (ub >= 0) { int on = 1; setsockopt(ub, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)); }
	static Cli cl[NCLI]; for (int i = 0; i < NCLI; i++) { cl[i].s = -1; cl[i].ws = 0; cl[i].n = 0; cl[i].run = 0; cl[i].closing = 0; cl[i].t = 0; }
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
			if (slot < 0) close(s); else { cl[slot].s = s; cl[slot].ws = 0; cl[slot].n = 0; cl[slot].run = 0; cl[slot].closing = 0; cl[slot].t = osGetTime(); }
			idle = 0;
		}
		for (int i = 0; i < NCLI; i++) {
			Cli* c = &cl[i]; if (c->s < 0) continue;
			if (!c->ws && osGetTime() - c->t > (c->closing ? 3000 : 10000)) { cliClose(c); continue; }
			if (c->closing) { char junk[256]; int k = recv(c->s, junk, sizeof(junk), 0); if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) cliClose(c); continue; }
			int k = recv(c->s, c->buf + c->n, sizeof(c->buf) - 1 - c->n, 0);
			if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { if (c->ws) gNetClients--; cliClose(c); continue; }
			if (k < 0) continue;
			c->n += k; idle = 0; c->t = osGetTime();
			if (c->ws) cliWs(c);
			else { c->buf[c->n] = 0; if (strstr(c->buf, "\r\n\r\n")) cliHttp(c); else if (c->n >= (int)sizeof(c->buf) - 1) cliClose(c); }
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
static int gMidiHeld[16], gMidiHeldN = 0, gMidiClk = 0; static float gMidiBend = 0, gMidiVel = 0.8f;
static u64 gMidiClkT = 0;
static void midiNoteOn(int n, float v) {
	if (gMidiHeldN < 16) gMidiHeld[gMidiHeldN++] = n;
	gMidiVel = v;
	if (gArp) arpSetPool(gMidiHeld, gMidiHeldN, v); else voiceOn(n, v);
}
static void midiNoteOff(int n) {
	for (int i = 0; i < gMidiHeldN; i++) if (gMidiHeld[i] == n) { gMidiHeld[i] = gMidiHeld[--gMidiHeldN]; break; }
	if (gArp) arpSetPool(gMidiHeld, gMidiHeldN, gMidiVel); else voiceOff(n);
}
static void acidGen(void);
// one MIDI message: inTick = from the audio tick (only light work there; patch changes etc. go to the main loop)
static int midiApply(u8 st, u8 a, u8 b, int inTick) {
	int ch = 0;
	if (inTick && ((st & 0xF0) == 0xC0 || ((st & 0xF0) == 0xB0 && a >= 102 && a <= 104))) { midiPush(st, a, b); return 0; }
	switch (st & 0xF0) {
	case 0x90: if (b) { midiNoteOn(a, 0.25f + 0.75f * b / 127.0f); break; } /* fall through: velocity 0 = off */
	case 0x80: midiNoteOff(a); break;
	case 0xE0: gMidiBend = (((b << 7) | a) - 8192) / 8192.0f; break;
	case 0xC0: { int c = presetCount(gStyle); if (c > 0) { gPreset = a % c; presetLoad(gStyle, gPreset); ch = 1; } break; }
	case 0xB0: {
		float v = b / 127.0f; ch = 1;
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
static int midiTick(void) {   // returns 1 when something on screen changed
	int ch = tqDrain(0) | gTqUi; gTqUi = 0;
	while (gMqR != gMqW) {
		u8 st = gMq[gMqR][0], a = gMq[gMqR][1], b = gMq[gMqR][2]; gMqR = (gMqR + 1) % MQ;
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
static volatile u32 gUpGot = 0, gUpTotal = 0; static volatile int gUpSd = 0;
static char gUpVer[12];
#ifndef SYRUP_NO_NET
typedef struct { Handle cia; FILE* f; u64 off; Result err; } UpSink;
static size_t upWrite(void* d, size_t sz, size_t nm, void* u) {
	UpSink* k = (UpSink*)u; u32 n = sz * nm, w = 0;
	if (k->f) { w = fwrite(d, 1, n, k->f); }
	else { Result rr = FSFILE_Write(k->cia, &w, k->off, d, n, FS_WRITE_FLUSH); if (R_FAILED(rr)) { k->err = rr; return 0; } }
	k->off += w; gUpGot = (u32)k->off; return w;
}
static int upProgress(void* c, curl_off_t dlt, curl_off_t dl, curl_off_t ult, curl_off_t ul) { (void)c; (void)dl; (void)ult; (void)ul; if (dlt > 0) gUpTotal = (u32)dlt; return 0; }
static void upMain(void* arg) {
	(void)arg;
	if (!netUp()) { snprintf(gDlErr, sizeof(gDlErr), "WI-FI NOT AVAILABLE"); gUpState = 5; return; }
	curl_global_init(CURL_GLOBAL_DEFAULT);
	MemBuf v = {0};
	int ok = httpGet(REL_URL "version.txt", &v, 0);
	if (ok && v.p) {
		snprintf(gUpVer, sizeof(gUpVer), "%.7s", v.p);
		if (!strncmp(gUpVer, SYRUP_VER, 7)) gUpState = 4;
		else {
			gUpState = 2; gUpGot = gUpTotal = 0;
			int hb = envIsHomebrew();   // .3dsx from the Homebrew Launcher: replace the file; installed CIA: reinstall through AM
			for (int attempt = 0; attempt < 2; attempt++) {   // attempt 1 (CIA only): save the CIA to SD for FBI
				int toSd = hb || attempt == 1;
				UpSink k = {0}; ok = 1; gUpGot = gUpTotal = 0;
				const char* tmp = hb ? "sdmc:/3ds/syrup.3dsx.part" : "sdmc:/cias/syrup.cia.part";
				if (toSd) { mkdir("sdmc:/cias", 0777); k.f = fopen(tmp, "wb"); if (!k.f) { snprintf(gDlErr, sizeof(gDlErr), "CAN'T WRITE TO SD"); ok = 0; } }
				else {
					Result rr = amInit(); if (R_SUCCEEDED(rr)) rr = AM_StartCiaInstall(MEDIATYPE_SD, &k.cia);
					if (R_FAILED(rr)) { snprintf(gDlErr, sizeof(gDlErr), "INSTALLER REFUSED %08lX", (unsigned long)rr); ok = 0; }
				}
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
					if (!ok) {
						if (k.err) snprintf(gDlErr, sizeof(gDlErr), "INSTALL WRITE %08lX", (unsigned long)k.err);
						else if (r != CURLE_OK) snprintf(gDlErr, sizeof(gDlErr), "%s (%d)", curlWords((int)r), (int)r);
						else snprintf(gDlErr, sizeof(gDlErr), "SERVER SAID %ld", code);
					}
				}
				if (toSd) {
					const char* fin = hb ? "sdmc:/3ds/syrup.3dsx" : "sdmc:/cias/syrup.cia";
					if (k.f) fclose(k.f);
					if (ok) { remove(fin); rename(tmp, fin); } else remove(tmp);
					if (ok && !hb) gUpSd = 1;
				} else if (k.cia) {
					if (ok) { Result rr = AM_FinishCiaInstall(k.cia); if (R_FAILED(rr)) { snprintf(gDlErr, sizeof(gDlErr), "INSTALL FAILED %08lX", (unsigned long)rr); ok = 0; } }
					else AM_CancelCIAInstall(k.cia);
				}
				if (!toSd) amExit();
				if (ok || hb) break;
				// direct install failed: keep the reason, then fall back to saving the CIA for FBI
			}
			gUpState = ok ? 3 : 5;
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
	case 3: if (gUpSd) snprintf(m, sizeof(m), "%s SAVED: FBI > SD > cias > syrup.cia", gUpVer); else snprintf(m, sizeof(m), "UPDATED TO %s - CLOSE AND REOPEN SYRUP", gUpVer); say(m); gMsgT = 30; gUpState = 0; gUpSd = 0; break;
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
	snprintf(s, sizeof(s), "LOOPER %s", LOOP_NAMES[gLoopMode]); text(10, 74, s, 1, TXT);
	rect(10, 86, 184, 6, RGB3(TH->pad));
	if (gLoopMode == 1 && gLoopLen) rect(10, 86, (int)(184.0f * gCapWB.offset / gLoopLen), 6, WARN);
	else if (gLoopHave) rect(10, 86, 184, 6, DIM);
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
}

// ================================================================ drums: synthesised kit on 3 CSND channels, pattern presets, tempo-locked
static int gDrums = 0, gKit = 0, gKickPat = 1, gSnarePat = 1, gHatPat = 1, gDrumBuilt = -1;
static float gDrumLevel = 0.8f;
static int gDrumCh[3] = {-1, -1, -1}, gDrumStep = -1;
static float gDrumPh = 0;
static s16* gDrumSmp[3]; static int gDrumLen[3];
static const char* KIT_NAMES[4] = {"808", "909", "LOFI", "CHIP"};
static const char* KICKP_NAMES[9] = {"OFF", "FOUR FLOOR", "BREAK", "TRAP", "DNB", "HALFTIME", "BOOM BAP", "MINE", "EUCLID"};
static const char* SNAREP_NAMES[8] = {"OFF", "BACKBEAT", "BREAK", "TRAP", "DNB", "CLAP 2+4", "MINE", "EUCLID"};
static const char* HATP_NAMES[8] = {"OFF", "8THS", "16THS", "OFFBEAT", "TRAP ROLL", "SHUFFLE", "MINE", "EUCLID"};
static const u16 KICKP[8] = {0, 0x8888, 0x8224, 0x8022, 0x8020, 0x8000, 0x8124, 0};
static const u16 SNAREP[7] = {0, 0x0808, 0x0849, 0x0080, 0x0808, 0x0808, 0};
static const u16 HATP[7] = {0, 0xAAAA, 0xFFFF, 0x2222, 0xFDEF, 0x9249, 0};
// MINE: your own pattern, a velocity (0 = rest) per step per drum, recorded by tapping on the mic
static u8 gMyDrum[3][16];
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
static const char* DREC_NAMES[5] = {"OFF", "TAP KICK", "TAP SNARE", "TAP HAT", "CLEAR MINE"};
static float gTapLevel = 0, gTapHold = 0;
static const u8 HATVEL[16] = {255, 120, 200, 110, 255, 120, 200, 140, 255, 120, 200, 110, 255, 130, 220, 160};
static int gNzCh[2] = {-1, -1}; static float gNzVol[2];   // hardware noise drums: retired (they wedged the sound chip); CHIP kit is now sampled LFSR noise
static int drumReserved(int c) { return gDrums && (c == gDrumCh[0] || c == gDrumCh[1] || c == gDrumCh[2]); }
static u32 csSend(float l, float r);

static void drumBuild(int kit) {   // synthesise kick / snare / hat into linear memory for CSND
	const float sr = 32728;
	int len[3] = {(int)(sr * 0.6f), (int)(sr * 0.3f), (int)(sr * (kit == 1 ? 0.12f : 0.07f))};
	unsigned seed = 1234;
	unsigned lfsr = 0x7FFF; float lph = 0;   // CHIP kit: Game Boy style noise register
	for (int d = 0; d < 3; d++) {
		if (!gDrumSmp[d]) gDrumSmp[d] = (s16*)linearAlloc(sr * 0.6f * 2 + 64);
		if (!gDrumSmp[d]) return;
		gDrumLen[d] = len[d];
		float ph = 0, lp = 0, hp = 0, prev = 0;
		for (int i = 0; i < len[d]; i++) {
			float t = i / sr, v = 0;
			seed = seed * 1664525u + 1013904223u; float nz = ((seed >> 9) & 0xFFFF) / 32768.0f - 1;
			if (d == 0) {   // kick: falling sine, a click, kit-shaped decay
				float f0 = kit == 1 ? 60 : (kit == 2 ? 50 : 45), f1 = kit == 3 ? 400 : 160, dec = kit == 0 ? 2.2f : (kit == 1 ? 5 : 4);
				float f = f0 + (f1 - f0) * expf(-t * 35);
				ph += f / sr; v = sinf(TWO_PI * ph) * expf(-t * dec) + (t < 0.004f ? nz * 0.4f : 0);
				if (kit == 3) v = v > 0 ? 0.8f : -0.8f, v *= floorf(expf(-t * 6) * 15) / 15;
				if (kit == 2) v = tanhf(v * 2.5f) * 0.8f;
			} else if (d == 1) {   // snare: body tone + filtered noise
				float body = sinf(TWO_PI * 185 * t) * expf(-t * 22);
				lp += (nz - lp) * (kit == 2 ? 0.25f : 0.55f);
				float sn = (nz - lp * 0.6f) * expf(-t * (kit == 1 ? 13 : 17));
				v = body * (kit == 0 ? 0.7f : 0.45f) + sn * (kit == 3 ? 0.3f : 0.8f);
				if (kit == 3) {   // 15-bit LFSR clocked at 9 kHz, 4-bit stepped decay
					lph += 9000.0f / sr; while (lph >= 1) { lph -= 1; unsigned b = (lfsr ^ (lfsr >> 1)) & 1; lfsr = (lfsr >> 1) | (b << 14); }
					float e = floorf(expf(-t * 15) * 15) / 15; v = ((lfsr & 1) ? 0.6f : -0.6f) * e + (t < 0.03f ? (fmodf(t * 220, 1) < 0.5f ? 0.3f : -0.3f) * e : 0);
				}
			} else {   // hat: high-passed noise, very short
				hp = nz - prev; prev = nz;
				v = hp * expf(-t * (kit == 1 ? 30 : 55)) * 0.7f;
				if (kit == 2) v = roundf(v * 8) / 8;
				if (kit == 3) {   // short 7-bit LFSR mode at 30 kHz: the metallic chip hat
					lph += 30000.0f / sr; while (lph >= 1) { lph -= 1; unsigned b = (lfsr ^ (lfsr >> 1)) & 1; lfsr = ((lfsr >> 1) & ~0x40u) | (b << 6) | (b << 14); }
					v = ((lfsr & 1) ? 0.45f : -0.45f) * (floorf(expf(-t * 60) * 15) / 15);
				}
			}
			if (v > 1) v = 1;
			if (v < -1) v = -1;
			gDrumSmp[d][i] = (s16)(v * 30000);
		}
		if (gCsndOK) CSND_FlushDataCache(gDrumSmp[d], len[d] * 2);
	}
	gDrumBuilt = kit;
}
static void drumPickChannels(void) {
	gNzCh[0] = gNzCh[1] = -1;
	int n = 0;
	for (int c = 16; c < NCS && n < 3; c++) if (gCsndOK && (csndChannels & BIT(c)) && c != gLoopCh[0] && c != gLoopCh[1]) gDrumCh[n++] = c;
}
static void drumHit(int d, float vel) {   // from the audio thread (voice lock held)
	int c = gDrumCh[d]; if (c < 0 || !gDrumSmp[d]) return;
	u32 flags = SOUND_FORMAT_16BIT | SOUND_ONE_SHOT | SOUND_LINEAR_INTERP | SOUND_ENABLE | SOUND_CHANNEL(c) | (CSND_TIMER(32728) << 16);
	float g = gDrumLevel * vel * (d == 2 ? 0.3f : 0.5f), pan = d == 2 ? 0.62f : 0.5f;
	u32 l = (u32)(g * sqrtf(1 - pan) * 1.41f * 0x7FFF), r = (u32)(g * sqrtf(pan) * 1.41f * 0x7FFF), vol = l | (r << 16);
	CSND_SetPlayState(c, 0);
	if (gKit == 3 && d >= 1 && gNzCh[d - 1] >= 0) {   // CHIP kit: snare and hat straight from the hardware noise generators
		int nc = gNzCh[d - 1]; gNzVol[d - 1] = g; CSND_SetPlayState(nc, 0);
		u32 t = CSND_TIMER(d == 2 ? 30000 : 9000); if (t < 0x42) t = 0x42;
		CSND_SetChnRegsNoise(SOUND_FORMAT_PSG | SOUND_ENABLE | SOUND_CHANNEL(nc) | (t << 16), vol, csSend(l / 32768.0f, r / 32768.0f));
		return;
	}
	CSND_SetChnRegs(flags, osConvertVirtToPhys(gDrumSmp[d]), osConvertVirtToPhys(gDrumSmp[d]), gDrumLen[d] * 2, vol, csSend(l / 32768.0f, r / 32768.0f));
}
static void drumTick(float dt) {   // audio thread: 16th-note clock, follows the sequencer when it plays
	if (!gDrums || !gCsndOK || gDrumBuilt < 0) { gDrumStep = -1; return; }
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
	int step = -1;
	if (gSeqPlaying) { static int lastSeq = -1; if (gSeqPos != lastSeq) { step = gSeqPos & 15; lastSeq = gSeqPos; } }
	else {
		float len = (gDrumStep & 1) ? 1.0f - gSwing : 1.0f + gSwing;
		gDrumPh += dt * gBPM / 60.0f * 4.0f;
		if (gDrumStep < 0) { gDrumStep = 15; gDrumPh = len; }
		if (gDrumPh >= len) { gDrumPh -= len; step = (gDrumStep + 1) & 15; }
	}
	if (step < 0) return;
	gDrumStep = step;
	if ((step & 3) == 0) gBeatPh = step / 4;   // keep the sidechain pump on the kick grid
	u16 bit = 0x8000 >> step; int any = 0;
	int eu = gEuLen >= 16 ? step : gEuPos % gEuLen; gEuPos++; gEuIdx = eu;
	for (int r = 0; r < 3; r++) if (*DPAT[r] == EU_IDX[r] && euOn(r, eu)) { drumHit(r, r == 2 ? HATVEL[eu] / 255.0f : (eu == gEuRot[r] % gEuLen ? 1.0f : 0.85f)); any = 1; }
	if (gKickPat == EU_IDX[0]) {}
	else if (gKickPat == 7) { if (gMyDrum[0][step]) { drumHit(0, gMyDrum[0][step] / 255.0f); any = 1; } }
	else if (KICKP[gKickPat] & bit) { drumHit(0, 1.0f); any = 1; }
	if (gSnarePat == EU_IDX[1]) {}
	else if (gSnarePat == 6) { if (gMyDrum[1][step]) { drumHit(1, gMyDrum[1][step] / 255.0f); any = 1; } }
	else if (SNAREP[gSnarePat] & bit) { drumHit(1, (gSnarePat == 5 && (step & 4)) ? 0.85f : 1.0f); any = 1; }
	if (gHatPat == EU_IDX[2]) {}
	else if (gHatPat == 6) { if (gMyDrum[2][step]) { drumHit(2, gMyDrum[2][step] / 255.0f); any = 1; } }
	else if (HATP[gHatPat] & bit) { drumHit(2, HATVEL[step] / 255.0f); any = 1; }
	if (any) csndExecCmds(false);
}
static void myDrumLoad(void);
static void drumsChanged(void) {   // main thread
	myDrumLoad();
	if (gDrums) {
		if (!gCsndOK) { say("DRUMS NEED THE SECOND SOUND CHIP"); gDrums = 0; return; }
		if (gDrumBuilt != gKit) drumBuild(gKit);
		drumPickChannels(); gDrumStep = -1; gEuPos = 0;
	} else if (gCsndOK) { for (int d = 0; d < 3; d++) if (gDrumCh[d] >= 0) CSND_SetPlayState(gDrumCh[d], 0); for (int k = 0; k < 2; k++) if (gNzCh[k] >= 0) CSND_SetPlayState(gNzCh[k], 0); csndExecCmds(false); }
}
// MIC TAP: knock or tap near the mic; each hit plays the drum now and is written into MINE at the nearest 16th,
// as loud as you hit it. Switching to a row copies its current pattern into MINE first, so you overdub.
static int gMyDrumLoaded = 0;
static void myDrumSave(void) { if (!gMyDrumLoaded) return; FILE* f = fopen("sdmc:/3ds/syrup/mydrums.bin", "wb"); if (f) { fwrite(gMyDrum, 1, sizeof(gMyDrum), f); fclose(f); } }
static void myDrumLoad(void) { if (gMyDrumLoaded) return; gMyDrumLoaded = 1; FILE* f = fopen("sdmc:/3ds/syrup/mydrums.bin", "rb"); if (f) { if (fread(gMyDrum, 1, sizeof(gMyDrum), f) != sizeof(gMyDrum)) memset(gMyDrum, 0, sizeof(gMyDrum)); fclose(f); } }
static void drumRecChanged(void) {
	myDrumLoad();
	if (gDrumRec == 4) { memset(gMyDrum, 0, sizeof(gMyDrum)); gDrumRec = 0; myDrumSave(); say("MINE CLEARED"); return; }
	if (!gDrumRec) { myDrumSave(); return; }
	int r = gDrumRec - 1;
	if (*DPAT[r] != MINE_IDX[r]) {   // start from what's playing on that row
		for (int i = 0; i < 16; i++) gMyDrum[r][i] = drumRowOn(r, *DPAT[r], i) ? (r == 2 ? HATVEL[i] : 230) : 0;
		*DPAT[r] = MINE_IDX[r];
	}
	if (!gDrums) { gDrums = 1; drumsChanged(); }
	say("TAP THE 2DS NEAR THE MIC - HARDER = LOUDER");
}
static void drumTapTick(float dt) {   // main thread (inside the voice lock), 60 times a second
	if (!gDrumRec || !gDrums || !gMicOK || !gMicBuf || gDrumBuilt < 0) return;
	u32 N = gMicSize / 2, end = micGetLastSampleOffset() / 2; const s16* m = (const s16*)gMicBuf;
	float pk = 0; for (int i = 0; i < 300; i++) { float v = fabsf(m[(end + N - 1 - i) % N] / 32768.0f); if (v > pk) pk = v; }   // last ~18 ms
	if (gTapHold > 0) { gTapHold -= dt; gTapLevel += (pk - gTapLevel) * 0.2f; return; }
	if (pk > 0.2f && pk > gTapLevel * 2.5f + 0.05f) {
		float vel = (pk - 0.2f) / 0.6f; if (vel > 1) vel = 1; vel = 0.3f + 0.7f * vel;
		int r = gDrumRec - 1;
		int st = gDrumStep < 0 ? 0 : gDrumStep;
		if (!gSeqPlaying) { float len = (st & 1) ? 1.0f - gSwing : 1.0f + gSwing; if (gDrumPh > len * 0.5f) st = (st + 1) & 15; }   // nearest 16th
		gMyDrum[r][st] = (u8)(vel * 255);
		if (gCsndOK) { drumHit(r, vel); csndExecCmds(false); }
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
	for (int i = 0; i < 8; i++) {
		int y = 52 + i * 16;
		if (on[i] && act) { rect(10, y - 2, 92, 12, HI); text(14, y, BL[i], 1, HITXT); }
		else { rect(10, y - 2, 92, 12, RGB3(TH->pad)); text(14, y, BL[i], 1, DIM); }
		if (i < 7) rect(55, y + 10, 1, 4, LINE);
	}
	char s[32];
	if (gModFx) { snprintf(s, sizeof(s), "%s", MODFX_NAMES[gModFx]); text(110, 68, s, 1, TXT); }
	if (gShimmer) text(110, 116, "SHIMMER", 1, TXT);
	if (gFreeze) text(110, 126, "FREEZE", 1, WARN);
	float cpu = gFxTicks / (268111856.0f * 160 / DSPRATE) * 100;
	snprintf(s, sizeof(s), "FX CPU %d%%", (int)(cpu + 0.5f)); text(10, 186, s, 1, DIM);
	rect(10, 198, 184, 6, RGB3(TH->pad)); rect(10, 198, (int)(184 * (cpu > 100 ? 1 : cpu / 100)), 6, cpu > 60 ? TH->warn[0] : TH->txt[0], cpu > 60 ? TH->warn[1] : TH->txt[1], cpu > 60 ? TH->warn[2] : TH->txt[2]);
	text(10, 210, "SUB, DRUMS, LOOP STAY DRY", 1, DIM);
}
static void drawDrumsPanel(void) {
	frame(4, 30, 196, 194);
	static const char* RN[3] = {"KICK", "SNARE", "HAT"};
	char s[40]; snprintf(s, sizeof(s), "%s KIT  %d BPM  %s", KIT_NAMES[gKit], gBPM, gDrums ? "ON" : "OFF"); text(10, 36, s, 1, gDrums ? TH->val[0] : TH->dim[0], gDrums ? TH->val[1] : TH->dim[1], gDrums ? TH->val[2] : TH->dim[2]);
	for (int r = 0; r < 3; r++) {
		int y = 56 + r * 34;
		text(10, y, RN[r], 1, DIM);
		if (*DPAT[r] == EU_IDX[r]) { char e[24]; snprintf(e, sizeof(e), "E(%d,%d) R%d", gEuHits[r], gEuLen, gEuRot[r]); text(70, y, e, 1, DIM); }
		for (int i = 0; i < 16; i++) {
			int x = 10 + i * 11, mine = *DPAT[r] == MINE_IDX[r], isEu = *DPAT[r] == EU_IDX[r];
			if (isEu && i >= gEuLen) continue;   // EUCLID rows shorter than 16 show only their own steps
			int on = mine ? gMyDrum[r][i] > 0 : drumRowOn(r, *DPAT[r], i), cur = gDrums && (isEu ? i == gEuIdx : i == gDrumStep);
			int hgt = mine && on ? 3 + gMyDrum[r][i] * 11 / 255 : 14;   // MINE: bar height = how hard you hit
			if (on) rect(x, y + 25 - hgt, 9, hgt, cur ? TH->txt[0] : TH->line[0], cur ? TH->txt[1] : TH->line[1], cur ? TH->txt[2] : TH->line[2]);
			else { rect(x, y + 23, 9, 2, DIM); if (cur) rect(x, y + 11, 9, 14, RGB3(TH->grid)); }
			if ((i & 3) == 0) rect(x, y + 27, 1, 3, DIM);
		}
	}
	text(10, 160, gSeqPlaying ? "FOLLOWING THE SEQUENCER" : "OWN CLOCK (BPM, SWING)", 1, DIM);
	text(10, 172, "PUMP ON MASTER FX LOCKS TO KICK", 1, DIM);
	if (gDrumRec && gDrumRec < 4) { char t[40]; snprintf(t, sizeof(t), "MIC TAP: %s  LEVEL", DREC_NAMES[gDrumRec] + 4); text(10, 186, t, 1, VAL); rect(10, 198, 184, 6, RGB3(TH->pad)); rect(10, 198, (int)(184 * (gTapLevel > 1 ? 1 : gTapLevel)), 6, TXT); }
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
// ---- voxels: isometric block columns, drawn back to front
static void voxColumn(float cx, float cy, float cw, float chh, float hu, int i, int j, int h, float r, float g, float b, int lit) {
	float x = cx + (i - j) * cw, y = cy + (i + j) * chh, top = y - h * hu;
	u32 cTop = rgba(r, g, b, 1), cL = rgba(r * 0.62f, g * 0.62f, b * 0.62f, 1), cR = rgba(r * 0.38f, g * 0.38f, b * 0.38f, 1);
	// left face, right face, top face (a rhombus)
	vTri(x - cw, top, x, top + chh, x, y + chh, cL); vTri(x - cw, top, x, y + chh, x - cw, y, cL);
	vTri(x, top + chh, x + cw, top, x + cw, y, cR); vTri(x, top + chh, x + cw, y, x, y + chh, cR);
	vTri(x - cw, top, x, top - chh, x + cw, top, cTop); vTri(x - cw, top, x + cw, top, x, top + chh, cTop);
	u32 edge = rgba(0, 0, 0, 0.55f);
	for (int k = 1; k < h; k++) {   // block seams so each column reads as stacked voxels
		float yy = y - k * hu;
		vLine(x - cw, yy, x, yy + chh, edge, 0.6f); vLine(x, yy + chh, x + cw, yy, edge, 0.6f);
	}
	if (lit) { blendAdd(1); vTri(x - cw, top, x, top - chh, x + cw, top, rgba(1, 1, 1, 0.35f)); vTri(x - cw, top, x + cw, top, x, top + chh, rgba(1, 1, 1, 0.35f)); blendAdd(0); }
}
#define VXR 14
#define VXC 16
static u8 gVox[VXR][VXC];   // scrolling note landscape: row 0 is the newest (front)
static void visVoxels(float dt) {
	static float acc = 0; acc += dt;
	if (acc > 0.09f) {   // push a new front row ~11 times a second
		acc = 0;
		memmove(gVox[1], gVox[0], sizeof(gVox) - sizeof(gVox[0]));
		float row[VXC] = {0};
		for (int k = 0; k < NVS; k++) {
			const SVoice* v = &gV[k]; if (!v->active || !v->started || v->isEcho) continue;
			int c = (v->note - 36) / 3; if (c < 0) c = 0; if (c >= VXC) c = VXC - 1;
			row[c] += v->env * (0.5f + v->vel) * 6;
			if (c > 0) row[c - 1] += v->env * 2;
			if (c < VXC - 1) row[c + 1] += v->env * 2;
		}
		for (int c = 0; c < VXC; c++) { int h = (int)(row[c] + 0.5f) + (gVisBeat > 0.5f ? 1 : 0); gVox[0][c] = h > 9 ? 9 : h; }
	}
	float cw = 11, chh = 5.5f, hu = 7, cx = 200, cy = 70;
	for (int sum = 0; sum <= VXR - 1 + VXC - 1; sum++)   // back (far) to front
		for (int i = 0; i < VXC; i++) {
			int j = (VXR - 1) - (sum - i); if (j < 0 || j >= VXR) continue;
			int rIdx = VXR - 1 - j, h = gVox[rIdx][i];   // front rows sit lower on screen
			float r, g, b; hue((float)i / VXC * 0.8f + gCutoff * 0.2f, &r, &g, &b);
			float fade = 0.35f + 0.65f * (1 - rIdx / (float)VXR);
			voxColumn(cx, cy, cw, chh, hu, i, j, h < 1 ? 1 : h + 1, r * fade, g * fade, b * fade, rIdx == 0 && h > 0);
		}
}
// the wavetable as blocks: rows are frames, columns are points along the cycle; the playing frame glows
static void visVoxTable(int o) {
	Osc* os = &gOsc[o]; Table* tb = &os->tb;
	int nf = tb->nFrames < 1 ? 1 : tb->nFrames; float pk = tb->peak > 1e-4f ? tb->peak : 1;
	enum { R = 14, C = 18 };
	float cw = 10, chh = 5, hu = 5, cx = 200, cy = 62;
	int cur = (int)(gPosEff[o] * (R - 1) + 0.5f);
	for (int sum = 0; sum <= R - 1 + C - 1; sum++)
		for (int i = 0; i < C; i++) {
			int j = (R - 1) - (sum - i); if (j < 0 || j >= R) continue;
			int fr = (int)((R - 1 - j) / (float)(R - 1) * (nf - 1));
			float v = os->on ? tb->frames[fr * CYC + i * (CYC - 1) / (C - 1)] / pk : 0;
			if (R - 1 - j == cur) { float s[TPTS]; sampleCycle(os->cyc, s, NULL); v = s[i * (TPTS - 1) / (C - 1)]; }   // live warped cycle
			int h = (int)((v + 1) * 4 + 0.5f) + 1;
			float r, g, b; hue(gCutoff * 0.6f + (R - 1 - j) / (float)R * 0.4f, &r, &g, &b);
			int isCur = R - 1 - j == cur; float k = isCur ? 1 : 0.55f;
			voxColumn(cx, cy, cw, chh, hu, i, j, h, r * k, g * k, b * k, isCur);
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
// SYNTHWAVE: striped sun, wavetable mountains, and a floor grid that scrolls in time with the BPM
static void visSynthwave(void) {
	float hz = 138;
	for (int i = 0; i < 12; i++) { float r, g, b; hue(0.55f + i * 0.04f, &r, &g, &b); vRect(0, i * hz / 12, 400, hz / 12 + 1, rgba(r * 0.18f, g * 0.1f, b * 0.3f, 1)); }
	float sr, sg, sb; hue(0.15f + gVisEnv * 0.1f, &sr, &sg, &sb);   // sun: horizontal bands cut out toward the bottom
	float R = 62 + gVisBeat * 6;
	for (int y = -62; y < 0; y += 2) {
		float yy = y / 62.0f; int cut = y > -30 && ((y / 2) & 3) == 0; if (cut) continue;
		float w = sqrtf(1 - yy * yy) * R;
		vRect(200 - w, hz + y * (R / 62), w * 2, 2.2f, rgba(sr, sg * (0.5f - yy * 0.6f), sb, 1));
	}
	float s[TPTS]; sampleCycle(gOsc[gVisOsc].cyc, s, NULL);   // mountains: the current waveform, mirrored
	for (int i = 0; i + 1 < TPTS; i += 2) {
		float x0 = i * 400.0f / (TPTS - 1), x1 = (i + 2 > TPTS - 1 ? TPTS - 1 : i + 2) * 400.0f / (TPTS - 1);
		float h0 = 18 + fabsf(s[i]) * 34 * (0.7f + 0.3f * gVisEnv), h1 = 18 + fabsf(s[i + 2 < TPTS ? i + 2 : TPTS - 1]) * 34 * (0.7f + 0.3f * gVisEnv);
		u32 m = rgba(0.06f, 0.02f, 0.14f, 1);
		vTri(x0, hz, x1, hz, x0, hz - h0, m); vTri(x1, hz, x1, hz - h1, x0, hz - h0, m);
		float r, g, b; hue(0.6f, &r, &g, &b); vLine(x0, hz - h0, x1, hz - h1, rgba(r, g, b, 0.9f), 1.2f);
	}
	vRect(0, hz, 400, 240 - hz, rgba(0.03f, 0.0f, 0.07f, 1));
	float gr, gg, gb; hue(0.55f + gVisEnv * 0.3f, &gr, &gg, &gb);
	u32 gc = rgba(gr, gg, gb, 0.8f);
	blendAdd(1);
	float scroll = fmodf(gVisT * gBPM / 60.0f, 1.0f);   // one grid row per beat
	for (int k = 0; k < 12; k++) {
		float z = (k + 1 - scroll) / 12.0f, y = hz + (240 - hz) * (1 - z) * (1 - z) * (1 - z) + 1;
		vLine(0, y, 400, y, rgba(gr, gg, gb, 0.25f + 0.6f * (1 - z)), 0.6f + 1.4f * (1 - z));
	}
	for (int k = -10; k <= 10; k++) vLine(200 + k * 8, hz, 200 + k * 70, 240, gc, 0.9f);
	vRect(0, hz - 1, 400, 2, rgba(gr, gg, gb, 0.7f + gVisBeat * 0.3f));
	blendAdd(0);
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

static void visFull(void) {
	int mode = gVisMode ? gVisMode : 1 + ((int)(gVisT / 10.0f)) % (NVISM - 1);   // AUTO: change every 10 s
	if (gVisBeat > 0.01f) { float r, g, b; hue(0.35f, &r, &g, &b); blendAdd(1); vRect(0, 0, 400, 240, rgba(r, g, b, gVisBeat * 0.12f)); blendAdd(0); }
	if (mode == 1) visWavetable(gVisOsc, 200, 128, 1.6f);
	else if (mode == 2) visScopes(10, 20, 380, 210);
	else if (mode == 3) visOrbit(200, 125, 100);
	else if (mode == 4) visSpectrum(12, 20, 376, 210, gVisOsc);
	else if (mode == 5) visVoxels(1.0f / 60);
	else if (mode == 6) visVoxTable(gVisOsc);
	else if (mode == 7) visTunnel();
	else if (mode == 8) visKaleido();
	else if (mode == 9) visSynthwave();
	else visRipples();
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
		u64 t0 = svcGetSystemTick();
		LightLock_Lock(&gMipsLock);
		doRebuild(&j);
		LightLock_Unlock(&gMipsLock);
		gCore1Busy += svcGetSystemTick() - t0;
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
	fftInit();
	for (int o = 0; o < 2; o++) { gOsc[o].tb.frames = (float*)malloc(sizeof(float) * CYC * MAXFR); mipsInit(&gOsc[o].mips); }
	mipsInit(&gSubMips);
	gNoise = (s16*)linearAlloc(gNoiseLen * sizeof(s16));
	for (int c = 0; c < NCH; c++) gDspOwner[c] = -1;
	for (int c = 0; c < NCS; c++) gCsOwner[c] = -1;
	makeBusyTable();
	scanTables();
	gTableCount = T_BUILTIN + gNFiles;

	// default patch: A = Basic Shapes, 3-voice unison; B = Harmonix an octave up (off)
	Osc* A = &gOsc[0]; Osc* B = &gOsc[1];
	A->on = 1; A->table = T_BASIC; A->pos = 0.6f; A->level = 0.8f; A->uni = 3; A->detune = 14; A->blend = 0.6f; A->width = 0.8f;
	B->on = 0; B->table = T_HARMONIX; B->pos = 0.3f; B->level = 0.5f; B->oct = 1; B->uni = 1; B->detune = 10; B->blend = 0.5f; B->width = 0.6f;
	tableSelect(&A->tb, A->table); tableSelect(&B->tb, B->table);
	LightLock_Init(&gMipsLock);
	LightLock_Init(&gVoiceLock); LightLock_Init(&gNetLock);
	userLoad();
	seqDefault();
	presetLoad(gStyle, gPreset);   // start on PAD / SYRUP PAD
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
	aptHook(&gSleepCookie, sleepHook, NULL);

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		wakeTick();
		LightLock_Lock(&gVoiceLock);   // audio state below; released before drawing
		if (!gSafeMode) { motionTick(1.0f / 60); camTick(); }
		micRecTick(1.0f / 60); loopTick(); loopLevelTick(); linkTick(1.0f / 60); dlTick(); upTick(); echoTick(); grainsTick(); revTypeApply(); netTabTick(); drumTapTick(1.0f / 60);
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

		if (helpOn && (kDown & ~(KEY_TOUCH | KEY_SELECT))) { helpOn = 0; topDirty = botDirty = 1; }
		if (kDown & KEY_SELECT) { helpOn = !helpOn; topDirty = botDirty = 1; }
		if (kDown & KEY_X) { page = navStep(page, 1); topDirty = 1; }
		if (kDown & KEY_Y) { page = navStep(page, -1); topDirty = 1; }
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
			if ((p->dirty & (D_LOOPTAB | D_MICTAB | D_CAMTAB | D_DLWT | D_UPDATE)) && dir > 0 && !fast) {
				if (p->dirty & D_UPDATE) upStart();
				else if ((p->dirty & D_DLWT) && !strcmp(p->name, "NET > TABLE")) netTabStart();
				else if ((p->dirty & D_DLWT) && !strcmp(p->name, "RULE > TABLE")) ruleTable();
				else if (p->dirty & D_DLWT) dlStart();
				else if (p->dirty & D_LOOPTAB) loopToTable();
				else if (p->dirty & D_MICTAB) micRecStart();
				else { gCamWant = 1; say("CAMERA: HOLD STILL..."); }
				topDirty = 1;
			}
			else if ((p->dirty & D_TILT) && dir > 0 && !fast) { tiltRecenter(); topDirty = 1; }
			else if ((p->dirty & (D_REST | D_CLEAR)) && dir > 0 && !fast) {
				if (p->dirty & D_REST) seqRest(); else seqClear();
				topDirty = 1;
			} else if ((p->dirty & (D_SAVE | D_INIT)) && dir > 0 && !fast) {
				if (p->dirty & D_SAVE) { patchCapture(&gUser[gSaveSlot]); userSave(); gStyle = NSTYLES - 1; gPreset = gSaveSlot; }
				else { SPatch ip; patchInit(&ip); patchApply(&ip); }
				curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; topDirty = 1;
			} else if ((p->dirty & (D_PRESET | D_STYLE)) && gEdited && !gGuard && !fast) {
				gGuard = 1; say("PATCH EDITED: PRESS AGAIN TO LEAVE IT (SAVE IT IN USER)"); topDirty = 1;
			} else if (paramChange(p, dir, fast)) {
				if (p->type == PT_E && p->names && page != 0) { snprintf(gCallTxt, sizeof(gCallTxt), "%s", p->names[*(int*)p->p]); gCallT = 0.9f; }
				if (page != 0 && page != 8 && page != 9) gEdited = 1;
				if (p->dirty & D_STYLE) { gPreset = 0; presetLoad(gStyle, 0); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
				if (p->dirty & D_OUT) outputApply();
				if (p->dirty & D_LOOP) loopModeChanged();
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
					int b = t.px * 7 / 320;
					static int seqBack = 0;
					if (b == 6) { if (page != 7) { seqBack = page; page = 7; } else page = seqBack; if (page == 7) say("SEQUENCER: TAP THE GRID, PLAY / ACID ABOVE IT"); }
					if (b == 0) { gPads = !gPads; gPadSel = -1; if (!latch) playSet(NULL, 0, 0.8f); if (gPads) say("PADS: LOW = TRIAD, MID = 7TH, TOP = 9TH; EDGES = INVERSION / SUS4"); }
					if (b == 1) page = navStep(page, -1);
					if (b == 2) page = navStep(page, 1);
					if (b == 3 && oct > -3) { oct--; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
					if (b == 4 && oct < 3) { oct++; allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; }
					if (b == 5) { latch = !latch; if (!latch) { allOff(); curKey = -1; gHeldN = 0; gArpN = 0; gArpOnN = 0; } }
					topDirty = botDirty = 1;
				}
			} else if (gModOn && !gPads) {
				gLastTX = t.px; gLastTY = t.py;
				if (modViewTouch(t.px, t.py, !prevTouch, 0)) { botDirty = 1; topDirty = 1; }
			} else if (gPads && !gGridOn) {
				float pv = 0.8f;
				if (padsTouch(base, t.px, t.py, !prevTouch, &pv)) { botDirty = 1; topDirty = 1; }
			} else if (t.py < KB_Y && !gGridOn && gAddEdit) {
				addTouch(t.px, t.py); botDirty = 1;
			} else if (t.py < KB_Y && gGridOn) {
				if (!prevTouch) { gridTouch(t.px, t.py); topDirty = botDirty = 1; }
			} else if (t.py < XY_Y + XY_H) {
				float np = (t.px - 4) / 311.0f, nc = 1.0f - (t.py - XY_Y) / (float)(XY_H - 1);
				if (np < 0) np = 0;
				if (np > 1) np = 1;
				if (nc < 0) nc = 0;
				if (nc > 1) nc = 1;
				if (gXyMode == 0) { A->pos = np; gCutoff = nc; }
				else { gMacro[gXyMode * 2 - 2] = np; gMacro[gXyMode * 2 - 1] = nc; }
				gXYmod = nc * 2 - 1;
				topDirty = botDirty = 1;
			} else if (t.py >= KB_Y) {
				key = keyAt(t.px, t.py, base);
				vel = 0.35f + 0.65f * (t.py - KB_Y) / (float)(240 - KB_Y);
			}
		}
		if (gModOn && !gPads && prevTouch && !touching && modViewTouch(gLastTX, gLastTY, 0, 1)) { botDirty = 1; topDirty = 1; }
		if (gPads && prevTouch && !touching) { gStrumZone = -1; if (!latch && gPadSel >= 0) playSet(NULL, 0, 0.8f); botDirty = 1; }   // let go of the pad
		padsTick(dt);
		prevTouch = touching;
		{ static int f = 0; if (gAddDirty && (++f % 6 == 0 || !touching)) { gAddDirty = 0; addApply(); topDirty = 1; } }   // additive editor: rebuild a few times a second while drawing
		if (key != curKey && !(latch && key < 0)) {
			int notes[4], n = key >= 0 ? chordNotes(key, notes) : 0;
			if (gSeqMode == 1) { if (key >= 0 && gSeqBase >= 0) { gSeqTr = key - gSeqBase; topDirty = 1; } }   // transpose the pattern
			else {
				if (gSeqMode == 2 && key >= 0) { seqRecord(notes, n, vel); topDirty = 1; }
				playSet(notes, n, vel);
			}
			curKey = key;
		}
		{ int g = page == 7; if (g != gGridOn) { gGridOn = g; botDirty = 1; } }
		{ int m = page == 5; if (m != gModOn) { gModOn = m; botDirty = 1; } if (gModOn && gModView && !gPads) botDirty = 1; }   // drawn LFO / motion playheads
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
		gFxModVal = modSum(DST_FXP, newestVoice());
		if (gAudioOK) voicesUpdate(dt, cx + gMidiBend);
		// effect-bus self check: if notes are sending but the DSP never delivers aux audio, fall back to taps
		if (gBusOK && !gBusFail && !gBusSeen && (masterActive() || busSend(0) > 0 || busSend(1) > 0)) {
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
		if (!helpOn && (page == 9 || page == 10 || page == 11)) topDirty = 1;   // live meters / step lights
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
				if (gCallT > 0 && page != 8) {   // big value pop-up, drawn last so the visuals don't cover it
					fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
					int w = (int)strlen(gCallTxt) * 16 + 16, x = 200 - w / 2;
					rect(x, 96, w, 40, RGB3(TH->tab)); outline(x, 96, w, 40, RGB3(TH->line));
					text(x + 8, 108, gCallTxt, 2, RGB3(TH->tabTxt));
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
	gAudioClock = 0; gDrums = 0; gLoopMode = 0;
	if (gCsndOK) {
		echoStop();   // capture units off and released before the chip goes away
		for (int c = 0; c < NCS; c++) if (gCsndOK && (csndChannels & BIT(c))) CSND_SetPlayState(c, 0);
		csWait();   // bounded: after a sleep or a fault the chip may never report back
		gCsndOK = 0;
		csndExit();
	}
	LightLock_Unlock(&gVoiceLock);
	gFxCore2 = 0; svcSleepThread(20000000); fxCore2Stop();   // effects back on the DSP thread before it goes away
	if (gAudioOK) { ndspSetCapture(NULL); for (int ch = 0; ch < NCH; ch++) ndspChnWaveBufClear(ch); ndspExit(); }
	if (!gSafeMode) { extrasExit(); sensorsExit(); }
	myDrumSave();
	gpuExit();
	remove(CRASH_PATH);   // clean exit
	gfxExit();
	return 0;
}
