// Shared helpers for the host tests: each test #includes this after syrup_host.c and defines SCRIPT(frame).
#include <stdarg.h>
static int gFails = 0;
static void fail(const char* fmt, ...) { va_list a; va_start(a, fmt); printf("FAIL: "); vprintf(fmt, a); printf("\n"); va_end(a); gFails++; }
static void loadFont(void) {
	FILE* f = fopen("default_font.bin", "rb");
	if (!f) { printf("FAIL: default_font.bin missing\n"); exit(2); }
	if (fread(FONT, 1, 2048, f) != 2048) { printf("FAIL: font short\n"); exit(2); }
	fclose(f);
}
static void rotFb(const u8* fb, int w, u8* o) { for (int x = 0; x < w; x++) for (int y = 0; y < 240; y++) { const u8* p = fb + (x * 240 + (239 - y)) * 3; u8* q = o + (y * w + x) * 3; q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; } }
static void shotTop(const char* fn) { static u8 T[400 * 240 * 3]; rotFb(TOPFB, 400, T); FILE* f = fopen(fn, "wb"); fprintf(f, "P6 400 240 255\n"); fwrite(T, 1, sizeof T, f); fclose(f); }
static void shotBot(const char* fn) { static u8 B[320 * 240 * 3]; rotFb(BOTFB, 320, B); FILE* f = fopen(fn, "wb"); fprintf(f, "P6 320 240 255\n"); fwrite(B, 1, sizeof B, f); fclose(f); }
static void writeWav(const char* fn, int from, int to) {
	if (to > ON) to = ON; if (from < 0) from = 0; int n = to - from; if (n < 0) n = 0;
	FILE* w = fopen(fn, "wb"); int bytes = n * 4;
	int hdr[] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 32728, 32728 * 4, 0x00100004, 0x61746164, bytes};
	fwrite(hdr, 4, 11, w); fwrite(OUT + from * 2, 4, n, w); fclose(w);
}
static int findPreset(const char* nm, int* st, int* ix) {
	for (int s = 0; s < NSTYLES - 1; s++) for (int i = 0; i < MAXPRE; i++) if (PRESETS[s][i].name && !strcmp(PRESETS[s][i].name, nm)) { *st = s; *ix = i; return 1; }
	return 0;
}
#define SEC(t) ((int)((t) * 32728))
static double rmsOf(int a, int b) { double s = 0; long n = 0; if (b > ON) b = ON; for (int i = a; i < b; i++) { double l = OUT[2 * i], r = OUT[2 * i + 1]; s += l * l + r * r; n += 2; } return n ? sqrt(s / n) : 0; }
static int peakOf(int a, int b) { int p = 0; if (b > ON) b = ON; for (int i = 2 * a; i < 2 * b; i++) { int v = abs(OUT[i]); if (v > p) p = v; } return p; }
static int clipsOf(int a, int b) { int c = 0; if (b > ON) b = ON; for (int i = 2 * a; i < 2 * b; i++) if (abs(OUT[i]) >= 32760) c++; return c; }
static double dcOf(int a, int b) { double s = 0; long n = 0; if (b > ON) b = ON; for (int i = 2 * a; i < 2 * b; i++) { s += OUT[i]; n++; } return n ? s / n : 0; }
// energy at one frequency (Goertzel over the mono mix) relative to the whole signal, in dB
static double toneDb(int a, int b, double hz) {
	if (b > ON) b = ON; double w = 2 * M_PI * hz / 32728, c = 2 * cos(w), s1 = 0, s2 = 0, tot = 0; int n = b - a;
	for (int i = a; i < b; i++) { double x = (OUT[2 * i] + OUT[2 * i + 1]) * 0.5, s0 = x + c * s1 - s2; s2 = s1; s1 = s0; tot += x * x; }
	double p = (s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / n;   // ~ power of that sinusoid * n/2 ... normalised below
	double tone = p / (n / 2.0), all = tot / n;
	return 10 * log10((tone + 1e-12) / (all + 1e-12));
}
