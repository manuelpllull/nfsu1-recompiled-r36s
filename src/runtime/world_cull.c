/*
 * world_cull.c - Speed.exe 1.4: the world draw-list flush (sub_0040ED50) on the R36S.
 *
 * sub_0040ED50 flushes the render lists: 112 buckets (heads at 0x78A4E8 + 8*b; b = layer * 7 + slot) of
 * circular lists of material groups; a group ([g+0x10] = material, set up by sub_0040F3D0) owns a circular
 * list of items at g+0x18. Slot k is the struct at 0x71ABB8 + 0xE0*k (effect [S+0x18], vertex declaration
 * [S+4], stream stride [S+8], hooks [S+0xD8]/[S+0xDC]); the current one is [0x73640C]. Per item the game
 * spends ~50 us on the device: the world matrix into the effect (sub_00410400 and friends), the effect's
 * Pass() (D3DX re-applies every state and constant of the pass), DrawIndexedPrimitive.
 * Item: [+8] strip (index range; [strip+0xC] index count, [strip+0x34] first index pointer), [+0xC] strip flag
 * (triangle strip through DrawPrimitiveUP), [+0x10] -> [solid] (+0x18 CPU index base, +0x20 IB, +0x28 CPU
 * vertices, +0x2C VB, +0x30 byte size, +0x34 vertex count), [+0x14] flags, [+0x18] effect info E ([E+0x14] =
 * passes, [E+0x18] = effect, [E+0x44] = needs sub_004100A0), [+0x1C]/[+0x20] lighting inputs, [+0x24] world
 * matrix (0x78BCC0 = identity), [+0x28] extra data. Items go back to the pool [0x7363D8], groups to [0x7363D4],
 * strips to [0x7363B8] (free list head +0x10, count +0x18).
 * The current view's matrices: block [0x740460 + 0x60 * [[0x736458]+4]], VIEW (row-vector D3D matrix) at +0.
 *
 * 1. Culling. Items whose bounding sphere lies entirely beyond NFSU2_DRAW_DIST metres from the camera and that
 *    are smaller than NFSU2_CULL_PX pixels (projected radius at 640x480, default 7, i.e. under ~14 pixels
 *    across) are unlinked and returned to the pool here, so the game never does their per-item work. The game
 *    draws its world without fog, so a plain distance cull would make whole buildings pop; this one only drops
 *    props too small to notice. NFSU2_DRAW_DIST=0 only counts; NFSU2_CULL_PX=0 culls by distance alone.
 * 2. A native port of the flush (NFSU2_NATIVE_FLUSH=0: the lifted function): same calls, same order, same
 *    globals.
 * 3. Item runs with a learned constant model (NFSU2_FLUSH_MERGE=0: off; see below).
 */
#include "recomp_types.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
extern void sub_0040ED50_orig(void);
extern uint32_t getD3D9PresentCount(void);
#define M32(a) (*(uint32_t*)ADDR(a))
#define M8(a) (*(uint8_t*)ADDR(a))
static int validGuest(uint32_t va) { return va >= 0x400000u && va < 0x30000000u; }

#define BSLOTS 8192
typedef struct { uint32_t solid, data; float c[3], r; int ok; } Bound;
static Bound bounds[BSLOTS];
static const Bound* solidBound(uint32_t solid) {
    Bound* b = &bounds[(solid >> 3) & (BSLOTS - 1)];
    const uint32_t data = M32(solid + 0x28);
    if (b->solid == solid && b->data == data) return b;
    b->solid = solid; b->data = data; b->ok = 0;
    const uint32_t size = M32(solid + 0x30), n = M32(solid + 0x34);
    if (!validGuest(data) || !n || size < n * 12 || size % n || size / n > 256) return b;
    const uint32_t stride = size / n;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < n; ++i) {
        const float* p = (const float*)ADDR(data + i * stride);
        for (int k = 0; k < 3; ++k) { if (!(p[k] == p[k]) || fabsf(p[k]) > 1e6f) return b; if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
    }
    float r2 = 0;
    for (int k = 0; k < 3; ++k) { b->c[k] = 0.5f * (mn[k] + mx[k]); const float h = 0.5f * (mx[k] - mn[k]); r2 += h * h; }
    b->r = sqrtf(r2); b->ok = 1;
    return b;
}

static int freshBound(uint32_t solid, Bound* o) {
    const uint32_t data = M32(solid + 0x28), size = M32(solid + 0x30), n = M32(solid + 0x34);
    o->ok = 0; o->solid = solid; o->data = data;
    if (!validGuest(data) || !n || size < n * 12 || size % n || size / n > 256) return 0;
    const uint32_t stride = size / n;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < n; ++i) {
        const float* p = (const float*)ADDR(data + i * stride);
        for (int k = 0; k < 3; ++k) { if (!(p[k] == p[k]) || fabsf(p[k]) > 1e6f) return 0; if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
    }
    float r2 = 0;
    for (int k = 0; k < 3; ++k) { o->c[k] = 0.5f * (mn[k] + mx[k]); const float h = 0.5f * (mx[k] - mn[k]); r2 += h * h; }
    o->r = sqrtf(r2); o->ok = 1;
    return 1;
}
static int g_cdbg = -1; static uint32_t stStale, stCulledNear;

static float drawDist = -1;
static uint32_t stFrame, stFrames, stItems, stCulled, stNoBound, stHist[8];
static float pxMax = -1;   /* NFSU2_CULL_PX: only items smaller than this many pixels (radius at 640x480) are culled; 0 = by distance alone */

/* ---- Native flush: guest calls ---- */
extern recomp_func_t recomp_lookup(uint32_t va);
extern recomp_func_t recomp_lookup_manual(uint32_t va);
extern recomp_func_t recomp_lookup_import(uint32_t va);
extern recomp_func_t recomp_lookup_cacheable(uint32_t va);
extern void sub_0040F3D0(void); extern void sub_00410400(void); extern void sub_004100A0(void);
extern void sub_0040F7C0(void); extern void sub_0040FAE0(void); extern void sub_004102F0(void);
extern void sub_0040ED10(void); extern void sub_00410650(void); extern void sub_00410550(void);
static inline void push32(uint32_t v) { g_esp -= 4; M32(g_esp) = v; }
/* cdecl: args[0] first; the caller pops */
static uint32_t ccall(recomp_func_t fn, int n, const uint32_t* a) {
    for (int i = n - 1; i >= 0; --i) push32(a[i]);
    push32(RECOMP_RETADDR);
    fn();
    g_esp += 4u * (uint32_t)n;
    return g_eax;
}
/* indirect call to a guest address (vtable entry), as RECOMP_ICALL resolves it; callee cleans (COM/thiscall) */
static uint32_t icall(uint32_t va, uint32_t thisEcx, int n, const uint32_t* a) {
    for (int i = n - 1; i >= 0; --i) push32(a[i]);
    push32(RECOMP_RETADDR);
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_cacheable(va);
    if (!fn) fn = recomp_lookup_import(va);
    g_ecx = thisEcx;
    g_cpu.icall_va = va;
    if (fn) fn(); else { g_esp += 4u + 4u * (uint32_t)n; g_eax = 0; }
    return g_eax;
}
static uint32_t devMethod(uint32_t method, int n, const uint32_t* a) {  /* a[0] = device */
    return icall(M32(M32(a[0]) + method * 4), 0, n, a);
}
static uint32_t fxMethod(uint32_t index, int n, const uint32_t* a) {  /* a[0] = effect */
    return icall(M32(M32(a[0]) + index * 4), 0, n, a);
}
/* D3D9 device method indices (vtable offset / 4) and ID3DXEffect (D3DX 9.0) methods used here */
enum { M_SetRenderState = 57, M_DIP = 82, M_DPUP = 83, M_SetVertexDecl = 87, M_SetVSConst = 94, M_SetStream = 100, M_SetIndices = 104, M_SetPSConst = 109 };
enum { FX_Begin = 64, FX_Pass = 65, FX_End = 66 };

typedef struct { uint32_t kind, method, a[9], off[2], len[2], arg[2]; } CapRec;
extern void nfsCapBegin(void);
extern int nfsCapEnd(void);
extern const void* nfsCapRecs(void);
extern const uint8_t* nfsCapBytes(void);
extern int nfsD3dDeviceCall(uint32_t method, const uint32_t* args, uint32_t* result);
extern int nfsD3dCall(uint32_t kind, uint32_t method, const uint32_t* args);

/*
 * ---- Item runs with a learned constant model (NFSU2_FLUSH_MERGE, default 1) ----
 * The effect's Pass() re-applies every state and constant of the pass for every item. Consecutive items of a
 * group mostly differ in nothing but the world matrix (instanced scenery: same material, other placement);
 * the D3D calls Pass() emits for them are then identical except the vertex/pixel shader constants, and each
 * constant register is either unchanged or a column of W*X with X independent of the item (WVP, WV...), i.e.
 * r = W * (W_head^-1 * r_head).
 * The first item of a run ("head") runs the full path with its D3D calls captured. A following item with the
 * same inputs except the matrix ([it+0x18], [it+0x1C], [it+0x20], [it+0x14], [it+0x28] equal; single pass; no
 * strip) is learned: it runs the full path too and its calls are compared with the head's - every non-constant
 * call must be identical, and each constant register keeps the hypotheses (constant / W-linear) it satisfied.
 * After 3 observations with a different matrix the model of that effect + shaders is trusted: following items
 * only get their W-linear registers set (computed natively), bind their buffers and draw - no world matrix
 * into the effect, no Pass(). Every 16th item of a trusted model still takes the full path and is checked; any
 * mismatch disables the model for good (logged as [NFSU1 FLUSH] model ... rejected).
 * Single-item groups, strips and items with extra data (cars: per-item lighting) take the plain path.
 */
#define NREG 256
typedef struct {
    int valid;
    uint32_t sig[5];
    uint32_t fx, vs, ps, otherHash, nOther, hasHash;
    uint32_t dbg[64][5];             /* method, a[1..3], data hash of the first other calls (rejection log) */
    float W[16];
    uint8_t has[2][NREG];
    float r[2][NREG][4];
    double x[2][NREG][4];
} Head;
typedef struct {
    uint32_t fx, key, vs, ps;
    int state;                       /* 0 learning, 1 trusted, -1 rejected */
    uint32_t nobs, nfast;
    uint8_t hyp[2][NREG];            /* bit0: constant, bit1: W-linear */
    uint32_t hasHash;                /* register set the runs below were built for */
    int nrun;
    struct { uint8_t s; uint16_t start, cnt; } run[24];   /* consecutive W-linear registers (trusted models) */
} Model;
#define MODELS 1024
static Model models[MODELS];
static uint32_t modelCount;
static Head heads[2];
typedef struct { int open; uint32_t fx; Head* head; Model* model; uint32_t lastW; float lastWv[16]; int groupVerify; } RunState;
static int g_merge = -1, g_nativeFlush = -1, g_groupDebug = -1;
static int g_commitRun = -1;
static int g_ftime = -1;
__attribute__((weak)) uint64_t nfsD3dPushedCount(void) { return 0; }  /* d3d9_render_thread.inc */
static uint64_t tb[8], tn[8];   /* NFSU2_FLUSH_TIME=1: ns and counts of group setup, full+capture, plain, fast items, group tail */
static inline uint64_t monoNs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#define TSTART uint64_t _t0 = g_ftime ? monoNs() : 0
#define TEND(k) do { if (g_ftime) { tb[k] += monoNs() - _t0; ++tn[k]; } } while (0)
static uint32_t stFlushItems, stFlushFast, stFlushLearn, stFlushVerify, stFlushPlain, stFlushCommit, stFlushNative, stFlushLifted, stRejected, stGroups, stGroupsEmpty;
static void closeRun(RunState* rs) { rs->open = 0; }
static int invert4(const float* m, double* out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-9) return 0;
        if (p != c) for (int j = 0; j < 8; ++j) { const double t = a[p][j]; a[p][j] = a[c][j]; a[c][j] = t; }
        const double d = a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; if (f != 0) for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
    return 1;
}
static inline void mulWx(const float* W, const double* x, float* out) {
    for (int i = 0; i < 4; ++i) out[i] = (float)(W[i * 4 + 0] * x[0] + W[i * 4 + 1] * x[1] + W[i * 4 + 2] * x[2] + W[i * 4 + 3] * x[3]);
}
static inline int close4(const float* a, const float* b) {
    for (int k = 0; k < 4; ++k) if (!(fabsf(a[k] - b[k]) <= 1e-3f + 2e-4f * fabsf(b[k]))) return 0;
    return 1;
}
/* group replay rebuilds a register from W and the flush's reference: the game recomputes large view/fog
 * coefficients with ~3e-4 relative jitter between groups, which is invisible but fails the exact test */
static inline int close4g(const float* a, const float* b) {
    for (int k = 0; k < 4; ++k) if (!(fabsf(a[k] - b[k]) <= 1e-3f + 5e-4f * fabsf(b[k]))) return 0;
    return 1;
}
static void sigOf(uint32_t it, uint32_t* s) { s[0] = M32(it + 0x18); s[1] = M32(it + 0x1c); s[2] = M32(it + 0x20); s[3] = M32(it + 0x14); s[4] = M32(it + 0x28); }
static int sameSig(uint32_t a, uint32_t b) { uint32_t x[5], y[5]; sigOf(a, x); sigOf(b, y); return !memcmp(x, y, sizeof x); }
static int runnable(uint32_t it) {
    return !M32(it + 0xC) && !M32(it + 0x1c) && !M32(it + 0x20) && !(M32(it + 0x28) && (M8(it + 0x14) & 1)) && validGuest(M32(it + 0x24)) && validGuest(M32(it + 0x10)) && validGuest(M32(it + 0x18)) && M32(M32(it + 0x18) + 0x14) == 1;
}
/* the head's constants and a hash of every other call it made (buffers and the draw excluded) */
static int buildHead(Head* h, uint32_t it, int n, const uint32_t* sig) {
    const CapRec* r = (const CapRec*)nfsCapRecs();
    const uint8_t* bytes = nfsCapBytes();
    memset(h->has, 0, sizeof h->has);
    h->valid = 0; h->vs = h->ps = 0; h->nOther = 0;
    uint32_t oh = 2166136261u;
#define HMIX(v) (oh = (oh ^ (uint32_t)(v)) * 16777619u)
    for (int i = 0; i < n; ++i) {
        const uint32_t m = r[i].method;
        if (r[i].kind != 2) { HMIX(r[i].kind); HMIX(m); continue; }
        if (m == M_SetStream || m == M_SetIndices || m == M_DIP) continue;
        if (m == M_SetVSConst || m == M_SetPSConst) {
            const int s = m == M_SetPSConst;
            const uint32_t start = r[i].a[1], cnt = r[i].a[3];
            if (start + cnt > NREG || r[i].len[0] != cnt * 16) return 0;
            const float* f = (const float*)(bytes + r[i].off[0]);
            for (uint32_t k = 0; k < cnt; ++k) { memcpy(h->r[s][start + k], f + 4 * k, 16); h->has[s][start + k] = 1; }
            continue;
        }
        if (m == 92) h->vs = r[i].a[1];
        if (m == 107) h->ps = r[i].a[1];
        HMIX(m);
        /* argument count (with the device) of the methods Pass() emits; the rest of a[] is stale */
        int na = 9;
        switch (m) {
            case 92: case 107: case 87: case 89: na = 2; break;
            case 57: case 65: case 44: case 46: na = 3; break;
            case 67: case 69: case 96: case 98: case 111: case 113: na = 4; break;
            default: break;
        }
        for (int k = 1; k < na; ++k) if (!((r[i].arg[0] == (uint32_t)k && r[i].len[0]) || (r[i].arg[1] == (uint32_t)k && r[i].len[1]))) HMIX(r[i].a[k]);
        uint32_t dh = 0;
        for (int sl = 0; sl < 2; ++sl) for (uint32_t k = 0; k < r[i].len[sl]; ++k) { HMIX(bytes[r[i].off[sl] + k]); dh = dh * 31u + bytes[r[i].off[sl] + k]; }
        if (h->nOther < 64) { uint32_t* d = h->dbg[h->nOther]; d[0] = m; d[1] = r[i].a[1]; d[2] = na > 2 ? r[i].a[2] : 0; d[3] = na > 3 ? r[i].a[3] : 0; d[4] = dh; }
        ++h->nOther;
    }
#undef HMIX
    h->otherHash = oh;
    memcpy(h->W, (const void*)ADDR(M32(it + 0x24)), 64);
    double inv[16];
    if (!invert4(h->W, inv)) return 0;
    for (int s = 0; s < 2; ++s) for (int k = 0; k < NREG; ++k) if (h->has[s][k])
        for (int i = 0; i < 4; ++i) h->x[s][k][i] = inv[i * 4 + 0] * h->r[s][k][0] + inv[i * 4 + 1] * h->r[s][k][1] + inv[i * 4 + 2] * h->r[s][k][2] + inv[i * 4 + 3] * h->r[s][k][3];
    { uint32_t hh = 2166136261u; for (int s = 0; s < 2; ++s) for (int k = 0; k < NREG; ++k) hh = (hh ^ h->has[s][k]) * 16777619u; h->hasHash = hh; }
    memcpy(h->sig, sig, sizeof h->sig);
    h->fx = M32(sig[0] + 0x18);
    h->valid = 1;
    return 1;
}
static Model* findModel(const Head* h) {
    uint32_t k = (h->fx * 0x9e3779b1u) ^ (h->sig[0] * 0x85ebca6bu) ^ (h->vs * 0xc2b2ae35u) ^ h->ps;
    for (int probe = 0; probe < 16; ++probe) {
        Model* m = &models[(k + (uint32_t)probe) & (MODELS - 1)];
        if (m->fx == h->fx && m->key == h->sig[0] && m->vs == h->vs && m->ps == h->ps) return m;
        if (!m->fx) {
            m->fx = h->fx; m->key = h->sig[0]; m->vs = h->vs; m->ps = h->ps; m->state = 0; m->nobs = 0; m->nfast = 0;
            memset(m->hyp, 3, sizeof m->hyp);
            ++modelCount;
            return m;
        }
    }
    return NULL;
}
/* B ran the full path after A in the same run: check / refine A's model with it */
static void learn(const Head* A, const Head* B, Model* m, int verifying) {
    if (!m || m->state < 0) return;
    const char* why = NULL; int bs = -1, br = -1;
    if (A->vs != B->vs || A->ps != B->ps) why = "shaders";
    else if (A->otherHash != B->otherHash || A->nOther != B->nOther) why = "other calls";
    else if (memcmp(A->has, B->has, sizeof A->has)) why = "register set";
    else {
        for (int s = 0; s < 2 && !why; ++s) for (int k = 0; k < NREG; ++k) {
            if (!A->has[s][k]) continue;
            float p[4]; mulWx(B->W, A->x[s][k], p);
            const uint8_t h = (uint8_t)((close4(A->r[s][k], B->r[s][k]) ? 1 : 0) | (close4(p, B->r[s][k]) ? 2 : 0));
            if (verifying) {
                /* what the fast path would have sent must match */
                const uint8_t hy = m->hyp[s][k];
                const int okv = (hy & 1) ? (h & 1) : (h & 2);
                if (!okv) { why = "verify"; bs = s; br = k; break; }
            } else {
                m->hyp[s][k] &= h;
                if (!m->hyp[s][k]) { why = "register"; bs = s; br = k; break; }
            }
        }
    }
    if (why) {
        m->state = -1; ++stRejected;
        static int shown;
        if (shown++ < 24) {
            printf("[NFSU1 FLUSH] model fx %08x key %08x vs %08x ps %08x rejected (%s", m->fx, m->key, m->vs, m->ps, why);
            if (br >= 0) { float p[4]; mulWx(B->W, A->x[bs][br], p);
                printf(" %c%d: head %.4g %.4g %.4g %.4g, item %.4g %.4g %.4g %.4g, W-linear %.4g %.4g %.4g %.4g", bs ? 'p' : 'c', br,
                       A->r[bs][br][0], A->r[bs][br][1], A->r[bs][br][2], A->r[bs][br][3], B->r[bs][br][0], B->r[bs][br][1], B->r[bs][br][2], B->r[bs][br][3], p[0], p[1], p[2], p[3]); }
            printf(") after %u observations\n", m->nobs);
            if (!strcmp(why, "other calls") || !strcmp(why, "register set")) {
                for (uint32_t i = 0; i < A->nOther && i < B->nOther && i < 64; ++i)
                    if (memcmp(A->dbg[i], B->dbg[i], 20)) { printf("   call %u: head m%u %08x %08x %08x %08x, item m%u %08x %08x %08x %08x\n", i,
                        A->dbg[i][0], A->dbg[i][1], A->dbg[i][2], A->dbg[i][3], A->dbg[i][4], B->dbg[i][0], B->dbg[i][1], B->dbg[i][2], B->dbg[i][3], B->dbg[i][4]); }
                printf("   calls: head %u, item %u\n", A->nOther, B->nOther);
                if (!strcmp(why, "register set")) for (int s = 0; s < 2; ++s) { int na = 0, nb = 0; for (int k = 0; k < NREG; ++k) { na += A->has[s][k]; nb += B->has[s][k]; } printf("   %s registers: head %d, item %d\n", s ? "pixel" : "vertex", na, nb); }
            }
        }
        return;
    }
    if (!verifying && memcmp(A->W, B->W, 64) && ++m->nobs >= 3) {
        /* trusted: the runs of W-linear registers the fast path sets */
        m->nrun = 0; m->hasHash = A->hasHash;
        for (int s = 0; s < 2; ++s) for (int k = 0; k < NREG; ++k) {
            if (!(A->has[s][k] && m->hyp[s][k] == 2)) continue;
            if (m->nrun && m->run[m->nrun - 1].s == s && m->run[m->nrun - 1].start + m->run[m->nrun - 1].cnt == k) { ++m->run[m->nrun - 1].cnt; continue; }
            if (m->nrun >= 24) { m->state = -1; ++stRejected; printf("[NFSU1 FLUSH] model fx %08x key %08x: too many register runs\n", m->fx, m->key); return; }
            m->run[m->nrun].s = (uint8_t)s; m->run[m->nrun].start = (uint16_t)k; m->run[m->nrun].cnt = 1; ++m->nrun;
        }
        m->state = 1;
    }
}

/* ---- item paths ---- */
static void freeTo(uint32_t poolPtrAddr, uint32_t obj) {
    const uint32_t pool = M32(poolPtrAddr);
    M32(obj) = M32(pool + 0x10); M32(pool + 0x18) -= 1; M32(pool + 0x10) = obj;
}
/* Native sub_00410400 for world-flush items (NFSU2_WORLD_MATRIX: 0 lifted, 1 native, 2 verify).
 * NFSU2_MATRIX_MULTIPLY selects its sub_005ECE7A helper independently: 0 lifted, 1 native, 2 bit-exact verify.
 * The active D3DX SSE multiply rounds every product and sum to float. Rows 0 and 3 sum pairwise,
 * (p0 + p1) + (p2 + p3); rows 1 and 2 sum sequentially, ((p0 + p1) + p2) + p3. */
static int g_worldMatrix = -1, g_matrixMultiply = -1;
static uint32_t matrixVerifyCalls, matrixVerifyBad, matrixVerifyFrame;
static uint32_t multiplyVerifyCalls, multiplyVerifyBad, multiplyVerifyFrame;
extern void sub_005AC69C(void);
static void worldMatrixMultiplyLifted(uint32_t out, uint32_t a, uint32_t b) {
    push32(b); push32(a); push32(out); push32(RECOMP_RETADDR);
    sub_005AC69C();                              /* stdcall: callee pops all three arguments */
}
static void worldMatrixMultiplyCompute(float tmp[16], uint32_t a, uint32_t b) {
    const float* A = (const float*)ADDR(a);
    const float* B = (const float*)ADDR(b);
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        const float p0 = A[r * 4]     * B[c];
        const float p1 = A[r * 4 + 1] * B[c + 4];
        const float p2 = A[r * 4 + 2] * B[c + 8];
        const float p3 = A[r * 4 + 3] * B[c + 12];
        if (r == 0 || r == 3)
            tmp[r * 4 + c] = (p0 + p1) + (p2 + p3);
        else {
            float s = p0 + p1;
            s = s + p2;
            tmp[r * 4 + c] = s + p3;
        }
    }
}
static void worldMatrixMultiplyNative(uint32_t out, uint32_t a, uint32_t b) {
    float tmp[16];
    worldMatrixMultiplyCompute(tmp, a, b);
    memcpy((void*)ADDR(out), tmp, sizeof tmp);   /* also handles out == a or out == b like the original */
}
static void worldMatrixMultiply(uint32_t out, uint32_t a, uint32_t b) {
    if (g_matrixMultiply < 0) { const char* e = getenv("NFSU2_MATRIX_MULTIPLY"); g_matrixMultiply = e ? atoi(e) : 1; }
    if (g_matrixMultiply == 0 || M32(0x6D4ACC) != 0x005ECE7A) { worldMatrixMultiplyLifted(out, a, b); return; }
    if (g_matrixMultiply != 2) { worldMatrixMultiplyNative(out, a, b); return; }

    float expected[16], native[16];
    /* Native writes only this host temporary during verification; the lifted call owns the real output. */
    worldMatrixMultiplyCompute(native, a, b);
    worldMatrixMultiplyLifted(out, a, b);
    memcpy(expected, (const void*)ADDR(out), 64);
    ++multiplyVerifyCalls;
    if (memcmp(native, expected, 64)) {
        const uint32_t report = multiplyVerifyBad++;
        if (report < 10) {
            int bad = 0; while (bad < 16 && !memcmp(&native[bad], &expected[bad], sizeof(float))) ++bad;
            uint32_t nb, eb; memcpy(&nb, &native[bad], 4); memcpy(&eb, &expected[bad], 4);
            printf("[NFSU1 MATMUL] mismatch: out %08x element %d native %08x lifted %08x (a %08x b %08x)\n",
                   out, bad, nb, eb, a, b);
            if (!report) {
                const uint32_t* av = (const uint32_t*)ADDR(a); const uint32_t* bv = (const uint32_t*)ADDR(b);
                printf("[NFSU1 MATMUL] A"); for (int i = 0; i < 16; ++i) printf(" %08x", av[i]);
                printf("\n[NFSU1 MATMUL] B"); for (int i = 0; i < 16; ++i) printf(" %08x", bv[i]);
                printf(" target %08x\n", M32(0x6D4ACC));
            }
        }
    }
    const uint32_t now = getD3D9PresentCount();
    if (now != multiplyVerifyFrame) {
        multiplyVerifyFrame = now;
        if (now % 600 == 0) printf("[NFSU1 MATMUL] verify: %u calls, %u mismatches\n", multiplyVerifyCalls, multiplyVerifyBad);
    }
}
static void worldMatrixNative(uint32_t W) {
    M32(0x71B1F4) += 1;
    if (W == 0x78BCC0) M32(0x71B200) += 1;
    const uint32_t S = M32(0x73640C), view = g_edi;
    if (W == M32(0x78EB80) && S == M32(0x78EB7C) && view == M32(0x7010B8)) return;
    const uint32_t V = M32(0x740460 + view * 0x60), dev = M32(0x73636C);
    if (view != M32(0x7010B8)) {
        if (M32(0x736414)) {
            { const uint32_t a[3] = {dev, 2, V}; devMethod(44, 3, a); }
            { const uint32_t a[3] = {dev, 3, V + 0x40}; devMethod(44, 3, a); }
        }
        M32(0x7010B8) = view;
        M32(0x71B1FC) += 1;
    }
    M32(0x71B1F8) += 1;
    M32(0x78EB7C) = S;
    M32(0x78EB80) = W;
    if (M32(S) == 5 && M32(0x736418)) {
        g_esp -= 64; const uint32_t out = g_esp;
        worldMatrixMultiply(out, W, V);
        const uint32_t fx = M32(S + 0x18);
        const uint32_t a[3] = {fx, M32(S + 0x24), out};
        fxMethod(38, 3, a);                    /* ID3DXEffect::SetMatrix */
        g_esp += 64;
    }
    if (M32(0x736414)) { const uint32_t a[3] = {dev, 0x100, W}; devMethod(44, 3, a); }
    if (M32(0x736418)) {
        g_esp -= 64; const uint32_t out = g_esp;
        worldMatrixMultiply(out, W, V + 0x80);
        const uint32_t fx = M32(S + 0x18);
        const uint32_t a[3] = {fx, M32(S + 0x1C), out};
        fxMethod(38, 3, a);
        g_esp += 64;
    }
}
static void worldMatrixLifted(uint32_t W) { const uint32_t a[1] = {W}; ccall(sub_00410400, 1, a); }
static uint64_t matrixCallHash(int n) {
    const CapRec* r = (const CapRec*)nfsCapRecs();
    const uint8_t* bytes = nfsCapBytes();
    uint64_t h = 1469598103934665603ull;
#define MATRIX_HMIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    for (int i = 0; i < n; ++i) {
        MATRIX_HMIX(r[i].kind); MATRIX_HMIX(r[i].method);
        for (int k = 1; k < 4; ++k) {
            int pointer = 0;
            for (int s = 0; s < 2; ++s) if (r[i].arg[s] == (uint32_t)k && r[i].len[s]) pointer = 1;
            if (!pointer) MATRIX_HMIX(r[i].a[k]);
        }
        for (int s = 0; s < 2; ++s) for (uint32_t k = 0; k < r[i].len[s]; ++k) MATRIX_HMIX(bytes[r[i].off[s] + k]);
    }
#undef MATRIX_HMIX
    return h;
}
static void worldMatrixCalls(uint32_t W) {
    if (g_worldMatrix < 0) { const char* e = getenv("NFSU2_WORLD_MATRIX"); g_worldMatrix = e ? atoi(e) : 1; }
    if (g_worldMatrix == 0) { worldMatrixLifted(W); return; }
    if (g_worldMatrix != 2) { worldMatrixNative(W); return; }

    /* Run the original, restore its cache/counter side effects, then run native and compare the
     * complete transform/SetMatrix call sequence. Duplicate state calls are idempotent; counters
     * are restored so verification has the same final guest state as one real call. */
    const uint32_t saved[7] = {M32(0x71B1F4), M32(0x71B1F8), M32(0x71B1FC), M32(0x71B200),
                               M32(0x78EB80), M32(0x78EB7C), M32(0x7010B8)};
    nfsCapBegin(); worldMatrixLifted(W); const int on = nfsCapEnd();
    const uint64_t oh = matrixCallHash(on);
    M32(0x71B1F4) = saved[0]; M32(0x71B1F8) = saved[1]; M32(0x71B1FC) = saved[2]; M32(0x71B200) = saved[3];
    M32(0x78EB80) = saved[4]; M32(0x78EB7C) = saved[5]; M32(0x7010B8) = saved[6];
    nfsCapBegin(); worldMatrixNative(W); const int nn = nfsCapEnd();
    const uint64_t nh = matrixCallHash(nn);
    ++matrixVerifyCalls;
    if (on != nn || oh != nh) {
        if (matrixVerifyBad++ < 10) printf("[NFSU1 MATRIX] mismatch: lifted calls %d hash %016llx, native calls %d hash %016llx\n",
                                           on, (unsigned long long)oh, nn, (unsigned long long)nh);
    }
    const uint32_t now = getD3D9PresentCount();
    if (now != matrixVerifyFrame) {
        matrixVerifyFrame = now;
        if (now % 600 == 0) printf("[NFSU1 MATRIX] verify: %u calls, %u mismatches\n", matrixVerifyCalls, matrixVerifyBad);
    }
}
/* world matrix and lighting inputs into the effect (0x40EE3C..0x40EE8A) */
static void paramCalls(uint32_t it) {
    const uint32_t W = M32(it + 0x24), E = M32(it + 0x18);
    g_edi = M32(M32(0x736458) + 4);                       /* sub_00410400 takes the view index in edi */
    worldMatrixCalls(W);
    if (M32(E + 0x44)) { g_eax = 0x740760; g_ecx = W; push32(RECOMP_RETADDR); sub_004100A0(); }
    if (M32(it + 0x1c)) { g_eax = M32(it + 0x1c); g_ecx = W; const uint32_t a[1] = {W}; ccall(sub_0040F7C0, 1, a); }
    if (M32(it + 0x20)) { g_eax = M32(it + 0x14); g_ecx = M32(it + 0x20); push32(RECOMP_RETADDR); sub_0040FAE0(); }
}
/* device call: queued directly when the render thread can take it, else through the guest COM object */
static void devFast(uint32_t method, int n, const uint32_t* a) {
    uint32_t b[9] = {0};
    for (int i = 0; i < n && i < 9; ++i) b[i] = a[i];
    if (!nfsD3dCall(2, method, b)) devMethod(method, n, a);
}
/* stream source and indices of the item's mesh; returns whether there is something to draw */
static int bindBuffers(uint32_t it) {
    const uint32_t strip = M32(it + 8);
    const uint32_t solid = M32(M32(it + 0x10));
    const uint32_t dev = M32(0x73636C);
    if (M32(solid + 0x2c) != M32(0x7363E8)) {
        const uint32_t a[5] = {dev, 0, M32(solid + 0x2c), 0, M32(M32(0x73640C) + 8)};
        devFast(M_SetStream, 5, a);
        M32(0x7363E8) = M32(solid + 0x2c); M32(0x71B1E8) += 1;
    }
    if (M32(solid + 0x20) != M32(0x7363EC)) {
        const uint32_t a[2] = {dev, M32(solid + 0x20)};
        devFast(M_SetIndices, 2, a);
        M32(0x7363EC) = M32(solid + 0x20); M32(0x71B1EC) += 1;
    }
    return M32(strip + 0xC) && M32(solid + 0x34) < 0xFFFF;
}
static void drawItem(uint32_t it) {
    const uint32_t strip = M32(it + 8);
    const uint32_t solid = M32(M32(it + 0x10));
    const uint32_t dev = M32(0x73636C);
    const uint32_t prims = M32(strip + 0xC) / 3;
    const uint32_t start = (uint32_t)((int32_t)(M32(strip + 0x34) - M32(solid + 0x18)) >> 1);
    const uint32_t a[7] = {dev, 4, 0, 0, M32(solid + 0x34), start, prims};
    devFast(M_DIP, 7, a);
}
static void countItem(uint32_t it) {
    const uint32_t solid = M32(M32(it + 0x10));
    M32(0x71B1E0) += 1;
    M32(0x71B1F0) += M32(solid + 0x34) / 3;
}
/* the original per-item path; keepOpen: leave a single pass "open" for a run */
static void nativeItemFull(uint32_t it, RunState* rs, int keepOpen) {
    paramCalls(it);
    const uint32_t E = M32(it + 0x18), strip = M32(it + 8);
    if (M32(it + 0xC)) {
        /* triangle strip (0x40EFB4): z writes off, DrawPrimitiveUP per pass, z writes on, the strip back to its pool */
        uint32_t dev = M32(0x73636C);
        { const uint32_t a[3] = {dev, 0xE, 0}; devMethod(M_SetRenderState, 3, a); }
        M32(0x7363E8) = 0;
        for (uint32_t p = 0; p < M32(E + 0x14); ++p) {
            const uint32_t fx = M32(E + 0x18);
            { const uint32_t a[2] = {fx, p}; fxMethod(FX_Pass, 2, a); }
            dev = M32(0x73636C);
            { const uint32_t a[5] = {dev, 5, M32(strip) - 2, strip + 4, 0x18}; devMethod(M_DPUP, 5, a); }
        }
        dev = M32(0x73636C);
        { const uint32_t a[3] = {dev, 0xE, 1}; devMethod(M_SetRenderState, 3, a); }
        M32(0x71B1E4) += 1;
        freeTo(0x7363B8, strip);
        return;
    }
    if (M32(it + 0x28) && (M8(it + 0x14) & 1)) { g_eax = strip; const uint32_t a[1] = {M32(it + 0x28)}; ccall(sub_004102F0, 1, a); }
    if (!bindBuffers(it)) return;
    const uint32_t np = M32(E + 0x14);
    for (uint32_t p = 0; p < np; ++p) {
        const uint32_t fx = M32(E + 0x18);
        { const uint32_t a[2] = {fx, p}; fxMethod(FX_Pass, 2, a); }
        drawItem(it);
        if (np == 1 && keepOpen) { rs->open = 1; rs->fx = fx; }
    }
    countItem(it);
}
/* ---- Commit runs for plain items (NFSU2_COMMIT_RUN, default 1) ----
 * This game's D3DX effect object has Begin/BeginPass/End at vtable 64/65/66 and no exposed
 * CommitChanges, so every per-item BeginPass re-runs all of the pass's state and constant
 * assignments. For items of one material group, the first item's BeginPass has already applied the
 * material states; the follow-up items' sub_00410400 and friends set the per-item matrices and
 * constants themselves. If a BeginPass would then queue no changed device state, it is pure guest
 * overhead and can be skipped. Mode 1 checks each effect once with a real BeginPass and counts the
 * non-redundant device calls the render thread would queue (they are filtered against the current
 * state); a zero count marks the effect safe and later items skip the Pass. Mode 2 always runs the
 * BeginPass and reports the count (diagnostic); mode 0 is the old full path.
 */
static struct { uint32_t fx; int8_t safe; } commitFx[64];
static uint32_t stCommitItems, stCommitFull, stCommitChecked, stCommitMissed, commitProbes;
static int commitEffect(uint32_t fx) {
    for (int i = 0; i < 64; ++i) if (commitFx[i].fx == fx) return commitFx[i].safe;
    return 0;
}
static void commitEffectSet(uint32_t fx, int safe) {
    int empty = -1;
    for (int i = 0; i < 64; ++i) {
        if (commitFx[i].fx == fx) { commitFx[i].safe = (int8_t)safe; return; }
        if (empty < 0 && !commitFx[i].fx) empty = i;
    }
    if (empty >= 0) { commitFx[empty].fx = fx; commitFx[empty].safe = (int8_t)safe; }
}
static int canCommitItem(uint32_t it, const RunState* rs) {
    if (g_commitRun <= 0 || M32(it + 0xC)) return 0;
    const uint32_t E = M32(it + 0x18);
    if (!validGuest(E) || M32(E + 0x14) != 1) return 0;
    const uint32_t fx = M32(E + 0x18);
    return validGuest(fx) && rs->open && rs->fx == fx && commitEffect(fx) >= 0;
}
static void commitItem(uint32_t it, RunState* rs) {
    const uint32_t E = M32(it + 0x18), fx = M32(E + 0x18), strip = M32(it + 8);
    paramCalls(it);
    if (M32(it + 0x28) && (M8(it + 0x14) & 1)) { g_eax = strip; const uint32_t a[1] = {M32(it + 0x28)}; ccall(sub_004102F0, 1, a); }
    if (!bindBuffers(it)) { ++stCommitFull; return; }
    const int state = commitEffect(fx);
    int verify = g_commitRun == 2 || state == 0;
    if (!verify && state == 1 && ((++commitProbes & 0x7FF) == 0)) verify = 1;   /* periodic re-check */
    if (verify) {
        const uint64_t p0 = nfsD3dPushedCount();
        const uint32_t a[2] = {fx, 0};
        fxMethod(FX_Pass, 2, a);
        const uint64_t missed = nfsD3dPushedCount() - p0;
        ++stCommitChecked;
        stCommitMissed += (uint32_t)missed;
        if (g_commitRun == 1 && (state == 0 || missed)) commitEffectSet(fx, missed ? -1 : 1);
        if (missed) { static int shown; if (shown++ < 10) printf("[NFSU1 COMMIT] fx %08x: BeginPass still changed %u device states\n", fx, (unsigned)missed); }
    }
    drawItem(it);
    countItem(it);
    rs->open = 1; rs->fx = fx;
    ++stCommitItems;
}
/*
 * ---- Instanced drawing of world geometry (GLES 3.0 / GL_MAX_VERTEX_UNIFORM_VECTORS) ----
 * Consecutive fast items of one group with identical mesh and index range are merged
 * into a single indexed instanced draw call via nfsD3dDrawInstanced.
 */
#define INSTANCE_MAX 64
#define INSTANCE_REGS 16
extern int nfsD3dDrawInstanced(const uint32_t* dip, uint32_t registerCount, const uint16_t* registers, uint32_t instances, const float* data);

static int instancingEnabled(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("NFSU2_INSTANCING"); on = !(e && e[0] == '0'); }
    return on;
}

/* NFSU2_INSTANCING_GROUP (default 1): inside a replayed group, merge all items with the same mesh,
 * not just the consecutive ones. The group shares one material/effect record and the group's items
 * share its input signature; only the order within the group changes. That is safe for an opaque
 * material (alpha blend off, z test/write on), which grecCanBatchG() requires. */
static int g_instGroup = -1;
static int groupInstancingEnabled(void) {
    if (g_instGroup < 0) {
        const char* e = getenv("NFSU2_INSTANCING_GROUP");
        if (!e) e = getenv("NFSU2_GROUP_BATCH");
        g_instGroup = e ? atoi(e) : 1;
    }
    return g_instGroup != 0;
}

static uint32_t stInstancedDraws, stInstancedItems;
static uint32_t stGroupBatchDraws, stGroupBatchItems, stGroupBatchSaved;

/* Extract vertex shader registers that vary with W for this model; returns 0 if any pixel register varies or too many */
static int worldMatrixRegisters(const Model* m, uint16_t* regs) {
    int n = 0;
    for (int q = 0; q < m->nrun; ++q) {
        if (m->run[q].s != 0) return 0; /* pixel shader constant varies with W -> cannot instance */
        for (uint32_t j = 0; j < m->run[q].cnt; ++j) {
            if (n >= INSTANCE_REGS) return 0;
            regs[n++] = (uint16_t)(m->run[q].start + j);
        }
    }
    return n;
}

/* Attempts to merge `it` and any consecutive matching items in `ihead` into an instanced draw.
 * Returns 1 if an instanced draw was submitted (and all items consumed), 0 otherwise. */
static int tryDrawInstancedRun(uint32_t it, RunState* rs, uint32_t ihead, const uint16_t* regs, int nregs) {
    const uint32_t strip = M32(it + 8);
    const uint32_t solid = M32(M32(it + 0x10));
    if (!validGuest(strip) || !validGuest(solid)) return 0;
    if (!M32(strip + 0xC) || M32(solid + 0x34) >= 0xFFFF) return 0;

    uint32_t items[INSTANCE_MAX];
    items[0] = it;
    uint32_t n = 1;
    for (uint32_t next = M32(ihead); next != ihead && n < INSTANCE_MAX; next = M32(next)) {
        if (!validGuest(next) || !runnable(next) || !sameSig(it, next)) break;
        if (M32(M32(next + 0x10)) != solid) break;
        const uint32_t s2 = M32(next + 8);
        if (!validGuest(s2)) break;
        if (M32(s2 + 0x34) != M32(strip + 0x34) || M32(s2 + 0xC) != M32(strip + 0xC)) break;
        items[n++] = next;
    }
    if (n < 2) return 0;

    if (!bindBuffers(it)) return 0;

    const Head* h = rs->head;
    float data[INSTANCE_MAX * INSTANCE_REGS * 4];
    for (uint32_t i = 0; i < n; ++i) {
        const float* W = (const float*)ADDR(M32(items[i] + 0x24));
        for (int j = 0; j < nregs; ++j) {
            mulWx(W, h->x[0][regs[j]], data + (i * (uint32_t)nregs + (uint32_t)j) * 4);
        }
    }

    const uint32_t dip[6] = {
        4, /* D3DPT_TRIANGLELIST */
        0, /* BaseVertexIndex */
        0, /* MinVertexIndex */
        M32(solid + 0x34), /* NumVertices */
        (uint32_t)((int32_t)(M32(strip + 0x34) - M32(solid + 0x18)) >> 1), /* StartIndex */
        M32(strip + 0xC) / 3 /* PrimitiveCount */
    };

    if (!nfsD3dDrawInstanced(dip, (uint32_t)nregs, regs, n, data)) {
        return 0;
    }

    /* The instanced draw feeds the per-instance array; it does not change the global W-linear
     * registers, which still belong to the last item fastItem() ran. Advancing lastW to the batch's
     * last item would make the next fastItem() skip its register upload while the globals are stale. */

    for (uint32_t i = 0; i < n; ++i) countItem(items[i]);

    for (uint32_t i = 1; i < n; ++i) {
        const uint32_t item = items[i];
        const uint32_t nxt = M32(item), prv = M32(item + 4);
        M32(prv) = nxt; M32(nxt + 4) = prv;
        freeTo(0x7363D8, item);
        ++stFlushItems;
        ++stFlushFast;
    }

    ++stInstancedDraws;
    stInstancedItems += n;
    return 1;
}

/*
 * ---- Group-head replay (NFSU2_GROUP_CACHE, default 1) ----
 * Every material group pays the full path for its first item: the world matrix into the effect and the effect's
 * Pass() (state re-application, every constant). That head cost is per group, not per item, and the groups repeat
 * every frame: the same material emits the same non-constant calls and the same shader constants, except the ones
 * that follow the world matrix. Unlike NFSU2 the setup itself (sub_0040F3D0) is still the game's; only the head's
 * captured device calls are replayed, which is where most of the per-group time goes.
 *
 * Per effect (vs, ps) the class of each constant register is learned from full heads: L (W * x with x the same for
 * every group of the effect in this flush/view), G (same as this flush's reference group, e.g. fog) or M (the same
 * for the material in every frame). Non-constant calls are G or M. A group whose (slot, material) has a record and
 * whose items all have the record's inputs is replayed: sub_0040F3D0, the recorded calls, the constants by class,
 * then the items' buffers and draws (instanced where possible). Every 16th use runs the full group and checks the
 * prediction; a mismatch retires the record. Cars (per-item lighting/extra data) are not runnable, so never replay.
 */
#define GRC_MAX 48
#define GRD_MAX 1024
#define GRREG_MAX 96
#define GEFFECTS 32
#define GRECS 1024
typedef struct { uint32_t method, na, a[8], doff, dlen, darg, textureSerial; } GCall;
typedef struct { uint16_t s, reg; float v[4]; } GReg;
typedef struct {
    int valid;                       /* 2 = complete */
    uint32_t mat, slot, fx, sig[5], vs, ps;
    uint32_t texturesCheckedAtChurn; int texturesStale;
    int ncalls; GCall calls[GRC_MAX]; uint8_t data[GRD_MAX]; uint32_t dused;
    int nregs; GReg regs[GRREG_MAX];
    uint32_t uses, changes, flush; int bad, validated; uint32_t cleanVerifies;
    uint32_t badAt;                  /* present count when retired: retried after NFSU2_GROUP_RETRY frames */
} GRec;
typedef struct {
    uint32_t fx, vs, ps;
    int state; uint32_t nobs, nbad, nlog;
    uint8_t hyp[2][NREG];            /* bit0 L (world-linear), bit1 G (flush reference), bit2 M (material record) */
    uint8_t cls[2][NREG];
    uint64_t refKey; uint8_t refHas[2][NREG]; float rref[2][NREG][4]; double xref[2][NREG][4]; float refW[16];
    float lastLearnW[16];
    uint8_t chyp[GRC_MAX], ccls[GRC_MAX];
    uint32_t unclassifiedMats[4];
    GRec refRec;
} GEffect;
static GEffect geffects[GEFFECTS];
static GRec* grecs;
static uint64_t g_flushKey;
static uint32_t g_flushSeq;
static uint32_t g_learnBudget;
static uint32_t stGroupsReplayed, stGroupsVerify, stGroupsFull, stRecBad, stRecRecovered, stRecTexturesStale, stRecUnrecordable;
static uint32_t stCR[8];   /* why canReplay said no: 0 no record, 1 unvalidated, 2 effect, 3 refkey, 4 shape, 5 items, 6 textures, 7 verify */
static uint32_t stCRbad, stCRsig, stCRvb, stCRrun;  /* split of stCR[5]: retired, item signature, vertex buffer, not runnable */
static uint32_t stGroupCompared, stGroupChanged, stGroupHeads, stCapTries, stCapNoOpen, stCapBuildFail;
static int g_groupCache = -1;
static int g_retryFrames = -1;   /* NFSU2_GROUP_RETRY: frames before a retired material is re-learned; 0 = never */
extern uint32_t nfsD3dTextureChurn(void);
extern uint32_t nfsD3dObjectSerial(uint32_t handle);

static int naOf4(uint32_t m) {
    switch (m) {
        case 92: case 107: case 87: case 89: return 2;
        case 57: case 65: case 44: case 46: return 3;
        case 67: case 69: case 96: case 98: case 111: case 113: return 4;
        default: return 0;
    }
}
static void unlinkItem(uint32_t it) { const uint32_t n = M32(it), pv = M32(it + 4); M32(pv) = n; M32(n + 4) = pv; }
static uint32_t currentTextureChurn(void) { return nfsD3dTextureChurn(); }
/* A material whose record changed three times is not kept out of replay forever: most of those are a one-off
 * LOD / blend / weather transition. After this many presented frames it is re-recorded once and, if it is now
 * stable, validated and replayed again. NFSU2_GROUP_RETRY=0 keeps the old permanent-retirement behaviour. */
static uint32_t retryFrames(void) {
    if (g_retryFrames < 0) {
        const char* e = getenv("NFSU2_GROUP_RETRY");
        g_retryFrames = e ? atoi(e) : 120;
        if (g_retryFrames < 0) g_retryFrames = 0;
    }
    return (uint32_t)g_retryFrames;
}
/* Keyed on the game's effect object: the world effect does not re-set its shaders inside a group's item
 * capture (their handles are 0 there), but the effect pointer is always available and identifies the pass. */
static GEffect* geffectFor(uint32_t fx, uint32_t vs, uint32_t ps, int create) {
    for (int i = 0; i < GEFFECTS; ++i) if (geffects[i].fx == fx && fx) return &geffects[i];
    if (!create) return NULL;
    for (int i = 0; i < GEFFECTS; ++i) if (!geffects[i].fx) {
        GEffect* x = &geffects[i]; memset(x, 0, sizeof *x); x->fx = fx; x->vs = vs; x->ps = ps;
        memset(x->hyp, 7, sizeof x->hyp); memset(x->chyp, 6, sizeof x->chyp);
        return x;
    }
    return NULL;
}
static GRec* grecFor(uint32_t slot, uint32_t mat, int create) {
    if (!grecs) { grecs = (GRec*)calloc(GRECS, sizeof(GRec)); if (!grecs) return NULL; }
    uint32_t k = (mat * 0x9e3779b1u) ^ (slot * 0x85ebca6bu);
    for (int p = 0; p < 32; ++p) {
        GRec* r = &grecs[(k + (uint32_t)p) & (GRECS - 1)];
        if (r->mat == mat && r->slot == slot && r->mat) return r;
        if (!r->mat && !r->slot) { if (!create) return NULL; r->mat = mat; r->slot = slot; return r; }
    }
    return NULL;
}
/* append the captured non-constant device calls to a record; 0 = this head cannot be replayed */
static int grecCalls(GRec* rc, int n) {
    const CapRec* r = (const CapRec*)nfsCapRecs();
    const uint8_t* bytes = nfsCapBytes();
    for (int i = 0; i < n; ++i) {
        const uint32_t m = r[i].method;
        if (r[i].kind != 2) return 0;
        if (m == M_SetStream || m == M_SetIndices || m == M_DIP || m == M_DPUP ||
            m == M_SetVSConst || m == M_SetPSConst || m == 96 || m == 111 || m == 98 || m == 113) continue;
        const int na = naOf4(m);
        if (!na || rc->ncalls >= GRC_MAX || r[i].len[1]) return 0;
        GCall* c = &rc->calls[rc->ncalls++];
        c->method = m; c->na = (uint32_t)na;
        for (int k = 1; k < na; ++k) c->a[k - 1] = r[i].a[k];
        c->textureSerial = m == 65 ? nfsD3dObjectSerial(c->a[1]) : 0;
        c->dlen = r[i].len[0]; c->darg = r[i].arg[0]; c->doff = rc->dused;
        if (c->dlen) {
            if (c->darg < 1 || c->darg >= (uint32_t)na || rc->dused + c->dlen > GRD_MAX) return 0;
            memcpy(rc->data + rc->dused, bytes + r[i].off[0], c->dlen); rc->dused += (c->dlen + 3) & ~3u;
        }
    }
    return 1;
}
static int sameShapeG(const GRec* a, const GRec* b) {
    if (a->ncalls != b->ncalls) return 0;
    for (int i = 0; i < a->ncalls; ++i) {
        const GCall* p = &a->calls[i]; const GCall* q = &b->calls[i];
        if (p->method != q->method) return 0;
        if ((p->method == 44 || p->method == 46 || p->method == 57 || p->method == 65 || p->method == 67 || p->method == 69) && p->a[0] != q->a[0]) return 0;
        if ((p->method == 67 || p->method == 69) && p->a[1] != q->a[1]) return 0;
    }
    return 1;
}
static int callEqG(const GRec* a, const GCall* p, const GRec* b, const GCall* q) {
    if (p->method != q->method || p->dlen != q->dlen) return 0;
    for (uint32_t k = 0; k + 1 < p->na; ++k) if (k + 1 != p->darg || !p->dlen) if (p->a[k] != q->a[k]) return 0;
    if (p->dlen && memcmp(a->data + p->doff, b->data + q->doff, p->dlen)) return 0;
    return 1;
}
static int recordTexturesCurrentG(const GRec* rec) {
    for (int i = 0; i < rec->ncalls; ++i) {
        const GCall* c = &rec->calls[i];
        if (c->method == 65 && c->a[1] && nfsD3dObjectSerial(c->a[1]) != c->textureSerial) return 0;
    }
    return 1;
}
static uint32_t verifyIntervalG(const GRec* rec) { return rec->cleanVerifies >= 4 ? 32u : rec->cleanVerifies >= 1 ? 16u : 8u; }
static void resetGroupCaches(void) {
    if (grecs) memset(grecs, 0, GRECS * sizeof(GRec));
    memset(geffects, 0, sizeof geffects);
}
/* fill a pending record from a captured head (the Head was built from the same capture) */
static int grecFromHead(GRec* rc, const Head* h, uint32_t mat, uint32_t slot, int n) {
    memset(rc, 0, sizeof *rc);
    rc->mat = mat; rc->slot = slot; rc->fx = h->fx; rc->vs = h->vs; rc->ps = h->ps;
    memcpy(rc->sig, h->sig, sizeof rc->sig);
    rc->valid = 1;
    if (!grecCalls(rc, n)) return 0;
    rc->nregs = 0;
    for (int s = 0; s < 2; ++s) for (int k = 0; k < NREG; ++k) if (h->has[s][k]) {
        if (rc->nregs >= GRREG_MAX) return 0;
        GReg* q = &rc->regs[rc->nregs++]; q->s = (uint16_t)s; q->reg = (uint16_t)k; memcpy(q->v, h->r[s][k], 16);
    }
    rc->valid = 2;
    rc->texturesCheckedAtChurn = currentTextureChurn();
    return 1;
}
static inline void regValueG(const GEffect* x, const GRec* rc, const GReg* q, const float* W, float* out) {
    const uint8_t c = x->cls[q->s][q->reg];
    if (c == 1) mulWx(W, x->xref[q->s][q->reg], out);
    else if (c == 2) memcpy(out, x->rref[q->s][q->reg], 16);
    else memcpy(out, q->v, 16);
    (void)rc;
}
static void sendRegsG(const GEffect* x, const GRec* rc, const float* W, int lOnly) {
    const uint32_t dev = M32(0x73636C);
    int i = 0;
    while (i < rc->nregs) {
        const GReg* q = &rc->regs[i];
        if (lOnly && x->cls[q->s][q->reg] != 1) { ++i; continue; }
        int e = i + 1;
        while (e < rc->nregs && rc->regs[e].s == q->s && rc->regs[e].reg == rc->regs[e - 1].reg + 1 &&
               (!lOnly || x->cls[rc->regs[e].s][rc->regs[e].reg] == 1)) ++e;
        const uint32_t cnt = (uint32_t)(e - i);
        g_esp -= cnt * 16; const uint32_t buf = g_esp;
        float* out = (float*)ADDR(buf);
        for (int j = i; j < e; ++j) regValueG(x, rc, &rc->regs[j], W, out + 4 * (j - i));
        const uint32_t a[4] = {dev, q->reg, buf, cnt};
        uint32_t res;
        if (!nfsD3dDeviceCall(q->s ? M_SetPSConst : M_SetVSConst, a, &res)) devMethod(q->s ? M_SetPSConst : M_SetVSConst, 4, a);
        g_esp += cnt * 16;
        i = e;
    }
}
static int worldMatrixRegistersG(const GEffect* x, const GRec* rc, uint16_t* regs) {
    int n = 0;
    for (int i = 0; i < rc->nregs; ++i) {
        const GReg* q = &rc->regs[i];
        if (x->cls[q->s][q->reg] != 1) continue;
        if (q->s) return 0;             /* a pixel-shader L register: not instanced */
        int dup = 0; for (int j = 0; j < n; ++j) dup |= regs[j] == q->reg;
        if (dup) continue;
        if (n == INSTANCE_REGS) return 0;
        regs[n++] = q->reg;
    }
    return n;
}
/* A replayed group may be reordered only when its material is opaque: alpha blending off, z test
 * and z write on. Its items already passed canReplayGroupG (same input signature, same vertex
 * buffer); batching same-mesh items out of list order then cannot change the visible result. */
static int grecCanBatchG(const GEffect* x, const GRec* rc) {
    if (!validGuest(rc->mat) || (int8_t)M8(rc->mat + 0x56) != 0) return 0;
    for (int p = 0; p < 2; ++p) {
        const GRec* r = p ? &x->refRec : rc;
        if (!r->valid) continue;
        for (int i = 0; i < r->ncalls; ++i) {
            const GCall* c = &r->calls[i];
            if (c->method != M_SetRenderState || c->na < 3) continue;
            const uint32_t state = c->a[0], value = c->a[1];
            if (state == 27 && value) return 0;      /* D3DRS_ALPHABLENDENABLE */
            if (state == 14 && !value) return 0;     /* D3DRS_ZWRITEENABLE */
            if (state == 7 && !value) return 0;      /* D3DRS_ZENABLE */
        }
    }
    return 1;
}
/* items of a replayed group with the same mesh as one instanced draw. scanAll merges every matching
 * item of the group (grecCanBatchG() has proved the material is opaque); otherwise only the run that
 * starts at `it`. Returns the last consumed item or 0. */
static uint32_t drawInstancedRunG(const GEffect* x, uint32_t g, uint32_t it, const uint16_t* regs, int nregs, int scanAll, int* refused, uint32_t* lastWp) {
    const uint32_t solid = M32(M32(it + 0x10)), strip = M32(it + 8);
    if (!validGuest(solid) || !validGuest(strip) || !M32(strip + 0xC) || M32(solid + 0x34) >= 0xFFFF) return 0;
    uint32_t items[INSTANCE_MAX]; uint32_t n = 1; items[0] = it;
    const uint32_t ihead = g + 0x18;
    for (uint32_t next = M32(ihead); next != ihead && n < INSTANCE_MAX; next = M32(next)) {
        if (!validGuest(next)) break;
        if (!runnable(next) || !sameSig(it, next)) { if (scanAll) continue; break; }
        const uint32_t mesh = M32(next + 0x10);
        if (!validGuest(mesh) || M32(mesh) != solid) { if (scanAll) continue; break; }
        const uint32_t s2 = M32(next + 8);
        if (!validGuest(s2)) { if (scanAll) continue; break; }
        if (M32(s2 + 0x34) != M32(strip + 0x34) || M32(s2 + 0xC) != M32(strip + 0xC)) { if (scanAll) continue; break; }
        items[n++] = next;
    }
    if (n < 2) return 0;
    float data[INSTANCE_MAX * INSTANCE_REGS * 4];
    for (uint32_t i = 0; i < n; ++i) {
        const float* W = (const float*)ADDR(M32(items[i] + 0x24));
        for (int j = 0; j < nregs; ++j) mulWx(W, x->xref[0][regs[j]], data + (i * (uint32_t)nregs + (uint32_t)j) * 4);
    }
    const uint32_t dip[6] = {4, 0, 0, M32(solid + 0x34),
                             (uint32_t)((int32_t)(M32(strip + 0x34) - M32(solid + 0x18)) >> 1), M32(strip + 0xC) / 3};
    if (!nfsD3dDrawInstanced(dip, (uint32_t)nregs, regs, n, data)) { *refused = 1; return 0; }
    /* The caller has just sent `it`'s W-linear registers; the instanced draw itself does not change
     * them, so the next item's upload decision must compare against `it`, not the batch's last item. */
    *lastWp = M32(it + 0x24);
    for (uint32_t i = 1; i < n; ++i) {
        unlinkItem(items[i]); countItem(items[i]);
        ++stFlushItems; ++stFlushFast;
        freeTo(0x7363D8, items[i]);
    }
    ++stInstancedDraws; stInstancedItems += n;
    if (scanAll) { ++stGroupBatchDraws; stGroupBatchItems += n; stGroupBatchSaved += n - 1; }
    return items[n - 1];
}
/* ---- Native material setup for a replayed group (NFSU2_REPLAY_SETUP, default 1) ----
 * sub_0040F3D0 sets the material's effect (texture, UV transform, alpha/blend parameters) and the device
 * alpha/blend/cull/z-write states. A replayed group sends the effect pass's recorded device calls, so neither
 * the material parameters nor the alpha/blend states need to be computed again: the recorded pass carries the
 * device states it emitted at capture time. With a non-null shader the game's sub_00410650/sub_00410550 only
 * feed effect parameters ([shader+0x18] method +0x68) plus the 0x7070C0/C4/C8/CC/D0 globals, and those are
 * dead for a replay. The cull/z-write states are set outside the effect pass, so they are still needed here.
 * The "current material" is cleared so a following full group always runs its own setup and restores the
 * parameters and globals. Returns 0: run the lifted function.
 */
static int g_replaySetup = -1;
static int replaySetupG(uint32_t mat) {
    if (g_replaySetup < 0) { const char* e = getenv("NFSU2_REPLAY_SETUP"); g_replaySetup = e && e[0] ? atoi(e) : 1; }
    if (!g_replaySetup) return 0;
    const uint32_t shader = M32(0x73640C);
    if (!validGuest(mat) || !validGuest(shader)) return 0;
    if (mat == M32(0x736404) && M32(0x736408) == shader) return 1;   /* this material and shader are already set up */
    M32(0x736404) = 0;                                                /* a full group must set up again */
    if (!validGuest(M32(mat)) || !M32(M32(mat) + 0x18)) return 1;      /* no effect */
    const int bm = (int8_t)M8(mat + 0x56);
    const uint32_t dev = M32(0x73636C);
    const uint32_t stype = M32(shader);   /* 5: the cull state is relevant */
    if (!bm) {
        if (stype == 5) { const uint32_t a[3] = {dev, 0x16, 1}; devFast(M_SetRenderState, 3, a); }
        { const uint32_t a[3] = {dev, 0xE, 1}; devFast(M_SetRenderState, 3, a); }
    } else if (stype == 5) {
        { const uint32_t a[3] = {dev, 0x16, 2}; devFast(M_SetRenderState, 3, a); }
        { const uint32_t a[3] = {dev, 0xE, 0}; devFast(M_SetRenderState, 3, a); }
    } else { const uint32_t a[3] = {dev, 0xE, 0}; devFast(M_SetRenderState, 3, a); }
    return 1;
}
static void replayGroupG(GRec* rc, GEffect* x, uint32_t g) {
    const uint32_t dev = M32(0x73636C);
    for (int i = 0; i < rc->ncalls; ++i) {
        const GRec* src = x->ccls[i] == 2 ? &x->refRec : rc;
        const GCall* c = &src->calls[i];
        uint32_t a[9] = {0};
        a[0] = dev;
        for (uint32_t k = 1; k < c->na; ++k) a[k] = c->a[k - 1];
        uint32_t room = 0;
        if (c->dlen) {
            room = (c->dlen + 15) & ~15u; g_esp -= room;
            memcpy((void*)ADDR(g_esp), src->data + c->doff, c->dlen); a[c->darg] = g_esp;
        }
        if (!nfsD3dCall(2, c->method, a)) devMethod(c->method, (int)c->na, a);
        g_esp += room;
    }
    int first = 1, instancing = instancingEnabled();
    const int batchAll = instancing && groupInstancingEnabled() && grecCanBatchG(x, rc);
    uint32_t lastW = 0;
    uint16_t instRegs[INSTANCE_REGS]; int nInstRegs = -1;
    const uint32_t ihead = g + 0x18;
    while (M32(ihead) != ihead) {
        const uint32_t it = M32(ihead);
        unlinkItem(it);
        const uint32_t wp = M32(it + 0x24);
        if (first || wp != lastW) sendRegsG(x, rc, (const float*)ADDR(wp), !first);
        first = 0; lastW = wp;
        if (bindBuffers(it)) {
            if (instancing && nInstRegs < 0) nInstRegs = worldMatrixRegistersG(x, rc, instRegs);
            int refused = 0; uint32_t lastWp = 0;
            const uint32_t lastOfRun = (instancing && nInstRegs > 0) ? drawInstancedRunG(x, g, it, instRegs, nInstRegs, batchAll, &refused, &lastWp) : 0;
            if (refused) instancing = 0;
            if (lastOfRun) lastW = lastWp;
            else drawItem(it);
            countItem(it);
        }
        ++stFlushItems; ++stFlushFast;
        freeTo(0x7363D8, it);
    }
}
/* 1 = replay group g from its material's record; 0 = draw it fully (*verify = this full draw is its periodic check) */
static int canReplayGroupG(GRec* rec, uint32_t g, GEffect** effectOut, int* verify) {
    *verify = 0;
    if (!rec || rec->valid != 2) { ++stCR[0]; return 0; }
    if (rec->bad) {
        const uint32_t retry = retryFrames();
        const uint32_t now = getD3D9PresentCount();
        if (!retry || (uint32_t)(now - rec->badAt) < retry) { ++stCRbad; ++stCR[5]; return 0; }
        /* The material had changed three times, but those may have been a one-off LOD/blend/weather transition.
         * Re-record it from the next full head and let a stable material validate and replay again. */
        rec->bad = 0; rec->changes = 0; rec->validated = 0; rec->cleanVerifies = 0; rec->badAt = 0;
        if (!g_learnBudget) g_learnBudget = 1;   /* a single-item group must capture this head to re-record it */
        ++stRecRecovered; ++stCRbad; ++stCR[5];
        return 0;
    }
    if (!rec->validated) { ++stCR[1]; return 0; }
    if (rec->texturesStale) { ++stCR[6]; return 0; }
    if (!validGuest(M32(0x73640C)) || M32(M32(0x73640C) + 0x18) != rec->fx) { ++stCR[4]; return 0; }   /* slot reused by another effect */
    GEffect* x = geffectFor(rec->fx, rec->vs, rec->ps, 0);
    if (!x || x->state != 1) { ++stCR[2]; return 0; }
    if (x->refKey != g_flushKey) { ++stCR[3]; return 0; }
    if (!sameShapeG(&x->refRec, rec)) {
        for (int i = 0; i < rec->ncalls; ++i) if (x->ccls[i] == 2) { ++stCR[4]; return 0; }
    }
    const uint32_t ihead = g + 0x18;
    for (uint32_t it = M32(ihead); it != ihead; it = M32(it)) {
        uint32_t sig[5];
        if (!validGuest(it) || !runnable(it)) { ++stCRrun; ++stCR[5]; return 0; }
        sigOf(it, sig);
        if (memcmp(sig, rec->sig, sizeof sig)) { ++stCRsig; ++stCR[5]; return 0; }
        const uint32_t solid = M32(M32(it + 0x10));
        if (!validGuest(solid) || !M32(solid + 0x2c)) { ++stCRvb; ++stCR[5]; return 0; }   /* vertex buffer not created yet: full path */
    }
    const uint32_t churn = currentTextureChurn();
    if (rec->texturesCheckedAtChurn != churn) {
        if (!recordTexturesCurrentG(rec)) { rec->texturesStale = 1; ++stRecTexturesStale; ++stCR[6]; return 0; }
        rec->texturesCheckedAtChurn = churn;
    }
    if (++rec->uses % verifyIntervalG(rec) == 0) { *verify = 1; ++stCR[7]; return 0; }
    *effectOut = x;
    return 1;
}
static int recDeadG(const GRec* rc) {
    if (!rc || !rc->valid) return 0;
    if (rc->bad) return 1;
    const GEffect* x = geffectFor(rc->fx, rc->vs, rc->ps, 0);
    return x && x->state < 0;
}
/* a full group head was captured: learn / verify the effect classes and the material's record */
static void groupLearnG(const Head* h, int n, uint32_t mat, uint32_t slot, int verifying) {
    GRec pend;
    if (!grecFromHead(&pend, h, mat, slot, n)) { ++stRecUnrecordable; return; }
    GEffect* x = geffectFor(pend.fx, pend.vs, pend.ps, 1);
    if (!x) return;
    ++stGroupHeads;
    GRec* old = grecFor(slot, mat, 0);
    int verificationPassed = 0, compared = 0, validatedNow = 0;
    if (x->refKey != g_flushKey) {
        /* first full group of this effect in this flush/view: the reference */
        x->refKey = g_flushKey;
        memcpy(x->refHas, h->has, sizeof x->refHas);
        for (int s = 0; s < 2; ++s) for (int k = 0; k < NREG; ++k) if (h->has[s][k]) {
            memcpy(x->rref[s][k], h->r[s][k], 16);
            memcpy(x->xref[s][k], h->x[s][k], sizeof h->x[s][k]);
        }
        memcpy(&x->refRec, &pend, sizeof x->refRec);
        memcpy(x->lastLearnW, h->W, 64);
        memcpy(x->refW, h->W, 64);
    } else if (x->state >= 0 && old && old->valid == 2 && !old->bad && !old->texturesStale && old->flush != g_flushSeq) {
        /* Compare this full group with the flush's reference (G, L) and with the material's own record (M).
         * A register or call of this material that fits no class only retires this material's record: the
         * front end and the race share effect objects, and one animated/per-object constant must not kill
         * replay for the whole effect. Only records that have passed a comparison are ever replayed. */
        const int trusted = x->state == 1;
        compared = 1; ++stGroupCompared;
        int matChanged = !sameShapeG(old, &pend), predFail = 0, relearn = 0;
        const int refShape = sameShapeG(&x->refRec, &pend);
        for (int i = 0; i < pend.ncalls && !matChanged; ++i) {
            const GCall* q = &pend.calls[i];
            uint8_t hh = (uint8_t)((refShape && callEqG(&x->refRec, &x->refRec.calls[i], &pend, q) ? 2 : 0) |
                                   (callEqG(old, &old->calls[i], &pend, q) ? 4 : 0));
            if (!refShape) hh |= 2;
            if (verifying && trusted && !(hh & x->ccls[i])) {
                predFail = 1;
                static int vshown;
                if (vshown++ < 40) printf("[NFSU1 GROUPS] verify call %d (class %u) material %08x: now m%u %08x %08x %08x, record m%u %08x %08x %08x\n",
                    i, x->ccls[i], mat, q->method, q->a[0], q->a[1], q->a[2], old->calls[i].method, old->calls[i].a[0], old->calls[i].a[1], old->calls[i].a[2]);
            }
            if (x->chyp[i] & hh) { if ((x->chyp[i] & hh) != x->chyp[i]) relearn = 1; x->chyp[i] &= hh; }
            else matChanged = 1;
        }
        for (int i = 0; i < pend.nregs && !matChanged; ++i) {
            const GReg* q = &pend.regs[i];
            const GReg* oq = NULL;
            for (int j = 0; j < old->nregs; ++j) if (old->regs[j].s == q->s && old->regs[j].reg == q->reg) { oq = &old->regs[j]; break; }
            if (!oq) { matChanged = 1; break; }
            /* a register the flush's reference group does not set: L and G are not available, M still is */
            float l[4] = {0, 0, 0, 0};
            uint8_t hh = 0;
            if (x->refHas[q->s][q->reg]) {
                mulWx(h->W, x->xref[q->s][q->reg], l);
                hh = (uint8_t)((close4g(l, q->v) ? 1 : 0) | (close4g(x->rref[q->s][q->reg], q->v) ? 2 : 0));
            }
            if (close4g(oq->v, q->v)) hh |= 4;
            uint8_t* hy = &x->hyp[q->s][q->reg];
            if (verifying && trusted && !(hh & x->cls[q->s][q->reg])) {
                predFail = 1;
                if (g_groupDebug && x->nlog++ < 12) printf("[NFSU1 GROUPS] verify %c%d (class %u) material %08x: now %.4g %.4g %.4g %.4g, record %.4g %.4g %.4g %.4g, L %.4g %.4g %.4g %.4g\n",
                    q->s ? 'p' : 'c', q->reg, x->cls[q->s][q->reg], mat, q->v[0], q->v[1], q->v[2], q->v[3], oq->v[0], oq->v[1], oq->v[2], oq->v[3], l[0], l[1], l[2], l[3]);
            }
            if (*hy & hh) { if ((*hy & hh) != *hy) relearn = 1; *hy &= hh; }
            else {
                if (!trusted && (g_groupDebug || x->nlog++ < 4)) {
                    printf("[NFSU1 GROUPS] unclassified %c%d mat %08x slot %08x vs %08x: now %.5g %.5g %.5g %.5g | ref %.5g %.5g %.5g %.5g | old %.5g %.5g %.5g %.5g | L %.5g %.5g %.5g %.5g | Wt %.3g %.3g %.3g\n",
                        q->s ? 'p' : 'c', q->reg, mat, slot, x->vs, q->v[0], q->v[1], q->v[2], q->v[3],
                        x->rref[q->s][q->reg][0], x->rref[q->s][q->reg][1], x->rref[q->s][q->reg][2], x->rref[q->s][q->reg][3],
                        oq->v[0], oq->v[1], oq->v[2], oq->v[3], l[0], l[1], l[2], l[3], h->W[12], h->W[13], h->W[14]);
                    if (g_groupDebug && x->nlog <= 2) {
                        printf("[NFSU1 GROUPS]   regs now:");
                        for (int k = 0; k < 8; ++k) if (h->has[0][k]) printf(" c%d %.4g %.4g %.4g %.4g", k, h->r[0][k][0], h->r[0][k][1], h->r[0][k][2], h->r[0][k][3]);
                        printf("\n[NFSU1 GROUPS]   regs ref:");
                        for (int k = 0; k < 8; ++k) if (x->refHas[0][k]) printf(" c%d %.4g %.4g %.4g %.4g", k, x->rref[0][k][0], x->rref[0][k][1], x->rref[0][k][2], x->rref[0][k][3]);
                        printf("\n[NFSU1 GROUPS]   W now:");
                        for (int r2 = 0; r2 < 4; ++r2) printf(" %.4g %.4g %.4g %.4g;", h->W[r2*4], h->W[r2*4+1], h->W[r2*4+2], h->W[r2*4+3]);
                        printf("\n[NFSU1 GROUPS]   W ref:");
                        for (int r2 = 0; r2 < 4; ++r2) printf(" %.4g %.4g %.4g %.4g;", x->refW[r2*4], x->refW[r2*4+1], x->refW[r2*4+2], x->refW[r2*4+3]);
                        printf("\n");
                    }
                }
                matChanged = 1;
            }
        }
        verificationPassed = verifying && !matChanged && !predFail;
        validatedNow = !matChanged && !predFail;
        if (matChanged || predFail) {
            if (matChanged) {
                ++stGroupChanged;
                /* a record that just changed may be mid-transition (texture LOD, blend mode, weather): give it
                 * three chances, then stop capturing it for a while. canReplayGroupG re-records it after the
                 * NFSU2_GROUP_RETRY cooldown, so a material that settles replays again instead of staying dead. */
                if (++old->changes >= 3 && !old->bad) { old->bad = 1; old->badAt = getD3D9PresentCount(); ++stRecBad; }
            }
            /* a verification prediction miss is not a material change: it only asks for another full check */
            validatedNow = 0;
        }
        /* only count a group whose world matrix differs from the last one we learned from: with a fixed
         * camera every group has the same W, so L would be indistinguishable from G/M */
        if (x->state == 0 && !matChanged && memcmp(h->W, x->lastLearnW, 64)) {
            memcpy(x->lastLearnW, h->W, 64);
            if (++x->nobs >= 24) {
                x->state = 1; relearn = 1;
                printf("[NFSU1 GROUPS] effect vs %08x ps %08x trusted after %u observations\n", x->vs, x->ps, x->nobs);
            }
        }
        if (x->state == 1 && relearn) {
            for (int s2 = 0; s2 < 2; ++s2) for (int k = 0; k < NREG; ++k) {
                const uint8_t hh = x->hyp[s2][k];
                x->cls[s2][k] = (hh & 1) ? 1 : (hh & 2) ? 2 : 4;
            }
            for (int i = 0; i < GRC_MAX; ++i) x->ccls[i] = (x->chyp[i] & 4) ? 4 : 2;
        }
    }
    GRec* r = old ? old : grecFor(slot, mat, 1);
    if (!r) return;
    const uint32_t uses = old ? old->uses : 0, changes = old ? old->changes : 0;
    const int bad = old ? old->bad : 0;
    const uint32_t badAt = old ? old->badAt : 0;
    const int validated = compared ? validatedNow : (old ? old->validated : 0);
    const uint32_t clean = !old ? 0 : verificationPassed ? old->cleanVerifies + 1 : verifying ? 0 : old->cleanVerifies;
    memcpy(r, &pend, sizeof *r);
    r->uses = uses; r->changes = changes; r->bad = bad; r->badAt = badAt; r->flush = g_flushSeq;
    r->valid = 2; r->validated = validated; r->cleanVerifies = clean;
    r->texturesCheckedAtChurn = currentTextureChurn();
}

/* an item of a trusted run: its W-linear constant registers, buffers, draw - no matrix into the effect, no Pass() */
static void fastItem(uint32_t it, RunState* rs) {
    const Head* h = rs->head;
    const Model* m = rs->model;
    const uint32_t wp = M32(it + 0x24);
    const float* W = (const float*)ADDR(wp);
    if (!(rs->lastW == wp && !memcmp(rs->lastWv, W, 64))) {
        const uint32_t dev = M32(0x73636C);
        for (int q = 0; q < m->nrun; ++q) {
            const int s = m->run[q].s, k = m->run[q].start;
            const uint32_t cnt = m->run[q].cnt;
            g_esp -= cnt * 16; const uint32_t buf = g_esp;
            float* out = (float*)ADDR(buf);
            for (uint32_t j = 0; j < cnt; ++j) mulWx(W, h->x[s][k + j], out + 4 * j);
            const uint32_t a[4] = {dev, (uint32_t)k, buf, cnt};
            uint32_t r;
            if (!nfsD3dDeviceCall(s ? M_SetPSConst : M_SetVSConst, a, &r)) devMethod(s ? M_SetPSConst : M_SetVSConst, 4, a);
            g_esp += cnt * 16;
        }
        rs->lastW = wp; memcpy(rs->lastWv, W, 64);
    }
    if (bindBuffers(it)) { drawItem(it); countItem(it); }
}
/* Items whose run model was rejected (cars...) gain nothing from the capture and head build. */
static struct { uint32_t key; uint8_t skip; } noCap[512];
static int traceFrames[8], nTraceFrames = -1;
static int isTraceFrame(void) {
    if (nTraceFrames < 0) {
        nTraceFrames = 0;
        const char* e = getenv("NFSU2_ITEM_TRACE");
        while (e && *e && nTraceFrames < 8) { traceFrames[nTraceFrames++] = atoi(e); e = strchr(e, ','); if (e) ++e; }
    }
    const int now = (int)getD3D9PresentCount();
    for (int i = 0; i < nTraceFrames; ++i) if (traceFrames[i] == now) return 1;
    return 0;
}
static void printCalls(int n) {
    const CapRec* r = (const CapRec*)nfsCapRecs();
    const uint8_t* bytes = nfsCapBytes();
    for (int i = 0; i < n && i < 120; ++i) {
        printf("   k%u m%u %08x %08x %08x %08x", r[i].kind, r[i].method, r[i].a[1], r[i].a[2], r[i].a[3], r[i].a[4]);
        if (r[i].len[0]) {
            uint32_t h = 2166136261u; for (uint32_t k = 0; k < r[i].len[0]; ++k) h = (h ^ bytes[r[i].off[0] + k]) * 16777619u;
            printf(" data %u bytes hash %08x", r[i].len[0], h);
            if (r[i].method == M_SetVSConst || r[i].method == M_SetPSConst) { const float* f = (const float*)(bytes + r[i].off[0]); printf(" ["); for (uint32_t k = 0; k < r[i].len[0] / 4; ++k) printf(" %.6g", f[k]); printf("]"); }
        }
        printf("\n");
    }
}
static void traceItem(uint32_t it, uint32_t g, int n) {
    static int shown;
    if (shown++ > 60) return;
    printf("[ITEM] g=%08x it=%08x mat=%08x E=%08x fx=%08x npass=%u solid=%08x calls=%d slot=%08x\n", g, it, M32(it + 0x24), M32(it + 0x18),
           M32(M32(it + 0x18) + 0x18), M32(M32(it + 0x18) + 0x14), validGuest(M32(it + 0x10)) ? M32(M32(it + 0x10)) : 0u, n, M32(0x73640C));
    if (validGuest(M32(it + 0x24))) { const float* W = (const float*)ADDR(M32(it + 0x24)); printf("   W"); for (int k = 0; k < 16; ++k) printf(" %.6g", W[k]); printf("\n"); }
    printCalls(n);
    { uint32_t sg[5]; sigOf(it, sg); printf("   sig %08x %08x %08x %08x %08x strip %08x\n", sg[0], sg[1], sg[2], sg[3], sg[4], M32(it + 8)); }
}
/* hasNext: the next item of the group has the same inputs (so a head is worth capturing) */
static void processItem(uint32_t it, RunState* rs, int hasNext, uint32_t ihead, int groupHead, uint32_t mat) {
    ++stFlushItems;
    TSTART;
    if (!g_merge) { nativeItemFull(it, rs, 0); ++stFlushPlain; TEND(2); return; }
    uint32_t sig[5]; sigOf(it, sig);
    const uint32_t ck = (sig[0] >> 4) & 511;
    const int run = runnable(it) && !(noCap[ck].key == sig[0] && noCap[ck].skip);
    const int cont = rs->open && rs->head && run && !memcmp(sig, rs->head->sig, sizeof sig);
    Model* m = cont ? rs->model : NULL;
    /* a head with a continuation is captured for the item model anyway, so its group record is free */
    const int groupLearn = groupHead && g_groupCache && run && !rs->groupVerify && (hasNext || g_learnBudget);
    int verifying = 0;
    if (m && m->state == 1 && rs->head->hasHash == m->hasHash && !groupLearn && !rs->groupVerify) {
        if (g_merge < 2 && (++m->nfast & 15)) {
            uint16_t instRegs[INSTANCE_REGS];
            int nRegs = instancingEnabled() ? worldMatrixRegisters(m, instRegs) : 0;
            int ok = 0;
            if (nRegs > 0) ok = tryDrawInstancedRun(it, rs, ihead, instRegs, nRegs);
            if (!ok) fastItem(it, rs);
            ++stFlushFast;
            TEND(3);
            return;
        }
        verifying = 1; ++stFlushVerify;
    } else if (m && m->state >= 0) ++stFlushLearn;
    const int plainOpen = rs->open;
    const uint32_t plainFx = rs->fx;
    closeRun(rs);
    Head* prev = rs->head;
    Head* nh = prev == &heads[0] ? &heads[1] : &heads[0];
    rs->head = NULL; rs->model = NULL; rs->lastW = 0;
    const int capture = run && (cont || hasNext || groupLearn || rs->groupVerify);
    if (groupLearn) ++stCapTries;
    if (!capture) {
        rs->open = plainOpen; rs->fx = plainFx;
        if (canCommitItem(it, rs)) { commitItem(it, rs); ++stFlushCommit; TEND(4); return; }
        closeRun(rs);
        nativeItemFull(it, rs, g_commitRun > 0);
        ++stFlushPlain; TEND(2); return;
    }
    nfsCapBegin();
    nativeItemFull(it, rs, 1);
    const int n = nfsCapEnd();
    int built = 0;
    if (!(rs->open && n >= 0)) ++stCapNoOpen;
    else if (!(built = buildHead(nh, it, n, sig))) ++stCapBuildFail;
    if (built) {
        if (m && m->state >= 0) learn(prev, nh, m, verifying);
        rs->head = nh;
        rs->model = findModel(nh);
        if (rs->model && rs->model->state < 0) { rs->model = NULL; noCap[ck].key = sig[0]; noCap[ck].skip = 1; }
        rs->lastW = M32(it + 0x24); memcpy(rs->lastWv, nh->W, 64);
        if (groupLearn || rs->groupVerify) {
            groupLearnG(nh, n, mat, M32(0x73640C), rs->groupVerify);
            if (groupLearn && !hasNext && g_learnBudget) --g_learnBudget;
        }
    }
    TEND(1);
}
/* ---- the flush ---- */
static void nativeFlush(void) {
    M32(0x71B1DC) += 1;
    const uint32_t frame = getD3D9PresentCount();
    static uint32_t lastFrame = 0xffffffffu, budgetFrame = 0xffffffffu, lastChurn = 0;
    if (lastFrame != 0xffffffffu && frame > lastFrame + 10) resetGroupCaches();
    lastFrame = frame;
    { const uint32_t churn = currentTextureChurn();   /* a load in progress: effect objects and materials are about to be reused */
      if (lastChurn && churn > lastChurn + 32) resetGroupCaches();
      lastChurn = churn; }
    if (frame != budgetFrame) { budgetFrame = frame; g_learnBudget = 96; }
    const uint32_t viewRec = M32(0x736458);
    const uint32_t viewId = validGuest(viewRec) ? M32(viewRec + 4) : 0xffffffffu;
    g_flushKey = ((uint64_t)frame << 8) | (viewId & 0xff);
    ++g_flushSeq;
    uint32_t cur = M32(0x73640C);
    for (uint32_t b = 0; b < 112; ++b) {
        const uint32_t head = 0x78A4E8u + 8 * b, S = 0x71ABB8u + 0xE0u * (b % 7);
        if (M32(head) == head) continue;
        if (S != cur) {
            if (cur) {
                { const uint32_t fx = M32(cur + 0x18); const uint32_t a[1] = {fx}; fxMethod(FX_End, 1, a); }
                cur = M32(0x73640C);
                icall(M32(cur + 0xDC), 0, 0, NULL);
            }
            M32(0x73640C) = S;
            icall(M32(S + 0xD8), S, 0, NULL);
            { const uint32_t dev = M32(0x73636C); const uint32_t a[2] = {dev, M32(S + 4)}; devMethod(M_SetVertexDecl, 2, a); }
            {
                const uint32_t fx = M32(S + 0x18);
                static int fxdbg = -1;
                if (fxdbg < 0) { const char* e = getenv("NFSU2_FX_DEBUG"); fxdbg = e && e[0] == '1'; }
                if (fxdbg && validGuest(fx) && validGuest(M32(fx))) {
                    static uint32_t seen[8]; static int nseen;
                    int have = 0;
                    for (int i = 0; i < nseen; ++i) have |= seen[i] == fx;
                    if (!have && nseen < 8) {
                        seen[nseen++] = fx;
                        const uint32_t vt = M32(fx);
                        printf("[NFSU2 FX] block %08x fx %08x vtable %08x", S, fx, vt);
                        for (int i = 56; i < 72; ++i) printf(" %d:%08x", i, M32(vt + (uint32_t)i * 4));
                        printf("\n");
                    }
                }
                push32(0); const uint32_t passesSlot = g_esp;
                const uint32_t a[3] = {fx, passesSlot, 0};
                fxMethod(FX_Begin, 3, a);
                g_esp += 4;
            }
            M32(0x71B204) += 1;
            cur = M32(0x73640C);
        }
        while (M32(head) != head) {
            RunState rs = {0};
            const uint32_t g = M32(head);
            { const uint32_t n = M32(g), pv = M32(g + 4); M32(pv) = n; M32(n + 4) = pv; }
            M32(0x71B1D8) += 1;
            ++stGroups;
            const uint32_t mat = M32(g + 0x10), ihead = g + 0x18;
            if (M32(ihead) == ihead) {
                /* every item was culled: no material setup for nothing */
                ++stGroupsEmpty;
            } else {
                const int tracing = isTraceFrame();
                GRec* rc = (g_groupCache && g_merge && !tracing) ? grecFor(M32(0x73640C), mat, 0) : NULL;
                GEffect* reff = NULL;
                int verify = 0;
                if (rc && canReplayGroupG(rc, g, &reff, &verify)) {
                    TSTART;
                    if (!replaySetupG(mat)) { const uint32_t a[1] = {mat}; ccall(sub_0040F3D0, 1, a); }
                    TEND(0);
                    replayGroupG(rc, reff, g);
                    ++stGroupsReplayed;
                } else {
                    ++stGroupsFull;
                    { TSTART; const uint32_t a[1] = {mat}; ccall(sub_0040F3D0, 1, a); TEND(0); }
                    if (verify) { rs.groupVerify = 1; ++stGroupsVerify; }
                    int first = 1;
                    while (M32(ihead) != ihead) {
                        const uint32_t it = M32(ihead);
                        { const uint32_t n = M32(it), pv = M32(it + 4); M32(pv) = n; M32(n + 4) = pv; }
                        const uint32_t nx = M32(ihead);
                        const int hasNext = nx != ihead && validGuest(nx) && sameSig(it, nx);
                        if (tracing) { closeRun(&rs); rs.head = NULL; nfsCapBegin(); nativeItemFull(it, &rs, 0); const int n = nfsCapEnd(); traceItem(it, g, n); }
                        else processItem(it, &rs, hasNext, ihead, first && !recDeadG(rc), mat);
                        if (first) rs.groupVerify = 0;   /* the head was the verification capture */
                        first = 0;
                        freeTo(0x7363D8, it);
                    }
                }
            }
            M32(M32(M32(g + 0x10)) + 0x1c) = 0;
            g_eax = g; push32(RECOMP_RETADDR); sub_0040ED10();
            freeTo(0x7363D4, g);
        }
    }
    M32(0x7363FC) = 0;
}

static uint64_t stFlushNs, stCullNs;

static inline uint64_t cpuNs(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void sub_0040ED50(void) {
    const uint64_t t0 = cpuNs();
    if (g_ftime < 0) { const char* e = getenv("NFSU2_FLUSH_TIME"); g_ftime = e && e[0] == '1'; }
    if (drawDist < 0) { const char* e = getenv("NFSU2_DRAW_DIST"); drawDist = e ? (float)atof(e) : 0; }
    if (g_cdbg < 0) { const char* e = getenv("NFSU2_CULL_DEBUG"); g_cdbg = e && e[0] == '1'; }
    if (pxMax < 0) { const char* e = getenv("NFSU2_CULL_PX"); pxMax = e ? (float)atof(e) : 7.0f; }
    if (g_nativeFlush < 0) { const char* e = getenv("NFSU2_NATIVE_FLUSH"); g_nativeFlush = e ? atoi(e) : 1; }
    if (g_merge < 0) { const char* e = getenv("NFSU2_FLUSH_MERGE"); g_merge = e ? atoi(e) : 1; }
    if (g_groupCache < 0) { const char* e = getenv("NFSU2_GROUP_CACHE"); g_groupCache = !(e && e[0] == '0'); }
    if (g_groupDebug < 0) { const char* e = getenv("NFSU2_GROUP_DEBUG"); g_groupDebug = e && e[0] != '0'; }
    if (g_commitRun < 0) { const char* e = getenv("NFSU2_COMMIT_RUN"); g_commitRun = e ? atoi(e) : 1; }
    /* The commit check counts queued device calls; without the render thread every call is synchronous and the
     * count is always zero, so never skip a Pass in that mode. */
    { const char* e = getenv("NFSU2_RENDER_THREAD"); if (e && e[0] == '0') g_commitRun = 0; }
    const uint32_t viewRec = M32(0x736458);
    const uint32_t viewId = validGuest(viewRec) ? M32(viewRec + 4) : 0xffffffffu;
    const uint32_t vm = viewId < 16 ? M32(0x740460u + viewId * 0x60u) : 0;
    const uint32_t itemPool = M32(0x7363D8);
    if (validGuest(vm) && validGuest(itemPool)) {
        const float* V = (const float*)ADDR(vm);
        /* camera = -t * R^T for a row-vector view matrix with rotation R (rows 0-2) and translation t (row 3) */
        float cam[3];
        for (int j = 0; j < 3; ++j) cam[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
        for (uint32_t b = 0; b < 112; ++b) {
            const uint32_t head = 0x78A4E8u + b * 8;
            for (uint32_t g = M32(head); g != head && validGuest(g); g = M32(g)) {
                const uint32_t ihead = g + 0x18;
                for (uint32_t it = M32(ihead), next; it != ihead && validGuest(it); it = next) {
                    next = M32(it);
                    ++stItems;
                    const uint32_t mesh = M32(it + 0x10), mat = M32(it + 0x24);
                    const uint32_t solid = validGuest(mesh) ? M32(mesh) : 0;
                    if (!validGuest(solid) || !validGuest(mat)) { ++stNoBound; continue; }
                    const Bound* bd = solidBound(solid);
                    if (!bd->ok) { ++stNoBound; continue; }
                    const float* W = (const float*)ADDR(mat);
                    float c[3], sc = 0;
                    for (int j = 0; j < 3; ++j) c[j] = bd->c[0] * W[0 * 4 + j] + bd->c[1] * W[1 * 4 + j] + bd->c[2] * W[2 * 4 + j] + W[12 + j];
                    for (int i = 0; i < 3; ++i) { const float l = W[i * 4] * W[i * 4] + W[i * 4 + 1] * W[i * 4 + 1] + W[i * 4 + 2] * W[i * 4 + 2]; if (l > sc) sc = l; }
                    const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
                    const float cd = sqrtf(dx * dx + dy * dy + dz * dz);
                    const float near = cd - bd->r * sqrtf(sc);
                    const int bin = near < 100 ? 0 : near < 150 ? 1 : near < 200 ? 2 : near < 250 ? 3 : near < 300 ? 4 : near < 400 ? 5 : near < 600 ? 6 : 7;
                    ++stHist[bin];
                    if (drawDist > 0 && near > drawDist) {
                        /* projected radius in pixels at 640x480 (focal = 240 * P[1][1]) */
                        const float* P = V + 16;
                        const float px = bd->r * sqrtf(sc) * 240.0f * (P[5] > 0 ? P[5] : 1.7f) / (cd + 1e-3f);
                        if (pxMax <= 0 || px < pxMax) {
                            if (g_cdbg) {
                                Bound f; const int okf = freshBound(solid, &f);
                                if (cd < 200) ++stCulledNear;
                                if (!okf || fabsf(f.r - bd->r) > 0.02f * (bd->r + 0.01f) || fabsf(f.c[0] - bd->c[0]) + fabsf(f.c[1] - bd->c[1]) + fabsf(f.c[2] - bd->c[2]) > 0.02f * (bd->r + 1.0f)) {
                                    ++stStale; static int shown; if (shown++ < 40) printf("[NFSU1 CULL] STALE bound: solid %08x data %08x verts %u size %u; cached c %.1f %.1f %.1f r %.2f, fresh ok %d c %.1f %.1f %.1f r %.2f; centre dist %.0f near %.0f px %.1f scale %.2f W t %.1f %.1f %.1f\n",
                                        solid, M32(solid + 0x28), M32(solid + 0x34), M32(solid + 0x30), bd->c[0], bd->c[1], bd->c[2], bd->r, okf, f.c[0], f.c[1], f.c[2], f.r, cd, near, px, sqrtf(sc), W[12], W[13], W[14]);
                                }
                            }
                            const uint32_t prev = M32(it + 4);
                            M32(prev) = next; M32(next + 4) = prev;
                            M32(it) = M32(itemPool + 0x10); M32(itemPool + 0x10) = it; M32(itemPool + 0x18) -= 1;
                            ++stCulled;
                        }
                    }
                }
            }
        }
    }
    const uint64_t t1 = cpuNs();
    stCullNs += t1 - t0;
    const uint32_t now = getD3D9PresentCount();
    if (now != stFrame) {
        stFrame = now;
        if (++stFrames % 240 == 0 && stItems) {
            printf("[NFSU1 CULL] items/flush-frame %u (no bound %u), nearest surface distance: <100m %u, <150 %u, <200 %u, <250 %u, <300 %u, <400 %u, <600 %u, more %u; culled %u (beyond %.0f m and under %.0f px)\n",
                   stItems / 240, stNoBound / 240, stHist[0] / 240, stHist[1] / 240, stHist[2] / 240, stHist[3] / 240, stHist[4] / 240, stHist[5] / 240, stHist[6] / 240, stHist[7] / 240, stCulled / 240, drawDist, pxMax);
            uint32_t tr = 0, le = 0, rj = 0; for (int k = 0; k < MODELS; ++k) if (models[k].fx) { if (models[k].state > 0) ++tr; else if (models[k].state < 0) ++rj; else ++le; }
            printf("[NFSU1 FLUSH] native %u, lifted %u; items/frame %u: fast %u, learning %u, verified %u, plain %u (merge %d); groups/frame %u (empty %u); models trusted %u, learning %u, rejected %u\n",
                   stFlushNative, stFlushLifted, stFlushItems / 240, stFlushFast / 240, stFlushLearn / 240, stFlushVerify / 240, stFlushPlain / 240, g_merge, stGroups / 240, stGroupsEmpty / 240, tr, le, rj);
            if (stInstancedDraws) {
                printf("[NFSU1 FLUSH] instanced draws/frame %u covering %u items (saved %u draws/frame)\n",
                       stInstancedDraws / 240, stInstancedItems / 240, (stInstancedItems - stInstancedDraws) / 240);
            }
            if (stGroupBatchDraws) {
                printf("[NFSU1 FLUSH] group-batched draws/frame %u covering %u items (saved %u draws/frame; non-consecutive, opaque groups)\n",
                       stGroupBatchDraws / 240, stGroupBatchItems / 240, (stGroupBatchItems - stGroupBatchDraws) / 240);
            }
            if (g_groupCache && g_merge) {
                uint32_t xt = 0, xl = 0; for (int k = 0; k < GEFFECTS; ++k) if (geffects[k].fx) { if (geffects[k].state > 0) ++xt; else ++xl; }
                printf("[NFSU1 FLUSH] group replay: %u replayed, %u verified, %u full/frame; effects trusted %u, learning %u; records retired %u, recovered %u, textures stale %u, unrecordable %u\n",
                       stGroupsReplayed / 240, stGroupsVerify / 240, stGroupsFull / 240, xt, xl, stRecBad, stRecRecovered, stRecTexturesStale, stRecUnrecordable);
                printf("[NFSU1 FLUSH] group learn: heads %u, compared %u, changed %u/frame; replay refused: norel %u, unvalid %u, effect %u, refkey %u, shape %u, items %u (bad %u sig %u vb %u run %u), tex %u, verify %u\n",
                       stGroupHeads / 240, stGroupCompared / 240, stGroupChanged / 240,
                       stCR[0] / 240, stCR[1] / 240, stCR[2] / 240, stCR[3] / 240, stCR[4] / 240, stCR[5] / 240,
                       stCRbad / 240, stCRsig / 240, stCRvb / 240, stCRrun / 240, stCR[6] / 240, stCR[7] / 240);
                if (g_groupDebug) printf("[NFSU1 FLUSH] group capture: tries %u, noopen %u, buildfail %u/frame\n", stCapTries / 240, stCapNoOpen / 240, stCapBuildFail / 240);
                { int n = 0; for (int k = 0; k < GEFFECTS && n < 6; ++k) if (geffects[k].fx) {
                    printf("[NFSU1 FLUSH]   effect %08x vs %08x ps %08x state %d nobs %u ref %llx\n", geffects[k].fx, geffects[k].vs, geffects[k].ps,
                           geffects[k].state, geffects[k].nobs, (unsigned long long)geffects[k].refKey); ++n; } }
                if (g_commitRun) {
                    int safe = 0, rejected = 0;
                    for (int k = 0; k < 64; ++k) if (commitFx[k].fx) { if (commitFx[k].safe > 0) ++safe; else if (commitFx[k].safe < 0) ++rejected; }
                    printf("[NFSU1 FLUSH] commit run: %u items/frame, %u checked, %u missed device states, %u without a draw; effects safe %d, rejected %d\n",
                           stCommitItems / 240, stCommitChecked, stCommitMissed, stCommitFull / 240, safe, rejected);
                    stCommitItems = stCommitChecked = stCommitMissed = stCommitFull = 0;
                }
                if (g_groupDebug) { int n = 0; for (int k = 0; k < MODELS && n < 4; ++k) if (models[k].state == 1) {
                    printf("[NFSU1 FLUSH]   model fx %08x vs %08x ps %08x key %08x L:", models[k].fx, models[k].vs, models[k].ps, models[k].key);
                    for (int r2 = 0; r2 < models[k].nrun; ++r2) printf(" c%d-%d", models[k].run[r2].start, models[k].run[r2].start + models[k].run[r2].cnt - 1);
                    printf("\n"); ++n; } }
            }
            printf("[NFSU1 FLUSH] game-thread CPU per frame: flush %.2f ms (%.1f us per item), culling pass %.2f ms\n", stFlushNs / 240e6, stFlushNs / 1e3 / (stFlushItems ? stFlushItems : 1), stCullNs / 240e6);
            if (g_ftime) { printf("[NFSU1 FLUSH] per frame: group setup %.2f ms (%u groups), full+capture %.2f ms (%u items), plain %.2f ms (%u), commit %.2f ms (%u), fast %.2f ms (%u)\n",
                tb[0] / 240e6, (unsigned)(tn[0] / 240), tb[1] / 240e6, (unsigned)(tn[1] / 240), tb[2] / 240e6, (unsigned)(tn[2] / 240), tb[4] / 240e6, (unsigned)(tn[4] / 240), tb[3] / 240e6, (unsigned)(tn[3] / 240)); memset(tb, 0, sizeof tb); memset(tn, 0, sizeof tn); }
            if (g_cdbg) { printf("[NFSU1 CULL] debug: stale-bound culls %u, culled with centre within 200 m %u\n", stStale, stCulledNear); stStale = stCulledNear = 0; }
            stFlushNs = stCullNs = 0;
            stItems = stCulled = stNoBound = 0; memset(stHist, 0, sizeof stHist);
            stFlushNative = stFlushLifted = stFlushItems = stFlushFast = stFlushLearn = stFlushVerify = stFlushPlain = stGroups = stGroupsEmpty = 0;
            stInstancedDraws = stInstancedItems = 0;
            stGroupBatchDraws = stGroupBatchItems = stGroupBatchSaved = 0;
            stGroupsReplayed = stGroupsVerify = stGroupsFull = stRecBad = stRecRecovered = stRecTexturesStale = stRecUnrecordable = 0;
            stGroupCompared = stGroupChanged = stGroupHeads = 0;
            stCapTries = stCapNoOpen = stCapBuildFail = 0;
            stCRbad = stCRsig = stCRvb = stCRrun = 0;
            memset(stCR, 0, sizeof stCR);
        }
    }
    if (g_nativeFlush) {
        const uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
        ++stFlushNative;
        nativeFlush();
        g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
        g_eax = M32(0x73640C);
        g_esp += 4;  /* ret */
        stFlushNs += cpuNs() - t1;
        return;
    }
    ++stFlushLifted;
    sub_0040ED50_orig();
    stFlushNs += cpuNs() - t1;
}
