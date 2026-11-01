#pragma once
// Force-included only into locally generated C; Windows generation is untouched.
#include <time.h>
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
// Fixed guest base: the 4 GB guest space is reserved at this host address (guest_memory.h), so a guest
// access is base-constant + VA. With the base in a global (g_mem_base), the compiler had to reload it after
// every guest store (a store through a uint8_t* may alias any global): one extra dependent load per access,
// which an in-order Cortex-A35 cannot hide.
#ifdef NFS_FIXED_GUEST_BASE
#undef ADDR
#define ADDR(va) ((uintptr_t)(uint32_t)(va) + (uintptr_t)(NFS_FIXED_GUEST_BASE))
#endif
// Speed.exe 1.4: asset loaders whose inputs are checked before they run (guest_asset_guards.inc).
void nfs_validate_asset_entry(uint32_t va);
#undef RECOMP_ENTER
// g_cur_func (diagnostics only: crash and import logs) is set on entry only with NFS_TRACK_CUR_FUNC: a
// constant materialisation and a store on every call otherwise.
#ifdef NFS_TRACK_CUR_FUNC
#define NFS_SET_CUR_FUNC(va) (g_cur_func=(va))
#else
#define NFS_SET_CUR_FUNC(va) ((void)0)
#endif
// Per-game entry hooks (the VA is a constant, so this folds away in every other function). Speed.exe 1.4:
// the two asset loaders checked by guest_asset_guards.inc.
#define NFS_GAME_ENTRY_HOOKS(va) \
    if((va)==0x00447c00u||(va)==0x0059fb80u){ \
        RECOMP_REGS_OUT(); nfs_validate_asset_entry(va); RECOMP_REGS_IN(); \
    }
#define RECOMP_ENTER(va) do { \
    NFS_SET_CUR_FUNC(va); RECOMP_FLAGS_IN(); \
    NFS_GAME_ENTRY_HOOKS(va) \
} while(0)
static inline uint64_t nfs_guest_rdtsc(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    // A stable virtual 1 GHz counter, not an ARM hardware cycle counter.
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#define __rdtsc() nfs_guest_rdtsc()
static inline void nfs_guest_cpuid(uint32_t leaf) {
    g_eax=g_ebx=g_ecx=g_edx=0;
    if (leaf==0) {
        g_eax=1; g_ebx=0x756e6547; g_edx=0x49656e69; g_ecx=0x6c65746e;
    } else if (leaf==1) {
        g_eax=0x00000663;
        // x87, TSC, CX8, CMOV, MMX, SSE, SSE2; no SSE3/AVX/3DNow.
        g_edx=((1u<<0)|(1u<<4)|(1u<<8)|(1u<<15)|(1u<<23)|(1u<<25)|(1u<<26)) & g_cpuid_edx1;
    } else if (leaf==0x80000000u) g_eax=0x80000001u;
}
#undef CPUID
#define CPUID(a,b,c,d) do { RECOMP_REGS_OUT(); nfs_guest_cpuid((a)); RECOMP_REGS_IN(); } while (0)

// Indirect calls (vtables, function pointers): each call site keeps the last target it resolved through
// the lifted-function table (a per-site monomorphic inline cache), so a repeated call skips the three
// lookups (overrides, hash, imports). Only recomp_lookup hits are cached: they are pure; the override
// and import paths have side effects (dynamic thunk address, COM/DirectSound/DirectInput method token)
// and stay uncached.
// NFS_ICALL_TRACE keeps the crash-report ring of recent indirect calls (off by default: three stores per
// call on the hottest path).
#ifndef NFS_NO_ICALL_CACHE
// VA of the indirect call in flight (per thread), so import handlers that serve many VAs (the D3D9 COM
// methods) can be cached at the call site like lifted functions: recomp_lookup_cacheable returns such a
// handler only when it reads its method from nfs_icall_va instead of a lookup side effect.
#ifdef RECOMP_TLS_STRUCT
#define nfs_icall_va (g_cpu.icall_va)
#else
extern RECOMP_TLS uint32_t nfs_icall_va;
#endif
recomp_func_t recomp_lookup_cacheable(uint32_t va);
#ifdef NFS_ICALL_TRACE
#define NFS_ICALL_RECORD(va) do { g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = (va); \
    g_icall_from[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = g_cur_func; g_icall_trace_idx++; } while (0)
#define NFS_ICALL_COUNT() (g_icall_count++)
#else
#define NFS_ICALL_RECORD(va) ((void)0)
#define NFS_ICALL_COUNT() ((void)0)  /* a global counter store on every indirect call: diagnostics only */
#endif
// The cache is one 64-bit word (VA << 32 | function address), read and written atomically, so guest
// threads running in parallel (R36S) can never pair one target's VA with another's function. Function
// addresses fit in 32 bits in the non-PIE R36S executable; anywhere else the site is simply not cached.
// Two entries per site (polymorphic vtable calls alternate between a few targets): a new target goes to
// entry 0 and the previous entry 0 moves to entry 1.
#define NFS_ICALL_RESOLVE(_va, _fn) do { \
    static uint64_t _ic[2]; \
    const uint64_t _e = __atomic_load_n(&_ic[0], __ATOMIC_RELAXED); \
    if (__builtin_expect((uint32_t)(_e >> 32) == (_va) && (uint32_t)_e != 0, 1)) { \
        (_fn) = (recomp_func_t)(uintptr_t)(uint32_t)_e; \
    } else { \
        const uint64_t _e1 = __atomic_load_n(&_ic[1], __ATOMIC_RELAXED); \
        if ((uint32_t)(_e1 >> 32) == (_va) && (uint32_t)_e1 != 0) { \
            (_fn) = (recomp_func_t)(uintptr_t)(uint32_t)_e1; \
        } else { \
            (_fn) = recomp_lookup_manual(_va); \
            if (!(_fn)) { \
                (_fn) = recomp_lookup(_va); \
                if (!(_fn)) (_fn) = recomp_lookup_cacheable(_va); \
                if ((_fn) && ((uintptr_t)(_fn) >> 32) == 0) { \
                    __atomic_store_n(&_ic[1], _e, __ATOMIC_RELAXED); \
                    __atomic_store_n(&_ic[0], ((uint64_t)(_va) << 32) | (uint32_t)(uintptr_t)(_fn), __ATOMIC_RELAXED); \
                } \
            } \
            if (!(_fn)) (_fn) = recomp_lookup_import(_va); \
        } \
    } \
} while (0)
#undef RECOMP_ICALL
#define RECOMP_ICALL(target_va) do { \
    uint32_t _va = (uint32_t)(target_va); \
    NFS_ICALL_RECORD(_va); \
    NFS_ICALL_COUNT(); \
    recomp_func_t _fn; \
    NFS_ICALL_RESOLVE(_va, _fn); \
    if (_fn) { \
        NFS_CALLER_SAVE \
        PUSH32(esp, RECOMP_RETADDR); \
        RECOMP_FLAGS_OUT(); \
        RECOMP_REGS_OUT(); \
        nfs_icall_va = _va; \
        _fn(); \
        RECOMP_REGS_IN(); \
        NFS_CALLER_RESTORE \
    } else { \
        RECOMP_MISS("ICALL", _va); \
        eax = 0; \
    } \
} while(0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(target_va) do { \
    uint32_t _va = (uint32_t)(target_va); \
    NFS_ICALL_RECORD(_va); \
    NFS_ICALL_COUNT(); \
    recomp_func_t _fn; \
    NFS_ICALL_RESOLVE(_va, _fn); \
    if (_fn) { RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); nfs_icall_va = _va; _fn(); RECOMP_REGS_IN(); } \
    else if (_va == RECOMP_RETADDR) { } \
    else { RECOMP_MISS("ITAIL", _va); } \
} while(0)
#endif

// g_cur_func (the VA of the running lifted function) only feeds diagnostics (import logs, crash reports).
// Keeping it exact costs a thread-local load and store around every call; NFS_TRACK_CUR_FUNC restores it.
#ifdef NFS_TRACK_CUR_FUNC
#define NFS_CALLER_SAVE uint32_t _caller = g_cur_func;
#define NFS_CALLER_RESTORE g_cur_func = _caller;
#else
#define NFS_CALLER_SAVE
#define NFS_CALLER_RESTORE
#undef RECOMP_CALL
#define RECOMP_CALL(func) do { \
    PUSH32(esp, RECOMP_RETADDR); \
    RECOMP_FLAGS_OUT(); \
    RECOMP_REGS_OUT(); \
    func(); \
    RECOMP_REGS_IN(); \
} while(0)
#endif

// x87 stack push/pop always inline (gcc kept an out-of-line fp_push clone in hot float code).
#ifdef RECOMP_X87_WINDOW
static inline __attribute__((always_inline)) void nfs_fp_push(int* top, double val) {
    if (g_fp_sp <= 8) fp_window_recentre();
    --g_fp_sp;
    g_st_win[g_fp_sp] = val;
    g_st_i64_win[g_fp_sp] = 0;
    (*top)++;
}
static inline __attribute__((always_inline)) double nfs_fp_pop(int* top) {
    double val = g_st_win[g_fp_sp];
    ++g_fp_sp;
    if (g_fp_sp > RECOMP_X87_SLOTS - 9) fp_window_recentre();
    (*top)--;
    return val;
}
#undef fp_push
#undef fp_pop
#define fp_push(val) nfs_fp_push(&_fp_top, (val))
#define fp_pop()     nfs_fp_pop(&_fp_top)
#endif

// NFS_X87_LOCAL (needs RECOMP_TLS_STRUCT, RECOMP_LOCAL_REGS, RECOMP_X87_WINDOW): the x87 window position
// (fp_sp), TOP and the control word are locals of each lifted function, like the general registers: loaded
// at entry, written back around every call and return (RECOMP_REGS_OUT/IN). In memory, every guest store
// (an integer-derived pointer) could alias them, so GCC reloaded fp_sp after each one and stored it on each
// push/pop. The stack values themselves stay in g_cpu.st_win.
#if defined(NFS_X87_LOCAL) && defined(RECOMP_TLS_STRUCT) && defined(RECOMP_LOCAL_REGS) && defined(RECOMP_X87_WINDOW)
#undef RECOMP_REGS_LOCALS
#undef RECOMP_REGS_OUT
#undef RECOMP_REGS_IN
// l_stw / l_sti: the window's address (thread-local) computed once per function; GCC otherwise
// rematerialised it (tp + hi12 + lo12) at every stack access (-7.6% instructions in x87 code).
#define RECOMP_REGS_LOCALS uint32_t l_eax = g_eax, l_ecx = g_ecx, l_edx = g_edx, l_ebx = g_ebx, l_esp = g_esp, l_esi = g_esi, l_edi = g_edi, l_ebp = g_ebp; \
    int l_fp_sp = g_cpu.fp_sp, l_fp_top = g_cpu.fp_top; uint16_t l_fpu_cw = g_cpu.fpu_cw; \
    double *const l_stw = g_cpu.st_win; int64_t *const l_sti = g_cpu.st_i64_win;
#define RECOMP_REGS_OUT() (g_eax = l_eax, g_ecx = l_ecx, g_edx = l_edx, g_ebx = l_ebx, g_esp = l_esp, g_esi = l_esi, g_edi = l_edi, g_ebp = l_ebp, \
    g_cpu.fp_sp = l_fp_sp, g_cpu.fp_top = l_fp_top, g_cpu.fpu_cw = l_fpu_cw)
#define RECOMP_REGS_IN() (l_eax = g_eax, l_ecx = g_ecx, l_edx = g_edx, l_ebx = g_ebx, l_esp = g_esp, l_esi = g_esi, l_edi = g_edi, l_ebp = g_ebp, \
    l_fp_sp = g_cpu.fp_sp, l_fp_top = g_cpu.fp_top, l_fpu_cw = g_cpu.fpu_cw)
#undef _st
#define _st (l_stw + l_fp_sp)
#undef g_st_i64
#define g_st_i64 (l_sti + l_fp_sp)
#undef _fp_top
#define _fp_top l_fp_top
#undef _fpu_cw
#define _fpu_cw l_fpu_cw
#define NFS_FP_RECENTRE() do { g_cpu.fp_sp = l_fp_sp; fp_window_recentre(); l_fp_sp = g_cpu.fp_sp; } while (0)
#undef fp_push
#undef fp_pop
// A push no longer clears the slot's int64 shadow: its only reader, fistp qword (fp_st0_to_i64, 4 sites in
// the game), uses the shadow only while st0 == (double)shadow, which a stale shadow can satisfy only for
// |values| >= 2^53, where the double model is inexact anyway. One store less per push (-7% more).
#define fp_push(val) do { const double _fpv = (val); \
    if (__builtin_expect(l_fp_sp <= 8, 0)) NFS_FP_RECENTRE(); \
    --l_fp_sp; l_stw[l_fp_sp] = _fpv; ++l_fp_top; } while (0)
#define fp_pop() ({ const double _fpv = l_stw[l_fp_sp]; ++l_fp_sp; \
    if (__builtin_expect(l_fp_sp > RECOMP_X87_SLOTS - 9, 0)) NFS_FP_RECENTRE(); \
    --l_fp_top; _fpv; })
#endif

// ---- NFS_X87_LW: per-function local x87 window (needs NFS_X87_LOCAL; src/recomp/gen/nfs_lw.h) ----
// The x86 ABI keeps the FPU stack nearly empty across calls, and compiler output has one stack depth at every
// instruction. ports/r36s/lw/analyze.py proves that per function (consistent depth at every label and return,
// only direct calls whose depth change is known, at most 8 entries either way) and genheader.py writes
// NFS_LW_<entry VA> (1: in the scheme) and NFS_FPD_<function> (the depth change a call to it leaves).
// In such a function the stack lives in a local array whose index starts at a constant, so GCC's constant
// propagation resolves every stack slot and keeps the values in registers. The live entries meet the
// shared window only at calls and returns:
// - entry: the caller's 8 top entries are copied in (unused copies are dead and vanish);
// - before a call or return: the function's own entries are written out, and caller entries only if they
//   changed (compare with the entry snapshot);
// - after a call: the index moves by the callee's known delta and the own entries are read back
//   (the callee may have consumed or replaced them; intrinsics take arguments in ST0).
// Loop back edges (yield hook) keep the stack local. Functions outside the scheme use the shared window as before.
#ifdef NFS_X87_LW
#include "nfs_lw.h"
#ifdef NFS_LW_CHECK
#define NFS_LW_VA_DECL(va) const uint32_t l_lwva = (va);
#else
#define NFS_LW_VA_DECL(va)
#endif
#define NFS_LWB 16
static inline __attribute__((always_inline)) uint64_t nfs_dbits(double v) { uint64_t u; __builtin_memcpy(&u, &v, 8); return u; }
#undef RECOMP_REGS_LOCALS
#define RECOMP_REGS_LOCALS uint32_t l_eax = g_eax, l_ecx = g_ecx, l_edx = g_edx, l_ebx = g_ebx, l_esp = g_esp, l_esi = g_esi, l_edi = g_edi, l_ebp = g_ebp; \
    int l_fp_sp = g_cpu.fp_sp, l_fp_top = g_cpu.fp_top; uint16_t l_fpu_cw = g_cpu.fpu_cw; \
    double *const l_stw = g_cpu.st_win; int64_t *const l_sti = g_cpu.st_i64_win; \
    double l_lw[NFS_LWB + 8]; int64_t l_lwi[NFS_LWB + 8]; double l_lws[8]; int64_t l_lwis[8]; int l_g0 = 0, l_lwdelta = 0;
#undef RECOMP_ENTER
// l_lwk: caller entries the function may read or change (8), or 0 when the analysis proved it never goes below
// its entry depth (most functions): then nothing is copied in at entry nor compared at calls and returns.
#define RECOMP_ENTER(va) enum { l_lwon = NFS_LW_##va, l_lwk = NFS_LWK_##va }; NFS_LW_VA_DECL(va) \
    if (l_lwon) { l_g0 = l_fp_sp; l_fp_sp = NFS_LWB; \
        for (int _k = 0; _k < l_lwk; ++_k) { l_lw[NFS_LWB + _k] = l_lws[_k] = l_stw[l_g0 + _k]; l_lwi[NFS_LWB + _k] = l_lwis[_k] = l_sti[l_g0 + _k]; } } \
    do { \
        NFS_SET_CUR_FUNC(va); RECOMP_FLAGS_IN(); \
        NFS_GAME_ENTRY_HOOKS(va) \
    } while(0)
// x87 part of REGS_OUT / REGS_IN
#define NFS_LW_OUT() ({ if (l_lwon) { \
        const int _sp = l_g0 + (l_fp_sp - NFS_LWB); \
        for (int _k = l_fp_sp; _k < NFS_LWB; ++_k) { l_stw[_sp + _k - l_fp_sp] = l_lw[_k]; l_sti[_sp + _k - l_fp_sp] = l_lwi[_k]; } \
        for (int _k = 0; _k < l_lwk; ++_k) if (NFS_LWB + _k >= l_fp_sp && \
            (nfs_dbits(l_lw[NFS_LWB + _k]) != nfs_dbits(l_lws[_k]) || l_lwi[NFS_LWB + _k] != l_lwis[_k])) { \
            l_stw[l_g0 + _k] = l_lws[_k] = l_lw[NFS_LWB + _k]; l_sti[l_g0 + _k] = l_lwis[_k] = l_lwi[NFS_LWB + _k]; } \
        g_cpu.fp_sp = _sp; \
        if (__builtin_expect(_sp <= 8 || _sp > RECOMP_X87_SLOTS - 9, 0)) { fp_window_recentre(); l_g0 += g_cpu.fp_sp - _sp; } \
    } else g_cpu.fp_sp = l_fp_sp; 0; })
#define NFS_LW_IN() ({ if (l_lwon) { \
        l_fp_sp -= l_lwdelta; l_lwdelta = 0; \
        l_g0 = g_cpu.fp_sp - (l_fp_sp - NFS_LWB); \
        for (int _k = l_fp_sp; _k < NFS_LWB; ++_k) { l_lw[_k] = l_stw[l_g0 + _k - NFS_LWB]; l_lwi[_k] = l_sti[l_g0 + _k - NFS_LWB]; } \
    } else l_fp_sp = g_cpu.fp_sp; 0; })
#undef RECOMP_REGS_OUT
#undef RECOMP_REGS_IN
#define NFS_REGS_OUT_INT() (g_eax = l_eax, g_ecx = l_ecx, g_edx = l_edx, g_ebx = l_ebx, g_esp = l_esp, g_esi = l_esi, g_edi = l_edi, g_ebp = l_ebp, \
    g_cpu.fp_top = l_fp_top, g_cpu.fpu_cw = l_fpu_cw)
#define NFS_REGS_IN_INT() (l_eax = g_eax, l_ecx = g_ecx, l_edx = g_edx, l_ebx = g_ebx, l_esp = g_esp, l_esi = g_esi, l_edi = g_edi, l_ebp = g_ebp, \
    l_fp_top = g_cpu.fp_top, l_fpu_cw = g_cpu.fpu_cw)
#define RECOMP_REGS_OUT() (NFS_REGS_OUT_INT(), NFS_LW_OUT())
#define RECOMP_REGS_IN() (NFS_REGS_IN_INT(), NFS_LW_IN())
#undef _st
#define _st (l_lwon ? l_lw + l_fp_sp : l_stw + l_fp_sp)
#undef g_st_i64
#define g_st_i64 (l_lwon ? l_lwi + l_fp_sp : l_sti + l_fp_sp)
#undef fp_push
#undef fp_pop
#define fp_push(val) do { const double _fpv = (val); \
    if (l_lwon) { --l_fp_sp; l_lw[l_fp_sp] = _fpv; l_lwi[l_fp_sp] = 0; ++l_fp_top; } \
    else { if (__builtin_expect(l_fp_sp <= 8, 0)) NFS_FP_RECENTRE(); --l_fp_sp; l_stw[l_fp_sp] = _fpv; ++l_fp_top; } } while (0)
#define fp_pop() ({ double _fpv; \
    if (l_lwon) { _fpv = l_lw[l_fp_sp]; ++l_fp_sp; --l_fp_top; } \
    else { _fpv = l_stw[l_fp_sp]; ++l_fp_sp; if (__builtin_expect(l_fp_sp > RECOMP_X87_SLOTS - 9, 0)) NFS_FP_RECENTRE(); --l_fp_top; } \
    _fpv; })
// direct calls: the callee's depth change (NFS_FPD_<callee>) moves the local index after the call
// NFS_LW_CHECK (verification builds): after every call of a function in the scheme, the shared window's
// sp must have moved by the delta the analysis assumed (a callee that recentred the window is skipped);
// mismatches are reported by nfs_lw_mismatch (src/runtime/native_math.c).
#ifdef NFS_LW_CHECK
void nfs_lw_mismatch(uint32_t va, int expected, int actual);
#define NFS_LW_SP0() const int _lwsp0 = g_cpu.fp_sp
#define NFS_LW_VERIFY(d) do { if (l_lwon) { const int _a = _lwsp0 - g_cpu.fp_sp; \
    if (_a != (d) && g_cpu.fp_sp != RECOMP_X87_SLOTS / 2 - 4) nfs_lw_mismatch(l_lwva, (d), _a); } } while (0)
#else
#define NFS_LW_SP0() ((void)0)
#define NFS_LW_VERIFY(d) ((void)0)
#endif
#undef RECOMP_CALL
#define RECOMP_CALL(func) do { \
    PUSH32(esp, RECOMP_RETADDR); \
    RECOMP_FLAGS_OUT(); \
    RECOMP_REGS_OUT(); NFS_LW_SP0(); \
    func(); \
    NFS_LW_VERIFY(NFS_FPD_##func); \
    l_lwdelta = NFS_FPD_##func; \
    RECOMP_REGS_IN(); \
} while(0)
// a direct call whose callee's depth change is unknown to the analysis (a dynamic tail-jump thunk): the
// change inferred at this call site (ports/r36s/lw/annotate.py)
#define RECOMP_CALL_LW(d, func) do { \
    PUSH32(esp, RECOMP_RETADDR); \
    RECOMP_FLAGS_OUT(); \
    RECOMP_REGS_OUT(); NFS_LW_SP0(); \
    func(); \
    NFS_LW_VERIFY(d); \
    l_lwdelta = (d); \
    RECOMP_REGS_IN(); \
} while(0)
// indirect call with the depth change inferred at this site; a miss (no target) behaves as a call that
// returned 0 / 0.0, so the depth stays a compile-time constant
#define RECOMP_ICALL_LW(d, target_va) do { \
    uint32_t _va = (uint32_t)(target_va); \
    NFS_ICALL_RECORD(_va); \
    NFS_ICALL_COUNT(); \
    recomp_func_t _fn; \
    NFS_ICALL_RESOLVE(_va, _fn); \
    if (_fn) { \
        PUSH32(esp, RECOMP_RETADDR); \
        RECOMP_FLAGS_OUT(); \
        RECOMP_REGS_OUT(); NFS_LW_SP0(); \
        nfs_icall_va = _va; \
        _fn(); \
        NFS_LW_VERIFY(d); \
        l_lwdelta = (d); \
        RECOMP_REGS_IN(); \
    } else { \
        RECOMP_MISS("ICALL", _va); \
        eax = 0; \
        if (l_lwon && (d) > 0) { l_fp_sp -= (d); l_lw[l_fp_sp] = 0; l_lwi[l_fp_sp] = 0; l_fp_top += (d); } \
    } \
} while(0)
// tail jumps: in a function of the scheme the callee's state is the final state, so return right away
// (the generated RECOMP_RETURN after it would write the stale local stack back)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(target_va) do { \
    uint32_t _va = (uint32_t)(target_va); \
    NFS_ICALL_RECORD(_va); \
    NFS_ICALL_COUNT(); \
    recomp_func_t _fn; \
    NFS_ICALL_RESOLVE(_va, _fn); \
    if (_fn) { RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); nfs_icall_va = _va; _fn(); if (l_lwon) return; RECOMP_REGS_IN(); } \
    else if (_va == RECOMP_RETADDR) { } \
    else { RECOMP_MISS("ITAIL", _va); } \
} while(0)
// back edges: the yield hook never changes this thread's x87 state; the stack stays local
#undef RECOMP_BACKEDGE
#define RECOMP_BACKEDGE() do { \
    if (++g_backedges >= RECOMP_YIELD_EVERY && recomp_yield_hook) { \
        g_backedges = 0; \
        if (l_lwon) { NFS_REGS_OUT_INT(); g_cpu.fp_sp = l_g0 + (l_fp_sp - NFS_LWB); recomp_yield_hook(); NFS_REGS_IN_INT(); } \
        else { RECOMP_REGS_OUT(); recomp_yield_hook(); RECOMP_REGS_IN(); } \
    } } while (0)
#endif
// Annotated call forms (ports/r36s/lw/annotate.py) in builds without the local x87 window
#ifndef NFS_X87_LW
#define RECOMP_CALL_LW(d, func) RECOMP_CALL(func)
#define RECOMP_ICALL_LW(d, target_va) RECOMP_ICALL(target_va)
#endif
