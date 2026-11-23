/* PS2 EE recompiled-code runtime: CPU context, memory, dispatch. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

typedef union {
    u64 ud[2]; s64 sd[2];
    u32 uw[4]; s32 sw[4];
    u16 uh[8]; s16 sh[8];
    u8  ub[16]; s8 sb[16];
    float f[4];
} u128;

typedef union { float f; u32 u; s32 s; } fpr_t;

typedef struct R5900Context {
    u128 r[32];          /* GPRs, 128-bit (MMI) */
    u128 hi, lo;         /* HI/LO; upper halves are HI1/LO1 */
    u32  sa;             /* shift amount register (QFSRV) */
    fpr_t fpr[32];       /* COP1 */
    float acc;           /* COP1 accumulator */
    u32  fcr31;
    int  fcond;
    u32  cop0[32];
    u128 vf[32];         /* VU0 float regs (vf0 = 0,0,0,1) */
    u32  vi[32];         /* VU0 integer + control regs (CFC2/CTC2 ids) */
    u128 vacc;           /* VU0 ACC */
    u8  *ram;            /* 32 MB EE RAM */
    u8  *spr;            /* 16 KB scratchpad */
    u32  pc;             /* only meaningful at dispatch boundaries */
} R5900Context;

typedef void (*PS2Func)(R5900Context *c);
typedef struct { u32 start, end; PS2Func fn; } PS2FuncEntry;

extern const PS2FuncEntry ps2_func_table[];
extern const unsigned ps2_func_count;
extern const unsigned ps2_entry_point;

/* ---- interrupt delivery points: emitted on backward branches (kernel.c) */
extern int ps2_poll_budget;
extern volatile u32 ps2_last_loop;   /* last backward-branch target: read by the watchdog */
void ps2_poll(R5900Context *c);
#define PS2_POLL(pc) do { ps2_last_loop = (pc); if (--ps2_poll_budget < 0) ps2_poll(c); } while (0)
void ps2_raise_dmac(R5900Context *c, int ch);

/* ---- dispatch / exceptions (runtime.c) */
void ps2_call(R5900Context *c, u32 addr);   /* indirect call (jalr) */
void ps2_jump(R5900Context *c, u32 addr);   /* indirect tail jump (jr / fall-through) */
void ps2_syscall(R5900Context *c, u32 pc);
void ps2_break(R5900Context *c, u32 pc, u32 code);
void ps2_trap(R5900Context *c, u32 pc);
void ps2_invalid(R5900Context *c, u32 pc, u32 word);
void ps2_unimpl(R5900Context *c, u32 pc, u32 word, const char *what);
void ps2_cop0_op(R5900Context *c, u32 pc, u32 word);  /* EI/DI/ERET/TLB* */
u32  ps2_cop0_read(R5900Context *c, int reg);
void ps2_cop0_write(R5900Context *c, int reg, u32 v);
int  ps2_cop0_cond(R5900Context *c);
void vu0_macro(R5900Context *c, u32 pc, u32 word);    /* COP2 special ops (vu0.c) */
void vu0_ctc2(R5900Context *c, int reg, u32 v);
int  vu0_cond(R5900Context *c);

/* ---- memory (memory.c) */
u32  ps2_io_read32(u32 addr);  void ps2_io_write32(u32 addr, u32 v);
u64  ps2_io_read64(u32 addr);  void ps2_io_write64(u32 addr, u64 v);
u128 ps2_io_read128(u32 addr); void ps2_io_write128(u32 addr, u128 v);
u16  ps2_io_read16(u32 addr);  void ps2_io_write16(u32 addr, u16 v);
u8   ps2_io_read8(u32 addr);   void ps2_io_write8(u32 addr, u8 v);

extern u8 *ps2_ram, *ps2_spr;
extern u8 vu0_code[4096], vu0_data[4096], vu1_code[16384], vu1_data[16384];
void vu0_copy_state(R5900Context *dst, const R5900Context *src);   /* VU0 is shared hardware */

/* Returns a host pointer for RAM / scratchpad / VU memory, or NULL for MMIO. */
static inline u8 *ps2_ptr(u32 a) {
    if (a < 0x02000000) return ps2_ram + a;   /* the common case: main RAM, cached segment */
    if ((a >> 28) == 0x7) return ps2_spr + (a & 0x3fff);
    u32 p = a & 0x1fffffff;
    if (p < 0x02000000) return ps2_ram + p;
    if ((p >> 16) == 0x1100) {
        extern volatile int g_vu_code_dirty[2];   /* translated VU pages must be re-resolved */
        u32 o = p & 0xffff;
        if (o < 0x4000) { g_vu_code_dirty[0] = 1; return vu0_code + (o & 0xfff); }
        if (o < 0x8000) return vu0_data + (o & 0xfff);
        if (o < 0xc000) { g_vu_code_dirty[1] = 1; return vu1_code + (o & 0x3fff); }
        return vu1_data + (o & 0x3fff);
    }
    if (a >= 0x20000000 && a < 0x22000000) return ps2_ram + (a & 0x01ffffff);   /* uncached mirror */
    if (a >= 0x30000000 && a < 0x32000000) return ps2_ram + (a & 0x01ffffff);   /* uncached-accel */
    return NULL;
}

/* PS2_WATCH=addr[,addr2]: log stores that touch those 16-byte blocks (diagnostic; see runtime.c) */
extern u32 g_watch_qw[2];
void ps2_watch_hit(u32 a, const void *val, int size);   /* names its caller (the recompiled function) */
#define DEF_MEM(bits, T)                                                         \
static inline T MEM_R##bits(u32 a) { u8 *p = ps2_ptr(a); T v;                    \
    if (p) { memcpy(&v, p, sizeof v); return v; } return ps2_io_read##bits(a); }  \
static inline void MEM_W##bits(u32 a, T v) { u8 *p = ps2_ptr(a);                 \
    if (((a & 0x1FFFFFF0u) == g_watch_qw[0] || (a & 0x1FFFFFF0u) == g_watch_qw[1]) && g_watch_qw[0]) \
        ps2_watch_hit(a, &v, sizeof v);                                          \
    if (p) memcpy(p, &v, sizeof v); else ps2_io_write##bits(a, v); }
DEF_MEM(8, u8) DEF_MEM(16, u16) DEF_MEM(32, u32) DEF_MEM(64, u64) DEF_MEM(128, u128)
#undef DEF_MEM
