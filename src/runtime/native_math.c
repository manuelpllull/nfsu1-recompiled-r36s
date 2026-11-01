/*
 * native_math.c - native version of a small, hot lifted math helper of Speed.exe 1.4 (R36S).
 *
 * The lifted code does every x87 operation in double precision on the emulated stack window; this port does
 * the same operation (a saturating conversion, then _ftol2's correction) on registers, so the result is
 * identical to the lifted function. The lifted definition is renamed *_orig (ports/r36s/guest_wraps.txt);
 * every caller, direct or through the dispatch table, reaches this one.
 *   NFSU2_NATIVE_MATH=0  lifted function
 *                     1  native (default)
 *                     2  verify: run both, compare, report mismatches ([NFSU1 MATH])
 *
 *   sub_00674898  MSVC _ftol2: ST(0) truncated toward zero to edx:eax, popped (called ~7000 times a frame in a
 *                 race: physics, camera, animation).
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
extern void sub_00674898_orig(void);
extern uint32_t getD3D9PresentCount(void);

static int g_mode = -1;
static inline int mode(void) {
    if (g_mode < 0) { const char* e = getenv("NFSU2_NATIVE_MATH"); g_mode = e ? atoi(e) : 1; }
    return g_mode;
}
static uint32_t vCalls, vBad, vFrame;

static inline int64_t ftolValue(double v, int64_t shadow) {
    if (shadow && (double)shadow == v) return shadow;          /* exact value of a fild qword */
    /* as the lifted fistp (a saturating conversion) followed by _ftol2's correction: NaN 0, overflow saturates */
    if (!(v == v)) return 0;
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)v;                                         /* C conversion truncates toward zero */
}
void sub_00674898(void) {
    const int m = mode();
    if (m == 0) { sub_00674898_orig(); return; }
    const int sp = g_cpu.fp_sp;
    const int64_t r = ftolValue(g_cpu.st_win[sp], g_cpu.st_i64_win[sp]);
    if (m == 2) {
        sub_00674898_orig();
        ++vCalls;
        if (g_eax != (uint32_t)r || g_edx != (uint32_t)((uint64_t)r >> 32)) {
            if (vBad++ < 10) printf("[NFSU1 MATH] ftol mismatch: native %lld, lifted %08x%08x\n", (long long)r, g_edx, g_eax);
        }
        const uint32_t now = getD3D9PresentCount();
        if (now != vFrame) { vFrame = now; if (now % 600 == 0) printf("[NFSU1 MATH] verify: ftol %u calls %u bad\n", vCalls, vBad); }
        return;
    }
    /* fistp + fild + compare/correct in the original; net effect: pop ST(0), edx:eax = trunc */
    g_cpu.fp_sp = sp + 1;
    if (g_cpu.fp_sp > RECOMP_X87_SLOTS - 9) fp_window_recentre();
    g_cpu.fp_top -= 1;
    g_eax = (uint32_t)r; g_edx = (uint32_t)((uint64_t)r >> 32);
    g_esp += 4;  /* ret */
}
