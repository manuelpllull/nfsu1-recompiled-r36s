/*
 * nfsu1_runtime.c - Speed.exe 1.4 (Need for Speed: Underground) specifics of the R36S host.
 *
 * Graphics profile. NFSU1 has no registry options (NFSU2's NFSU2_REGISTRY mechanism finds nothing to
 * set): its video options are globals at 0x700E70..0x700EB4 plus the detail level at 0x736334. The game
 * fills them from a detail level 0..3 (sub_00410ED0, edx = level; sub_00410A00 picks the level from a
 * benchmark score, low on this hardware) and clamps each to its cap at 0x701040..0x701080
 * (0x410CE0: option = min(option, cap)). The launcher's profiles (NFSU.sh NFSU_GRAPHICS ->
 * NFSU_GFX -> NFSU2_GFX) are those presets, written as "VA=value;..." pairs (hex VA, or a hex offset
 * from 0x700000, e.g. "736334=2;E74=2;EB0=3"). They are applied on the game thread at Present, from
 * frame 30 on (before any race loads), and re-applied whenever the game resets them; a write happens
 * only when the value differs. NFSU2_GFX=0 (NFSU_GFX=0 in nfsu.cfg) leaves the game's own choice.
 * Each change is logged once per slot; with NFSU2_DIAG_FRAMES the bridge logs options and caps at
 * frame 1500.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

/* The NFSU2 port's D3DX effect-state memo (fx_cache.c) is NFSU2-specific: no capture here. The bridge
 * still tests the depth on every deferred device call. */
int nfs_fx_cap_depth;
void nfs_fx_capture(uint32_t token, const uint32_t* args, uint32_t count, int deferred) {
    (void)token; (void)args; (void)count; (void)deferred;
}

typedef struct { uint32_t va, value; } nfsu1_slot;
static nfsu1_slot s_gfx[48];
static unsigned s_gfx_count;
static int s_gfx_parsed;
static unsigned s_frames;

static void parse_gfx(void) {
    s_gfx_parsed = 1;
    const char* e = getenv("NFSU2_GFX");
    if (!e || !*e || !strcmp(e, "0")) {
        printf("[NFSU1 GFX] graphics options: the game's own (NFSU_GFX not set)\n");
        return;
    }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", e);
    for (char* tok = strtok(buf, ";, "); tok && s_gfx_count < sizeof s_gfx / sizeof s_gfx[0]; tok = strtok(NULL, ";, ")) {
        unsigned off = 0, value = 0;
        if (sscanf(tok, "%x=%u", &off, &value) != 2) continue;
        const uint32_t va = off >= 0x400000u ? off : 0x700000u + off;
        /* the options 0x700E70..0x700EB4, their caps 0x701040..0x701080 and the detail level only */
        if (!((va >= 0x700E70u && va <= 0x700EB4u) || (va >= 0x701040u && va <= 0x701080u) || va == 0x736334u) || (va & 3)) {
            printf("[NFSU1 GFX] ignored %s (not an option or cap slot)\n", tok);
            continue;
        }
        s_gfx[s_gfx_count].va = va;
        s_gfx[s_gfx_count].value = value;
        ++s_gfx_count;
    }
    printf("[NFSU1 GFX] profile: %u option slots from NFSU_GFX=\"%s\"\n", s_gfx_count, e);
}

/* Game thread, at every Present (guest_runtime.cpp d3dDispatch). */
void nfsu1_present_hook(void) {
    ++s_frames;
    if (!s_gfx_parsed) parse_gfx();
    if (!s_gfx_count || s_frames < 30) return;
    static unsigned logged;   /* one line per slot, the first time it is changed */
    for (unsigned i = 0; i < s_gfx_count; ++i) {
        const uint32_t now = MEM32(s_gfx[i].va);
        if (now == s_gfx[i].value) continue;
        if (i < 32 && !(logged & (1u << i))) {
            logged |= 1u << i;
            printf("[NFSU1 GFX] frame %u: %06X %u -> %u\n", s_frames, (unsigned)s_gfx[i].va, (unsigned)now, (unsigned)s_gfx[i].value);
        }
        MEM32(s_gfx[i].va) = s_gfx[i].value;
    }
}
