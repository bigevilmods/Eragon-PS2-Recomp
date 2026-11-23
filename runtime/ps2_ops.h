/* Semantics of every R5900 instruction the recompiler emits as OP_<MNEMONIC>(c, ...).
 * Operand order matches the decoder formats in tools/r5900.py:
 *   r3 (rd,rs,rt)  sh (rd,rt,sa)  shv (rd,rt,rs)  i (rt,rs,imm)  lui (rt,imm)
 *   mem (rt,base,off)  rd (rd)  rs (rs)  st (rs,rt)  fmv (rt,reg)  f3 (fd,fs,ft)
 *   cop0 (rt,rd)  i1 (rs,imm)  mmisub (rd,rs,sa)  none/raw (pc,word)  code (pc,code)
 */
#pragma once
#include <math.h>
#include "ps2_runtime.h"

/* NB: macro parameters must not be named r/vf/vi/hi/lo: they would replace field names. */
#define GPR_U64(n) (c->r[n].ud[0])
#define GPR_S64(n) (c->r[n].sd[0])
#define GPR_U32(n) (c->r[n].uw[0])
#define GPR_S32(n) (c->r[n].sw[0])
#define GPR_Q(n)   (c->r[n])
#define SE32(x)    ((u64)(s64)(s32)(u32)(x))
#define SET_GPR_U64(n, v) do { u64 _v = (u64)(v); if ((n) != 0) c->r[n].ud[0] = _v; } while (0)
#define SET_GPR_Q(n, v)   do { u128 _q = (v); if ((n) != 0) c->r[n] = _q; } while (0)
#define EA(base, off) ((u32)(GPR_U32(base) + (s32)(off)))
#define FPR(n)  (c->fpr[n].f)
#define FPRU(n) (c->fpr[n].u)
#define FPU_COND  (c->fcond)
#define COP0_COND ps2_cop0_cond(c)
#define VU0_COND  vu0_cond(c)

/* ---------------------------------------------------------------- immediates */
#define OP_ADDI(rt, rs, imm)   SET_GPR_U64(rt, SE32(GPR_U32(rs) + (u32)(s32)(imm)))
#define OP_ADDIU(rt, rs, imm)  OP_ADDI(rt, rs, imm)
#define OP_DADDI(rt, rs, imm)  SET_GPR_U64(rt, GPR_U64(rs) + (u64)(s64)(imm))
#define OP_DADDIU(rt, rs, imm) OP_DADDI(rt, rs, imm)
#define OP_SLTI(rt, rs, imm)   SET_GPR_U64(rt, GPR_S64(rs) < (s64)(imm))
#define OP_SLTIU(rt, rs, imm)  SET_GPR_U64(rt, GPR_U64(rs) < (u64)(s64)(imm))
#define OP_ANDI(rt, rs, imm)   SET_GPR_U64(rt, GPR_U64(rs) & (u64)(imm))
#define OP_ORI(rt, rs, imm)    SET_GPR_U64(rt, GPR_U64(rs) | (u64)(imm))
#define OP_XORI(rt, rs, imm)   SET_GPR_U64(rt, GPR_U64(rs) ^ (u64)(imm))
#define OP_LUI(rt, imm)        SET_GPR_U64(rt, SE32((u32)(imm) << 16))

/* ---------------------------------------------------------------- loads/stores */
#define OP_LB(rt, b, o)  SET_GPR_U64(rt, (u64)(s64)(s8)MEM_R8(EA(b, o)))
#define OP_LBU(rt, b, o) SET_GPR_U64(rt, MEM_R8(EA(b, o)))
#define OP_LH(rt, b, o)  SET_GPR_U64(rt, (u64)(s64)(s16)MEM_R16(EA(b, o)))
#define OP_LHU(rt, b, o) SET_GPR_U64(rt, MEM_R16(EA(b, o)))
#define OP_LW(rt, b, o)  SET_GPR_U64(rt, SE32(MEM_R32(EA(b, o))))
#define OP_LWU(rt, b, o) SET_GPR_U64(rt, MEM_R32(EA(b, o)))
#define OP_LD(rt, b, o)  SET_GPR_U64(rt, MEM_R64(EA(b, o)))
#define OP_LQ(rt, b, o)  SET_GPR_Q(rt, MEM_R128(EA(b, o) & ~15u))
#define OP_SB(rt, b, o)  MEM_W8(EA(b, o), (u8)GPR_U32(rt))
#define OP_SH(rt, b, o)  MEM_W16(EA(b, o), (u16)GPR_U32(rt))
#define OP_SW(rt, b, o)  MEM_W32(EA(b, o), GPR_U32(rt))
#define OP_SD(rt, b, o)  MEM_W64(EA(b, o), GPR_U64(rt))
#define OP_SQ(rt, b, o)  MEM_W128(EA(b, o) & ~15u, GPR_Q(rt))
#define OP_LWC1(ft, b, o) (FPRU(ft) = MEM_R32(EA(b, o)))
#define OP_SWC1(ft, b, o) MEM_W32(EA(b, o), FPRU(ft))
#define OP_LQC2(vt, b, o) do { u128 _q = MEM_R128(EA(b, o) & ~15u); if (vt) c->vf[vt] = _q; } while (0)
#define OP_SQC2(vt, b, o) MEM_W128(EA(b, o) & ~15u, c->vf[vt])

static inline void op_lwl(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, mem = MEM_R32(a & ~3u);
    u32 v = (GPR_U32(rt) & (0x00ffffffu >> sh)) | (mem << (24 - sh));
    SET_GPR_U64(rt, SE32(v));
}
static inline void op_lwr(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, mem = MEM_R32(a & ~3u);
    if (sh == 0) { SET_GPR_U64(rt, SE32(mem)); return; }
    u32 v = (GPR_U32(rt) & (0xffffff00u << (24 - sh))) | (mem >> sh);
    if (rt) c->r[rt].uw[0] = v;   /* upper 32 bits unchanged */
}
static inline void op_swl(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, mem = MEM_R32(a & ~3u);
    MEM_W32(a & ~3u, (GPR_U32(rt) >> (24 - sh)) | (sh == 24 ? 0 : mem & (0xffffff00u << sh)));
}
static inline void op_swr(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, mem = MEM_R32(a & ~3u);
    MEM_W32(a & ~3u, (GPR_U32(rt) << sh) | (sh == 0 ? 0 : mem & (0x00ffffffu >> (24 - sh))));
}
static inline void op_ldl(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 mem = MEM_R64(a & ~7u);
    SET_GPR_U64(rt, (GPR_U64(rt) & (0x00ffffffffffffffull >> sh)) | (mem << (56 - sh)));
}
static inline void op_ldr(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 mem = MEM_R64(a & ~7u);
    SET_GPR_U64(rt, (sh == 0 ? 0 : GPR_U64(rt) & (0xffffffffffffff00ull << (56 - sh))) | (mem >> sh));
}
static inline void op_sdl(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 mem = MEM_R64(a & ~7u);
    MEM_W64(a & ~7u, (GPR_U64(rt) >> (56 - sh)) | (sh == 56 ? 0 : mem & (0xffffffffffffff00ull << sh)));
}
static inline void op_sdr(R5900Context *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 mem = MEM_R64(a & ~7u);
    MEM_W64(a & ~7u, (GPR_U64(rt) << sh) | (sh == 0 ? 0 : mem & (0x00ffffffffffffffull >> (56 - sh))));
}
#define OP_LWL(rt, b, o) op_lwl(c, rt, EA(b, o))
#define OP_LWR(rt, b, o) op_lwr(c, rt, EA(b, o))
#define OP_SWL(rt, b, o) op_swl(c, rt, EA(b, o))
#define OP_SWR(rt, b, o) op_swr(c, rt, EA(b, o))
#define OP_LDL(rt, b, o) op_ldl(c, rt, EA(b, o))
#define OP_LDR(rt, b, o) op_ldr(c, rt, EA(b, o))
#define OP_SDL(rt, b, o) op_sdl(c, rt, EA(b, o))
#define OP_SDR(rt, b, o) op_sdr(c, rt, EA(b, o))

/* ---------------------------------------------------------------- ALU */
#define OP_ADD(rd, rs, rt)   SET_GPR_U64(rd, SE32(GPR_U32(rs) + GPR_U32(rt)))
#define OP_ADDU(rd, rs, rt)  OP_ADD(rd, rs, rt)
#define OP_SUB(rd, rs, rt)   SET_GPR_U64(rd, SE32(GPR_U32(rs) - GPR_U32(rt)))
#define OP_SUBU(rd, rs, rt)  OP_SUB(rd, rs, rt)
#define OP_DADD(rd, rs, rt)  SET_GPR_U64(rd, GPR_U64(rs) + GPR_U64(rt))
#define OP_DADDU(rd, rs, rt) OP_DADD(rd, rs, rt)
#define OP_DSUB(rd, rs, rt)  SET_GPR_U64(rd, GPR_U64(rs) - GPR_U64(rt))
#define OP_DSUBU(rd, rs, rt) OP_DSUB(rd, rs, rt)
#define OP_AND(rd, rs, rt)   SET_GPR_U64(rd, GPR_U64(rs) & GPR_U64(rt))
#define OP_OR(rd, rs, rt)    SET_GPR_U64(rd, GPR_U64(rs) | GPR_U64(rt))
#define OP_XOR(rd, rs, rt)   SET_GPR_U64(rd, GPR_U64(rs) ^ GPR_U64(rt))
#define OP_NOR(rd, rs, rt)   SET_GPR_U64(rd, ~(GPR_U64(rs) | GPR_U64(rt)))
#define OP_SLT(rd, rs, rt)   SET_GPR_U64(rd, GPR_S64(rs) < GPR_S64(rt))
#define OP_SLTU(rd, rs, rt)  SET_GPR_U64(rd, GPR_U64(rs) < GPR_U64(rt))
#define OP_MOVZ(rd, rs, rt)  do { if (GPR_U64(rt) == 0) SET_GPR_U64(rd, GPR_U64(rs)); } while (0)
#define OP_MOVN(rd, rs, rt)  do { if (GPR_U64(rt) != 0) SET_GPR_U64(rd, GPR_U64(rs)); } while (0)

#define OP_SLL(rd, rt, sa)    SET_GPR_U64(rd, SE32(GPR_U32(rt) << (sa)))
#define OP_SRL(rd, rt, sa)    SET_GPR_U64(rd, SE32(GPR_U32(rt) >> (sa)))
#define OP_SRA(rd, rt, sa)    SET_GPR_U64(rd, SE32(GPR_S32(rt) >> (sa)))
#define OP_SLLV(rd, rt, rs)   SET_GPR_U64(rd, SE32(GPR_U32(rt) << (GPR_U32(rs) & 31)))
#define OP_SRLV(rd, rt, rs)   SET_GPR_U64(rd, SE32(GPR_U32(rt) >> (GPR_U32(rs) & 31)))
#define OP_SRAV(rd, rt, rs)   SET_GPR_U64(rd, SE32(GPR_S32(rt) >> (GPR_U32(rs) & 31)))
#define OP_DSLL(rd, rt, sa)   SET_GPR_U64(rd, GPR_U64(rt) << (sa))
#define OP_DSRL(rd, rt, sa)   SET_GPR_U64(rd, GPR_U64(rt) >> (sa))
#define OP_DSRA(rd, rt, sa)   SET_GPR_U64(rd, (u64)(GPR_S64(rt) >> (sa)))
#define OP_DSLL32(rd, rt, sa) SET_GPR_U64(rd, GPR_U64(rt) << ((sa) + 32))
#define OP_DSRL32(rd, rt, sa) SET_GPR_U64(rd, GPR_U64(rt) >> ((sa) + 32))
#define OP_DSRA32(rd, rt, sa) SET_GPR_U64(rd, (u64)(GPR_S64(rt) >> ((sa) + 32)))
#define OP_DSLLV(rd, rt, rs)  SET_GPR_U64(rd, GPR_U64(rt) << (GPR_U32(rs) & 63))
#define OP_DSRLV(rd, rt, rs)  SET_GPR_U64(rd, GPR_U64(rt) >> (GPR_U32(rs) & 63))
#define OP_DSRAV(rd, rt, rs)  SET_GPR_U64(rd, (u64)(GPR_S64(rt) >> (GPR_U32(rs) & 63)))

/* ---------------------------------------------------------------- mul/div (pipe 0 and 1) */
static inline void op_mult(R5900Context *c, int rd, s64 p, int pipe) {
    c->lo.ud[pipe] = SE32((u32)p); c->hi.ud[pipe] = SE32((u32)(p >> 32));
    SET_GPR_U64(rd, c->lo.ud[pipe]);
}
static inline void op_div(R5900Context *c, s32 a, s32 b, int pipe) {
    s32 q, r;
    if (b == 0) { q = a < 0 ? 1 : -1; r = a; }
    else if (a == INT32_MIN && b == -1) { q = INT32_MIN; r = 0; }
    else { q = a / b; r = a % b; }
    c->lo.ud[pipe] = SE32(q); c->hi.ud[pipe] = SE32(r);
}
static inline void op_divu(R5900Context *c, u32 a, u32 b, int pipe) {
    u32 q = b ? a / b : 0xffffffffu, r = b ? a % b : a;
    c->lo.ud[pipe] = SE32(q); c->hi.ud[pipe] = SE32(r);
}
static inline void op_madd(R5900Context *c, int rd, s64 p, int pipe) {
    u64 acc = ((u64)c->hi.uw[pipe * 2] << 32) | c->lo.uw[pipe * 2];
    op_mult(c, rd, (s64)(acc + (u64)p), pipe);
}
#define OP_MULT(rd, rs, rt)   op_mult(c, rd, (s64)GPR_S32(rs) * GPR_S32(rt), 0)
#define OP_MULTU(rd, rs, rt)  op_mult(c, rd, (s64)((u64)GPR_U32(rs) * GPR_U32(rt)), 0)
#define OP_MULT1(rd, rs, rt)  op_mult(c, rd, (s64)GPR_S32(rs) * GPR_S32(rt), 1)
#define OP_MULTU1(rd, rs, rt) op_mult(c, rd, (s64)((u64)GPR_U32(rs) * GPR_U32(rt)), 1)
#define OP_MADD(rd, rs, rt)   op_madd(c, rd, (s64)GPR_S32(rs) * GPR_S32(rt), 0)
#define OP_MADDU(rd, rs, rt)  op_madd(c, rd, (s64)((u64)GPR_U32(rs) * GPR_U32(rt)), 0)
#define OP_MADD1(rd, rs, rt)  op_madd(c, rd, (s64)GPR_S32(rs) * GPR_S32(rt), 1)
#define OP_MADDU1(rd, rs, rt) op_madd(c, rd, (s64)((u64)GPR_U32(rs) * GPR_U32(rt)), 1)
#define OP_DIV(rs, rt)   op_div(c, GPR_S32(rs), GPR_S32(rt), 0)
#define OP_DIVU(rs, rt)  op_divu(c, GPR_U32(rs), GPR_U32(rt), 0)
#define OP_DIV1(rd, rs, rt)  op_div(c, GPR_S32(rs), GPR_S32(rt), 1)
#define OP_DIVU1(rd, rs, rt) op_divu(c, GPR_U32(rs), GPR_U32(rt), 1)
#define OP_MFHI(rd)  SET_GPR_U64(rd, c->hi.ud[0])
#define OP_MFLO(rd)  SET_GPR_U64(rd, c->lo.ud[0])
#define OP_MTHI(rs)  (c->hi.ud[0] = GPR_U64(rs))
#define OP_MTLO(rs)  (c->lo.ud[0] = GPR_U64(rs))
#define OP_MFHI1(rd, rs, rt) SET_GPR_U64(rd, c->hi.ud[1])
#define OP_MFLO1(rd, rs, rt) SET_GPR_U64(rd, c->lo.ud[1])
#define OP_MTHI1(rd, rs, rt) (c->hi.ud[1] = GPR_U64(rs))
#define OP_MTLO1(rd, rs, rt) (c->lo.ud[1] = GPR_U64(rs))
#define OP_MFSA(rd)  SET_GPR_U64(rd, c->sa)
#define OP_MTSA(rs)  (c->sa = GPR_U32(rs))
#define OP_MTSAB(rs, imm) (c->sa = ((GPR_U32(rs) & 15) ^ ((imm) & 15)))
#define OP_MTSAH(rs, imm) (c->sa = (((GPR_U32(rs) & 7) ^ ((imm) & 7)) << 1))

/* ---------------------------------------------------------------- traps / system */
#define OP_SYSCALL(pc, code) ps2_syscall(c, pc)
#define OP_BREAK(pc, code)   ps2_break(c, pc, code)
#define TRAPIF(cond) do { if (cond) ps2_trap(c, 0); } while (0)
#define OP_TGE(rs, rt)  TRAPIF(GPR_S64(rs) >= GPR_S64(rt))
#define OP_TGEU(rs, rt) TRAPIF(GPR_U64(rs) >= GPR_U64(rt))
#define OP_TLT(rs, rt)  TRAPIF(GPR_S64(rs) < GPR_S64(rt))
#define OP_TLTU(rs, rt) TRAPIF(GPR_U64(rs) < GPR_U64(rt))
#define OP_TEQ(rs, rt)  TRAPIF(GPR_U64(rs) == GPR_U64(rt))
#define OP_TNE(rs, rt)  TRAPIF(GPR_U64(rs) != GPR_U64(rt))
#define OP_TGEI(rs, i)  TRAPIF(GPR_S64(rs) >= (s64)(i))
#define OP_TGEIU(rs, i) TRAPIF(GPR_U64(rs) >= (u64)(s64)(i))
#define OP_TLTI(rs, i)  TRAPIF(GPR_S64(rs) < (s64)(i))
#define OP_TLTIU(rs, i) TRAPIF(GPR_U64(rs) < (u64)(s64)(i))
#define OP_TEQI(rs, i)  TRAPIF(GPR_S64(rs) == (s64)(i))
#define OP_TNEI(rs, i)  TRAPIF(GPR_S64(rs) != (s64)(i))
#define OP_MFC0(rt, rd) SET_GPR_U64(rt, SE32(ps2_cop0_read(c, rd)))
#define OP_MTC0(rt, rd) ps2_cop0_write(c, rd, GPR_U32(rt))
#define OP_EI(pc, w)    ps2_cop0_op(c, pc, w)
#define OP_DI(pc, w)    ps2_cop0_op(c, pc, w)
#define OP_ERET(pc, w)  ps2_cop0_op(c, pc, w)
#define OP_TLBR(pc, w)  ps2_cop0_op(c, pc, w)
#define OP_TLBWI(pc, w) ps2_cop0_op(c, pc, w)
#define OP_TLBWR(pc, w) ps2_cop0_op(c, pc, w)
#define OP_TLBP(pc, w)  ps2_cop0_op(c, pc, w)

/* ---------------------------------------------------------------- COP1 (FPU)
 * The PS2 FPU has no Inf/NaN/denormals; define PS2_FPU_CLAMP to emulate that. */
static inline float fpu_clamp(float f) {
#ifdef PS2_FPU_CLAMP
    u32 u; memcpy(&u, &f, 4);
    if ((u & 0x7f800000u) == 0x7f800000u) u = (u & 0x80000000u) | 0x7f7fffffu;
    else if ((u & 0x7f800000u) == 0) u &= 0x80000000u;
    memcpy(&f, &u, 4);
#endif
    return f;
}
static inline float fpu_div(float a, float b) {
    if (b == 0.0f) { u32 u = 0x7f7fffffu; float m; memcpy(&m, &u, 4);
        return (signbit(a) ^ signbit(b)) ? -m : m; }
    return fpu_clamp(a / b);
}
#define OP_MFC1(rt, fs) SET_GPR_U64(rt, SE32(FPRU(fs)))
#define OP_MTC1(rt, fs) (FPRU(fs) = GPR_U32(rt))
#define OP_CFC1(rt, fs) SET_GPR_U64(rt, SE32((fs) == 31 ? ((c->fcr31 & ~0x00800000u) | (c->fcond ? 0x00800000u : 0)) : (fs) == 0 ? 0x2e30u : 0))
#define OP_CTC1(rt, fs) do { if ((fs) == 31) { c->fcr31 = GPR_U32(rt); c->fcond = (c->fcr31 >> 23) & 1; } } while (0)
#define OP_ADD_S(fd, fs, ft)   (FPR(fd) = fpu_clamp(FPR(fs) + FPR(ft)))
#define OP_SUB_S(fd, fs, ft)   (FPR(fd) = fpu_clamp(FPR(fs) - FPR(ft)))
#define OP_MUL_S(fd, fs, ft)   (FPR(fd) = fpu_clamp(FPR(fs) * FPR(ft)))
#define OP_DIV_S(fd, fs, ft)   (FPR(fd) = fpu_div(FPR(fs), FPR(ft)))
#define OP_SQRT_S(fd, fs, ft)  (FPR(fd) = sqrtf(fabsf(FPR(ft))))
#define OP_RSQRT_S(fd, fs, ft) (FPR(fd) = fpu_div(FPR(fs), sqrtf(fabsf(FPR(ft)))))
#define OP_ABS_S(fd, fs, ft)   (FPR(fd) = fabsf(FPR(fs)))
#define OP_MOV_S(fd, fs, ft)   (FPRU(fd) = FPRU(fs))
#define OP_NEG_S(fd, fs, ft)   (FPRU(fd) = FPRU(fs) ^ 0x80000000u)
#define OP_ADDA_S(fd, fs, ft)  (c->acc = fpu_clamp(FPR(fs) + FPR(ft)))
#define OP_SUBA_S(fd, fs, ft)  (c->acc = fpu_clamp(FPR(fs) - FPR(ft)))
#define OP_MULA_S(fd, fs, ft)  (c->acc = fpu_clamp(FPR(fs) * FPR(ft)))
#define OP_MADD_S(fd, fs, ft)  (FPR(fd) = fpu_clamp(c->acc + FPR(fs) * FPR(ft)))
#define OP_MSUB_S(fd, fs, ft)  (FPR(fd) = fpu_clamp(c->acc - FPR(fs) * FPR(ft)))
#define OP_MADDA_S(fd, fs, ft) (c->acc = fpu_clamp(c->acc + FPR(fs) * FPR(ft)))
#define OP_MSUBA_S(fd, fs, ft) (c->acc = fpu_clamp(c->acc - FPR(fs) * FPR(ft)))
#define OP_MAX_S(fd, fs, ft)   (FPR(fd) = FPR(fs) > FPR(ft) ? FPR(fs) : FPR(ft))
#define OP_MIN_S(fd, fs, ft)   (FPR(fd) = FPR(fs) < FPR(ft) ? FPR(fs) : FPR(ft))
#define OP_C_F_S(fd, fs, ft)   (c->fcond = 0)
#define OP_C_EQ_S(fd, fs, ft)  (c->fcond = FPR(fs) == FPR(ft))
#define OP_C_LT_S(fd, fs, ft)  (c->fcond = FPR(fs) < FPR(ft))
#define OP_C_LE_S(fd, fs, ft)  (c->fcond = FPR(fs) <= FPR(ft))
static inline s32 fpu_cvtw(float f) {
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    if (f != f) return INT32_MAX;
    return (s32)f;  /* truncates, like the EE */
}
#define OP_CVT_W_S(fd, fs, ft) (c->fpr[fd].s = fpu_cvtw(FPR(fs)))
#define OP_CVT_S_W(fd, fs, ft) (FPR(fd) = (float)c->fpr[fs].s)

/* ---------------------------------------------------------------- COP2 (VU0 macro mode) */
#define OP_QMFC2(rt, v) SET_GPR_Q(rt, c->vf[v])
#define OP_QMTC2(rt, v) do { if (v) c->vf[v] = GPR_Q(rt); } while (0)
#define OP_CFC2(rt, id)  SET_GPR_U64(rt, SE32(c->vi[id]))
#define OP_CTC2(rt, id)  vu0_ctc2(c, id, GPR_U32(rt))
#define OP_VU0(pc, w)    vu0_macro(c, pc, w)

/* ---------------------------------------------------------------- MMI (128-bit SIMD) */
#define LANES(n, expr) for (int i = 0; i < (n); i++) { expr; }

static inline s32 sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline s64 sat32(s64 v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v; }

#define MMI3(name, body) \
    static inline void mmi_##name(R5900Context *c, int rd, int rs_, int rt_) { \
        u128 _a = GPR_Q(rs_), _b = GPR_Q(rt_), _d; (void)_a; (void)_b; body; SET_GPR_Q(rd, _d); }
MMI3(paddw,  LANES(4, _d.uw[i] = _a.uw[i] + _b.uw[i]))
MMI3(psubw,  LANES(4, _d.uw[i] = _a.uw[i] - _b.uw[i]))
MMI3(paddh,  LANES(8, _d.uh[i] = _a.uh[i] + _b.uh[i]))
MMI3(psubh,  LANES(8, _d.uh[i] = _a.uh[i] - _b.uh[i]))
MMI3(paddb,  LANES(16, _d.ub[i] = _a.ub[i] + _b.ub[i]))
MMI3(psubb,  LANES(16, _d.ub[i] = _a.ub[i] - _b.ub[i]))
MMI3(pcgtw,  LANES(4, _d.uw[i] = _a.sw[i] > _b.sw[i] ? ~0u : 0))
MMI3(pcgth,  LANES(8, _d.uh[i] = _a.sh[i] > _b.sh[i] ? 0xffff : 0))
MMI3(pcgtb,  LANES(16, _d.ub[i] = _a.sb[i] > _b.sb[i] ? 0xff : 0))
MMI3(pceqw,  LANES(4, _d.uw[i] = _a.uw[i] == _b.uw[i] ? ~0u : 0))
MMI3(pceqh,  LANES(8, _d.uh[i] = _a.uh[i] == _b.uh[i] ? 0xffff : 0))
MMI3(pceqb,  LANES(16, _d.ub[i] = _a.ub[i] == _b.ub[i] ? 0xff : 0))
MMI3(pmaxw,  LANES(4, _d.sw[i] = _a.sw[i] > _b.sw[i] ? _a.sw[i] : _b.sw[i]))
MMI3(pminw,  LANES(4, _d.sw[i] = _a.sw[i] < _b.sw[i] ? _a.sw[i] : _b.sw[i]))
MMI3(pmaxh,  LANES(8, _d.sh[i] = _a.sh[i] > _b.sh[i] ? _a.sh[i] : _b.sh[i]))
MMI3(pminh,  LANES(8, _d.sh[i] = _a.sh[i] < _b.sh[i] ? _a.sh[i] : _b.sh[i]))
MMI3(paddsw, LANES(4, _d.sw[i] = (s32)sat32((s64)_a.sw[i] + _b.sw[i])))
MMI3(psubsw, LANES(4, _d.sw[i] = (s32)sat32((s64)_a.sw[i] - _b.sw[i])))
MMI3(paddsh, LANES(8, _d.sh[i] = (s16)sat16(_a.sh[i] + _b.sh[i])))
MMI3(psubsh, LANES(8, _d.sh[i] = (s16)sat16(_a.sh[i] - _b.sh[i])))
MMI3(paddsb, LANES(16, { int v = _a.sb[i] + _b.sb[i]; _d.sb[i] = v > 127 ? 127 : v < -128 ? -128 : v; }))
MMI3(psubsb, LANES(16, { int v = _a.sb[i] - _b.sb[i]; _d.sb[i] = v > 127 ? 127 : v < -128 ? -128 : v; }))
MMI3(padduw, LANES(4, { u64 v = (u64)_a.uw[i] + _b.uw[i]; _d.uw[i] = v > 0xffffffffu ? 0xffffffffu : (u32)v; }))
MMI3(psubuw, LANES(4, _d.uw[i] = _a.uw[i] > _b.uw[i] ? _a.uw[i] - _b.uw[i] : 0))
MMI3(padduh, LANES(8, { u32 v = (u32)_a.uh[i] + _b.uh[i]; _d.uh[i] = v > 0xffff ? 0xffff : v; }))
MMI3(psubuh, LANES(8, _d.uh[i] = _a.uh[i] > _b.uh[i] ? _a.uh[i] - _b.uh[i] : 0))
MMI3(paddub, LANES(16, { u32 v = (u32)_a.ub[i] + _b.ub[i]; _d.ub[i] = v > 0xff ? 0xff : v; }))
MMI3(psubub, LANES(16, _d.ub[i] = _a.ub[i] > _b.ub[i] ? _a.ub[i] - _b.ub[i] : 0))
MMI3(pabsw,  LANES(4, _d.sw[i] = _b.sw[i] == INT32_MIN ? INT32_MAX : _b.sw[i] < 0 ? -_b.sw[i] : _b.sw[i]))
MMI3(pabsh,  LANES(8, _d.sh[i] = _b.sh[i] == -32768 ? 32767 : _b.sh[i] < 0 ? -_b.sh[i] : _b.sh[i]))
MMI3(pextlw, _d.uw[0] = _b.uw[0]; _d.uw[1] = _a.uw[0]; _d.uw[2] = _b.uw[1]; _d.uw[3] = _a.uw[1])
MMI3(pextuw, _d.uw[0] = _b.uw[2]; _d.uw[1] = _a.uw[2]; _d.uw[2] = _b.uw[3]; _d.uw[3] = _a.uw[3])
MMI3(pextlh, LANES(4, (_d.uh[2 * i] = _b.uh[i], _d.uh[2 * i + 1] = _a.uh[i])))
MMI3(pextuh, LANES(4, (_d.uh[2 * i] = _b.uh[i + 4], _d.uh[2 * i + 1] = _a.uh[i + 4])))
MMI3(pextlb, LANES(8, (_d.ub[2 * i] = _b.ub[i], _d.ub[2 * i + 1] = _a.ub[i])))
MMI3(pextub, LANES(8, (_d.ub[2 * i] = _b.ub[i + 8], _d.ub[2 * i + 1] = _a.ub[i + 8])))
MMI3(ppacw,  _d.uw[0] = _b.uw[0]; _d.uw[1] = _b.uw[2]; _d.uw[2] = _a.uw[0]; _d.uw[3] = _a.uw[2])
MMI3(ppach,  LANES(4, (_d.uh[i] = _b.uh[2 * i], _d.uh[i + 4] = _a.uh[2 * i])))
MMI3(ppacb,  LANES(8, (_d.ub[i] = _b.ub[2 * i], _d.ub[i + 8] = _a.ub[2 * i])))
MMI3(pext5,  LANES(4, { u32 v = _b.uw[i]; _d.uw[i] = ((v & 0x1f) << 3) | ((v & 0x3e0) << 6) | ((v & 0x7c00) << 9) | ((v & 0x8000) << 16); }))
MMI3(ppac5,  LANES(4, { u32 v = _b.uw[i]; _d.uw[i] = ((v >> 3) & 0x1f) | ((v >> 6) & 0x3e0) | ((v >> 9) & 0x7c00) | ((v >> 16) & 0x8000); }))
MMI3(pcpyld, _d.ud[0] = _b.ud[0]; _d.ud[1] = _a.ud[0])
MMI3(pcpyud, _d.ud[0] = _a.ud[1]; _d.ud[1] = _b.ud[1])
MMI3(pcpyh,  LANES(4, (_d.uh[i] = _b.uh[0], _d.uh[i + 4] = _b.uh[4])))
MMI3(pand,   _d.ud[0] = _a.ud[0] & _b.ud[0]; _d.ud[1] = _a.ud[1] & _b.ud[1])
MMI3(por,    _d.ud[0] = _a.ud[0] | _b.ud[0]; _d.ud[1] = _a.ud[1] | _b.ud[1])
MMI3(pxor,   _d.ud[0] = _a.ud[0] ^ _b.ud[0]; _d.ud[1] = _a.ud[1] ^ _b.ud[1])
MMI3(pnor,   _d.ud[0] = ~(_a.ud[0] | _b.ud[0]); _d.ud[1] = ~(_a.ud[1] | _b.ud[1]))
MMI3(pinth,  LANES(4, (_d.uh[2 * i] = _b.uh[i], _d.uh[2 * i + 1] = _a.uh[i + 4])))
MMI3(pinteh, LANES(4, (_d.uh[2 * i] = _b.uh[2 * i], _d.uh[2 * i + 1] = _a.uh[2 * i])))
MMI3(pexeh,  _d = _b; _d.uh[0] = _b.uh[2]; _d.uh[2] = _b.uh[0]; _d.uh[4] = _b.uh[6]; _d.uh[6] = _b.uh[4])
MMI3(prevh,  LANES(4, (_d.uh[i] = _b.uh[3 - i], _d.uh[i + 4] = _b.uh[7 - i])))
MMI3(pexch,  _d = _b; _d.uh[1] = _b.uh[2]; _d.uh[2] = _b.uh[1]; _d.uh[5] = _b.uh[6]; _d.uh[6] = _b.uh[5])
MMI3(pexew,  _d = _b; _d.uw[0] = _b.uw[2]; _d.uw[2] = _b.uw[0])
MMI3(pexcw,  _d = _b; _d.uw[1] = _b.uw[2]; _d.uw[2] = _b.uw[1])
MMI3(prot3w, _d.uw[0] = _b.uw[1]; _d.uw[1] = _b.uw[2]; _d.uw[2] = _b.uw[0]; _d.uw[3] = _b.uw[3])
MMI3(psllvw, LANES(2, _d.sd[i] = (s32)(_b.uw[2 * i] << (_a.uw[2 * i] & 31))))
MMI3(psrlvw, LANES(2, _d.sd[i] = (s32)(_b.uw[2 * i] >> (_a.uw[2 * i] & 31))))
MMI3(psravw, LANES(2, _d.sd[i] = _b.sw[2 * i] >> (_a.uw[2 * i] & 31)))
MMI3(plzcw,  LANES(2, { u32 v = _a.uw[i]; if (v & 0x80000000u) v = ~v; int n = 0;
                         while (n < 32 && !(v & (0x80000000u >> n))) n++; _d.uw[i] = n ? n - 1 : 0; }); _d.ud[1] = c->r[rd].ud[1])
MMI3(qfsrv,  { unsigned s = (c->sa & 15) * 8;
               if (s == 0) _d = _b;
               else if (s < 64) { _d.ud[0] = (_b.ud[0] >> s) | (_b.ud[1] << (64 - s)); _d.ud[1] = (_b.ud[1] >> s) | (_a.ud[0] << (64 - s)); }
               else if (s == 64) { _d.ud[0] = _b.ud[1]; _d.ud[1] = _a.ud[0]; }
               else { unsigned t = s - 64; _d.ud[0] = (_b.ud[1] >> t) | (_a.ud[0] << (64 - t)); _d.ud[1] = (_a.ud[0] >> t) | (_a.ud[1] << (64 - t)); } })
MMI3(pmulth, { s32 p[8]; LANES(8, p[i] = (s32)_a.sh[i] * _b.sh[i]);
               c->lo.sw[0] = p[0]; c->lo.sw[1] = p[1]; c->hi.sw[0] = p[2]; c->hi.sw[1] = p[3];
               c->lo.sw[2] = p[4]; c->lo.sw[3] = p[5]; c->hi.sw[2] = p[6]; c->hi.sw[3] = p[7];
               _d.sw[0] = p[0]; _d.sw[1] = p[2]; _d.sw[2] = p[4]; _d.sw[3] = p[6]; })
MMI3(pmultw, { LANES(2, { s64 p = (s64)_a.sw[2 * i] * _b.sw[2 * i];
               c->lo.sd[i] = (s32)p; c->hi.sd[i] = (s32)(p >> 32); _d.sd[i] = p; }) })
MMI3(pmultuw, { LANES(2, { u64 p = (u64)_a.uw[2 * i] * _b.uw[2 * i];
               c->lo.sd[i] = (s32)p; c->hi.sd[i] = (s32)(p >> 32); _d.ud[i] = p; }) })
MMI3(pmfhi,  _d = c->hi)
MMI3(pmflo,  _d = c->lo)
#define OP_PMTHI(rd, rs, rt) (c->hi = GPR_Q(rs))
#define OP_PMTLO(rd, rs, rt) (c->lo = GPR_Q(rs))

static inline void mmi_pmfhl(R5900Context *c, int rd, int rs, int fmt) {
    u128 d, lo = c->lo, hi = c->hi;
    switch (fmt) {
    case 0: d.uw[0] = lo.uw[0]; d.uw[1] = hi.uw[0]; d.uw[2] = lo.uw[2]; d.uw[3] = hi.uw[2]; break;
    case 1: d.uw[0] = lo.uw[1]; d.uw[1] = hi.uw[1]; d.uw[2] = lo.uw[3]; d.uw[3] = hi.uw[3]; break;
    case 2: for (int i = 0; i < 2; i++) d.sd[i] = sat32((s64)(((u64)hi.uw[2 * i] << 32) | lo.uw[2 * i])); break;
    case 3: d.uh[0] = lo.uh[0]; d.uh[1] = lo.uh[2]; d.uh[2] = hi.uh[0]; d.uh[3] = hi.uh[2];
            d.uh[4] = lo.uh[4]; d.uh[5] = lo.uh[6]; d.uh[6] = hi.uh[4]; d.uh[7] = hi.uh[6]; break;
    default: d.sh[0] = sat16(lo.sw[0]); d.sh[1] = sat16(lo.sw[1]); d.sh[2] = sat16(hi.sw[0]); d.sh[3] = sat16(hi.sw[1]);
             d.sh[4] = sat16(lo.sw[2]); d.sh[5] = sat16(lo.sw[3]); d.sh[6] = sat16(hi.sw[2]); d.sh[7] = sat16(hi.sw[3]); break;
    }
    SET_GPR_Q(rd, d);
}
static inline void mmi_pmthl(R5900Context *c, int rd, int rs, int fmt) {
    (void)rd;
    if (fmt == 0) { u128 s = GPR_Q(rs);
        c->lo.uw[0] = s.uw[0]; c->hi.uw[0] = s.uw[1]; c->lo.uw[2] = s.uw[2]; c->hi.uw[2] = s.uw[3]; }
    else ps2_unimpl(c, 0, fmt, "pmthl");
}
#define OP_PMFHL(rd, rs, sa) mmi_pmfhl(c, rd, rs, sa)
#define OP_PMTHL(rd, rs, sa) mmi_pmthl(c, rd, rs, sa)

#define MMISH(name, n, field, T, expr) \
    static inline void mmi_##name(R5900Context *c, int rd, int rt, int sa) { \
        u128 _b = GPR_Q(rt), _d; for (int i = 0; i < n; i++) { T x = _b.field[i]; _d.field[i] = (expr); } SET_GPR_Q(rd, _d); }
MMISH(psllh, 8, uh, u16, (u16)(x << (sa & 15)))
MMISH(psrlh, 8, uh, u16, (u16)(x >> (sa & 15)))
MMISH(psrah, 8, sh, s16, (s16)(x >> (sa & 15)))
MMISH(psllw, 4, uw, u32, x << sa)
MMISH(psrlw, 4, uw, u32, x >> sa)
MMISH(psraw, 4, sw, s32, x >> sa)
#define OP_PSLLH(rd, rt, sa) mmi_psllh(c, rd, rt, sa)
#define OP_PSRLH(rd, rt, sa) mmi_psrlh(c, rd, rt, sa)
#define OP_PSRAH(rd, rt, sa) mmi_psrah(c, rd, rt, sa)
#define OP_PSLLW(rd, rt, sa) mmi_psllw(c, rd, rt, sa)
#define OP_PSRLW(rd, rt, sa) mmi_psrlw(c, rd, rt, sa)
#define OP_PSRAW(rd, rt, sa) mmi_psraw(c, rd, rt, sa)

#define OP_PADDW(rd, rs, rt)  mmi_paddw(c, rd, rs, rt)
#define OP_PSUBW(rd, rs, rt)  mmi_psubw(c, rd, rs, rt)
#define OP_PADDH(rd, rs, rt)  mmi_paddh(c, rd, rs, rt)
#define OP_PSUBH(rd, rs, rt)  mmi_psubh(c, rd, rs, rt)
#define OP_PADDB(rd, rs, rt)  mmi_paddb(c, rd, rs, rt)
#define OP_PSUBB(rd, rs, rt)  mmi_psubb(c, rd, rs, rt)
#define OP_PCGTW(rd, rs, rt)  mmi_pcgtw(c, rd, rs, rt)
#define OP_PCGTH(rd, rs, rt)  mmi_pcgth(c, rd, rs, rt)
#define OP_PCGTB(rd, rs, rt)  mmi_pcgtb(c, rd, rs, rt)
#define OP_PCEQW(rd, rs, rt)  mmi_pceqw(c, rd, rs, rt)
#define OP_PCEQH(rd, rs, rt)  mmi_pceqh(c, rd, rs, rt)
#define OP_PCEQB(rd, rs, rt)  mmi_pceqb(c, rd, rs, rt)
#define OP_PMAXW(rd, rs, rt)  mmi_pmaxw(c, rd, rs, rt)
#define OP_PMINW(rd, rs, rt)  mmi_pminw(c, rd, rs, rt)
#define OP_PMAXH(rd, rs, rt)  mmi_pmaxh(c, rd, rs, rt)
#define OP_PMINH(rd, rs, rt)  mmi_pminh(c, rd, rs, rt)
#define OP_PADDSW(rd, rs, rt) mmi_paddsw(c, rd, rs, rt)
#define OP_PSUBSW(rd, rs, rt) mmi_psubsw(c, rd, rs, rt)
#define OP_PADDSH(rd, rs, rt) mmi_paddsh(c, rd, rs, rt)
#define OP_PSUBSH(rd, rs, rt) mmi_psubsh(c, rd, rs, rt)
#define OP_PADDSB(rd, rs, rt) mmi_paddsb(c, rd, rs, rt)
#define OP_PSUBSB(rd, rs, rt) mmi_psubsb(c, rd, rs, rt)
#define OP_PADDUW(rd, rs, rt) mmi_padduw(c, rd, rs, rt)
#define OP_PSUBUW(rd, rs, rt) mmi_psubuw(c, rd, rs, rt)
#define OP_PADDUH(rd, rs, rt) mmi_padduh(c, rd, rs, rt)
#define OP_PSUBUH(rd, rs, rt) mmi_psubuh(c, rd, rs, rt)
#define OP_PADDUB(rd, rs, rt) mmi_paddub(c, rd, rs, rt)
#define OP_PSUBUB(rd, rs, rt) mmi_psubub(c, rd, rs, rt)
#define OP_PABSW(rd, rs, rt)  mmi_pabsw(c, rd, rs, rt)
#define OP_PABSH(rd, rs, rt)  mmi_pabsh(c, rd, rs, rt)
#define OP_PEXTLW(rd, rs, rt) mmi_pextlw(c, rd, rs, rt)
#define OP_PEXTUW(rd, rs, rt) mmi_pextuw(c, rd, rs, rt)
#define OP_PEXTLH(rd, rs, rt) mmi_pextlh(c, rd, rs, rt)
#define OP_PEXTUH(rd, rs, rt) mmi_pextuh(c, rd, rs, rt)
#define OP_PEXTLB(rd, rs, rt) mmi_pextlb(c, rd, rs, rt)
#define OP_PEXTUB(rd, rs, rt) mmi_pextub(c, rd, rs, rt)
#define OP_PPACW(rd, rs, rt)  mmi_ppacw(c, rd, rs, rt)
#define OP_PPACH(rd, rs, rt)  mmi_ppach(c, rd, rs, rt)
#define OP_PPACB(rd, rs, rt)  mmi_ppacb(c, rd, rs, rt)
#define OP_PEXT5(rd, rs, rt)  mmi_pext5(c, rd, rs, rt)
#define OP_PPAC5(rd, rs, rt)  mmi_ppac5(c, rd, rs, rt)
#define OP_PCPYLD(rd, rs, rt) mmi_pcpyld(c, rd, rs, rt)
#define OP_PCPYUD(rd, rs, rt) mmi_pcpyud(c, rd, rs, rt)
#define OP_PCPYH(rd, rs, rt)  mmi_pcpyh(c, rd, rs, rt)
#define OP_PAND(rd, rs, rt)   mmi_pand(c, rd, rs, rt)
#define OP_POR(rd, rs, rt)    mmi_por(c, rd, rs, rt)
#define OP_PXOR(rd, rs, rt)   mmi_pxor(c, rd, rs, rt)
#define OP_PNOR(rd, rs, rt)   mmi_pnor(c, rd, rs, rt)
#define OP_PINTH(rd, rs, rt)  mmi_pinth(c, rd, rs, rt)
#define OP_PINTEH(rd, rs, rt) mmi_pinteh(c, rd, rs, rt)
#define OP_PEXEH(rd, rs, rt)  mmi_pexeh(c, rd, rs, rt)
#define OP_PREVH(rd, rs, rt)  mmi_prevh(c, rd, rs, rt)
#define OP_PEXCH(rd, rs, rt)  mmi_pexch(c, rd, rs, rt)
#define OP_PEXEW(rd, rs, rt)  mmi_pexew(c, rd, rs, rt)
#define OP_PEXCW(rd, rs, rt)  mmi_pexcw(c, rd, rs, rt)
#define OP_PROT3W(rd, rs, rt) mmi_prot3w(c, rd, rs, rt)
#define OP_PSLLVW(rd, rs, rt) mmi_psllvw(c, rd, rs, rt)
#define OP_PSRLVW(rd, rs, rt) mmi_psrlvw(c, rd, rs, rt)
#define OP_PSRAVW(rd, rs, rt) mmi_psravw(c, rd, rs, rt)
#define OP_PLZCW(rd, rs, rt)  mmi_plzcw(c, rd, rs, rt)
#define OP_QFSRV(rd, rs, rt)  mmi_qfsrv(c, rd, rs, rt)
#define OP_PMULTH(rd, rs, rt) mmi_pmulth(c, rd, rs, rt)
#define OP_PMULTW(rd, rs, rt) mmi_pmultw(c, rd, rs, rt)
#define OP_PMULTUW(rd, rs, rt) mmi_pmultuw(c, rd, rs, rt)
#define OP_PMFHI(rd, rs, rt)  mmi_pmfhi(c, rd, rs, rt)
#define OP_PMFLO(rd, rs, rt)  mmi_pmflo(c, rd, rs, rt)

/* Not used by this game (see out/report.json); trap loudly if ever reached. */
#define MMI_TODO(n) ps2_unimpl(c, 0, 0, n)
#define OP_PADSBH(rd, rs, rt)  MMI_TODO("padsbh")
#define OP_PMADDW(rd, rs, rt)  MMI_TODO("pmaddw")
#define OP_PMSUBW(rd, rs, rt)  MMI_TODO("pmsubw")
#define OP_PDIVW(rd, rs, rt)   MMI_TODO("pdivw")
#define OP_PMADDH(rd, rs, rt)  MMI_TODO("pmaddh")
#define OP_PHMADH(rd, rs, rt)  MMI_TODO("phmadh")
#define OP_PMSUBH(rd, rs, rt)  MMI_TODO("pmsubh")
#define OP_PHMSBH(rd, rs, rt)  MMI_TODO("phmsbh")
#define OP_PDIVBW(rd, rs, rt)  MMI_TODO("pdivbw")
#define OP_PMADDUW(rd, rs, rt) MMI_TODO("pmadduw")
#define OP_PDIVUW(rd, rs, rt)  MMI_TODO("pdivuw")
