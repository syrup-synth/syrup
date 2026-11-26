// SAMPLE KIT loader: a kit folder of WAVs in every shape people actually have - 8 / 16 / 24 / 32-bit, float, mono /
// stereo, 22-48 kHz, WAVE_FORMAT_EXTENSIBLE, a LIST chunk (odd size) before fmt, data before fmt, a data size that runs
// past the end of the file, a 30 s file, a file that isn't a WAV, names by word / number / short code. Each drum
// is checked for: it loaded, its pitch survived the resample to 32728 Hz, the long file was cut (and read fast).
#include "syrup_host.c"
#include "harness.h"
#include <time.h>
static void w16(FILE* f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void w32(FILE* f, u32 v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }
enum { O_NORMAL, O_LISTFIRST, O_DATAFIRST, O_BIGDATA };
// a sine at hz, amp 0.5, written as fmt (1 PCM, 3 float, 0xFFFE extensible) / bits / channels / rate
static void mkwav(const char* name, int fmt, int bits, int ch, int rate, float secs, float hz, int order) {
	char p[200]; snprintf(p, sizeof p, "sd/kits/Test Kit/%s", name); FILE* f = fopen(p, "wb"); if (!f) { fail("can't write %s", p); return; }
	int n = (int)(rate * secs), bps = bits / 8 * ch; u32 dbytes = (u32)n * bps; int fl = fmt == 0xFFFE ? 40 : 16;
	u32 riff = 4 + 8 + fl + 8 + dbytes + (order == O_LISTFIRST ? 8 + 6 : 0);
	fwrite("RIFF", 1, 4, f); w32(f, riff); fwrite("WAVE", 1, 4, f);
	if (order == O_LISTFIRST) { fwrite("LIST", 1, 4, f); w32(f, 5); fwrite("INFOx", 1, 5, f); fputc(0, f); }   // odd size + pad byte
	long fmtAt = -1;
	#define FMT() do { fwrite("fmt ", 1, 4, f); w32(f, fl); w16(f, fmt); w16(f, ch); w32(f, rate); w32(f, rate * bps); w16(f, bps); w16(f, bits); \
		if (fmt == 0xFFFE) { w16(f, 22); w16(f, bits); w32(f, 0); w16(f, 1); for (int i = 0; i < 14; i++) fputc(0, f); } } while (0)
	if (order != O_DATAFIRST) FMT();
	fwrite("data", 1, 4, f); w32(f, order == O_BIGDATA ? 0xFFFFFFF0u : dbytes);
	for (int i = 0; i < n; i++) { float v = 0.5f * sinf(6.2831853f * hz * i / rate);
		for (int c = 0; c < ch; c++) {
			if (fmt == 3) fwrite(&v, 4, 1, f);
			else if (bits == 8) fputc((int)(128 + v * 127), f);
			else if (bits == 16) w16(f, (int)(v * 32767));
			else if (bits == 24) { int x = (int)(v * 8388607); fputc(x & 255, f); fputc((x >> 8) & 255, f); fputc((x >> 16) & 255, f); }
			else w32(f, (u32)(int)(v * 2147483000.0f));
		} }
	if (order == O_DATAFIRST) FMT();
	(void)fmtAt; fclose(f);
}
static int done = 0;
static const char* DN[NDRUM] = {"KICK", "SNARE", "HAT", "OPEN HAT", "CLAP", "RIM", "LO TOM", "HI TOM", "BELL", "CRASH"};
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) {
		mkdir("sd/kits", 0777); mkdir("sd/kits/Test Kit", 0777);
		mkwav("Kick 01.wav", 1, 16, 1, 44100, 0.3f, 100, O_NORMAL);         // KICK: word + number
		mkwav("SNARE.wav", 1, 24, 2, 48000, 0.2f, 300, O_NORMAL);           // SNARE: 24-bit stereo 48k
		mkwav("03.wav", 1, 8, 1, 22050, 0.1f, 1000, O_NORMAL);              // HAT by number: 8-bit 22k
		mkwav("open hat.WAV", 1, 16, 2, 44100, 30.0f, 500, O_NORMAL);       // OPEN HAT: 30 s, 5 MB
		mkwav("CP.wav", 3, 32, 2, 44100, 0.2f, 700, O_LISTFIRST);           // CLAP: float stereo, LIST first
		mkwav("RIM.wav", 0xFFFE, 32, 1, 32728, 0.05f, 2000, O_NORMAL);      // RIM: 32-bit extensible
		mkwav("LO TOM.wav", 1, 16, 1, 44100, 0.1f, 150, O_BIGDATA);         // LO TOM: data size runs past the end
		{ FILE* f = fopen("sd/kits/Test Kit/HI TOM.wav", "wb"); fputs("this is not a wav file at all, just text pretending", f); fclose(f); }   // HI TOM: not a WAV
		mkwav("Bell.wav", 1, 16, 1, 44100, 0.3f, 800, O_DATAFIRST);         // BELL: data before fmt
		{ FILE* f = fopen("sd/kits/Test Kit/readme.txt", "wb"); fputs("x", f); fclose(f); }   // ignored; CRASH has no file
		for (int d = 0; d < NDRUM; d++) gDV[d].decay = 100;
		kitScanDirs(); int k = -1; for (int i = 0; i < gNKitDirs; i++) if (!strcmp(gKitRaw[i], "Test Kit")) k = i;
		if (k < 0) { fail("SAMPLE: the kit folder wasn't found"); return; }
		gKitDir = k; gKit = KIT_SAMPLE; gDrums = 1;
		struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); drumBuild(KIT_SAMPLE); clock_gettime(CLOCK_MONOTONIC, &b);
		double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
		static const float HZ[NDRUM] = {100, 300, 1000, 500, 700, 2000, 150, 0, 800, 0};
		static const float SECS[NDRUM] = {0.3f, 0.2f, 0.1f, -1, 0.2f, 0.05f, 0.1f, 0, 0.3f, 0};
		for (int d = 0; d < NDRUM; d++) {
			float tmp[32728 * 3]; int n = kitSampleLoad(d, tmp, 32728 * 3);   // the loader itself, straight
			int up = 0; for (int i = 1; i < n - 300; i++) if (tmp[i - 1] < 0 && tmp[i] >= 0) up++;
			float hz = n > 300 ? up / ((n - 300) / 32728.0f) : 0, pk = 0; for (int i = 0; i < n; i++) if (fabsf(tmp[i]) > pk) pk = fabsf(tmp[i]);
			printf("  %-8s %6d samples  %6.0f Hz  peak %.2f  kit len %d\n", DN[d], n, hz, pk, gDrumLen[d]);
			if (HZ[d] > 0) {
				if (n <= 0) { fail("SAMPLE %s: didn't load", DN[d]); continue; }
				if (fabsf(hz - HZ[d]) > HZ[d] * 0.04f + 6) fail("SAMPLE %s: pitch %.0f Hz, want %.0f (resample)", DN[d], hz, HZ[d]);
				if (pk < 0.4f || pk > 0.6f) fail("SAMPLE %s: level %.2f, want 0.5", DN[d], pk);
				if (SECS[d] > 0 && abs(n - (int)(SECS[d] * 32728)) > 40) fail("SAMPLE %s: %d samples, want ~%d", DN[d], n, (int)(SECS[d] * 32728));
			} else if (n != 0) fail("SAMPLE %s: should not load (%d)", DN[d], n);
			if (gDrumLen[d] <= 0) fail("SAMPLE %s: the kit should still have a sound (the 808 one if no file)", DN[d]);
		}
		if (gDrumLen[3] > (int)(32728 * 0.5f * 2.1f) + 2) fail("SAMPLE OPEN HAT: the 30 s file should be cut to the drum's room (%d)", gDrumLen[3]);
		printf("  whole kit built in %.1f ms (30 s file included)\n", ms);
		if (ms > 400) fail("SAMPLE: building the kit took %.0f ms (the long file should only be read as far as it's used)", ms);
	}
	if (fr == 6) { drumHit(0, 1.0f); drumHit(3, 1.0f); drumHit(4, 1.0f); }
	if (fr == 20) { done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 24; app_main(); if (!done) fail("didn't run"); printf("samples: SAMPLE kit loader, every WAV shape\n"); return gFails ? 1 : 0; }
