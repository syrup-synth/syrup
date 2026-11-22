// MIDI over the network, end to end through real sockets on this machine, the way the outside world sends it:
// - a wired-style box on Wi-Fi (FM-1 LINK and friends): plain MIDI bytes over UDP 5008
// - a computer / phone / DAW: an AppleMIDI (RTP-MIDI) session on 5004 + 5005, then notes, then Syrup's CC OUT back
// - a browser: the phone remote page over HTTP 8080, then a WebSocket carrying MIDI (what a Web MIDI bridge page
//   does with a keyboard plugged into the computer), ping / pong, a split frame, close
#include "syrup_host.c"
#include "harness.h"
#include <netinet/tcp.h>
static int sounding(int n) { for (int i = 0; i < NVS; i++) if (gV[i].active && !gV[i].isEcho && gV[i].stage != 4 && gV[i].note == n) return 1; return 0; }
static struct sockaddr_in lo(int port) { struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(0x7F000001); return a; }
static int udp(void) { int s = socket(AF_INET, SOCK_DGRAM, 0); struct timeval tv = {1, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); return s; }
static int tcp(void) { int s = socket(AF_INET, SOCK_STREAM, 0); struct timeval tv = {2, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); struct sockaddr_in a = lo(8080);
	if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return -1; } int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); return s; }
static int readAll(int s, char* b, int n) { int got = 0; for (;;) { int k = recv(s, b + got, n - 1 - got, 0); if (k <= 0) break; got += k; if (got >= n - 1) break; if (strstr(b, "</html>")) break; b[got] = 0; if (strstr(b, "\r\n\r\n") && strstr(b, "101 Switching")) break; } b[got] = 0; return got; }
static void wsSend(int s, const u8* d, int n, int op, int split) {   // a masked client frame, as a browser sends it
	u8 f[64]; int k = 0; f[k++] = 0x80 | op; f[k++] = 0x80 | n; u8 m[4] = {0x12, 0x34, 0x56, 0x78}; memcpy(f + k, m, 4); k += 4;
	for (int i = 0; i < n; i++) f[k++] = d[i] ^ m[i & 3];
	if (split) { send(s, f, 3, 0); svcSleepThread(30000000); send(s, f + 3, k - 3, 0); } else send(s, f, k, 0);
}
static int us5008, uc, ud, ws;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 3) { int st, ix; if (findPreset("WARM FELT", &st, &ix)) presetLoad(st, ix); gMidiNet = 1; netMidiChanged(); }
	if (fr == 30 && !strstr(gNetAddr, "8080")) fail("net: Syrup didn't start listening (%s)", gNetAddr);
	// 1. a wired-style box: plain MIDI bytes over UDP 5008, a note split across two packets (running status after)
	if (fr == 32) { us5008 = udp(); struct sockaddr_in a = lo(5008); u8 p1[] = {0x90, 60}, p2[] = {100}; sendto(us5008, p1, 2, 0, (struct sockaddr*)&a, sizeof a); sendto(us5008, p2, 1, 0, (struct sockaddr*)&a, sizeof a); }
	if (fr == 40) { if (!sounding(60)) fail("UDP 5008: a note split over two packets should play"); if (strcmp(gNetPeer, "FM-1 LINK")) fail("UDP 5008: the peer should show as FM-1 LINK (%s)", gNetPeer);
		struct sockaddr_in a = lo(5008); u8 off[] = {60, 0}; sendto(us5008, off, 2, 0, (struct sockaddr*)&a, sizeof a); }   // running status: note 60 velocity 0 = off
	if (fr == 48 && sounding(60)) fail("UDP 5008: running-status note off should stop it");
	// 2. an RTP-MIDI session, the way macOS Network MIDI / rtpMIDI open one
	if (fr == 50) {
		uc = udp(); ud = udp();
		struct sockaddr_in b = {0}; b.sin_family = AF_INET; b.sin_port = htons(15004); bind(uc, (struct sockaddr*)&b, sizeof b); b.sin_port = htons(15005); bind(ud, (struct sockaddr*)&b, sizeof b);
		u8 inv[32] = {0xFF, 0xFF, 'I', 'N', 0, 0, 0, 2, 0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}; memcpy(inv + 16, "Test DAW", 9);
		struct sockaddr_in c = lo(5004), d = lo(5005); u8 r[64];
		sendto(uc, inv, 25, 0, (struct sockaddr*)&c, sizeof c); int k = recv(uc, r, sizeof r, 0);
		if (k < 16 || r[2] != 'O' || r[3] != 'K' || memcmp(r + 4, inv + 4, 8)) fail("RTP-MIDI: the control port should accept the invitation (%d)", k);
		sendto(ud, inv, 25, 0, (struct sockaddr*)&d, sizeof d); k = recv(ud, r, sizeof r, 0);
		if (k < 16 || r[2] != 'O' || r[3] != 'K') fail("RTP-MIDI: the data port should accept too (%d)", k);
		u8 ck[36] = {0xFF, 0xFF, 'C', 'K', 0xAA, 0xBB, 0xCC, 0xDD, 0}; sendto(ud, ck, 36, 0, (struct sockaddr*)&d, sizeof d); k = recv(ud, r, sizeof r, 0);
		if (k != 36 || r[8] != 1) fail("RTP-MIDI: clock sync should answer with count 1 (%d)", k);
		u8 rtp[] = {0x80, 0x61, 0, 1, 0, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 0x06, 0x90, 62, 100, 0x00, 64, 90};   // two notes, the 2nd with a delta time and running status
		sendto(ud, rtp, sizeof rtp, 0, (struct sockaddr*)&d, sizeof d);
	}
	if (fr == 58) { if (!sounding(62) || !sounding(64)) fail("RTP-MIDI: both notes should play (62 %d, 64 %d)", sounding(62), sounding(64)); if (strncmp(gNetPeer, "Test DAW", 8)) fail("RTP-MIDI: the peer name should show (%s)", gNetPeer);
		u8 off[] = {0x80, 0x61, 0, 2, 0, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 0x06, 0x80, 62, 0, 0x00, 64, 0}; struct sockaddr_in d = lo(5005); sendto(ud, off, sizeof off, 0, (struct sockaddr*)&d, sizeof d);
		rtpSend(0xB0, 74, 99);   // Syrup's CC OUT (LFO -> CC) goes back to whoever invited it
		u8 r[64]; int k = recv(ud, r, sizeof r, 0);
		if (k != 16 || r[13] != 0xB0 || r[14] != 74 || r[15] != 99) fail("RTP-MIDI OUT: the CC should arrive at the DAW's data port (%d)", k); }
	if (fr == 66 && (sounding(62) || sounding(64))) fail("RTP-MIDI: note offs should stop them");
	// 3. a browser: the remote page, then a WebSocket with MIDI in it
	if (fr == 70) {
		int s = tcp(); if (s < 0) { fail("HTTP: can't connect to 8080"); return; }
		const char* get = "GET / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n\r\n"; send(s, get, strlen(get), 0);
		static char b[65536]; int k = readAll(s, b, sizeof b); close(s);
		if (k < 100 || !strstr(b, "200 OK") || !strstr(b, "<html")) fail("HTTP: the remote page should load (%d bytes)", k);
		ws = tcp(); if (ws < 0) { fail("WebSocket: can't connect"); return; }
		const char* up = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
		send(ws, up, strlen(up), 0); char h[512]; readAll(ws, h, sizeof h);
		if (!strstr(h, "101 Switching") || !strstr(h, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")) fail("WebSocket: the handshake should answer with the right accept key");
		u8 n1[] = {0x90, 67, 110}; wsSend(ws, n1, 3, 2, 0);
		u8 n2[] = {0x90, 69, 100}; wsSend(ws, n2, 3, 2, 1);   // a frame that arrives in two TCP pieces
	}
	if (fr == 80) { if (!sounding(67)) fail("WebSocket: a note in a binary frame should play"); if (!sounding(69)) fail("WebSocket: a frame split across TCP reads should still play");
		if (gNetClients != 1) fail("WebSocket: one remote should be counted (%d)", gNetClients);
		u8 p[] = {'h', 'i'}; wsSend(ws, p, 2, 9, 0); u8 r[16]; int k = recv(ws, r, sizeof r, 0);
		if (k < 4 || r[0] != 0x8A || r[1] != 2 || r[2] != 'h') fail("WebSocket: a ping should get its pong (%d)", k);
		u8 off[] = {0x80, 67, 0, 0x80, 69, 0}; wsSend(ws, off, 6, 2, 0); }
	if (fr == 88) { if (sounding(67) || sounding(69)) fail("WebSocket: note offs should stop them"); u8 z[1]; wsSend(ws, z, 0, 8, 0); }
	if (fr == 96) { if (gNetClients != 0) fail("WebSocket: a close should drop the remote (%d)", gNetClients); close(ws); close(us5008); close(uc); close(ud); gMidiNet = 0; netMidiChanged(); }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 110; app_main(); printf("net: UDP 5008, RTP-MIDI session + CC out, HTTP page, WebSocket MIDI through real sockets\n"); return gFails ? 1 : 0; }
