// FM-1 LINK timing: messages arrive over a jittery network; NET BUFFER should turn that into one steady delay.
// Also checks the tick -> microsecond conversion is exact.
#include "syrup_host.c"
#include "harness.h"
static void SCRIPT(int fr) { (void)fr; }
static unsigned rng = 12345; static double rnd(void) { rng = rng * 1103515245u + 12345u; return ((rng >> 8) & 0xFFFF) / 65535.0; }
int main(void) {
	SIMTICK_SET = 1;
	// 1. exact clock
	SIMTICK = 268111856ULL * 3600ULL; if (nowUs() != 3600000000ULL) fail("nowUs: 1 hour of ticks gave %llu us", (unsigned long long)nowUs());
	SIMTICK = 268111856ULL * 86400ULL * 30ULL; if (nowUs() != 86400000000ULL * 30ULL) fail("nowUs: 30 days of ticks gave %llu us", (unsigned long long)nowUs());
	// 2. jittery packets: 16ths at 120 BPM (125 ms), network delay 30 ms + 0..15 ms jitter, NET BUFFER 20 ms
	gNetLat = 20; const double period = 125000, base = 30000; int N = 96;
	double arr[96]; u64 due[96]; int got = 0;
	for (int k = 0; k < N; k++) {
		double j = (k == 5) ? 0 : rnd() * 15000; arr[k] = 1e6 + k * period + base + j;
		SIMTICK = (u64)(arr[k] * 268.111856);
		u32 st = (u32)((k * period) * 24);   // sender clock, 24 MHz
		u8 pk[16] = {'S', 'L', 1, st & 255, st >> 8 & 255, st >> 16 & 255, st >> 24, 0x90, 60, 100};
		int before = gTqW; timedParse(pk, 10);
		if (gTqW != before) { due[got] = gTq[before].due; got++; }
	}
	if (got != N) fail("timing: %d of %d messages queued", got, N);
	double worstLate = 0, worstStep = 0, rawStep = 0;
	for (int k = 0; k < got; k++) {
		double late = arr[k] - (double)due[k]; if (late > worstLate) worstLate = late;   // >0: due before it even arrived
		if (k > 8) { double st = fabs((double)(due[k] - due[k - 1]) - period); if (st > worstStep) worstStep = st;
			double rs = fabs((arr[k] - arr[k - 1]) - period); if (rs > rawStep) rawStep = rs; }
	}
	printf("timing: arrival jitter %.1f ms -> scheduled jitter %.3f ms, worst lateness %.1f ms\n", rawStep / 1000, worstStep / 1000, worstLate / 1000);
	if (worstLate > 0) fail("timing: a message was due %.1f ms before it arrived (buffer too small for the jitter)", worstLate / 1000);
	if (worstStep > 1000) fail("timing: steps wobble by %.3f ms after settling (limit 1 ms)", worstStep / 1000);
	return gFails ? 1 : 0;
}
