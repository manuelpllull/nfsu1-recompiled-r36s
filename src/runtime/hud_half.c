/*
 * hud_half.c - Speed.exe 1.4: draw the race HUD at half rate while driving (R36S; approved trade-off, as
 * in the NFSU2 port).
 *
 * sub_004F0390 (called from the overlay renderer sub_00409CD0, after the world) runs the FEng HUD: the
 * lap/position panels, standings, tachometer and their text, ~120-260 textured quads per frame. While
 * driving it runs every other frame; on the frames in between the HUD is still drawn every frame but its
 * contents update at 15 Hz. Two native paths do that:
 *   - NFSU2_HUD_TARGET=1 (default): the HUD is rendered into an offscreen RGBA target on its record frame;
 *     the replay frame composites that texture over the world with one quad. This removes the ~150
 *     D3D9/GL draws the old replay issued every second frame.
 *   - NFSU2_HUD_TARGET=0: the D3D9 calls of the record frame are recorded and replayed (the old path).
 * Menus (including the pause menu, which installs the menu key map) are not affected. sub_004F0390 takes
 * its argument in eax and returns with a plain `ret`.
 * NFSU2_HUD_HALF=0 (NFSU_HUD_HALF=0 in nfsu.cfg) turns the whole half-rate path off.
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
extern void sub_004F0390_orig(void);
extern void nfsHudRecordBegin(void);
extern void nfsHudRecordEnd(void);
extern int nfsHudReplay(void);
extern int nfsHudTargetAvailable(void);
extern int nfsHudTargetBegin(void);
extern void nfsHudTargetEnd(void);
extern int nfsHudComposite(void);
extern int nfsGuestDriving(void);
extern uint32_t getD3D9PresentCount(void);

static uint32_t lastFrame = 0xffffffffu, lastRecorded = 0xffffffffu, replays, runs;
void sub_004F0390(void) {
    static int mode = -1, targetMode = -1, announced = 0;
    if (mode < 0) { const char* e = getenv("NFSU2_HUD_HALF"); mode = e ? atoi(e) : 1; }
    if (targetMode < 0) { const char* e = getenv("NFSU2_HUD_TARGET"); targetMode = !e || atoi(e) != 0; }
    const uint32_t frame = getD3D9PresentCount();
    const int firstCallThisFrame = frame != lastFrame;
    lastFrame = frame;
    if (!mode || !nfsGuestDriving() || !firstCallThisFrame) { sub_004F0390_orig(); return; }
    if (!announced) {
        announced = 1;
        printf("[NFSU1 HUD] half rate (%s)\n", targetMode && nfsHudTargetAvailable() ? "offscreen target" : "recorded calls");
    }
    if (targetMode && nfsHudTargetAvailable()) {
        if (frame == lastRecorded + 1 && nfsHudComposite()) {
            ++replays;
            g_esp += 4;   /* ret */
            return;
        }
        if (nfsHudTargetBegin()) {
            sub_004F0390_orig();
            nfsHudTargetEnd();
            lastRecorded = frame;
            if (++runs % 120 == 0) printf("[NFSU1 HUD] half rate: %u runs, %u replays\n", runs, replays);
            return;
        }
    }
    if (frame == lastRecorded + 1 && nfsHudReplay()) {
        ++replays;
        g_esp += 4;   /* ret */
        return;
    }
    nfsHudRecordBegin();
    sub_004F0390_orig();
    nfsHudRecordEnd();
    lastRecorded = frame;
    if (++runs % 120 == 0) printf("[NFSU1 HUD] half rate: %u runs, %u replays\n", runs, replays);
}
