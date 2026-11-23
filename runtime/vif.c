/* VIF0 / VIF1 command processors. Data arrives as a 32-bit word stream (DMA ch0/ch1, FIFOs,
 * and the upper half of DMA tags when TTE is set). */
#include <stdio.h>
#include <stdlib.h>
#include "gif.h"

extern u8 vu0_code[4096], vu0_data[4096], vu1_code[16384], vu1_data[16384];

typedef struct {
    int unit;
    /* current command */
    u32 code;           /* 0 = waiting for a command */
    u32 need;           /* words still needed for the current command */
    u32 got;
    u32 buf[0x10000 / 4 + 8];
    /* registers */
    u32 cl, wl, mode, mask, row[4], col[4];
    u32 base, ofst, tops, top, dbf, itop, itops, mark;
    u64 stat_unpack, stat_direct, stat_mscal, stat_mpg;
} Vif;
static Vif g_vif[2] = {{0}, {1}};

/* DIRECT data is staged and handed to the GIF queue in batches */
static u8 g_direct_stage[64 * 1024 * 16];
static u32 g_direct_n;
static void direct_flush(void) { if (g_direct_n) { gif_feed(2, g_direct_stage, g_direct_n, 0); g_direct_n = 0; } }
static void direct_push(const u32 *qw) {
    memcpy(g_direct_stage + g_direct_n * 16, qw, 16);
    if (++g_direct_n == 64 * 1024) direct_flush();
}

static u32 vu_mem_qw(int unit) { return unit ? 1024 : 256; }   /* data memory size in qwords */
static u8 *vu_mem(int unit) { return unit ? vu1_data : vu0_data; }

/* ---- UNPACK */
static u32 unpack_elem_bytes(u32 vn, u32 vl) {
    static const u8 sz[4][4] = {{4, 2, 1, 0}, {8, 4, 2, 0}, {12, 6, 3, 0}, {16, 8, 4, 2}};
    return sz[vn][vl];
}

static u32 unpack_data_count(Vif *v, u32 num) {
    if (v->cl >= v->wl || v->wl == 0) return num;
    u32 full = num / v->wl, rest = num % v->wl;
    return full * v->cl + (rest < v->cl ? rest : v->cl);
}

static void do_unpack(Vif *v) {
    u32 cmd = (v->code >> 24) & 0x7F, num = (v->code >> 16) & 0xFF, imm = v->code & 0xFFFF;
    if (!num) num = 256;
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3, msk = (cmd >> 4) & 1;
    int usn = (imm >> 14) & 1, flg = (imm >> 15) & 1;
    u32 addr = imm & 0x3FF;
    if (v->unit == 1 && flg) addr += v->tops;
    u32 esz = unpack_elem_bytes(vn, vl);
    const u8 *src = (const u8 *)v->buf;
    u8 *mem = vu_mem(v->unit);
    u32 memqw = vu_mem_qw(v->unit);
    u32 cl = v->cl ? v->cl : 1, wl = v->wl ? v->wl : 1;
    u32 cycle = 0, qaddr = addr, di = 0;
    if ((!msk || v->mask == 0) && v->mode == 0 && wl <= cl && !(vn == 3 && vl == 3)) {
        /* fast path: every element comes from the stream, written straight (no mask, no ROW offset, no fill) */
        u32 skip = cl - wl;
        if (vn == 3 && vl == 0 && skip == 0 && (qaddr % memqw) + num <= memqw) {   /* V4-32, contiguous: one copy */
            memcpy(mem + (qaddr % memqw) * 16, src, (size_t)num * 16);
            v->stat_unpack++;
            return;
        }
        /* one loop per format, with fixed-size element copies the compiler can inline */
#define UNPACK_LOOP(BODY) for (u32 n = 0; n < num; n++) {                         \
            u32 *dst = (u32 *)(mem + (qaddr % memqw) * 16); const u8 *e = src + n * esz; \
            BODY                                                                  \
            qaddr++; if (++cycle >= wl) { cycle = 0; qaddr += skip; } }
        switch (vl * 4 + vn) {
        case 0 * 4 + 0: UNPACK_LOOP({ u32 x; memcpy(&x, e, 4); dst[0] = dst[1] = dst[2] = dst[3] = x; }) break;
        case 0 * 4 + 1: UNPACK_LOOP(memcpy(dst, e, 8);) break;
        case 0 * 4 + 2: UNPACK_LOOP(memcpy(dst, e, 12);) break;
        case 0 * 4 + 3: UNPACK_LOOP(memcpy(dst, e, 16);) break;
        default: {
            u32 ncomp = vn == 0 ? 4 : vn + 1;
            if (vl == 1) {
                if (usn) UNPACK_LOOP({ u16 h[4]; memcpy(h, e, 8); if (vn == 0) h[1] = h[2] = h[3] = h[0];
                                       for (u32 k = 0; k < 4; k++) if (k < ncomp) dst[k] = h[k]; })
                else     UNPACK_LOOP({ s16 h[4]; memcpy(h, e, 8); if (vn == 0) h[1] = h[2] = h[3] = h[0];
                                       for (u32 k = 0; k < 4; k++) if (k < ncomp) dst[k] = (u32)(s32)h[k]; })
            } else {
                if (usn) UNPACK_LOOP({ u8 b[4]; memcpy(b, e, 4); if (vn == 0) b[1] = b[2] = b[3] = b[0];
                                       for (u32 k = 0; k < 4; k++) if (k < ncomp) dst[k] = b[k]; })
                else     UNPACK_LOOP({ s8 b[4]; memcpy(b, e, 4); if (vn == 0) b[1] = b[2] = b[3] = b[0];
                                       for (u32 k = 0; k < 4; k++) if (k < ncomp) dst[k] = (u32)(s32)b[k]; })
            }
        } }
#undef UNPACK_LOOP
        v->stat_unpack++;
        return;
    }
    for (u32 n = 0; n < num; n++) {
        int filling = (wl > cl) && (cycle >= cl);
        s32 in[4] = {0, 0, 0, 0};
        if (!filling) {
            const u8 *e = src + di * esz; di++;
            if (vn == 3 && vl == 3) {   /* V4-5 (RGBA5551) */
                u16 c; memcpy(&c, e, 2);
                in[0] = (c & 0x1F) << 3; in[1] = ((c >> 5) & 0x1F) << 3; in[2] = ((c >> 10) & 0x1F) << 3; in[3] = (c >> 15) << 7;
            } else {
                u32 comps = vn + 1;
                for (u32 k = 0; k < comps; k++) {
                    if (vl == 0) { s32 x; memcpy(&x, e + k * 4, 4); in[k] = x; }
                    else if (vl == 1) { u16 x; memcpy(&x, e + k * 2, 2); in[k] = usn ? (s32)x : (s32)(s16)x; }
                    else { u8 x = e[k]; in[k] = usn ? (s32)x : (s32)(s8)x; }
                }
                if (vn == 0) in[1] = in[2] = in[3] = in[0];
            }
        }
        u32 *dst = (u32 *)(mem + (qaddr % memqw) * 16);
        u32 mcyc = cycle < 3 ? cycle : 3;
        int ncomp = (vn == 3 || vn == 0) ? 4 : (int)vn + 1;   /* V2/V3 leave the other fields alone */
        for (int k = 0; k < 4; k++) {
            u32 m = msk ? (v->mask >> (mcyc * 8 + k * 2)) & 3 : 0;
            if (filling && m == 0) m = 1;   /* fill writes use ROW by default */
            if (k >= ncomp && !filling && m == 0) continue;
            u32 val;
            switch (m) {
            case 0:
                val = (u32)in[k];
                if (v->mode == 1) val += v->row[k];
                else if (v->mode == 2) { val += v->row[k]; v->row[k] = val; }
                break;
            case 1: val = v->row[k]; break;
            case 2: val = v->col[mcyc]; break;
            default: continue;              /* write protect */
            }
            dst[k] = val;
        }
        qaddr++;
        if (++cycle >= wl) { cycle = 0; if (cl > wl) qaddr += cl - wl; }
    }
    v->stat_unpack++;
}

/* ---- command dispatch */
static void cmd_done(Vif *v) { v->code = 0; v->need = 0; v->got = 0; }

static void mscal(Vif *v, u32 addr) {
    v->stat_mscal++;
    direct_flush();
    if (v->unit == 1) {
        v->top = v->tops; v->itop = v->itops;
        v->dbf ^= 1; v->tops = v->base + (v->dbf ? v->ofst : 0);
        vu1_start(addr);
    } else vu0_micro_start(addr);
}

static void start_cmd(Vif *v, u32 code) {
    u32 cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
    { static int tr = -1, n; if (tr < 0) tr = getenv("PS2_VIFTRACE") != NULL;
      extern int g_frame_no_pub;
      if (tr && v->unit == 1 && g_frame_no_pub > 1500 && n < 400 && cmd != 0) { n++;
          fprintf(stderr, "[vif1] f%d cmd %02x imm %04x num %u\n", g_frame_no_pub, cmd, imm, num); } }
    v->code = code; v->got = 0; v->need = 0;
    switch (cmd) {
    case 0x00: break;                                           /* NOP */
    case 0x01: v->cl = imm & 0xFF; v->wl = imm >> 8; break;     /* STCYCL */
    case 0x02: v->ofst = imm & 0x3FF; v->dbf = 0; v->tops = v->base; break;  /* OFFSET */
    case 0x03: v->base = imm & 0x3FF; break;                    /* BASE */
    case 0x04: v->itops = imm & 0x3FF; break;                   /* ITOP */
    case 0x05: v->mode = imm & 3; break;                        /* STMOD */
    case 0x06: direct_flush(); gif_path3_vifmask((imm >> 15) & 1); break;   /* MSKPATH3 */
    case 0x07: v->mark = imm; break;                            /* MARK */
    case 0x10: case 0x11: break;                                /* FLUSHE / FLUSH */
    case 0x13: direct_flush(); gif_path3_flush(); break;        /* FLUSHA: wait for PATH3 */
    case 0x14: mscal(v, imm * 8); break;                        /* MSCAL */
    case 0x15: mscal(v, imm * 8); break;                        /* MSCALF */
    case 0x17: mscal(v, 0xFFFFFFFFu); break;                    /* MSCNT: continue */
    case 0x20: v->need = 1; return;                             /* STMASK */
    case 0x30: case 0x31: v->need = 4; return;                  /* STROW / STCOL */
    case 0x4A: v->need = (num ? num : 256) * 2; return;         /* MPG */
    case 0x50: case 0x51: v->need = (imm ? imm : 65536) * 4; return;   /* DIRECT / DIRECTHL */
    default:
        if ((cmd & 0x60) == 0x60) {                             /* UNPACK */
            u32 vn = (cmd >> 2) & 3, vl = cmd & 3;
            u32 n = num ? num : 256;
            u32 bytes = unpack_data_count(v, n) * unpack_elem_bytes(vn, vl);
            v->need = (bytes + 3) / 4;
            if (!v->need) { do_unpack(v); break; }
            return;
        }
        { static int warned; if (warned++ < 10) fprintf(stderr, "[vif%d] unknown command %08x\n", v->unit, code); }
        break;
    }
    cmd_done(v);
}

static void finish_data_cmd(Vif *v) {
    u32 cmd = (v->code >> 24) & 0x7F, imm = v->code & 0xFFFF;
    switch (cmd) {
    case 0x20: v->mask = v->buf[0]; break;
    case 0x30: memcpy(v->row, v->buf, 16); break;
    case 0x31: memcpy(v->col, v->buf, 16); break;
    case 0x4A: {
        u8 *code = v->unit ? vu1_code : vu0_code;
        u32 size = v->unit ? 16384 : 4096;
        u32 dst = imm * 8;
        for (u32 i = 0; i < v->need * 4; i++) code[(dst + i) % size] = ((u8 *)v->buf)[i];
        v->stat_mpg++;
        { extern volatile int g_vu_code_dirty[2]; g_vu_code_dirty[v->unit] = 1; extern int g_vu_stat_dirty; g_vu_stat_dirty = 1; }
        break; }
    case 0x50: case 0x51: break;   /* streamed */
    default: do_unpack(v); break;
    }
    cmd_done(v);
}

static void feed_word(Vif *v, u32 w) {
    if (!v->code) {
        start_cmd(v, w);
        return;
    }
    u32 cmd = (v->code >> 24) & 0x7F;
    if (cmd == 0x50 || cmd == 0x51) {     /* DIRECT: stream qwords to GIF PATH2 */
        v->buf[v->got & 3] = w;
        v->got++;
        if ((v->got & 3) == 0) { direct_push(v->buf); v->stat_direct++; }
        if (v->got >= v->need) cmd_done(v);
        return;
    }
    v->buf[v->got++] = w;
    if (v->got >= v->need) finish_data_cmd(v);
}

void vif_feed(int unit, const u8 *data, u32 nqw, int is_dma_tag) {
    Vif *v = &g_vif[unit & 1];
    if (is_dma_tag) {   /* TTE: the upper 64 bits of the tag are two VIF codes */
        u32 w[2]; memcpy(w, data + 8, 8);
        feed_word(v, w[0]); feed_word(v, w[1]);
        return;
    }
    const u32 *w = (const u32 *)data;
    u32 nw = nqw * 4;
    for (u32 i = 0; i < nw;) {
        if (v->code && v->need > v->got) {   /* payload of the current command: take it in bulk */
            u32 cmd = (v->code >> 24) & 0x7F, k = v->need - v->got;
            if (k > nw - i) k = nw - i;
            if (cmd == 0x50 || cmd == 0x51) {
                if ((v->got & 3) == 0 && k >= 4) {   /* whole qwords straight to PATH2 */
                    u32 q = k / 4;
                    for (u32 j = 0; j < q; j++) direct_push(w + i + j * 4);
                    v->stat_direct += q; v->got += q * 4; i += q * 4;
                    if (v->got >= v->need) cmd_done(v);
                    continue;
                }
                feed_word(v, w[i++]);
                continue;
            }
            memcpy(v->buf + v->got, w + i, k * 4);
            v->got += k; i += k;
            if (v->got >= v->need) finish_data_cmd(v);
            continue;
        }
        feed_word(v, w[i++]);
    }
    direct_flush();
    gif_path3_flush();
}

void vif_report(void) {
    for (int i = 0; i < 2; i++)
        fprintf(stderr, "[vif%d] unpack %llu, direct qw %llu, mscal %llu, mpg %llu\n", i,
                (unsigned long long)g_vif[i].stat_unpack, (unsigned long long)g_vif[i].stat_direct,
                (unsigned long long)g_vif[i].stat_mscal, (unsigned long long)g_vif[i].stat_mpg);
}

/* VIF1 register reads used by vu1.c for XTOP/XITOP */
u32 vif1_top(void) { return g_vif[1].top; }
u32 vif1_itop(void) { return g_vif[1].itop; }
