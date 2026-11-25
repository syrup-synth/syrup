// SECOND UNIT: a whole Syrup in a browser at http://<the 3DS>:8080/unit, on the 3DS's clock. Through real sockets:
// the loader page, the /unit WebSocket's hello (the 3DS is the master, its PLAY / BPM), PLAY + BPM from the browser
// reach the 3DS, the clock arrives 8 times a second and runs at the tempo, ping gets its pong, STOP goes out.
// LINK PHONE's 2ND UNIT tab puts /unit in the QR.
#include "syrup_host.c"
#include "harness.h"
#include <netinet/tcp.h>
static int tcp(void) { int s = socket(AF_INET, SOCK_STREAM, 0); struct timeval tv = {2, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(8080); a.sin_addr.s_addr = htonl(0x7F000001);
	if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return -1; } int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); return s; }
static void cliText(int s, const char* t) {   // a masked client text frame
	u8 f[300]; int n = (int)strlen(t), k = 0; f[k++] = 0x81; f[k++] = 0x80 | n; u8 m[4] = {9, 8, 7, 6}; memcpy(f + k, m, 4); k += 4;
	for (int i = 0; i < n; i++) f[k++] = (u8)t[i] ^ m[i & 3];
	send(s, f, k, 0);
}
static u8 rb[65536]; static int rn = 0;
static char msgs[400][128]; static double msgT[400]; static int nm = 0;
static void drain(int s) {   // server frames (not masked) -> msgs
	for (;;) { int k = recv(s, rb + rn, sizeof rb - rn, MSG_DONTWAIT); if (k <= 0) break; rn += k; }
	int i = 0;
	while (i + 2 <= rn) {
		int L = rb[i + 1] & 127, h = 2; if (L == 126) { if (i + 4 > rn) break; L = rb[i + 2] << 8 | rb[i + 3]; h = 4; }
		if (i + h + L > rn) break;
		if ((rb[i] & 15) == 1 && nm < 400 && L < 127) { memcpy(msgs[nm], rb + i + h, L); msgs[nm][L] = 0; msgT[nm] = osGetTime(); nm++; }
		i += h + L;
	}
	memmove(rb, rb + i, rn - i); rn -= i;
}
static int has(const char* k) { for (int i = 0; i < nm; i++) if (strstr(msgs[i], k)) return 1; return 0; }
static int us = -1, done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) { gMidiNet = 1; netMidiChanged(); gBPM = 120; }
	if (fr == 30) {
		if (!strstr(gNetAddr, "8080")) fail("net: not listening (%s)", gNetAddr);
		int s = tcp(); static char b[8192]; int n = 0; send(s, "GET /unit HTTP/1.1\r\nHost: x\r\n\r\n", 33, 0);
		for (;;) { int k = recv(s, b + n, sizeof b - 1 - n, 0); if (k <= 0) break; n += k; b[n] = 0; if (strstr(b, "</html>")) break; } close(s);
		if (!strstr(b, "SYRUP_MIRROR")) fail("UNIT: /unit should serve the loader (%d bytes)", n);
		us = tcp(); const char* up = "GET /unit HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
		send(us, up, strlen(up), 0); char h[512]; int hn = 0; while (hn < 511) { int k = recv(us, h + hn, 1, 0); if (k <= 0) break; hn++; h[hn] = 0; if (strstr(h, "\r\n\r\n")) break; }
		if (!strstr(h, "101")) fail("UNIT: the WebSocket should open");
		cliText(us, "{\"id\":\"web1\",\"n\":0.5,\"hi\":1}");
	}
	if (us >= 0 && fr > 30) drain(us);
	if (fr == 40) {
		if (!gUnitClients) fail("UNIT: the 3DS should count the browser");
		if (!has("\"m\":\"3ds\"")) fail("UNIT: the 3DS should say it's the master");
		if (!has("\"st\":1920")) fail("UNIT: the 3DS should send its transport (stopped, 120 BPM)");
		if (gSeqMode == 1) fail("UNIT: shouldn't be playing yet");
		cliText(us, "{\"id\":\"web1\",\"n\":0.7,\"st\":2083}");   // PLAY at 130 BPM (+ the browser's drums bit, which stays its own)
	}
	static double hp0, ht0;
	if (fr == 46) { if (gSeqMode != 1 || gBPM != 130) fail("UNIT: PLAY + 130 BPM from the browser should reach the 3DS (%d %d)", gSeqMode, gBPM); nm = 0; hp0 = gSeqClock + gSeqPh / stepLenClk(gSeqClock); ht0 = osGetTime(); }
	if (fr == 60) cliText(us, "{\"id\":\"web1\",\"n\":0.9,\"ping\":0.123456789}");
	if (fr == 170) {
		if (has("\"st\":2081")) fail("UNIT: the 3DS shouldn't echo the browser's PLAY back");
		if (!has("\"pong\":0.123456789,\"to\":\"web1\"")) fail("UNIT: a ping should get its pong, to the browser that asked");
		double c0 = -1, t0 = 0, c1 = -1, t1 = 0; int nc = 0;
		for (int i = 0; i < nm; i++) { const char* p = strstr(msgs[i], "\"clk\":"); if (!p) continue; double c = atof(p + 6); if (c0 < 0) { c0 = c; t0 = msgT[i]; } c1 = c; t1 = msgT[i]; nc++; }
		double now = gSeqClock + gSeqPh / stepLenClk(gSeqClock), hsec = (osGetTime() - ht0) / 1000.0;
		double sec = (t1 - t0) / 1000.0, rate = sec > 0 ? (c1 - c0) / sec : 0, want = hsec > 0 ? (now - hp0) / hsec : 0;   // (the host's own transport, per second of wall time)
		if (nc < (int)(hsec * 8 * 0.6)) fail("UNIT: the clock should come 8 times a second (%d in %.2f s)", nc, hsec);
		else if (fabs(rate - want) > want * 0.2) fail("UNIT: the clock should run with the transport (%.2f steps/s, the transport %.2f)", rate, want);
		if (fabs(now - c1) > 2.5) fail("UNIT: the clock should be where the 3DS's transport is (%.2f vs %.2f)", c1, now);
		printf("unit: %d clocks in %.2f s, %.2f steps/s (transport %.2f, 130 BPM = %.2f)\n", nc, hsec, rate, want, 130 / 60.0 * 4);
		transportToggle(); nm = 0;   // STOP on the 3DS
	}
	if (fr == 176) { if (!has("\"st\":2080")) fail("UNIT: STOP on the 3DS should go to the browser"); close(us); us = -1; }
	if (fr == 184) { if (gUnitClients) fail("UNIT: the count should drop when the browser goes"); }
	// LINK PHONE: the 2ND UNIT tab puts /unit in the QR
	if (fr == 186) { gPhoneLink = 1; }
	if (fr == 188) { TOUCH = 1; TX = 200; TY = 10; }
	if (fr == 191) { if (gPhoneLink != 2) fail("LINK PHONE: tapping 2ND UNIT should pick it (%d)", gPhoneLink); }
	if (fr == 194) shotBot("shots/phonelink_unit.ppm");
	if (fr == 196) HD = KEY_DLEFT;
	if (fr == 199) { if (gPhoneLink != 1) fail("LINK PHONE: LEFT should go back to REMOTE"); if (!strstr(REMOTE_HTML, "2ND UNIT")) fail("the phone page should have 2ND UNIT"); gPhoneLink = 0; done = 1; gMidiNet = 0; netMidiChanged(); }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 202; app_main(); if (!done) fail("didn't run"); printf("unit: /unit loader + WebSocket, PLAY/BPM both ways, clock, ping\n"); return gFails ? 1 : 0; }
