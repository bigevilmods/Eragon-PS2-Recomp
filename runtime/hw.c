/* EE hardware: DMAC (10 channels, normal + source-chain), GS privileged registers.
 * Transfers complete instantly. Device sinks (VIF0/VIF1/GIF/IPU/SIF) are stubs that
 * count data; scratchpad channels really copy. */
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ps2_runtime.h"
#include "gs.h"
#include "gif.h"

/* ---- device hooks: VIF0/VIF1 -> vif.c, GIF (PATH3) -> gif.c. TODO: IPU, SIF via DMA. */
static u64 g_dev_qw[10];
extern double g_gs_time;
static double qpc_now(void) { static double f; LARGE_INTEGER t; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = 1.0 / (double)q.QuadPart; } QueryPerformanceCounter(&t); return (double)t.QuadPart * f; }
static void dev_write(int ch, const u8 *qw, u32 n, int is_tag) {
    g_dev_qw[ch] += n;

    switch (ch) {
    case 0: vif_feed(0, qw, n, is_tag); break;
    case 1: vif_feed(1, qw, n, is_tag); break;
    case 2: if (!is_tag) { static int tr = -1, k; extern int g_frame_no_pub; if (tr < 0) tr = getenv("PS2_VIFTRACE") != NULL;
                if (tr && g_frame_no_pub > 1500 && k++ < 200) fprintf(stderr, "[path3] f%d %u qw\n", g_frame_no_pub, n);
                gif_path3_dma(qw, n); } break;
    default: break;
    }

}

/* ---- DMAC state */
typedef struct { u32 chcr, madr, qwc, tadr, asr0, asr1, sadr; } DmaChan;
static DmaChan g_ch[10];
static const u32 k_ch_base[10] = {0x8000, 0x9000, 0xa000, 0xb000, 0xb400, 0xc000, 0xc400, 0xc800, 0xd000, 0xd400};
u32 g_dstat_cis, g_dstat_cim;            /* D_STAT interrupt status / mask (bits 0-9) */
static u32 g_dctrl, g_dpcr, g_dsqwc, g_drbsr, g_drbor, g_denable = 0x1201;

static u8 *dma_ptr(u32 addr) {
    if (addr & 0x80000000u) return ps2_spr + (addr & 0x3ff0);   /* SPR flag */
    u8 *p = ps2_ptr(addr & 0x01fffff0u);
    return p;
}

void kernel_request_intc(int cause);
static void dma_finish(int ch) {
    g_ch[ch].chcr &= ~0x100u;     /* STR off */
    g_dstat_cis |= 1u << ch;
    if (g_dstat_cim & (1u << ch)) ps2_poll_budget = 0;   /* deliver soon */
}

static void spr_copy(int ch, u32 madr, u32 qwc) {
    u8 *m = dma_ptr(madr);
    for (u32 i = 0; i < qwc; i++) {
        u8 *s = ps2_spr + ((g_ch[ch].sadr + i * 16) & 0x3ff0);
        if (ch == 8) memcpy(m + i * 16, s, 16);   /* fromSPR: SPR -> memory */
        else         memcpy(s, m + i * 16, 16);   /* toSPR: memory -> SPR */
    }
    g_ch[ch].sadr = (g_ch[ch].sadr + qwc * 16) & 0x3ff0;
}

static void xfer(int ch, u32 madr, u32 qwc) {
    if (!qwc) return;
    if (ch == 8 || ch == 9) { spr_copy(ch, madr, qwc); return; }
    u8 *p = dma_ptr(madr);
    if (!p) { fprintf(stderr, "[dmac] ch%d bad MADR %08x\n", ch, madr); return; }
    dev_write(ch, p, qwc, 0);
}

static void dma_run(int ch) {
    DmaChan *d = &g_ch[ch];
    static int trace = -1;
    if (trace < 0) trace = getenv("PS2_TRACE") != NULL;
    if (trace && ch != 1 && ch != 2) fprintf(stderr, "[dma] ch%d start chcr=%08x madr=%08x qwc=%x tadr=%08x\n", ch, d->chcr, d->madr, d->qwc, d->tadr);
    int mode = (d->chcr >> 2) & 3;
    if (mode == 0 || mode == 2) {                     /* normal / interleave (treated as normal) */
        xfer(ch, d->madr, d->qwc);
        d->madr += d->qwc * 16; d->qwc = 0;
        dma_finish(ch); return;
    }
    if (ch == 8 || ch == 5) {                         /* destination chain: tags come from the device */
        fprintf(stderr, "[dmac] ch%d destination-chain not implemented\n", ch);
        dma_finish(ch); return;
    }
    /* source chain */
    int asp = (d->chcr >> 4) & 3;
    if (d->qwc) { xfer(ch, d->madr, d->qwc); d->madr += d->qwc * 16; d->qwc = 0; }   /* resume leftover */
    for (int guard = 0; guard < 1000000; guard++) {
        u8 *tp = dma_ptr(d->tadr);
        if (!tp) { fprintf(stderr, "[dmac] ch%d bad TADR %08x\n", ch, d->tadr); break; }
        u64 tag; memcpy(&tag, tp, 8);
        u32 qwc = tag & 0xffff, id = (tag >> 28) & 7, irq = (tag >> 31) & 1;
        u32 addr = (u32)(tag >> 32) & 0xfffffff0u;
        if (tag >> 63) addr |= 0x80000000u;
        d->chcr = (d->chcr & 0xffff) | ((u32)tag & 0xffff0000u);   /* CHCR.TAG = tag bits 16-31 (PCE/ID/IRQ) */
        if (d->chcr & 0x40) dev_write(ch, tp, 1, 1);   /* TTE: tag goes to the device too */
        int end = 0;
        switch (id) {
        case 0: xfer(ch, addr, qwc); d->tadr += 16; end = 1; break;                         /* refe */
        case 1: xfer(ch, d->tadr + 16, qwc); d->tadr += 16 + qwc * 16; break;               /* cnt */
        case 2: xfer(ch, d->tadr + 16, qwc); d->tadr = addr; break;                         /* next */
        case 3: case 4: xfer(ch, addr, qwc); d->tadr += 16; break;                          /* ref/refs */
        case 5:                                                                             /* call */
            xfer(ch, d->tadr + 16, qwc);
            if (asp == 0) d->asr0 = d->tadr + 16 + qwc * 16; else d->asr1 = d->tadr + 16 + qwc * 16;
            asp++; d->tadr = addr;
            if (asp > 2) { fprintf(stderr, "[dmac] ch%d call stack overflow\n", ch); end = 1; }
            break;
        case 6:                                                                             /* ret */
            xfer(ch, d->tadr + 16, qwc);
            if (asp == 0) { end = 1; break; }
            asp--; d->tadr = asp == 1 ? d->asr1 : d->asr0;
            if (asp == 0) d->tadr = d->asr0;
            break;
        case 7: xfer(ch, d->tadr + 16, qwc); end = 1; break;                                /* end */
        }
        d->madr = addr;
        if (end || (irq && (d->chcr & 0x80))) break;
    }
    d->chcr = (d->chcr & ~0x30u) | ((u32)asp << 4);
    if (trace && ch != 1 && ch != 2) fprintf(stderr, "[dma] ch%d chain done chcr=%08x tadr=%08x madr=%08x\n", ch, d->chcr, d->tadr, d->madr);
    dma_finish(ch);
}

void hw_vsync(void) { gs_vsync(); }

/* 128-bit FIFO writes: VIF0 0x10004000, VIF1 0x10005000, GIF (PATH3) 0x10006000 */
int hw_fifo_write(u32 a, const u8 *q) {
    u32 p = a & 0x1fffffff;
    if (p >= 0x10004000 && p < 0x10005000) { vif_feed(0, q, 1, 0); return 1; }
    if (p >= 0x10005000 && p < 0x10006000) { vif_feed(1, q, 1, 0); return 1; }
    if (p >= 0x10006000 && p < 0x10007000) { gif_path3_dma(q, 1); return 1; }
    return 0;
}

/* ---- register dispatch: returns 1 if handled */
static int hw_io_read_reg(u32 p, u64 *out);
/* Registers are 32/64-bit at 16-byte spacing; byte/halfword reads (e.g. lhu CHCR+2 for the tag ID)
 * read the whole register and shift. */
int hw_io_read(u32 a, u64 *out) {
    u32 p = a & 0x1fffffff, sub = p & 0xf;
    if (!hw_io_read_reg(p & ~0xfu, out)) return 0;
    *out >>= sub * 8;
    return 1;
}

static int hw_io_read_reg(u32 p, u64 *out) {
    if (p >= 0x10008000 && p < 0x1000e000) {
        for (int ch = 0; ch < 10; ch++) {
            u32 b = 0x10000000 | k_ch_base[ch];
            if (p >= b && p < b + 0x100) {
                DmaChan *d = &g_ch[ch];
                switch (p - b) {
                case 0x00: *out = d->chcr; return 1;
                case 0x10: *out = d->madr; return 1;
                case 0x20: *out = d->qwc; return 1;
                case 0x30: *out = d->tadr; return 1;
                case 0x40: *out = d->asr0; return 1;
                case 0x50: *out = d->asr1; return 1;
                case 0x80: *out = d->sadr; return 1;
                default: *out = 0; return 1;
                }
            }
        }
    }
    switch (p) {
    case 0x1000e000: *out = g_dctrl; return 1;
    case 0x1000e010: *out = g_dstat_cis | (g_dstat_cim << 16); return 1;
    case 0x1000e020: *out = g_dpcr; return 1;
    case 0x1000e030: *out = g_dsqwc; return 1;
    case 0x1000e040: *out = g_drbsr; return 1;
    case 0x1000e050: *out = g_drbor; return 1;
    case 0x1000f520: *out = g_denable; return 1;
    }
    if (p >= 0x12000000 && p < 0x12002000) { *out = gs_priv_read(p); return 1; }
    /* VIF/GIF status: report idle so polling loops finish */
    if (p == 0x10003800 || p == 0x10003c00 || p == 0x10003020) { *out = 0; return 1; }
    return 0;
}

int hw_io_write(u32 a, u64 v) {
    u32 p = a & 0x1fffffff;
    if (p >= 0x10008000 && p < 0x1000e000) {
        for (int ch = 0; ch < 10; ch++) {
            u32 b = 0x10000000 | k_ch_base[ch];
            if (p >= b && p < b + 0x100) {
                DmaChan *d = &g_ch[ch];
                switch (p - b) {
                case 0x00: d->chcr = (u32)v; if ((d->chcr & 0x100) && (g_dctrl & 1)) dma_run(ch); return 1;
                case 0x10: d->madr = (u32)v; return 1;
                case 0x20: d->qwc = (u32)v & 0xffff; return 1;
                case 0x30: d->tadr = (u32)v; return 1;
                case 0x40: d->asr0 = (u32)v; return 1;
                case 0x50: d->asr1 = (u32)v; return 1;
                case 0x80: d->sadr = (u32)v & 0x3ff0; return 1;
                default: return 1;
                }
            }
        }
    }
    switch (p) {
    case 0x1000e000: g_dctrl = (u32)v; return 1;
    case 0x1000e010: g_dstat_cis &= ~((u32)v & 0x3ff); g_dstat_cim ^= ((u32)v >> 16) & 0x3ff; return 1;
    case 0x1000e020: g_dpcr = (u32)v; return 1;
    case 0x1000e030: g_dsqwc = (u32)v; return 1;
    case 0x1000e040: g_drbsr = (u32)v; return 1;
    case 0x1000e050: g_drbor = (u32)v; return 1;
    case 0x1000f590: g_denable = (u32)v; return 1;
    }
    if (p >= 0x12000000 && p < 0x12002000) { gs_priv_write(p, v); return 1; }
    if (p == 0x10003010) { gif_path3_modemask((int)(v & 1)); return 1; }   /* GIF_MODE.M3R */
    if (p >= 0x10003000 && p < 0x10004000) return 1;   /* other GIF/VIF control regs: accepted (TODO) */
    return 0;
}

void hw_report(void) {
    static const char *const n[10] = {"VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR"};
    for (int i = 0; i < 10; i++) if (g_dev_qw[i]) fprintf(stderr, "[dmac] %s: %llu qwords\n", n[i], (unsigned long long)g_dev_qw[i]);
}
