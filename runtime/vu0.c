/* VU0 macro mode (COP2) interpreter + VU memories.
 * Flags: MAC (vi17), status (vi16) and clip (vi18) are updated immediately (EE code pads with VNOPs).
 * VCALLMS/VCALLMSR run microprograms through the vu1.c interpreter. */
#include <math.h>
#include <stdio.h>
#include "ps2_runtime.h"

u8 vu0_code[4096], vu0_data[4096], vu1_code[16384], vu1_data[16384];

enum { VI_STATUS = 16, VI_MAC = 17, VI_CLIP = 18, VI_R = 20, VI_I = 21, VI_Q = 22, VI_P = 23,
       VI_TPC = 26, VI_CMSAR0 = 27, VI_FBRST = 28, VI_VPUSTAT = 29, VI_CMSAR1 = 31 };

static float getf(u32 u) { float f; memcpy(&f, &u, 4); return f; }
static u32 putf(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* VU floats have no Inf/NaN: clamp to +-max like PCSX2's default clamping. */
static float vclamp(float f) {
    u32 u = putf(f);
    if ((u & 0x7f800000u) == 0x7f800000u) u = (u & 0x80000000u) | 0x7f7fffffu;
    return getf(u);
}

#define W_FT ((w >> 16) & 31)
#define W_FS ((w >> 11) & 31)
#define W_FD ((w >> 6) & 31)
#define W_DEST ((w >> 21) & 15)
#define W_BC (w & 3)
#define HAS(dest, i) ((dest) & (8 >> (i)))

enum { K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MIN, K_MUL };
enum { S_VEC, S_BC, S_Q, S_I };

/* MAC flag (vi17) and status (vi16) for an FMAC result. Macro-mode code reads them with CFC2 after
 * padding with VNOPs, so they are updated immediately. x is the highest bit of each nibble. */
static void set_flags(R5900Context *c, int dest, const float r[4]) {
    u32 mac = 0;
    for (int i = 0; i < 4; i++) {
        if (!HAS(dest, i)) continue;
        u32 u = putf(r[i]); int bit = 3 - i;
        if ((u & 0x7fffffffu) == 0) mac |= 1u << bit;
        if (u & 0x80000000u) mac |= 1u << (bit + 4);
        if ((u & 0x7f800000u) == 0 && (u & 0x7fffff)) mac |= 1u << (bit + 8);
        if ((u & 0x7f800000u) == 0x7f800000u) mac |= 1u << (bit + 12);
    }
    c->vi[VI_MAC] = mac;
    u32 st = c->vi[VI_STATUS] & 0xff0;
    if (mac & 0x000f) st |= 0x001;
    if (mac & 0x00f0) st |= 0x002;
    if (mac & 0x0f00) st |= 0x004;
    if (mac & 0xf000) st |= 0x008;
    st |= (st & 0xf) << 6;
    c->vi[VI_STATUS] = st;
}

static void fmac(R5900Context *c, u32 w, int kind, int src, int to_acc) {
    int dest = W_DEST, fd = to_acc ? -1 : W_FD;
    u128 a = c->vf[W_FS], b = c->vf[W_FT], acc = c->vacc;
    float s = src == S_BC ? b.f[W_BC] : src == S_Q ? getf(c->vi[VI_Q]) : src == S_I ? getf(c->vi[VI_I]) : 0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float x = a.f[i], y = src == S_VEC ? b.f[i] : s;
        switch (kind) {
        case K_ADD:  r[i] = x + y; break;
        case K_SUB:  r[i] = x - y; break;
        case K_MUL:  r[i] = x * y; break;
        case K_MADD: r[i] = acc.f[i] + x * y; break;
        case K_MSUB: r[i] = acc.f[i] - x * y; break;
        case K_MAX:  r[i] = x > y ? x : y; break;
        default:     r[i] = x < y ? x : y; break;
        }
        if (kind != K_MAX && kind != K_MIN) r[i] = vclamp(r[i]);
    }
    if (kind != K_MAX && kind != K_MIN) set_flags(c, dest, r);
    for (int i = 0; i < 4; i++) if (HAS(dest, i)) {
        if (to_acc) c->vacc.f[i] = r[i];
        else if (fd) c->vf[fd].f[i] = r[i];
    }
}

static void wr_vec(R5900Context *c, int reg, int dest, const u128 *v) {
    if (!reg) return;
    for (int i = 0; i < 4; i++) if (HAS(dest, i)) c->vf[reg].uw[i] = v->uw[i];
}

static void advance_r(R5900Context *c) {
    u32 r = c->vi[VI_R];
    int x = (r >> 4) & 1, y = (r >> 22) & 1;
    r = (r << 1) ^ (u32)(x ^ y);
    c->vi[VI_R] = (r & 0x007fffffu) | 0x3f800000u;
}

static u8 *vu0mem(u32 vaddr_qw) { return vu0_data + ((vaddr_qw * 16) & 0xfff); }

static void special2(R5900Context *c, u32 pc, u32 w) {
    int op = ((w >> 4) & 0x7c) | (w & 3);
    int dest = W_DEST, ft = W_FT, fs = W_FS;
    int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
    u128 s = c->vf[fs], r;
    if (op < 0x10) { static const int k[4] = {K_ADD, K_SUB, K_MADD, K_MSUB}; fmac(c, w, k[op >> 2], S_BC, 1); return; }
    switch (op) {
    case 0x10: case 0x11: case 0x12: case 0x13: {   /* VITOF0/4/12/15 */
        static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
        for (int i = 0; i < 4; i++) r.f[i] = (float)s.sw[i] * sc[op & 3];
        wr_vec(c, ft, dest, &r); return; }
    case 0x14: case 0x15: case 0x16: case 0x17: {   /* VFTOI0/4/12/15 */
        static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        for (int i = 0; i < 4; i++) {
            float v = s.f[i] * sc[op & 3];
            r.sw[i] = v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (s32)v;
        }
        wr_vec(c, ft, dest, &r); return; }
    case 0x18: case 0x19: case 0x1a: case 0x1b: fmac(c, w, K_MUL, S_BC, 1); return;   /* VMULAbc */
    case 0x1c: fmac(c, w, K_MUL, S_Q, 1); return;
    case 0x1d: for (int i = 0; i < 4; i++) r.f[i] = fabsf(s.f[i]); wr_vec(c, ft, dest, &r); return;   /* VABS */
    case 0x1e: fmac(c, w, K_MUL, S_I, 1); return;
    case 0x1f: {                                                       /* VCLIPw.xyz */
        float lim = fabsf(c->vf[ft].f[3]);
        u32 cf = (c->vi[VI_CLIP] << 6) & 0xffffff;
        for (int i = 0; i < 3; i++) {
            if (s.f[i] > lim)  cf |= 1u << (2 * i);
            if (s.f[i] < -lim) cf |= 2u << (2 * i);
        }
        c->vi[VI_CLIP] = cf; return; }
    case 0x20: fmac(c, w, K_ADD, S_Q, 1); return;
    case 0x21: fmac(c, w, K_MADD, S_Q, 1); return;
    case 0x22: fmac(c, w, K_ADD, S_I, 1); return;
    case 0x23: fmac(c, w, K_MADD, S_I, 1); return;
    case 0x24: fmac(c, w, K_SUB, S_Q, 1); return;
    case 0x25: fmac(c, w, K_MSUB, S_Q, 1); return;
    case 0x26: fmac(c, w, K_SUB, S_I, 1); return;
    case 0x27: fmac(c, w, K_MSUB, S_I, 1); return;
    case 0x28: fmac(c, w, K_ADD, S_VEC, 1); return;
    case 0x29: fmac(c, w, K_MADD, S_VEC, 1); return;
    case 0x2a: fmac(c, w, K_MUL, S_VEC, 1); return;
    case 0x2c: fmac(c, w, K_SUB, S_VEC, 1); return;
    case 0x2d: fmac(c, w, K_MSUB, S_VEC, 1); return;
    case 0x2e: {                                                       /* VOPMULA.xyz */
        u128 t = c->vf[ft];
        c->vacc.f[0] = vclamp(s.f[1] * t.f[2]);
        c->vacc.f[1] = vclamp(s.f[2] * t.f[0]);
        c->vacc.f[2] = vclamp(s.f[0] * t.f[1]);
        set_flags(c, 0xE, c->vacc.f);
        return; }
    case 0x2f: return;                                                 /* VNOP */
    case 0x30: wr_vec(c, ft, dest, &s); return;                        /* VMOVE */
    case 0x31: r.uw[0] = s.uw[1]; r.uw[1] = s.uw[2]; r.uw[2] = s.uw[3]; r.uw[3] = s.uw[0];
               wr_vec(c, ft, dest, &r); return;                        /* VMR32 */
    case 0x34: { int is = fs; memcpy(&r, vu0mem(c->vi[is]), 16); wr_vec(c, ft, dest, &r);   /* VLQI */
                 if (is) c->vi[is] = (c->vi[is] + 1) & 0xffff; return; }
    case 0x35: { int it = ft; u8 *m = vu0mem(c->vi[it]);                                     /* VSQI */
                 for (int i = 0; i < 4; i++) if (HAS(dest, i)) memcpy(m + 4 * i, &s.uw[i], 4);
                 if (it) c->vi[it] = (c->vi[it] + 1) & 0xffff; return; }
    case 0x36: { int is = fs; if (is) c->vi[is] = (c->vi[is] - 1) & 0xffff;                  /* VLQD */
                 memcpy(&r, vu0mem(c->vi[is]), 16); wr_vec(c, ft, dest, &r); return; }
    case 0x37: { int it = ft; if (it) c->vi[it] = (c->vi[it] - 1) & 0xffff;                  /* VSQD */
                 u8 *m = vu0mem(c->vi[it]);
                 for (int i = 0; i < 4; i++) if (HAS(dest, i)) memcpy(m + 4 * i, &s.uw[i], 4); return; }
    case 0x38: {                                                       /* VDIV Q = fs.fsf / ft.ftf */
        float d = c->vf[ft].f[ftf], n = s.f[fsf];
        float q = d == 0.0f ? ((signbit(n) ^ signbit(d)) ? -3.402823466e38f : 3.402823466e38f) : vclamp(n / d);
        c->vi[VI_Q] = putf(q); return; }
    case 0x39: c->vi[VI_Q] = putf(sqrtf(fabsf(c->vf[ft].f[ftf]))); return;                   /* VSQRT */
    case 0x3a: {                                                       /* VRSQRT Q = fs.fsf / sqrt(ft.ftf) */
        float d = sqrtf(fabsf(c->vf[ft].f[ftf])), n = s.f[fsf];
        c->vi[VI_Q] = putf(d == 0.0f ? (signbit(n) ? -3.402823466e38f : 3.402823466e38f) : vclamp(n / d));
        return; }
    case 0x3b: return;                                                 /* VWAITQ */
    case 0x3c: if (ft) c->vi[ft] = s.uw[fsf] & 0xffff; return;         /* VMTIR it = fs.fsf */
    case 0x3d: { int is = fs; for (int i = 0; i < 4; i++) r.sw[i] = (s16)c->vi[is];          /* VMFIR */
                 wr_vec(c, ft, dest, &r); return; }
    case 0x3e: { int is = fs, it = ft; u8 *m = vu0mem(c->vi[is]);                            /* VILWR */
                 for (int i = 0; i < 4; i++) if (HAS(dest, i)) { u32 v; memcpy(&v, m + 4 * i, 4); if (it) c->vi[it] = v & 0xffff; break; }
                 return; }
    case 0x3f: { int is = fs, it = ft; u8 *m = vu0mem(c->vi[is]);                            /* VISWR */
                 for (int i = 0; i < 4; i++) if (HAS(dest, i)) { u32 v = c->vi[it] & 0xffff; memcpy(m + 4 * i, &v, 4); }
                 return; }
    case 0x40: advance_r(c); /* fallthrough */                         /* VRNEXT */
    case 0x41: for (int i = 0; i < 4; i++) r.uw[i] = c->vi[VI_R]; wr_vec(c, ft, dest, &r); return;   /* VRGET */
    case 0x42: c->vi[VI_R] = 0x3f800000u | (s.uw[fsf] & 0x007fffffu); return;                        /* VRINIT */
    case 0x43: c->vi[VI_R] = 0x3f800000u | ((c->vi[VI_R] ^ s.uw[fsf]) & 0x007fffffu); return;        /* VRXOR */
    }
    ps2_unimpl(c, pc, w, "VU0 special2 op");
}

void vu0_macro(R5900Context *c, u32 pc, u32 w) {
    int f = w & 63;
    if (f >= 0x3c) { special2(c, pc, w); return; }
    if (f < 0x1c) {
        static const int k[7] = {K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MIN, K_MUL};
        fmac(c, w, k[f >> 2], S_BC, 0); return;
    }
    int is = W_FS, it = W_FT, id = W_FD;
    switch (f) {
    case 0x1c: fmac(c, w, K_MUL, S_Q, 0); return;
    case 0x1d: fmac(c, w, K_MAX, S_I, 0); return;
    case 0x1e: fmac(c, w, K_MUL, S_I, 0); return;
    case 0x1f: fmac(c, w, K_MIN, S_I, 0); return;
    case 0x20: fmac(c, w, K_ADD, S_Q, 0); return;
    case 0x21: fmac(c, w, K_MADD, S_Q, 0); return;
    case 0x22: fmac(c, w, K_ADD, S_I, 0); return;
    case 0x23: fmac(c, w, K_MADD, S_I, 0); return;
    case 0x24: fmac(c, w, K_SUB, S_Q, 0); return;
    case 0x25: fmac(c, w, K_MSUB, S_Q, 0); return;
    case 0x26: fmac(c, w, K_SUB, S_I, 0); return;
    case 0x27: fmac(c, w, K_MSUB, S_I, 0); return;
    case 0x28: fmac(c, w, K_ADD, S_VEC, 0); return;
    case 0x29: fmac(c, w, K_MADD, S_VEC, 0); return;
    case 0x2a: fmac(c, w, K_MUL, S_VEC, 0); return;
    case 0x2b: fmac(c, w, K_MAX, S_VEC, 0); return;
    case 0x2c: fmac(c, w, K_SUB, S_VEC, 0); return;
    case 0x2d: fmac(c, w, K_MSUB, S_VEC, 0); return;
    case 0x2e: {                                                       /* VOPMSUB.xyz */
        u128 s = c->vf[W_FS], t = c->vf[W_FT], a = c->vacc;
        float r0 = vclamp(a.f[0] - s.f[1] * t.f[2]);
        float r1 = vclamp(a.f[1] - s.f[2] * t.f[0]);
        float r2 = vclamp(a.f[2] - s.f[0] * t.f[1]);
        int fd = W_FD;
        float rr[4] = {r0, r1, r2, 0};
        set_flags(c, 0xE, rr);
        if (fd) { c->vf[fd].f[0] = r0; c->vf[fd].f[1] = r1; c->vf[fd].f[2] = r2; }
        return; }
    case 0x2f: fmac(c, w, K_MIN, S_VEC, 0); return;
    case 0x30: if (id) c->vi[id] = (c->vi[is] + c->vi[it]) & 0xffff; return;                  /* VIADD */
    case 0x31: if (id) c->vi[id] = (c->vi[is] - c->vi[it]) & 0xffff; return;                  /* VISUB */
    case 0x32: { int imm = (w >> 6) & 31; if (imm & 16) imm -= 32;                            /* VIADDI */
                 if (it) c->vi[it] = (c->vi[is] + imm) & 0xffff; return; }
    case 0x34: if (id) c->vi[id] = c->vi[is] & c->vi[it]; return;                             /* VIAND */
    case 0x35: if (id) c->vi[id] = c->vi[is] | c->vi[it]; return;                             /* VIOR */
    case 0x38: case 0x39: {   /* VCALLMS / VCALLMSR: run a VU0 microprogram (vu1.c engine) */
        void vu0_micro_run(R5900Context *c, u32 addr);
        u32 addr = f == 0x38 ? ((w >> 6) & 0x7fff) << 3 : (c->vi[VI_CMSAR0] & 0xffff) << 3;
        vu0_micro_run(c, addr); return; }
    }
    ps2_unimpl(c, pc, w, "VU0 special1 op");
}

void vu0_ctc2(R5900Context *c, int reg, u32 v) {
    if (reg == 0) return;
    if (reg < 16) { c->vi[reg] = v & 0xffff; return; }
    switch (reg) {
    case VI_FBRST: if (v & 0x202) { /* VU0/VU1 reset */ } return;
    case VI_CMSAR1: { void vu1_start(u32 addr); vu1_start((v & 0xffff) << 3); return; }   /* start VU1 from the EE */
    case VI_VPUSTAT: return;
    default: c->vi[reg] = v; return;
    }
}

int vu0_cond(R5900Context *c) { (void)c; return 0; }   /* BC2: VU0 never "running" */

void vu0_copy_state(R5900Context *dst, const R5900Context *src) {
    if (dst == src) return;
    memcpy(dst->vf, src->vf, sizeof dst->vf);
    memcpy(dst->vi, src->vi, sizeof dst->vi);
    dst->vacc = src->vacc;
}
