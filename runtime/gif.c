/* GIF: tag parser for PATH1 (VU1 XGKICK), PATH2 (VIF1 DIRECT) and PATH3 (GIF DMA / FIFO).
 *
 * Like the real GS, drawing runs in parallel with the EE: producers (DMA, VIF, XGKICK) copy
 * GIF data into a queue and continue; a host worker thread parses it and rasterizes in
 * submission order. FINISH/SIGNAL interrupts are raised from the worker. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "gs.h"
#include "gif.h"
void gs_flush(void);

typedef struct {
    int active;
    u32 nloop, flg, nreg, reg_i;
    u64 regs;
    int eop;
} GifPath;
static GifPath g_path[4];
static volatile LONG64 g_stats_tags[4], g_stats_qw[4];

static void tag(GifPath *p, const u8 *q) {
    u64 lo, hi; memcpy(&lo, q, 8); memcpy(&hi, q + 8, 8);
    p->nloop = lo & 0x7FFF;
    p->eop = (lo >> 15) & 1;
    int pre = (lo >> 46) & 1;
    u32 prim = (lo >> 47) & 0x7FF;
    p->flg = (lo >> 58) & 3;
    p->nreg = (u32)(lo >> 60); if (!p->nreg) p->nreg = 16;
    p->regs = hi;
    p->reg_i = 0;
    if (pre && p->flg == 0) gs_write_reg(0x00, prim);
    p->active = p->nloop != 0;
}

/* GS-thread side: interpret qwords of one path */
int g_gif_cur_path;
static void gif_process(int path, const u8 *data, u32 nqw) {
    GifPath *p = &g_path[path];
    g_gif_cur_path = path;
    for (u32 i = 0; i < nqw; i++) {
        const u8 *q = data + i * 16;
        if (!p->active) { tag(p, q); g_stats_tags[path]++; continue; }
        switch (p->flg) {
        case 0: {   /* PACKED: one register per qword */
            u32 desc = (u32)(p->regs >> (p->reg_i * 4)) & 15;
            if (desc == 0xE) { u64 v; memcpy(&v, q, 8); gs_write_reg(q[8], v); }
            else if (desc != 0xF) gs_write_packed((int)desc, q);
            if (++p->reg_i >= p->nreg) { p->reg_i = 0; if (--p->nloop == 0) p->active = 0; }
            break; }
        case 1: {   /* REGLIST: two 64-bit registers per qword */
            for (int h = 0; h < 2 && p->active; h++) {
                u32 desc = (u32)(p->regs >> (p->reg_i * 4)) & 15;
                u64 v; memcpy(&v, q + h * 8, 8);
                if (desc < 0xE) gs_write_reg((int)desc, v);
                if (++p->reg_i >= p->nreg) { p->reg_i = 0; if (--p->nloop == 0) p->active = 0; }
            }
            break; }
        default:    /* IMAGE (and disabled) */
            gs_image_data(q, 16);
            if (--p->nloop == 0) p->active = 0;
            break;
        }
    }
}

/* ---- producer/consumer queue */
typedef struct Chunk { struct Chunk *next; int path; u32 nqw; u8 data[]; } Chunk;
static Chunk *g_head, *g_tail;
static u64 g_queued_qw;
static CRITICAL_SECTION g_lock;
static CONDITION_VARIABLE g_not_empty, g_not_full, g_drained;
static int g_busy, g_started, g_sync_mode;
#define QUEUE_LIMIT_QW (4u << 20)   /* 64 MB of pending GIF data */
extern double g_gs_time;

int gs_service_display(void);
int gs_display_pending(void);

/* wake the GS thread (e.g. for a display readout request) */
void gif_wake_gs(void) {
    if (!g_started || g_sync_mode) { if (g_sync_mode) gs_service_display(); return; }
    EnterCriticalSection(&g_lock);
    WakeConditionVariable(&g_not_empty);
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI gs_thread(void *arg) {
    (void)arg;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    for (;;) {
        gs_service_display();
        EnterCriticalSection(&g_lock);
        if (!g_head) {   /* idle: finish pending draws so the display is current */
            LeaveCriticalSection(&g_lock);
            gs_flush();
            EnterCriticalSection(&g_lock);
        }
        while (!g_head && !gs_display_pending()) { g_busy = 0; WakeAllConditionVariable(&g_drained); SleepConditionVariableCS(&g_not_empty, &g_lock, INFINITE); }
        if (!g_head) { LeaveCriticalSection(&g_lock); continue; }   /* woken for a display readout */
        Chunk *c = g_head; g_head = c->next; if (!g_head) g_tail = NULL;
        g_busy = 1;
        LeaveCriticalSection(&g_lock);
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        gif_process(c->path, c->data, c->nqw);
        QueryPerformanceCounter(&t1);
        g_gs_time += (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        EnterCriticalSection(&g_lock);
        g_queued_qw -= c->nqw;
        WakeConditionVariable(&g_not_full);
        LeaveCriticalSection(&g_lock);
        free(c);
    }
    return 0;
}

static void ensure_started(void) {
    if (g_started) return;
    g_started = 1;
    InitializeCriticalSection(&g_lock);
    InitializeConditionVariable(&g_not_empty); InitializeConditionVariable(&g_not_full); InitializeConditionVariable(&g_drained);
    g_sync_mode = getenv("PS2_GS_SYNC") != NULL;     /* debugging: draw on the EE thread */
    if (!g_sync_mode) {
        HANDLE h = CreateThread(NULL, 0, gs_thread, NULL, 0, NULL);
        void profiler_set_gs_thread(HANDLE); profiler_set_gs_thread(h);
    }
}

u32 gif_feed(int path, const u8 *data, u32 nqw, int stop_at_eop) {
    (void)stop_at_eop;
    if (!nqw) return 0;
    ensure_started();
    g_stats_qw[path] += nqw;
    if (g_sync_mode) { gif_process(path, data, nqw); return nqw; }
    Chunk *c = malloc(sizeof *c + (size_t)nqw * 16);
    c->next = NULL; c->path = path; c->nqw = nqw;
    memcpy(c->data, data, (size_t)nqw * 16);
    EnterCriticalSection(&g_lock);
    while (g_queued_qw > QUEUE_LIMIT_QW) SleepConditionVariableCS(&g_not_full, &g_lock, INFINITE);
    if (g_tail) g_tail->next = c; else g_head = c;
    g_tail = c;
    g_queued_qw += nqw;
    WakeConditionVariable(&g_not_empty);
    LeaveCriticalSection(&g_lock);
    return nqw;
}

/* Wait until the GS has consumed everything queued so far. */
void gif_sync(void) {
    if (!g_started || g_sync_mode) return;
    EnterCriticalSection(&g_lock);
    while (g_head || g_busy) SleepConditionVariableCS(&g_drained, &g_lock, INFINITE);
    LeaveCriticalSection(&g_lock);
}

/* ---- PATH3 arbitration (EE side, before the GS queue)
 * PATH3 can be masked by VIF1 MSKPATH3 or GIF_MODE.M3R. While masked, GIF DMA data waits here.
 * The mask takes effect at the end of a GIF packet (EOP), so unmasking lets one packet through
 * before a following re-mask. FLUSHA (or staying unmasked) drains everything. */
static u8 *g_p3; static u32 g_p3_n, g_p3_cap, g_p3_off;   /* pending qwords start at g_p3 + g_p3_off * 16 */
static int g_p3_vifmask, g_p3_modemask;
static int p3_masked(void) { return g_p3_vifmask || g_p3_modemask; }

/* qwords in the first complete EOP-terminated packet of buf, or 0 if incomplete */
static u32 packet_len(const u8 *buf, u32 n) {
    u32 i = 0;
    while (i < n) {
        u64 lo, hi; memcpy(&lo, buf + i * 16, 8); memcpy(&hi, buf + i * 16 + 8, 8);
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (u32)(lo >> 60);
        if (!nreg) nreg = 16;
        u32 body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        i += 1 + body;
        if (i > n) return 0;
        if (eop) return i;
    }
    return 0;
}

static void p3_release(int one_packet) {
    while (g_p3_n) {
        const u8 *head = g_p3 + (size_t)g_p3_off * 16;
        u32 len = packet_len(head, g_p3_n);
        if (!len) { if (one_packet) return; len = g_p3_n; }   /* incomplete packet: send what we have */
        gif_feed(3, head, len, 0);
        g_p3_off += len; g_p3_n -= len;   /* consumed from the front: no memmove per packet */
        if (!g_p3_n) g_p3_off = 0;
        if (one_packet) return;
    }
}

void gif_path3_dma(const u8 *data, u32 nqw) {
    if (!p3_masked() && !g_p3_n) { gif_feed(3, data, nqw, 0); return; }
    if (g_p3_off && g_p3_off + g_p3_n + nqw > g_p3_cap) {   /* compact before growing */
        memmove(g_p3, g_p3 + (size_t)g_p3_off * 16, (size_t)g_p3_n * 16);
        g_p3_off = 0;
    }
    if (g_p3_n + nqw > g_p3_cap) {
        g_p3_cap = (g_p3_n + nqw) * 2;
        g_p3 = realloc(g_p3, (size_t)g_p3_cap * 16);
    }
    memcpy(g_p3 + (size_t)(g_p3_off + g_p3_n) * 16, data, (size_t)nqw * 16);
    g_p3_n += nqw;
    if (!p3_masked()) p3_release(0);
}

void gif_path3_vifmask(int masked) {
    int was = p3_masked();
    g_p3_vifmask = masked;
    if (was && !p3_masked()) p3_release(1);   /* the packet that slips through before a re-mask */
}
void gif_path3_modemask(int masked) {
    int was = p3_masked();
    g_p3_modemask = masked;
    if (was && !p3_masked()) p3_release(1);
}
void gif_path3_flush(void) { if (!p3_masked()) p3_release(0); }

void gif_report(void) {
    for (int i = 1; i <= 3; i++)
        if (g_stats_qw[i]) fprintf(stderr, "[gif] PATH%d: %llu tags, %llu qwords\n", i,
                                   (unsigned long long)g_stats_tags[i], (unsigned long long)g_stats_qw[i]);
}
