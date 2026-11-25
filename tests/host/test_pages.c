// Long patterns: 64 steps in 4 pages of 16 on the piano roll and the drum grid (page strip, COPY>, FOLLOW),
// saved and loaded, and files from before pages still load into page 1.
#include "syrup_host.c"
#include "harness.h"
static unsigned h0; static int maxPos = 0, maxDr = 0, pgSeen = 0;
static int pgX(int i) { return ROLL_KW + i * PG_W + PG_W / 2; }
static void SCRIPT(int fr) {
	HD = 0; TOUCH = 0; gFxCore2 = 0;
	if (fr == 3) {
		SPatch p; patchInit(&p); patchApply(&p); gVoiceMode = 0; gArp = 0; gBPM = 240; gSwing = 0;
		memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 16; gRollPg = 0; gPatView = 0; gStepEd = -1;
		// page 3 of the roll: one past the end makes the pattern 48 steps
		if (!pageStripTouch(pgX(2)) || gRollPg != 2 || gSeqLen != 48) fail("PAGE 3: should show page 3 and make 48 steps (page %d, len %d)", gRollPg, gSeqLen);
		int row = 4, nt = rollNote(row), y = ROLL_TOP + (ROLL_ROWS - 1 - row) * ROLL_RH + ROLL_RH / 2, x = ROLL_KW + (int)(3.5f * ROLL_CW);
		gridTouch(x, y, 1, 0); gridTouch(x, y, 0, 1);
		if (noteCover(35, nt, NULL) != 35) fail("a tap on page 3, column 4, should put a note on step 36");
		pageStripTouch(ROLL_KW + 4 * PG_W + 10);   // COPY> page 3 onto page 4
		if (gRollPg != 3 || gSeqLen != 64 || noteCover(51, nt, NULL) != 51) fail("COPY>: page 3 onto page 4 (page %d, len %d)", gRollPg, gSeqLen);
		stepSetNote(0, 60, 1); gRollFollow = 1; gSeqMode = 1; h0 = gSeqHits;
	}
	if (fr > 3 && fr < 200) { if (gSeqPos > maxPos) maxPos = gSeqPos; if (gSeqPos >= 32 && gSeqPos < 48 && gRollPg == 2) pgSeen = 1; }
	if (fr == 200) {
		if (maxPos < 50) fail("a 64-step pattern should play past step 50 (%d)", maxPos);
		if (gSeqHits - h0 < 3) fail("notes on pages 1, 3 and 4 should play (%u hits)", gSeqHits - h0);
		if (!pgSeen) fail("FOLLOW: the roll should turn to page 3 while it plays there");
		gSeqMode = 0; seqStop();
		seqSave(); memset(gPat, 0, sizeof(gPat)); memset(gSeq, 0, sizeof(gSeq)); gSeqLen = 1; seqLoad();
		if (gSeqLen != 64 || !gSeq[51].n || !gSeq[35].n) fail("save + load: 64 steps and the notes on pages 3 / 4 (len %d)", gSeqLen);
		// a file from before pages: 16-step patterns
		{ static SeqPat16 old[NPAT]; memset(old, 0, sizeof(old)); old[0].len = 12; old[0].st[5].n = 1; old[0].st[5].note[0] = 62; old[0].st[5].vel = 200; old[0].st[5].len[0] = 1;
		  FILE* f = fopen("sd/seq.bin", "wb"); u32 m = SEQ_MAGIC; int hdr[4] = {0, 0, 0, (int)sizeof(SeqPat16)}; u8 song[NSONG] = {0};
		  fwrite(&m, 4, 1, f); fwrite(hdr, sizeof(hdr), 1, f); fwrite(old, sizeof(old), 1, f); fwrite(song, sizeof(song), 1, f); fclose(f);
		  seqLoad(); if (gSeqLen != 12 || !gSeq[5].n || gSeq[5].note[0] != 62 || gSeq[40].n) fail("an old 16-step seq.bin should load into page 1 (len %d)", gSeqLen); }
		// DRUMS: page 2 grows the rows, repeating them; a tap there; the drums play into it
		myDrumLoad(); memset(gMyDrum, 0, sizeof gMyDrum); memset(gDrumX, 0, sizeof gDrumX); for (int r = 0; r < 10; r++) gDrLen[r] = 16;
		gMyDrum[0][0] = 230; gKickPat = MINE_IDX[0]; gSnarePat = MINE_IDX[1]; gHatPat = MINE_IDX[2]; gDgPg = 0; gDgShift = 0;
		int pw = (320 - DG_LX - 52) / 4; dgPageTouch(DG_LX + pw + pw / 2);
		if (gDgPg != 1 || gDrLen[0] != 32 || gMyDrum[0][16] != 230) fail("DRUM PAGE 2: rows grow to 32, repeating (page %d len %d hit %d)", gDgPg, gDrLen[0], gMyDrum[0][16]);
		drumGridTouch(DG_LX + (int)(4.5f * (320 - DG_LX - 2) / 16.0f), DG_TOP + 1 * DG_RH + 5, 1); drumGridRelease();
		if (!gMyDrum[1][20]) fail("a tap on drum page 2 should set step 21");
		gDgFollow = 1; gDrums = 1; drumsChanged(); gSeqMode = 1;   // PLAY: the drums run with the transport
	}
	if (fr > 200 && fr < 400 && gDrIdx[0] > maxDr) maxDr = gDrIdx[0];
	if (fr == 400) {
		if (maxDr < 20) fail("32-step drum rows should play past step 20 (%d)", maxDr);
		gDrums = 0; drumsChanged();
		myDrumSave(); memset(gMyDrum, 0, sizeof gMyDrum); gMyDrumLoaded = 0; myDrumLoad();
		{ memset(gSeq, 0, sizeof gSeq); stepSetNote(3, 60, 1); stepSetNote(40, 62, 1); gSeqLen = 48; int cx = ROLL_KW + 4 * PG_W + 60;
		  pageStripTouch(cx); if (!gLs || gLsN != NSTEP / ROLL_VIEW + 2) fail("BARS: a tap opens its list (%d %d)", gLs, gLsN);
		  lsPick(NSTEP / ROLL_VIEW); if (gLs || gSeqLen != 48) fail("BARS: CUT EMPTY keeps bars 1-3 (len %d)", gSeqLen);
		  stepSetNote(40, 62, 0); gGtHit = -1; seqTrimTail(); if (gSeqLen != 16) fail("an erased last note should cut the empty bars (len %d)", gSeqLen);
		  stepSetNote(40, 62, 1); gSeqLen = 48; lsPick(0); if (gSeqLen != 16 || !gSeq[40].n) fail("BARS: 1 BAR plays one bar, the notes past it stay (len %d)", gSeqLen);
		  pageStripTouch(cx); lsPick(NSTEP / ROLL_VIEW + 1); if (gSeq[3].n || gSeq[40].n || gSeqLen != 16) fail("BARS: CLEAR ALL should clear the pattern (len %d)", gSeqLen); }
		if (!gMyDrum[1][20] || gDrLen[0] != 32) fail("drums save + load at 32 steps (%d, len %d)", gMyDrum[1][20], gDrLen[0]);
		{   // SAMPLE kit: WAVs in a folder, named by drum; missing ones stay 808
			static const char* NM[][2] = {{"Kick 01.wav", "0"}, {"808 Open Hat.wav", "3"}, {"hh.WAV", "2"}, {"Lo Tom.wav", "6"}, {"tom2.wav", "7"}, {"Cowbell.wav", "8"}, {"7.wav", "6"}, {"readme.txt", "-1"}};
			for (int i = 0; i < 8; i++) if (kitFileDrum(NM[i][0]) != atoi(NM[i][1])) fail("SAMPLE: %s should be drum %s (%d)", NM[i][0], NM[i][1], kitFileDrum(NM[i][0]));
			mkdir("sd/kits", 0777); mkdir("sd/kits/Test Kit", 0777);
			static float w[8000]; for (int i = 0; i < 8000; i++) w[i] = 0.5f * sinf(i * 0.05f) * (1 - i / 8000.0f);
			wavWrite16("sd/kits/Test Kit/Kick 01.wav", w, 8000);
			kitScanDirs(); int k = -1; for (int i = 0; i < gNKitDirs; i++) if (!strcmp(gKitDirs[i], "TEST KIT")) k = i;
			if (k < 0) fail("SAMPLE: the folder should be listed (%d folders)", gNKitDirs);
			else { gKitDir = k; drumBuild(KIT_SAMPLE);
				if (gDrumBuilt != KIT_SAMPLE || abs(gDrumLen[0] - 8000) > 2) fail("SAMPLE: the kick should be the file (len %d, built %d)", gDrumLen[0], gDrumBuilt);
				else if (abs(gDrumSmp[0][63] - (s16)(w[63] * 30000)) > 300) fail("SAMPLE: the kick's samples should be the file's");
				if (gDrumLen[1] < 1000) fail("SAMPLE: the snare (no file) should still be the 808 one"); }
		}
		{   // a mydrums.bin from before pages: 16-step rows, DRM2 block, accents
			static u8 my[3][16], dx[7][16], rt[10][16], cd[10][16], ra[10], ln[10], ac[16]; static DVoice dv[10];
			memset(my, 0, sizeof my); my[1][3] = 200; dx[1][7] = 180; cd[1][3] = 6; for (int r = 0; r < 10; r++) ln[r] = 12; ac[3] = 1; memcpy(dv, gDV, sizeof dv);
			FILE* f = fopen("sd/mydrums.bin", "wb"); u32 m = DRM2; fwrite(my, sizeof my, 1, f); fwrite(dx, sizeof dx, 1, f); fwrite(&m, 4, 1, f); fwrite(rt, sizeof rt, 1, f); fwrite(cd, sizeof cd, 1, f);
			fwrite(ra, sizeof ra, 1, f); fwrite(ln, sizeof ln, 1, f); fwrite(dv, sizeof dv, 1, f); fwrite(ac, sizeof ac, 1, f); fclose(f);
			memset(gMyDrum, 0, sizeof gMyDrum); memset(gDrumX, 0, sizeof gDrumX); gMyDrumLoaded = 0; myDrumLoad();
			if (gMyDrum[1][3] != 200 || gDrumX[1][7] != 180 || gDrCond[1][3] != 6 || gDrLen[0] != 12 || !gDrAcc[0][3] || !gDrAcc[5][3] || gMyDrum[1][19]) fail("an old 16-step mydrums.bin should load into page 1");
		}
	}
}
int main(void) { mkdir("sd", 0777); loadFont(); FRMAX = 402; app_main(); printf("pages: 64-step patterns and drum rows, page strips, COPY>, FOLLOW, files old and new\n"); return gFails ? 1 : 0; }
