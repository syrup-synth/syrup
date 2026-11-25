// LINK PHONE (START menu): Wi-Fi on and a QR code of the phone pages' address on the bottom screen; B closes it.
// The phone page has SOUND HERE (the mirror). run_tests decodes the QR from the screenshot.
#include "syrup_host.c"
#include "harness.h"
static int done = 0;
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0;
	if (fr == 4) HD = KEY_START;
	if (fr >= 6 && fr < 6 + 2 * START_LINK && (fr & 1) == 0) HD = KEY_DDOWN;
	if (fr == 30) { if (gStartSel != START_LINK) fail("START: the D-pad should reach LINK PHONE (%d)", gStartSel); HD = KEY_A; }
	if (fr == 34) { if (!gPhoneLink || !gMidiNet) fail("LINK PHONE: the QR screen and Wi-Fi should come on (%d %d)", gPhoneLink, gMidiNet); }
	if (fr == 34 && !strchr(gNetAddr, ':')) snprintf(gNetAddr, sizeof gNetAddr, "192.168.1.23:8080");   // (the host has no real Wi-Fi address)
	if (fr == 40) shotBot("shots/phonelink.ppm");
	if (fr == 42) HD = KEY_B;
	if (fr == 45) { if (gPhoneLink) fail("LINK PHONE: B should close it"); if (!strstr(REMOTE_HTML, "SOUND HERE")) fail("the phone page should have SOUND HERE"); done = 1; gMidiNet = 0; netMidiChanged(); }
}
int main(void) { mkdir("sd", 0777); mkdir("shots", 0777); loadFont(); FRMAX = 50; app_main(); if (!done) fail("didn't run"); printf("phonelink: START > LINK PHONE, QR, close\n"); return gFails ? 1 : 0; }
