/* VU micro-mode interpreter, shared by VU1 (VIF1 MSCAL/MSCNT, CMSAR1) and VU0 (VCALLMS, VIF0 MSCAL).
 * Each 64-bit instruction is an upper (FMAC) and a lower (integer/load/store/branch/FDIV/EFU) op.
 * Timing is cycle-based: one instruction issues per cycle; VF/ACC results are ready 4 cycles after
 * issue and readers stall until then; MAC/status/clip flags become visible 4 cycles after issue,
 * Q and P after their FDIV/EFU latency. Optimised microcode depends on reading the old values. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <emmintrin.h>
#include "gif.h"

extern u8 vu0_code[4096], vu0_data[4096], vu1_code[16384], vu1_data[16384];
u32 vif1_top(void);
u32 vif1_itop(void);


typedef struct {
    u128 *vf; u32 *vi; u128 *acc;
    u8 *mem; u32 mem_qw;          /* data memory size in qwords */
    u8 *code; u32 code_n;         /* code memory size in instructions */
    int unit;
    /* cycle model: results/flags become visible at a cycle; readers stall until registers are ready */
    u64 cyc;
    u64 vf_ready[32][4], acc_ready[4];
    float q_pending; int q_wait; u64 q_ready;
    float p, p_pending; int p_wait; u64 p_ready;
    u32 mac, status, clip;        /* visible flags (mirrored into vi[16..18] at the end) */
    u32 new_mac; int new_mac_valid;   /* produced by this instruction */
    int flags_off;                /* no instruction in the loaded code can read MAC/status: skip them */
    u32 new_clip; int new_clip_valid;
    /* flags pending until cycle `at`: a FIFO ring (pushed at issue+4, so `at` is non-decreasing) */
    struct { u64 at; int kind; u32 val; } fq[32]; int nfq, fqh;
    u64 next_event;               /* earliest cycle at which pipes_advance has work (fast path) */
    /* branches read the pre-write value of a VI register written by the previous instruction */
    int cur_vi_reg; u32 cur_vi_old;
    int vh_reg[4]; u32 vh_old[4]; int vh_head;   /* VI writes of the last 4 instructions */
} Vu;

static u128 g_vu1_vf[32], g_vu1_acc;
static u32 g_vu1_vi[32];
static u32 g_vu1_tpc, g_vu0_tpc;
static float g_vu1_p, g_vu0_p;
static unsigned long long g_vu1_starts, g_vu0_starts, g_vu1_instr, g_kicks;
static u32 g_vu1_prog;   /* start PC (instructions) of the current VU1 run */
int g_respawn_frame;     /* gs frame when the game console printed "Loading respawn ID" (kernel.c) */
volatile int g_vu_code_dirty[2] = {1, 1};   /* set by VIF MPG uploads and EE stores to VU code memory */
int g_vu_stat_dirty;                         /* same, for image_stats() only */

/* PS2_VUIMAGES=1: statistics about VU1 microcode images (distinct full images and 64-instruction pages) */
static void image_stats(void) {
    static int on = -1; if (on < 0) on = getenv("PS2_VUIMAGES") != NULL;
    if (!on || !g_vu_stat_dirty) return;
    g_vu_stat_dirty = 0;
    extern int g_frame_no_pub;
    if (!g_respawn_frame || g_frame_no_pub < g_respawn_frame + 250) return;
    static u64 imgs[4096]; static int nimg; static u64 pages[32][256]; static int npg[32]; static unsigned long long mpgs;
    mpgs++;
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < 16384; i++) h = (h ^ vu1_code[i]) * 1099511628211ull;
    int k; for (k = 0; k < nimg && imgs[k] != h; k++) {}
    if (k == nimg && nimg < 4096) imgs[nimg++] = h;
    for (int p = 0; p < 32; p++) {
        u64 ph = 1469598103934665603ull;
        for (int i = 0; i < 512; i++) ph = (ph ^ vu1_code[p * 512 + i]) * 1099511628211ull;
        int j; for (j = 0; j < npg[p] && pages[p][j] != ph; j++) {}
        if (j == npg[p] && npg[p] < 256) pages[p][npg[p]++] = ph;
    }
    if (mpgs % 200 == 0) {
        int tot = 0; for (int p = 0; p < 32; p++) tot += npg[p];
        fprintf(stderr, "[vuimg] %llu code changes, %d distinct full images, %d distinct pages; per page:", mpgs, nimg, tot);
        for (int p = 0; p < 32; p++) fprintf(stderr, " %d", npg[p]);
        fputc('\n', stderr);
    }
}
static unsigned long long g_vu0_ray_calls, g_vu0_ray_hits;   /* VU0 ray/triangle test (entries 0x0/0x18) */
static long long g_vu1_ticks, g_vu0_ticks;                      /* QPC time spent interpreting */
typedef struct { u32 pc; unsigned long long starts, kicks, drawn, adc, instr, inbox; u32 x0, x1, y0, y1; } ProgTally;
static ProgTally g_tally[64];
static ProgTally *prog_tally(u32 pc) {
    for (int i = 0; i < 64; i++) {
        if (g_tally[i].starts || g_tally[i].kicks) { if (g_tally[i].pc == pc) return &g_tally[i]; continue; }
        g_tally[i].pc = pc; return &g_tally[i];
    }
    return NULL;
}

enum { VI_STATUS = 16, VI_MAC = 17, VI_CLIP = 18, VI_R = 20, VI_I = 21, VI_Q = 22, VI_P = 23, VI_TPC = 26 };

static inline float f32of(u32 u) { float f; memcpy(&f, &u, 4); return f; }
static inline u32 u32of(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* VU floats: no Inf/NaN; clamp like PCSX2 */
static inline float vclamp(float f) {
    u32 u = u32of(f);
    if ((u & 0x7F800000u) == 0x7F800000u) u = (u & 0x80000000u) | 0x7F7FFFFFu;
    return f32of(u);
}

#define HAS(dest, i) ((dest) & (8 >> (i)))

/* MAC flags for one result vector; x is the highest bit of each nibble */
static void set_flags(Vu *v, int dest, const float r[4]) {
    u32 mac = 0;
    for (int i = 0; i < 4; i++) {
        if (!HAS(dest, i)) continue;
        u32 u = u32of(r[i]);
        int bit = 3 - i;
        if ((u & 0x7FFFFFFFu) == 0) mac |= 1u << bit;
        if (u & 0x80000000u) mac |= 1u << (bit + 4);
        if ((u & 0x7F800000u) == 0 && (u & 0x7FFFFF)) mac |= 1u << (bit + 8);
        if ((u & 0x7F800000u) == 0x7F800000u) mac |= 1u << (bit + 12);
    }
    v->new_mac = mac; v->new_mac_valid = 1;
}

static void apply_mac(Vu *v, u32 mac) {
    v->mac = mac;
    u32 st = v->status & 0xFF0;               /* keep I/D and sticky bits */
    if (mac & 0x000F) st |= 0x001;
    if (mac & 0x00F0) st |= 0x002;
    if (mac & 0x0F00) st |= 0x004;
    if (mac & 0xF000) st |= 0x008;
    st |= (st & 0xF) << 6;                    /* sticky Zs/Ss/Us/Os */
    v->status = st;
}

/* apply everything whose cycle has come (flags in issue order, then Q/P) */
static void pipes_advance(Vu *v) {
    if (v->cyc < v->next_event) return;
    while (v->nfq && v->fq[v->fqh].at <= v->cyc) {
        if (v->fq[v->fqh].kind == 0) apply_mac(v, v->fq[v->fqh].val); else v->clip = v->fq[v->fqh].val;
        v->fqh = (v->fqh + 1) & 31; v->nfq--;
    }
    if (v->q_wait && v->cyc >= v->q_ready) { v->vi[22] = u32of(v->q_pending); v->q_wait = 0; }
    if (v->p_wait && v->cyc >= v->p_ready) { v->p = v->p_pending; v->p_wait = 0; }
    u64 ne = v->nfq ? v->fq[v->fqh].at : ~0ull;
    if (v->q_wait && v->q_ready < ne) ne = v->q_ready;
    if (v->p_wait && v->p_ready < ne) ne = v->p_ready;
    v->next_event = ne;
}
static void flags_push(Vu *v) {
    if (v->new_mac_valid && v->nfq < 32) { int t = (v->fqh + v->nfq) & 31; v->fq[t].at = v->cyc + 4; v->fq[t].kind = 0; v->fq[t].val = v->new_mac; v->nfq++; }
    if (v->new_clip_valid && v->nfq < 32) { int t = (v->fqh + v->nfq) & 31; v->fq[t].at = v->cyc + 4; v->fq[t].kind = 1; v->fq[t].val = v->new_clip; v->nfq++; }
    if ((v->new_mac_valid || v->new_clip_valid) && v->cyc + 4 < v->next_event) v->next_event = v->cyc + 4;
    v->new_mac_valid = v->new_clip_valid = 0;
}
static void flags_flush(Vu *v) { v->cyc += 64; v->next_event = 0; pipes_advance(v); }

/* ------------------------------------------------------------------ upper ops */
enum { K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MIN, K_MUL };
enum { S_VEC, S_BC, S_Q, S_I };

typedef struct { int reg; int dest; u128 val; int to_acc; int valid; } UpperResult;

static void fmac(Vu *v, u32 w, int kind, int src, int to_acc, UpperResult *out) {
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, bc = w & 3;
    u128 a = v->vf[fs], b = v->vf[ft], acc = *v->acc;
    float s = src == S_BC ? b.f[bc] : src == S_Q ? f32of(v->vi[VI_Q]) : src == S_I ? f32of(v->vi[VI_I]) : 0.0f;
    float r[4], y[4];
    if (src == S_VEC) memcpy(y, b.f, 16); else y[0] = y[1] = y[2] = y[3] = s;
    switch (kind) {
    case K_ADD:  for (int i = 0; i < 4; i++) r[i] = vclamp(a.f[i] + y[i]); break;
    case K_SUB:  for (int i = 0; i < 4; i++) r[i] = vclamp(a.f[i] - y[i]); break;
    case K_MUL:  for (int i = 0; i < 4; i++) r[i] = vclamp(a.f[i] * y[i]); break;
    case K_MADD: for (int i = 0; i < 4; i++) r[i] = vclamp(acc.f[i] + a.f[i] * y[i]); break;
    case K_MSUB: for (int i = 0; i < 4; i++) r[i] = vclamp(acc.f[i] - a.f[i] * y[i]); break;
    case K_MAX:  for (int i = 0; i < 4; i++) r[i] = a.f[i] > y[i] ? a.f[i] : y[i]; break;
    default:     for (int i = 0; i < 4; i++) r[i] = a.f[i] < y[i] ? a.f[i] : y[i]; break;
    }
    if (kind != K_MAX && kind != K_MIN) set_flags(v, dest, r);
    out->valid = 1; out->to_acc = to_acc; out->dest = dest; out->reg = to_acc ? 0 : fd;
    memcpy(out->val.f, r, 16);
}

static void upper(Vu *v, u32 w, UpperResult *out) {
    out->valid = 0;
    int op = w & 63;
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31;
    if (op < 0x1C) { static const int k[7] = {K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MIN, K_MUL}; fmac(v, w, k[op >> 2], S_BC, 0, out); return; }
    switch (op) {
    case 0x1C: fmac(v, w, K_MUL, S_Q, 0, out); return;
    case 0x1D: fmac(v, w, K_MAX, S_I, 0, out); return;
    case 0x1E: fmac(v, w, K_MUL, S_I, 0, out); return;
    case 0x1F: fmac(v, w, K_MIN, S_I, 0, out); return;
    case 0x20: fmac(v, w, K_ADD, S_Q, 0, out); return;
    case 0x21: fmac(v, w, K_MADD, S_Q, 0, out); return;
    case 0x22: fmac(v, w, K_ADD, S_I, 0, out); return;
    case 0x23: fmac(v, w, K_MADD, S_I, 0, out); return;
    case 0x24: fmac(v, w, K_SUB, S_Q, 0, out); return;
    case 0x25: fmac(v, w, K_MSUB, S_Q, 0, out); return;
    case 0x26: fmac(v, w, K_SUB, S_I, 0, out); return;
    case 0x27: fmac(v, w, K_MSUB, S_I, 0, out); return;
    case 0x28: fmac(v, w, K_ADD, S_VEC, 0, out); return;
    case 0x29: fmac(v, w, K_MADD, S_VEC, 0, out); return;
    case 0x2A: fmac(v, w, K_MUL, S_VEC, 0, out); return;
    case 0x2B: fmac(v, w, K_MAX, S_VEC, 0, out); return;
    case 0x2C: fmac(v, w, K_SUB, S_VEC, 0, out); return;
    case 0x2D: fmac(v, w, K_MSUB, S_VEC, 0, out); return;
    case 0x2E: {   /* OPMSUB.xyz */
        u128 s = v->vf[fs], t = v->vf[ft], a = *v->acc;
        float r[4] = {vclamp(a.f[0] - s.f[1] * t.f[2]), vclamp(a.f[1] - s.f[2] * t.f[0]), vclamp(a.f[2] - s.f[0] * t.f[1]), 0};
        set_flags(v, 0xE, r);
        out->valid = 1; out->to_acc = 0; out->dest = 0xE; out->reg = (w >> 6) & 31; memcpy(out->val.f, r, 16);
        return; }
    case 0x2F: fmac(v, w, K_MIN, S_VEC, 0, out); return;
    }
    if (op < 0x3C) return;   /* undefined: treat as NOP */
    int sp = ((w >> 4) & 0x7C) | (w & 3);
    u128 s = v->vf[fs], r;
    if (sp < 0x10) { static const int k[4] = {K_ADD, K_SUB, K_MADD, K_MSUB}; fmac(v, w, k[sp >> 2], S_BC, 1, out); return; }
    switch (sp) {
    case 0x10: case 0x11: case 0x12: case 0x13: {   /* ITOF0/4/12/15 */
        static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
        __m128 f = _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i *)s.sw)), _mm_set1_ps(sc[sp & 3]));
        _mm_storeu_ps(r.f, f);
        out->valid = 1; out->to_acc = 0; out->dest = dest; out->reg = ft; out->val = r; return; }
    case 0x14: case 0x15: case 0x16: case 0x17: {   /* FTOI0/4/12/15: truncate, saturate */
        static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        __m128 x = _mm_mul_ps(_mm_loadu_ps(s.f), _mm_set1_ps(sc[sp & 3]));
        __m128i iv = _mm_cvttps_epi32(x);                       /* out of range (and NaN) -> 0x80000000 */
        __m128 big = _mm_cmpge_ps(x, _mm_set1_ps(2147483648.0f)); /* positive overflow -> INT32_MAX */
        iv = _mm_xor_si128(iv, _mm_and_si128(_mm_castps_si128(big), _mm_set1_epi32(-1)));
        _mm_storeu_si128((__m128i *)r.sw, iv);
        out->valid = 1; out->to_acc = 0; out->dest = dest; out->reg = ft; out->val = r; return; }
    case 0x18: case 0x19: case 0x1A: case 0x1B: fmac(v, w, K_MUL, S_BC, 1, out); return;
    case 0x1C: fmac(v, w, K_MUL, S_Q, 1, out); return;
    case 0x1D: for (int i = 0; i < 4; i++) r.f[i] = fabsf(s.f[i]);
               out->valid = 1; out->to_acc = 0; out->dest = dest; out->reg = ft; out->val = r; return;
    case 0x1E: fmac(v, w, K_MUL, S_I, 1, out); return;
    case 0x1F: {   /* CLIPw.xyz */
        float lim = fabsf(v->vf[ft].f[3]);
        u32 prev = v->clip;                     /* newest clip value, even if not yet visible */
        for (int i = v->nfq - 1; i >= 0; i--) { int k = (v->fqh + i) & 31; if (v->fq[k].kind == 1) { prev = v->fq[k].val; break; } }
        u32 cf = (prev << 6) & 0xFFFFFF;
        for (int i = 0; i < 3; i++) {
            if (s.f[i] > lim) cf |= 1u << (2 * i);
            if (s.f[i] < -lim) cf |= 2u << (2 * i);
        }
        v->new_clip = cf; v->new_clip_valid = 1; return; }
    case 0x20: fmac(v, w, K_ADD, S_Q, 1, out); return;
    case 0x21: fmac(v, w, K_MADD, S_Q, 1, out); return;
    case 0x22: fmac(v, w, K_ADD, S_I, 1, out); return;
    case 0x23: fmac(v, w, K_MADD, S_I, 1, out); return;
    case 0x24: fmac(v, w, K_SUB, S_Q, 1, out); return;
    case 0x25: fmac(v, w, K_MSUB, S_Q, 1, out); return;
    case 0x26: fmac(v, w, K_SUB, S_I, 1, out); return;
    case 0x27: fmac(v, w, K_MSUB, S_I, 1, out); return;
    case 0x28: fmac(v, w, K_ADD, S_VEC, 1, out); return;
    case 0x29: fmac(v, w, K_MADD, S_VEC, 1, out); return;
    case 0x2A: fmac(v, w, K_MUL, S_VEC, 1, out); return;
    case 0x2C: fmac(v, w, K_SUB, S_VEC, 1, out); return;
    case 0x2D: fmac(v, w, K_MSUB, S_VEC, 1, out); return;
    case 0x2E: {   /* OPMULA.xyz */
        u128 t = v->vf[ft];
        float rr[4] = {vclamp(s.f[1] * t.f[2]), vclamp(s.f[2] * t.f[0]), vclamp(s.f[0] * t.f[1]), 0};
        set_flags(v, 0xE, rr);
        out->valid = 1; out->to_acc = 1; out->dest = 0xE; memcpy(out->val.f, rr, 16); return; }
    case 0x2F: return;   /* NOP */
    }
}

/* lane mask per dest value (x = bit 3 = lane 0) */
static __m128 g_dest_mask[16];
static u8 g_rev4[16];      /* lane-indexed 4-bit mask -> dest-ordered (lane 0 -> bit 3) */
static void masks_init(void) {
    static int done; if (done) return; done = 1;
    for (int d = 0; d < 16; d++) {
        u32 m[4]; for (int i = 0; i < 4; i++) m[i] = HAS(d, i) ? 0xFFFFFFFFu : 0;
        g_dest_mask[d] = _mm_castsi128_ps(_mm_loadu_si128((const __m128i *)m));
        g_rev4[d] = (u8)(((d & 1) << 3) | ((d & 2) << 1) | ((d & 4) >> 1) | ((d & 8) >> 3));
    }
}

static void commit_upper(Vu *v, const UpperResult *u) {
    if (!u->valid) return;
    u128 *d = u->to_acc ? v->acc : (u->reg ? &v->vf[u->reg] : NULL);
    if (!d) return;
    __m128 m = g_dest_mask[u->dest], o = _mm_loadu_ps(d->f), n = _mm_loadu_ps(u->val.f);
    _mm_storeu_ps(d->f, _mm_or_ps(_mm_and_ps(m, n), _mm_andnot_ps(m, o)));
}

/* Classify an upper word as a plain FMAC (ADD/SUB/MUL/MADD/MSUB/MAX/MIN in bc/q/i/vector, to VF or ACC).
 * Returns 1 and the kind/source/accumulate flags for the SSE fast path; other ops use upper(). */
static int fmac_class(u32 w, u8 *kind, u8 *src, u8 *to_acc) {
    int op = w & 63;
    static const u8 bck[7] = {K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MIN, K_MUL};
    *to_acc = 0;
    if (op < 0x1C) { *kind = bck[op >> 2]; *src = S_BC; return 1; }
    if (op < 0x30 && op != 0x2E) {
        static const u8 k1[20] = {K_MUL, K_MAX, K_MUL, K_MIN, K_ADD, K_MADD, K_ADD, K_MADD, K_SUB, K_MSUB, K_SUB, K_MSUB,
                                  K_ADD, K_MADD, K_MUL, K_MAX, K_SUB, K_MSUB, 0, K_MIN};
        static const u8 s1[20] = {S_Q, S_I, S_I, S_I, S_Q, S_Q, S_I, S_I, S_Q, S_Q, S_I, S_I,
                                  S_VEC, S_VEC, S_VEC, S_VEC, S_VEC, S_VEC, 0, S_VEC};
        *kind = k1[op - 0x1C]; *src = s1[op - 0x1C]; return 1;
    }
    if (op < 0x3C) return 0;
    int sp = ((w >> 4) & 0x7C) | (w & 3);
    *to_acc = 1;
    if (sp < 0x10) { static const u8 k[4] = {K_ADD, K_SUB, K_MADD, K_MSUB}; *kind = k[sp >> 2]; *src = S_BC; return 1; }
    if (sp >= 0x18 && sp <= 0x1B) { *kind = K_MUL; *src = S_BC; return 1; }
    switch (sp) {
    case 0x1C: *kind = K_MUL; *src = S_Q; return 1;
    case 0x1E: *kind = K_MUL; *src = S_I; return 1;
    case 0x20: *kind = K_ADD; *src = S_Q; return 1;
    case 0x21: *kind = K_MADD; *src = S_Q; return 1;
    case 0x22: *kind = K_ADD; *src = S_I; return 1;
    case 0x23: *kind = K_MADD; *src = S_I; return 1;
    case 0x24: *kind = K_SUB; *src = S_Q; return 1;
    case 0x25: *kind = K_MSUB; *src = S_Q; return 1;
    case 0x26: *kind = K_SUB; *src = S_I; return 1;
    case 0x27: *kind = K_MSUB; *src = S_I; return 1;
    case 0x28: *kind = K_ADD; *src = S_VEC; return 1;
    case 0x29: *kind = K_MADD; *src = S_VEC; return 1;
    case 0x2A: *kind = K_MUL; *src = S_VEC; return 1;
    case 0x2C: *kind = K_SUB; *src = S_VEC; return 1;
    case 0x2D: *kind = K_MSUB; *src = S_VEC; return 1;
    }
    return 0;
}

/* SSE version of fmac(): same results and MAC flags (zero/sign/underflow; overflow cannot remain after
 * clamping), committed later through commit_upper(). */
/* macdead: the translator proved no FM* instruction can see this result's MAC (a later FMAC replaces it
 * first); it is then skipped when no FS* instruction exists either (v->flags_off: sticky status unused) */
static inline void fmac_sse(Vu *v, u8 kind, u8 src, u8 to_acc, int dest, int fd, int fs, int ft, int bc, UpperResult *out, int macdead) {
    static const float fmax_[4] = {3.402823466e38f, 3.402823466e38f, 3.402823466e38f, 3.402823466e38f};
    __m128 a = _mm_loadu_ps(v->vf[fs].f), y, r;
    if (src == S_VEC) y = _mm_loadu_ps(v->vf[ft].f);
    else y = _mm_set1_ps(src == S_BC ? v->vf[ft].f[bc] : f32of(v->vi[src == S_Q ? VI_Q : VI_I]));
    switch (kind) {
    case K_ADD:  r = _mm_add_ps(a, y); break;
    case K_SUB:  r = _mm_sub_ps(a, y); break;
    case K_MUL:  r = _mm_mul_ps(a, y); break;
    case K_MADD: r = _mm_add_ps(_mm_loadu_ps(v->acc->f), _mm_mul_ps(a, y)); break;
    case K_MSUB: r = _mm_sub_ps(_mm_loadu_ps(v->acc->f), _mm_mul_ps(a, y)); break;
    case K_MAX:  r = _mm_max_ps(a, y); break;
    default:     r = _mm_min_ps(a, y); break;
    }
    if (kind != K_MAX && kind != K_MIN) {
        __m128 mx = _mm_loadu_ps(fmax_);
        r = _mm_min_ps(_mm_max_ps(r, _mm_xor_ps(mx, _mm_set1_ps(-0.0f))), mx);   /* +-Inf -> +-max */
        if (macdead && v->flags_off) goto done;
        __m128i bits = _mm_castps_si128(r);
        __m128i absb = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        __m128i zero = _mm_cmpeq_epi32(absb, _mm_setzero_si128());
        __m128i expz = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
        int zm = _mm_movemask_ps(_mm_castsi128_ps(zero));
        int sm = _mm_movemask_ps(r);
        int um = _mm_movemask_ps(_mm_castsi128_ps(_mm_andnot_si128(zero, expz)));
        u32 d = (u32)dest;
        v->new_mac = (g_rev4[zm] & d) | ((g_rev4[sm] & d) << 4) | ((g_rev4[um] & d) << 8);
        v->new_mac_valid = 1;
    }
done:
    out->valid = 1; out->to_acc = to_acc; out->dest = dest; out->reg = to_acc ? 0 : fd;
    _mm_storeu_ps(out->val.f, r);
}

/* ------------------------------------------------------------------ lower ops */
static inline u8 *qw(Vu *v, u32 addr) { return v->mem + (addr & (v->mem_qw - 1)) * 16; }   /* sizes are powers of 2 */
static inline s32 imm11(u32 w) { return (s32)(w << 21) >> 21; }
static inline void set_vi(Vu *v, int r, u32 val) {
    if (r && r < 16) { v->cur_vi_reg = r; v->cur_vi_old = v->vi[r]; v->vi[r] = val & 0xFFFF; }
}
/* Conditional-branch VI read: if the instructions right before the branch wrote this register, the
 * branch sees the value from before that run of writes (as modelled by PCSX2's branch VI-delay). */
/* Off by default: Eragon's VU0 collision code does FMAND then IBGTZ on the result in the next
 * instruction, so branches must see the new value. PS2_VIBACKUP=1 re-enables it for testing. */
static int vi_backup_off(void) { static int off = -1; if (off < 0) off = getenv("PS2_VIBACKUP") == NULL; return off; }
static u32 bvi(Vu *v, int r) {
    u32 val = v->vi[r];
    if (!r || vi_backup_off()) return val;
    for (int k = 0; k < 4; k++) {
        int i = (v->vh_head - 1 - k) & 3;
        if (v->vh_reg[i] != r) break;
        val = v->vh_old[i];
    }
    return val;
}

static void q_issue(Vu *v, float val, int latency) {
    v->q_pending = val; v->q_wait = 1; v->q_ready = v->cyc + latency;
    if (v->q_ready < v->next_event) v->next_event = v->q_ready;
}
static void p_issue(Vu *v, float val, int latency) {
    v->p_pending = val; v->p_wait = 1; v->p_ready = v->cyc + latency;
    if (v->p_ready < v->next_event) v->next_event = v->p_ready;
}
static void q_finish(Vu *v) { if (v->q_wait) { v->vi[VI_Q] = u32of(v->q_pending); v->q_wait = 0; } }
static void p_finish(Vu *v) { if (v->p_wait) { v->p = v->p_pending; v->p_wait = 0; } }
static void vu_run(Vu *v, u32 pc);

static int g_force_interp, g_kick_suppress;   /* PS2_VUVERIFY reference run */
static void xgkick(Vu *v, u32 addr) {
    if (g_kick_suppress) return;
    g_kicks++;
    /* walk GIF tags in VU1 memory (wrapping) to find the EOP packet length, then copy just that */
    u32 i = 0, len = 0;
    while (i < v->mem_qw) {
        u64 tlo; memcpy(&tlo, qw(v, addr + i), 8);
        u32 nloop = tlo & 0x7FFF, eop = (tlo >> 15) & 1, flg = (tlo >> 58) & 3, nreg = (u32)(tlo >> 60);
        if (!nreg) nreg = 16;
        i += 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        if (eop) { len = i; break; }
    }
    if (!len || len > v->mem_qw) { static int once; if (!once++) fprintf(stderr, "[vu1] XGKICK at %u: no EOP found\n", addr); return; }
    static u8 buf[16384];
    for (u32 k = 0; k < len; k++) memcpy(buf + k * 16, qw(v, addr + k), 16);
    { static int dump = -1, np = -1, progs[4], cnt[4];
      if (dump < 0) { const char *e = getenv("PS2_KICKDUMP"); dump = e ? atoi(e) : 0; }
      if (np < 0) { np = 0; const char *e = getenv("PS2_KICKPROG");   /* comma list of program addresses */
          while (e && *e && np < 4) { char *end; progs[np++] = (int)strtol(e, &end, 0); e = *end == ',' ? end + 1 : end + strlen(end); } }
      extern int g_respawn_frame, g_frame_no_pub;
      int slot = -1;
      if (!np) slot = 0;
      else if (g_respawn_frame && g_frame_no_pub > g_respawn_frame + 250)
          for (int k = 0; k < np; k++) if (g_vu1_prog * 8 == (u32)progs[k]) slot = k;
      if (dump && g_kicks >= (unsigned long long)dump && slot >= 0 && cnt[slot] < (np ? 2 : 3)) {
          cnt[slot]++;
          fprintf(stderr, "[xgkick] prog 0x%04x\n", g_vu1_prog * 8);
          fprintf(stderr, "[xgkick] #%llu addr %u len %u\n", g_kicks, addr, len);
          for (u32 k = 0; k < len && k < 24; k++) {
              u32 w[4]; memcpy(w, buf + k * 16, 16);
              fprintf(stderr, "   %3u: %08x %08x %08x %08x\n", k, w[0], w[1], w[2], w[3]);
          }
      } }
    { /* PS2_KICKFIND: dump the first few packets holding a drawn XYZ2 vertex at raw (0,0) */
      static int on = -1, n; if (on < 0) on = getenv("PS2_KICKFIND") != NULL;
      if (on && n < 2) {
          u32 i2 = 0;
          while (i2 < len) {
              u64 lo2, hi2; memcpy(&lo2, buf + i2 * 16, 8); memcpy(&hi2, buf + i2 * 16 + 8, 8);
              u32 nl = lo2 & 0x7FFF, fl = (lo2 >> 58) & 3, nr = (u32)(lo2 >> 60); if (!nr) nr = 16;
              if (fl == 0) {
                  int hit = -1;
                  for (u32 l = 0; l < nl && hit < 0; l++) for (u32 r = 0; r < nr; r++) {
                      u32 reg = (u32)(hi2 >> (4 * r)) & 15, q = i2 + 1 + l * nr + r;
                      if (q >= len) break;
                      u32 w[4]; memcpy(w, buf + q * 16, 16);
                      if ((reg == 4 || reg == 5) && !(w[3] & 0x8000) && (w[0] & 0xFFFF) == 0x10 && (w[1] & 0xFFFF) == 0x10) { hit = (int)q; break; }
                  }
                  if (hit >= 0) {
                      n++;
                      fprintf(stderr, "[kickfind] kick #%llu prog 0x%04x addr %u len %u, bad vertex at qw %d\n",
                              g_kicks, g_vu1_prog * 8, addr, len, hit);
                      if (n == 1) {
                          FILE *f = fopen("vu1_code.bin", "wb"); if (f) { fwrite(vu1_code, 1, sizeof vu1_code, f); fclose(f); }
                          f = fopen("vu1_data.bin", "wb"); if (f) { fwrite(vu1_data, 1, sizeof vu1_data, f); fclose(f); }
                          f = fopen("vu1_regs.bin", "wb"); if (f) { fwrite(v->vf, 16, 32, f); fwrite(v->vi, 4, 32, f); fclose(f); }
                      }
                      for (u32 k = 0; k < len && k < 64; k++) {
                          u32 w[4]; memcpy(w, buf + k * 16, 16);
                          fprintf(stderr, "   %3u: %08x %08x %08x %08x\n", k, w[0], w[1], w[2], w[3]);
                      }
                      break;
                  }
              }
              i2 += 1 + (fl == 0 ? nl * nr : fl == 1 ? (nl * nr + 1) / 2 : nl);
          }
      } }
    { /* per-program tally: drawn vs ADC (no-draw) vertices in PACKED XYZ2/XYZF2 */
      u32 drawn = 0, adc = 0, i2 = 0, inbox = 0, bx0 = 0xFFFF, bx1 = 0, by0 = 0xFFFF, by1 = 0;
      while (i2 < len) {
          u64 lo2, hi2; memcpy(&lo2, buf + i2 * 16, 8); memcpy(&hi2, buf + i2 * 16 + 8, 8);
          u32 nl = lo2 & 0x7FFF, fl = (lo2 >> 58) & 3, nr = (u32)(lo2 >> 60); if (!nr) nr = 16;
          if (fl == 0)
              for (u32 l = 0; l < nl; l++) for (u32 r = 0; r < nr; r++) {
                  u32 reg = (u32)(hi2 >> (4 * r)) & 15, q = i2 + 1 + l * nr + r;
                  if (q >= len || (reg != 4 && reg != 5)) continue;
                  u32 w3; memcpy(&w3, buf + q * 16 + 12, 4);
                  if (w3 & 0x8000) adc++;
                  else {
                      drawn++;
                      u32 X, Y; memcpy(&X, buf + q * 16, 4); memcpy(&Y, buf + q * 16 + 4, 4); X &= 0xFFFF; Y &= 0xFFFF;
                      if (X < bx0) bx0 = X; if (X > bx1) bx1 = X; if (Y < by0) by0 = Y; if (Y > by1) by1 = Y;
                      /* where Eragon's body should be: screen x 260..380, y 230..448 (offset 1728,1824) */
                      if (X >= (1728 + 260) * 16 && X <= (1728 + 380) * 16 && Y >= (1824 + 230) * 16 && Y <= (1824 + 448) * 16) inbox++;
                  }
              }
          i2 += 1 + (fl == 0 ? nl * nr : fl == 1 ? (nl * nr + 1) / 2 : nl);
      }
      ProgTally *t = prog_tally(g_vu1_prog);
      if (t) {
          t->kicks++; t->drawn += drawn; t->adc += adc; t->inbox += inbox;
          if (drawn) {
              if (!t->x1) { t->x0 = bx0; t->y0 = by0; }
              if (bx0 < t->x0) t->x0 = bx0; if (bx1 > t->x1) t->x1 = bx1; if (by0 < t->y0) t->y0 = by0; if (by1 > t->y1) t->y1 = by1;
          }
      } }
    gif_feed(1, buf, len, 1);
}

/* returns: 0 = continue, 1 = branch taken (target in *br) */
static int lower(Vu *v, u32 w, u32 pc, u32 *br) {
    int ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, dest = (w >> 21) & 15;
    int it = ft & 15, is = fs & 15, id = fd & 15;
    u32 op = w >> 25;
    if (op == 0x40) {
        int f = w & 63;
        switch (f) {
        case 0x30: set_vi(v, id, v->vi[is] + v->vi[it]); return 0;
        case 0x31: set_vi(v, id, v->vi[is] - v->vi[it]); return 0;
        case 0x32: { int im = (w >> 6) & 31; if (im & 16) im -= 32; set_vi(v, it, v->vi[is] + im); return 0; }
        case 0x34: set_vi(v, id, v->vi[is] & v->vi[it]); return 0;
        case 0x35: set_vi(v, id, v->vi[is] | v->vi[it]); return 0;
        }
        if (f < 0x3C) return 0;
        int sp = ((w >> 4) & 0x7C) | (w & 3);
        u128 s = v->vf[fs], r;
        int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
        switch (sp) {
        case 0x30: if (ft) for (int i = 0; i < 4; i++) if (HAS(dest, i)) v->vf[ft].uw[i] = s.uw[i]; return 0;   /* MOVE */
        case 0x31: r.uw[0] = s.uw[1]; r.uw[1] = s.uw[2]; r.uw[2] = s.uw[3]; r.uw[3] = s.uw[0];                  /* MR32 */
                   if (ft) for (int i = 0; i < 4; i++) if (HAS(dest, i)) v->vf[ft].uw[i] = r.uw[i]; return 0;
        case 0x34: case 0x36: {                                                                                /* LQI / LQD */
            if (sp == 0x36) set_vi(v, is, v->vi[is] - 1);
            memcpy(&r, qw(v, v->vi[is]), 16);
            if (ft) for (int i = 0; i < 4; i++) if (HAS(dest, i)) v->vf[ft].uw[i] = r.uw[i];
            if (sp == 0x34) set_vi(v, is, v->vi[is] + 1);
            return 0; }
        case 0x35: case 0x37: {                                                                                /* SQI / SQD */
            if (sp == 0x37) set_vi(v, it, v->vi[it] - 1);
            u8 *m = qw(v, v->vi[it]);
            for (int i = 0; i < 4; i++) if (HAS(dest, i)) memcpy(m + 4 * i, &s.uw[i], 4);
            if (sp == 0x35) set_vi(v, it, v->vi[it] + 1);
            return 0; }
        case 0x38: {                                                                                           /* DIV */
            float n = s.f[fsf], d = v->vf[ft].f[ftf];
            v->status &= ~0x30u;
            float q;
            if (d == 0.0f) { v->status |= (n == 0.0f) ? 0x410 : 0x820; q = (signbit(n) ^ signbit(d)) ? -3.402823466e38f : 3.402823466e38f; }
            else q = vclamp(n / d);
            q_issue(v, q, 7); return 0; }
        case 0x39: { float t = v->vf[ft].f[ftf]; v->status &= ~0x30u; if (t < 0) v->status |= 0x410;            /* SQRT */
                     q_issue(v, sqrtf(fabsf(t)), 7); return 0; }
        case 0x3A: {                                                                                           /* RSQRT */
            float n = s.f[fsf], t = v->vf[ft].f[ftf];
            v->status &= ~0x30u;
            float q;
            if (t == 0.0f) { v->status |= 0x820; q = signbit(n) ? -3.402823466e38f : 3.402823466e38f; }
            else { if (t < 0) v->status |= 0x410; q = vclamp(n / sqrtf(fabsf(t))); }
            q_issue(v, q, 13); return 0; }
        case 0x3B: return 0;                                                                                   /* WAITQ (stall in vu_run) */
        case 0x3C: set_vi(v, it, s.uw[fsf]); return 0;                                                         /* MTIR */
        case 0x3D: if (ft) for (int i = 0; i < 4; i++) if (HAS(dest, i)) v->vf[ft].sw[i] = (s16)v->vi[is]; return 0;   /* MFIR */
        case 0x3E: { u8 *m = qw(v, v->vi[is]);                                                                 /* ILWR */
                     for (int i = 0; i < 4; i++) if (HAS(dest, i)) { u32 x; memcpy(&x, m + 4 * i, 4); set_vi(v, it, x); break; }
                     return 0; }
        case 0x3F: { u8 *m = qw(v, v->vi[is]);                                                                 /* ISWR */
                     for (int i = 0; i < 4; i++) if (HAS(dest, i)) { u32 x = v->vi[it] & 0xFFFF; memcpy(m + 4 * i, &x, 4); }
                     return 0; }
        case 0x40: case 0x41: {                                                                                /* RNEXT / RGET */
            if (sp == 0x40) { u32 rr = v->vi[VI_R]; int x = (rr >> 4) & 1, y = (rr >> 22) & 1; rr = (rr << 1) ^ (u32)(x ^ y);
                              v->vi[VI_R] = (rr & 0x7FFFFF) | 0x3F800000; }
            if (ft) for (int i = 0; i < 4; i++) if (HAS(dest, i)) v->vf[ft].uw[i] = v->vi[VI_R];
            return 0; }
        case 0x42: v->vi[VI_R] = 0x3F800000 | (s.uw[fsf] & 0x7FFFFF); return 0;                                /* RINIT */
        case 0x43: v->vi[VI_R] = 0x3F800000 | ((v->vi[VI_R] ^ s.uw[fsf]) & 0x7FFFFF); return 0;                /* RXOR */
        case 0x64: if (ft) for (int i = 0; i < 4; i++) if (HAS(dest, i)) v->vf[ft].f[i] = v->p; return 0;      /* MFP */
        case 0x68: set_vi(v, it, v->unit ? vif1_top() : 0); return 0;                                          /* XTOP */
        case 0x69: set_vi(v, it, v->unit ? vif1_itop() : 0); return 0;                                         /* XITOP */
        case 0x6C: if (v->unit) xgkick(v, v->vi[is]); return 0;                                                /* XGKICK */
        /* EFU -> P */
        case 0x70: p_issue(v, s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2], 10); return 0;             /* ESADD */
        case 0x71: { float t = s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]; p_issue(v, t ? 1.0f / t : 3.4e38f, 17); return 0; }
        case 0x72: p_issue(v, sqrtf(s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]), 17); return 0;     /* ELENG */
        case 0x73: { float t = sqrtf(s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]); p_issue(v, t ? 1.0f / t : 3.4e38f, 23); return 0; }
        case 0x74: p_issue(v, atan2f(s.f[1], s.f[0]), 53); return 0;                                           /* EATANxy */
        case 0x75: p_issue(v, atan2f(s.f[2], s.f[0]), 53); return 0;                                           /* EATANxz */
        case 0x76: p_issue(v, s.f[0] + s.f[1] + s.f[2] + s.f[3], 11); return 0;                               /* ESUM */
        case 0x78: p_issue(v, sqrtf(fabsf(s.f[fsf])), 11); return 0;                                           /* ESQRT */
        case 0x79: { float t = sqrtf(fabsf(s.f[fsf])); p_issue(v, t ? 1.0f / t : 3.4e38f, 17); return 0; }    /* ERSQRT */
        case 0x7A: { float t = s.f[fsf]; p_issue(v, t ? 1.0f / t : 3.4e38f, 11); return 0; }                  /* ERCPR */
        case 0x7B: return 0;                                                                                   /* WAITP (stall in vu_run) */
        case 0x7C: p_issue(v, sinf(s.f[fsf]), 28); return 0;                                                  /* ESIN */
        case 0x7D: p_issue(v, atanf(s.f[fsf]), 53); return 0;                                                 /* EATAN */
        case 0x7E: p_issue(v, expf(-s.f[fsf]), 43); return 0;                                                 /* EEXP */
        }
        return 0;
    }
    u32 base = v->vi[is];
    switch (op) {
    case 0x00: { u128 r; memcpy(&r, qw(v, base + imm11(w)), 16);                                               /* LQ */
                 if (ft) for (int i = 0; i < 4; i++) if (HAS(dest, i)) v->vf[ft].uw[i] = r.uw[i]; return 0; }
    case 0x01: { u8 *m = qw(v, v->vi[it] + imm11(w)); u128 s = v->vf[fs];                                     /* SQ */
                 for (int i = 0; i < 4; i++) if (HAS(dest, i)) memcpy(m + 4 * i, &s.uw[i], 4); return 0; }
    case 0x04: { u8 *m = qw(v, base + imm11(w));                                                               /* ILW */
                 for (int i = 0; i < 4; i++) if (HAS(dest, i)) { u32 x; memcpy(&x, m + 4 * i, 4); set_vi(v, it, x); break; }
                 return 0; }
    case 0x05: { u8 *m = qw(v, base + imm11(w)); u32 x = v->vi[it] & 0xFFFF;                                   /* ISW */
                 for (int i = 0; i < 4; i++) if (HAS(dest, i)) memcpy(m + 4 * i, &x, 4); return 0; }
    case 0x08: case 0x09: {                                                                                    /* IADDIU / ISUBIU */
        u32 im = ((w >> 10) & 0x7800) | (w & 0x7FF);
        set_vi(v, it, op == 0x08 ? v->vi[is] + im : v->vi[is] - im); return 0; }
    case 0x10: set_vi(v, 1, (v->clip & 0xFFFFFF) == (w & 0xFFFFFF)); return 0;                                 /* FCEQ */
    case 0x11: v->clip = w & 0xFFFFFF; return 0;                                                               /* FCSET */
    case 0x12: set_vi(v, 1, (v->clip & w & 0xFFFFFF) != 0); return 0;                                          /* FCAND */
    case 0x13: set_vi(v, 1, ((v->clip | w) & 0xFFFFFF) == 0xFFFFFF); return 0;                                 /* FCOR */
    case 0x14: { u32 im = ((w >> 10) & 0x800) | (w & 0x7FF); set_vi(v, it, (v->status & 0xFFF) == im); return 0; }   /* FSEQ */
    case 0x15: { u32 im = ((w >> 10) & 0x800) | (w & 0x7FF); v->status = (v->status & 0x3F) | (im & 0xFC0); return 0; }   /* FSSET */
    case 0x16: { u32 im = ((w >> 10) & 0x800) | (w & 0x7FF); set_vi(v, it, v->status & im); return 0; }      /* FSAND */
    case 0x17: { u32 im = ((w >> 10) & 0x800) | (w & 0x7FF); set_vi(v, it, v->status | im); return 0; }      /* FSOR */
    case 0x18: set_vi(v, it, (v->mac & 0xFFFF) == (v->vi[is] & 0xFFFF)); return 0;                             /* FMEQ */
    case 0x1A: set_vi(v, it, v->mac & v->vi[is]); return 0;                                                    /* FMAND */
    case 0x1B: set_vi(v, it, v->mac | v->vi[is]); return 0;                                                    /* FMOR */
    case 0x1C: set_vi(v, it, v->clip & 0xFFF); return 0;                                                       /* FCGET */
    case 0x20: *br = pc + 1 + imm11(w); return 1;                                                              /* B */
    case 0x21: set_vi(v, it, pc + 2); *br = pc + 1 + imm11(w); return 1;                                       /* BAL */
    /* JR/JALR read the current value (only conditional branches see the pre-write value) */
    case 0x24: *br = v->vi[is]; return 1;                                                                      /* JR */
    case 0x25: { u32 t = v->vi[is]; set_vi(v, it, pc + 2); *br = t; return 1; }                               /* JALR */
    case 0x28: if ((bvi(v, it) & 0xFFFF) == (bvi(v, is) & 0xFFFF)) { *br = pc + 1 + imm11(w); return 1; } return 0;   /* IBEQ */
    case 0x29: if ((bvi(v, it) & 0xFFFF) != (bvi(v, is) & 0xFFFF)) { *br = pc + 1 + imm11(w); return 1; } return 0;   /* IBNE */
    case 0x2C: if ((s16)bvi(v, is) < 0) { *br = pc + 1 + imm11(w); return 1; } return 0;                      /* IBLTZ */
    case 0x2D: if ((s16)bvi(v, is) > 0) { *br = pc + 1 + imm11(w); return 1; } return 0;                      /* IBGTZ */
    case 0x2E: if ((s16)bvi(v, is) <= 0) { *br = pc + 1 + imm11(w); return 1; } return 0;                     /* IBLEZ */
    case 0x2F: if ((s16)bvi(v, is) >= 0) { *br = pc + 1 + imm11(w); return 1; } return 0;                     /* IBGEZ */
    }
    static int warned; if (warned++ < 10) fprintf(stderr, "[vu%d] unknown lower op %08x at %u\n", v->unit, w, pc);
    return 0;
}

/* ------------------------------------------------------------------ hazard analysis */
typedef struct { int fs, fsm, ft, ftm, accm; int wr, wrm; int q, p; } Hazard;   /* q/p: 1 = needs unit, 2 = WAIT */

static void upper_hazard(u32 w, Hazard *h) {
    int op = w & 63, dest = (w >> 21) & 15, bc = w & 3;
    h->fs = (w >> 11) & 31; h->ft = (w >> 16) & 31;
    if (op < 0x1C) { h->fsm = dest; h->ftm = 8 >> bc; if ((op >> 2) == 2 || (op >> 2) == 3) h->accm = dest; return; }
    if (op < 0x28) { h->fsm = dest; if (op == 0x21 || op == 0x23 || op == 0x25 || op == 0x27) h->accm = dest; return; }
    if (op < 0x30) {
        if (op == 0x2E) { h->fsm = h->ftm = h->accm = 0xE; return; }
        h->fsm = h->ftm = dest; if (op == 0x29 || op == 0x2D) h->accm = dest; return;
    }
    if (op < 0x3C) return;
    int sp = ((w >> 4) & 0x7C) | (w & 3);
    if (sp < 0x10) { h->fsm = dest; h->ftm = 8 >> bc; if ((sp >> 2) >= 2) h->accm = dest; return; }
    if (sp < 0x18 || sp == 0x1D) { h->fsm = dest; return; }
    if (sp < 0x1C) { h->fsm = dest; h->ftm = 8 >> bc; return; }
    if (sp == 0x1C || sp == 0x1E) { h->fsm = dest; return; }
    if (sp == 0x1F) { h->fsm = 0xE; h->ftm = 0x1; return; }
    if (sp < 0x28) { h->fsm = dest; if (sp == 0x21 || sp == 0x23 || sp == 0x25 || sp == 0x27) h->accm = dest; return; }
    if (sp < 0x2E) { h->fsm = h->ftm = dest; if (sp == 0x29 || sp == 0x2D) h->accm = dest; return; }
    if (sp == 0x2E) { h->fsm = h->ftm = 0xE; return; }
}

static void lower_hazard(u32 w, Hazard *h) {
    int ft = (w >> 16) & 31, fs = (w >> 11) & 31, dest = (w >> 21) & 15;
    int fsf = 8 >> ((w >> 21) & 3), ftf = 8 >> ((w >> 23) & 3);
    u32 op = w >> 25;
    h->fs = fs; h->ft = ft;
    if (op == 0x00) { h->wr = ft; h->wrm = dest; return; }        /* LQ */
    if (op == 0x01) { h->fsm = dest; return; }                    /* SQ */
    if (op != 0x40 || (w & 63) < 0x3C) return;
    int sp = ((w >> 4) & 0x7C) | (w & 3);
    switch (sp) {
    case 0x30: h->fsm = dest; h->wr = ft; h->wrm = dest; return;  /* MOVE */
    case 0x31: h->fsm = 0xF; h->wr = ft; h->wrm = dest; return;   /* MR32 */
    case 0x34: case 0x36: h->wr = ft; h->wrm = dest; return;      /* LQI/LQD */
    case 0x35: case 0x37: h->fsm = dest; return;                  /* SQI/SQD */
    case 0x38: case 0x3A: h->fsm = fsf; h->ftm = ftf; h->q = 1; return;   /* DIV / RSQRT */
    case 0x39: h->ftm = ftf; h->q = 1; return;                    /* SQRT */
    case 0x3B: h->q = 2; return;                                  /* WAITQ */
    case 0x3C: case 0x42: case 0x43: h->fsm = fsf; return;        /* MTIR / RINIT / RXOR */
    case 0x3D: case 0x40: case 0x41: case 0x64: h->wr = ft; h->wrm = dest; return;   /* MFIR / RNEXT / RGET / MFP */
    case 0x70: case 0x71: case 0x72: case 0x73: h->fsm = 0xE; h->p = 1; return;
    case 0x74: h->fsm = 0xC; h->p = 1; return;
    case 0x75: h->fsm = 0xA; h->p = 1; return;
    case 0x76: h->fsm = 0xF; h->p = 1; return;
    case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E: h->fsm = fsf; h->p = 1; return;
    case 0x7B: h->p = 2; return;                                  /* WAITP */
    }
}

static inline u64 need_vf(Vu *v, int r, int m) {
    u64 t = 0;
    if (!r || !m) return 0;
    for (int i = 0; i < 4; i++) if (HAS(m, i) && v->vf_ready[r][i] > t) t = v->vf_ready[r][i];
    return t;
}
static inline u64 need_hz(Vu *v, const Hazard *h) {
    u64 t = need_vf(v, h->fs, h->fsm), u = need_vf(v, h->ft, h->ftm);
    if (u > t) t = u;
    for (int i = 0; i < 4; i++) if (HAS(h->accm, i) && v->acc_ready[i] > t) t = v->acc_ready[i];
    if (h->q && v->q_wait && v->q_ready > t) t = v->q_ready;   /* FDIV busy, or WAITQ */
    if (h->p && v->p_wait && v->p_ready > t) t = v->p_ready;   /* EFU busy, or WAITP */
    return t;
}

/* ------------------------------------------------------------------ run loop */
/* Execution state shared by the interpreter and the translated pages (out/vu_pages.inc). */
typedef struct { u32 pc, delay_target; int in_delay, stop_after, done; u32 steps; } VuExec;

/* Decoded-op cache, validated against the instruction words (MPG may rewrite code). */
typedef struct {
    u32 lo, hi; u8 valid, ibit, ebit, unop, lnop, ufast, fkind, fsrc, facc, fd, fs, ft, dest, bc;
    u8 nrd, rd_reg[4], rd_mask[4], accm, needq, needp, lw_reg, lw_mask;
} DOp;
static DOp g_dc[2][2048];

/* Interpret one instruction pair at x->pc (the reference semantics the translated pages follow). */
static void interp_step(Vu *v, VuExec *x, int backup) {
    {
        u32 pc = x->pc;
        u32 lo, hi;
        memcpy(&lo, v->code + pc * 8, 4); memcpy(&hi, v->code + pc * 8 + 4, 4);
        DOp *d = &g_dc[v->unit][pc];
        if (!d->valid || d->lo != lo || d->hi != hi) {
            memset(d, 0, sizeof *d);
            d->lo = lo; d->hi = hi; d->valid = 1;
            d->ibit = (hi >> 31) & 1; d->ebit = (hi >> 30) & 1;
            Hazard hu = {0}, hl = {0};
            upper_hazard(hi, &hu);
            if (!d->ibit) lower_hazard(lo, &hl);
            const u8 regs[4] = {(u8)hu.fs, (u8)hu.ft, (u8)hl.fs, (u8)hl.ft}, msks[4] = {(u8)hu.fsm, (u8)hu.ftm, (u8)hl.fsm, (u8)hl.ftm};
            for (int k = 0; k < 4; k++) if (regs[k] && msks[k]) { d->rd_reg[d->nrd] = regs[k]; d->rd_mask[d->nrd] = msks[k]; d->nrd++; }
            d->accm = (u8)(hu.accm | hl.accm); d->needq = (u8)hl.q; d->needp = (u8)hl.p;
            d->lw_reg = (u8)hl.wr; d->lw_mask = (u8)hl.wrm;
            d->lnop = d->ibit || lo == 0x8000033Cu;
            d->unop = (hi & 0x7FF) == 0x2FF;                       /* upper NOP */
            d->ufast = !d->unop && fmac_class(hi, &d->fkind, &d->fsrc, &d->facc);
            d->dest = (hi >> 21) & 15; d->ft = (hi >> 16) & 31; d->fs = (hi >> 11) & 31; d->fd = (hi >> 6) & 31; d->bc = hi & 3;
        }
        /* stall until every source this instruction pair reads is ready */
        u64 t = v->cyc;
        for (int k = 0; k < d->nrd; k++) {
            const u64 *rr = v->vf_ready[d->rd_reg[k]]; int m = d->rd_mask[k];
            if ((m & 8) && rr[0] > t) t = rr[0];
            if ((m & 4) && rr[1] > t) t = rr[1];
            if ((m & 2) && rr[2] > t) t = rr[2];
            if ((m & 1) && rr[3] > t) t = rr[3];
        }
        /* No stall on ACC: the FMAC accumulate path is forwarded, so ADDA/MULA -> MADD issue back to back.
         * Flag-timing code depends on it (the VU0 ray/triangle test reads MAC with FMAND exactly 4 cycles
         * after the MADD it wants; stalling here made it read another instruction's flags, so character
         * collision accepted triangles nowhere near the ray and Eragon could not walk).
         * PS2_VU_ACCSTALL=1 restores the old behaviour for comparison. */
        { static int accstall = -1; if (accstall < 0) accstall = getenv("PS2_VU_ACCSTALL") != NULL;
          if (accstall && d->accm) for (int i = 0; i < 4; i++) if (HAS(d->accm, i) && v->acc_ready[i] > t) t = v->acc_ready[i]; }
        if (d->needq && v->q_wait && v->q_ready > t) t = v->q_ready;   /* FDIV busy, or WAITQ */
        if (d->needp && v->p_wait && v->p_ready > t) t = v->p_ready;   /* EFU busy, or WAITP */
        v->cyc = t;
        if (v->cyc >= v->next_event) pipes_advance(v);
        UpperResult ur; ur.valid = 0;
        int taken = 0; u32 tgt = 0;
        /* I bit: the lower word is loaded into I *after* the upper op, which still reads the old I.
         * Eragon's character renderer relies on this: "ADDi.z vf31, vf0, I  loi 128.0" must take the
         * previous loi (16777215.0, max Z), otherwise every character vertex gets Z = 128. */
        if (d->ufast) fmac_sse(v, d->fkind, d->fsrc, d->facc, d->dest, d->fd, d->fs, d->ft, d->bc, &ur, 0);
        else if (!d->unop) upper(v, hi, &ur);
        if (d->ibit) v->vi[VI_I] = lo;
        else if (!d->lnop) { v->cur_vi_reg = 0; taken = lower(v, lo, pc, &tgt); }
        commit_upper(v, &ur);
        if (v->new_mac_valid | v->new_clip_valid) flags_push(v);
        /* result readiness */
        if (d->lw_reg && d->lw_mask) for (int i = 0; i < 4; i++) if (HAS(d->lw_mask, i)) v->vf_ready[d->lw_reg][i] = v->cyc + 4;
        if (ur.valid) {
            if (ur.to_acc) { for (int i = 0; i < 4; i++) if (HAS(ur.dest, i)) v->acc_ready[i] = v->cyc + 4; }
            else if (ur.reg) { u64 *rr = v->vf_ready[ur.reg]; for (int i = 0; i < 4; i++) if (HAS(ur.dest, i)) rr[i] = v->cyc + 4; }
        }
        if (backup) {
            int r = d->lnop ? 0 : v->cur_vi_reg;
            v->vh_reg[v->vh_head] = r; v->vh_old[v->vh_head] = v->cur_vi_old; v->vh_head = (v->vh_head + 1) & 3;
        }
        v->cyc++;
        x->steps++;
        u32 next = pc + 1;
        if (x->in_delay) { next = x->delay_target; x->in_delay = 0; }
        if (taken) { x->delay_target = tgt; x->in_delay = 1; }
        if (x->stop_after) { x->pc = next; x->done = 1; return; }
        if (d->ebit) x->stop_after = 1;   /* E bit: run the next instruction, then stop */
        x->pc = next;
    }
}

/* ---- statically translated microcode pages (tools/vurecomp.py -> out/vu_pages.inc)
 * Pages are 64 instructions; a page function runs from x->pc until control leaves the page or the
 * program ends. Pages are found by content hash whenever VU code memory changes. */
typedef void (*VuPageFn)(Vu *, VuExec *);
typedef struct { u8 unit, page; u64 hash; VuPageFn fn; } VuPageEntry;
#include "vu_pages.inc"     /* defines g_vu_page_table[] and G_VU_NPAGES */
static VuPageFn g_pagefn[2][32];

static u64 page_hash(const u8 *p) {
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < 512; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
/* Can any instruction in the code image observe the (sticky) status flags (FSEQ/FSSET/FSAND/FSOR)?
 * VU1's flags are not visible to the EE; without such a reader, MAC results the translator proved
 * invisible to FM* instructions need not be computed at all. */
static int g_flags_needed[2] = {1, 1};
static int code_reads_flags(const Vu *v) {
    for (u32 pc = 0; pc < v->code_n; pc++) {
        u32 lo, hi; memcpy(&lo, v->code + pc * 8, 4); memcpy(&hi, v->code + pc * 8 + 4, 4);
        if (hi >> 31) continue;                      /* I bit: the lower word is a float immediate */
        u32 op = lo >> 25;
        if (op >= 0x14 && op <= 0x17) {
            static int n; if (getenv("PS2_VUFLAGLOG") && n++ < 40) fprintf(stderr, "[vuflags] unit %d pc %04x lo %08x hi %08x\n", v->unit, pc * 8, lo, hi);
            return 1;
        }
    }
    return 0;
}
static void resolve_pages(Vu *v) {
    static int dump = -1; if (dump < 0) dump = getenv("PS2_VUDUMP") != NULL;
    int unit = v->unit, npages = (int)(v->code_n / 64);
    g_vu_code_dirty[unit] = 0;
    { static int keep = -1; if (keep < 0) keep = getenv("PS2_VUFLAGS") != NULL || getenv("PS2_VUVERIFY") != NULL;
      g_flags_needed[unit] = unit == 0 || keep || code_reads_flags(v); }
    for (int p = 0; p < npages; p++) {
        u64 h = page_hash(v->code + p * 512);
        VuPageFn fn = NULL;
        for (int i = 0; i < G_VU_NPAGES; i++)
            if (g_vu_page_table[i].hash == h && g_vu_page_table[i].unit == unit && g_vu_page_table[i].page == p) { fn = g_vu_page_table[i].fn; break; }
        g_pagefn[unit][p] = fn;
        if (!fn && dump) {   /* save unknown pages for the next translation run */
            static u64 seen[4096]; static int ns;
            int k; for (k = 0; k < ns && seen[k] != (h ^ (u64)(unit * 32 + p)); k++) {}
            if (k == ns && ns < 4096) {
                seen[ns++] = h ^ (u64)(unit * 32 + p);
                CreateDirectoryA("vu_pages", NULL);
                char fn_[96]; snprintf(fn_, sizeof fn_, "vu_pages/u%d_p%02d_%016llx.bin", unit, p, (unsigned long long)h);
                FILE *f = fopen(fn_, "wb"); if (f) { fwrite(v->code + p * 512, 1, 512, f); fclose(f); }
            }
        }
    }
}

static void vu_run(Vu *v, u32 pc) {
    v->mac = v->vi[VI_MAC]; v->status = v->vi[VI_STATUS]; v->clip = v->vi[VI_CLIP];
    u32 start_pc = pc;
    masks_init();
    const int backup = !vi_backup_off();
    static int interp_only = -1; if (interp_only < 0) interp_only = getenv("PS2_VUINTERP") != NULL;
    const int use_pages = !backup && !interp_only && !g_force_interp && G_VU_NPAGES > 0;
    if (use_pages && g_vu_code_dirty[v->unit]) resolve_pages(v);
    else if (!use_pages && g_vu_code_dirty[v->unit] && getenv("PS2_VUDUMP")) resolve_pages(v);
    v->flags_off = use_pages && !g_flags_needed[v->unit];
    VuExec x; memset(&x, 0, sizeof x); x.pc = pc;
    while (!x.done && x.steps < 4000000) {
        x.pc &= v->code_n - 1;
        VuPageFn fn = use_pages ? g_pagefn[v->unit][x.pc >> 6] : NULL;
        if (fn) fn(v, &x); else interp_step(v, &x, backup);
    }
    pc = x.pc;
    g_vu1_instr += x.steps;
    int stop_after = x.done;
    if (!stop_after) {
        static int warned;
        if (warned++ < 10) fprintf(stderr, "[vu%d] program at 0x%04x hit the step limit (pc 0x%04x)\n", v->unit, start_pc * 8, pc * 8);
    }
    flags_flush(v);
    v->vi[VI_MAC] = v->mac; v->vi[VI_STATUS] = v->status; v->vi[VI_CLIP] = v->clip;
    v->vi[VI_TPC] = pc;
    if (v->unit) g_vu1_tpc = pc; else g_vu0_tpc = pc;
}
/* ------------------------------------------------------------------ entry points */
void vu1_start(u32 addr) {
    if (!g_vu1_starts++) fprintf(stderr, "[vu1] first microprogram start at 0x%04x\n", addr);
    g_vu1_vf[0].f[0] = g_vu1_vf[0].f[1] = g_vu1_vf[0].f[2] = 0; g_vu1_vf[0].f[3] = 1.0f;
    Vu v = {0};
    v.vf = g_vu1_vf; v.vi = g_vu1_vi; v.acc = &g_vu1_acc;
    v.mem = vu1_data; v.mem_qw = 1024; v.code = vu1_code; v.code_n = 2048; v.unit = 1;
    v.p = g_vu1_p;
    u32 pc = addr == 0xFFFFFFFFu ? g_vu1_tpc : addr / 8;
    g_vu1_prog = pc;
    image_stats();
    { /* restart the tally once gameplay has begun, so it only covers in-game programs */
      static int reset; extern int g_frame_no_pub;
      if (!reset && g_respawn_frame && g_frame_no_pub > g_respawn_frame + 250) { reset = 1; memset(g_tally, 0, sizeof g_tally); } }
    { ProgTally *t = prog_tally(pc); if (t) t->starts++; }
    { /* PS2_MATLOG=N: during gs frame N, log each distinct vf1..vf4 matrix that VU1 programs start with */
      static int fr = -2; extern int g_frame_no_pub;
      if (fr == -2) { const char *e = getenv("PS2_MATLOG"); fr = e ? atoi(e) : -1; }
      /* PS2_MATLOG=0: start 250 frames after the game console printed "Loading respawn ID" */
      extern int g_respawn_frame;
      int from = fr == 0 ? (g_respawn_frame ? g_respawn_frame + 250 : -1) : fr;
      if (fr >= 0 && from > 0 && g_frame_no_pub >= from && g_frame_no_pub < from + 3) {
          static struct { float m[16]; u32 pc; int n; } seen[256]; static int ns;
          float m[16]; memcpy(m, &g_vu1_vf[1], 64);
          int k; for (k = 0; k < ns && (seen[k].pc != pc || memcmp(seen[k].m, m, 64)); k++) {}
          if (k < ns) seen[k].n++;
          else if (ns < 256) { memcpy(seen[ns].m, m, 64); seen[ns].pc = pc; seen[ns].n = 1; ns++;
              fprintf(stderr, "[mat] f%d prog 0x%04x vf1 %g %g %g %g | vf2 %g %g %g %g | vf3 %g %g %g %g | vf4 %g %g %g %g\n",
                      g_frame_no_pub, pc * 8, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]); }
      } }
    { /* PS2_VU1CODEDUMP=0xADDR: dump VU1 code/data/regs the first time a program starts there */
      static int want = -2, done;
      if (want == -2) { const char *e = getenv("PS2_VU1CODEDUMP"); want = e ? (int)strtol(e, NULL, 0) : -1; }
      extern int g_frame_no_pub;
      static int need_ingame = -1; if (need_ingame < 0) need_ingame = getenv("PS2_VU1DUMP_INGAME") != NULL;
      int ingame = !need_ingame || (g_respawn_frame && g_frame_no_pub > g_respawn_frame + 250);
      if (want >= 0 && !done && pc * 8 == (u32)want && ingame) {
          done = 1;
          FILE *f = fopen("vu1_code.bin", "wb"); if (f) { fwrite(vu1_code, 1, sizeof vu1_code, f); fclose(f); }
          f = fopen("vu1_data.bin", "wb"); if (f) { fwrite(vu1_data, 1, sizeof vu1_data, f); fclose(f); }
          f = fopen("vu1_regs.bin", "wb"); if (f) { fwrite(g_vu1_vf, 16, 32, f); fwrite(g_vu1_vi, 4, 32, f); fclose(f); }
          fprintf(stderr, "[vu1] dumped code at first start of 0x%04x\n", want);
      } }
    unsigned long long i0 = g_vu1_instr;
    /* PS2_VUVERIFY=1: run the interpreter on a snapshot first (kicks suppressed), then the normal path
     * (translated pages), and report the first difference in registers or VU data memory. */
    static int verify = -1; if (verify < 0) verify = getenv("PS2_VUVERIFY") != NULL;
    static u8 s_mem[16384], a_mem[16384]; static u128 s_vf[32], a_vf[32], s_acc, a_acc; static u32 s_vi[32], a_vi[32], s_tpc, a_tpc;
    if (verify) {
        memcpy(s_mem, vu1_data, 16384); memcpy(s_vf, g_vu1_vf, sizeof s_vf); memcpy(s_vi, g_vu1_vi, sizeof s_vi); s_acc = g_vu1_acc; s_tpc = g_vu1_tpc;
        Vu va = v;
        g_force_interp = 1; g_kick_suppress = 1;
        vu_run(&va, pc); q_finish(&va); p_finish(&va);
        g_force_interp = 0; g_kick_suppress = 0;
        memcpy(a_mem, vu1_data, 16384); memcpy(a_vf, g_vu1_vf, sizeof a_vf); memcpy(a_vi, g_vu1_vi, sizeof a_vi); a_acc = g_vu1_acc; a_tpc = g_vu1_tpc;
        memcpy(vu1_data, s_mem, 16384); memcpy(g_vu1_vf, s_vf, sizeof s_vf); memcpy(g_vu1_vi, s_vi, sizeof s_vi); g_vu1_acc = s_acc; g_vu1_tpc = s_tpc;
        g_vu1_instr = i0;
    }
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    vu_run(&v, pc);
    if (verify) {
        static int nbad; static unsigned long long nrun;
        char why[160] = "";
        nrun++;
        for (int r = 1; r < 32 && !why[0]; r++) for (int i = 0; i < 4; i++)
            if (a_vf[r].uw[i] != g_vu1_vf[r].uw[i]) { snprintf(why, sizeof why, "vf%d[%d] interp %08x pages %08x", r, i, a_vf[r].uw[i], g_vu1_vf[r].uw[i]); break; }
        for (int r = 1; r < 16 && !why[0]; r++)
            if ((a_vi[r] & 0xFFFF) != (g_vu1_vi[r] & 0xFFFF)) snprintf(why, sizeof why, "vi%d interp %04x pages %04x", r, a_vi[r] & 0xFFFF, g_vu1_vi[r] & 0xFFFF);
        for (int i = 0; i < 4 && !why[0]; i++)
            if (a_acc.uw[i] != g_vu1_acc.uw[i]) snprintf(why, sizeof why, "acc[%d] interp %08x pages %08x", i, a_acc.uw[i], g_vu1_acc.uw[i]);
        if (!why[0] && a_tpc != g_vu1_tpc) snprintf(why, sizeof why, "end pc interp 0x%04x pages 0x%04x", a_tpc * 8, g_vu1_tpc * 8);
        for (int q = 0; q < 1024 && !why[0]; q++)
            if (memcmp(a_mem + q * 16, vu1_data + q * 16, 16)) {
                u32 a4[4], b4[4]; memcpy(a4, a_mem + q * 16, 16); memcpy(b4, vu1_data + q * 16, 16);
                snprintf(why, sizeof why, "mem qw %d interp %08x %08x %08x %08x pages %08x %08x %08x %08x", q, a4[0], a4[1], a4[2], a4[3], b4[0], b4[1], b4[2], b4[3]);
            }
        if (why[0] && nbad++ < 40) fprintf(stderr, "[vuverify] run %llu prog 0x%04x: %s\n", nrun, pc * 8, why);
        if (nrun % 100000 == 0) fprintf(stderr, "[vuverify] %llu runs, %d mismatches\n", nrun, nbad);
    }
    { ProgTally *t = prog_tally(pc); if (t) t->instr += g_vu1_instr - i0; }
    QueryPerformanceCounter(&t1); g_vu1_ticks += t1.QuadPart - t0.QuadPart;
    q_finish(&v); p_finish(&v);
    g_vu1_p = v.p;
}

/* VU0 micro mode runs on the EE's view of VU0 (the current thread's context). */
R5900Context *kernel_current_ctx(void);
void vu0_micro_run(R5900Context *c, u32 addr) {
    if (!g_vu0_starts++) fprintf(stderr, "[vu0] first microprogram start at 0x%04x\n", addr);
    { /* PS2_VU0DUMP=N: after N starts, dump VU0 code and a histogram of start addresses */
      static long long dumpat = -2; static u32 hist[512];
      if (dumpat == -2) { const char *e = getenv("PS2_VU0DUMP"); dumpat = e ? atoll(e) : -1; }
      if (dumpat >= 0) {
          hist[(addr == 0xFFFFFFFFu ? g_vu0_tpc : addr / 8) & 511]++;
          if ((long long)g_vu0_starts == dumpat) {
              FILE *f = fopen("vu0_code.bin", "wb"); if (f) { fwrite(vu0_code, 1, sizeof vu0_code, f); fclose(f); }
              for (int i = 0; i < 512; i++) if (hist[i]) fprintf(stderr, "[vu0hist] 0x%04x x%u\n", i * 8, hist[i]);
          }
      } }
    Vu v = {0};
    v.vf = c->vf; v.vi = c->vi; v.acc = &c->vacc;
    v.mem = vu0_data; v.mem_qw = 256; v.code = vu0_code; v.code_n = 512; v.unit = 0;
    v.p = g_vu0_p;
    u32 pc = addr == 0xFFFFFFFFu ? g_vu0_tpc : addr / 8;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    vu_run(&v, pc);
    QueryPerformanceCounter(&t1); g_vu0_ticks += t1.QuadPart - t0.QuadPart;
    q_finish(&v); p_finish(&v);
    g_vu0_p = v.p;
    if (pc == 0 || pc == 3) { g_vu0_ray_calls++; if (c->vf[19].f[0] == 1.0f) g_vu0_ray_hits++; }
    c->vf[0].f[0] = c->vf[0].f[1] = c->vf[0].f[2] = 0; c->vf[0].f[3] = 1.0f;
}
void vu0_micro_start(u32 addr) { vu0_micro_run(kernel_current_ctx(), addr); }

void vu_report(void) {
    fprintf(stderr, "[vu] VU1 starts %llu (%llu instr, %llu XGKICKs), VU0 micro starts %llu, ray tests %llu hits %llu\n",
            g_vu1_starts, g_vu1_instr, g_kicks, g_vu0_starts, g_vu0_ray_calls, g_vu0_ray_hits);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    fprintf(stderr, "[vu] time: VU1 %.1f s, VU0 micro %.1f s\n", (double)g_vu1_ticks / f.QuadPart, (double)g_vu0_ticks / f.QuadPart);
    if (getenv("PS2_VUTALLY"))
        for (int i = 0; i < 64; i++) if (g_tally[i].starts)
            fprintf(stderr, "[vutally] prog 0x%04x starts %llu instr %llu kicks %llu drawn %llu adc %llu inbox %llu bbox x %d..%d y %d..%d\n",
                    g_tally[i].pc * 8, g_tally[i].starts, g_tally[i].instr, g_tally[i].kicks, g_tally[i].drawn, g_tally[i].adc, g_tally[i].inbox,
                    (int)g_tally[i].x0 / 16 - 1728, (int)g_tally[i].x1 / 16 - 1728, (int)g_tally[i].y0 / 16 - 1824, (int)g_tally[i].y1 / 16 - 1824);
}
