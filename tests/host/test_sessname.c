// SESSION NAMES: SAVE SESSION asks for a name (a random one typed in already; the host keyboard types TEST NAME),
// keeps it with the date and time, and LOAD SESSION shows them. A random name is two words that fit the keyboard.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 3) { system("rm -rf sd/sessions"); for (int i = 0; i < 50; i++) { char n[24]; sessionRandomName(n, sizeof n); if (strlen(n) > 13 || !strchr(n, ' ')) fail("SESSION NAME: '%s' should be two words, 13 at most", n); } }
	if (fr == 5) gSessDue = 1;
	if (fr == 12) {
		FILE* f = fopen("sd/sessions/S01-name.txt", "rb"); char b[96] = ""; if (f) { size_t n = fread(b, 1, 95, f); b[n] = 0; fclose(f); }
		printf("sessname: S01-name.txt = %s", b);
		if (!strstr(b, "TEST NAME")) fail("SESSION NAME: the typed name should be kept (%s)", b);
		if (!strchr(b, ':')) fail("SESSION NAME: the time should be kept with it (%s)", b);
		if (!strstr(gMsg, "TEST NAME")) fail("SESSION NAME: saving should say the name (%s)", gMsg);
		gSessAt = -1; gSessDue = 3;
	}
	if (fr == 18) { printf("sessname: load says '%s'\n", gMsg); if (!strstr(gMsg, "TEST NAME") || !strstr(gMsg, "1/1")) fail("SESSION NAME: LOAD should show it (%s)", gMsg); done = 1; }
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 20; app_main(); if (!done) fail("didn't run"); printf("sessname: name, date, shown on load\n"); return gFails ? 1 : 0; }
