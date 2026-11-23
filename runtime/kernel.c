/* EE kernel HLE: syscalls, threads (Windows fibers), semaphores, INTC/DMAC handlers,
 * alarms, timers, VBlank. Interrupts are delivered at safe points: syscalls, idle, and
 * the PS2_POLL() budget check the recompiler emits on every backward branch. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "ps2_runtime.h"
#include "iop.h"

void ps2_die(const char *fmt, ...);

/* ------------------------------------------------------------ time */
static double g_qpc_to_sec;
static LARGE_INTEGER g_qpc0;
static double now_sec(void) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) * g_qpc_to_sec;
}
#define BUSCLK   147456000.0
#define HSYNC_HZ 15734.25
#define VSYNC_HZ 59.94

/* ------------------------------------------------------------ syscall names (for logs) */
static const char *const k_names[256] = {
    [0x02]="SetGsCrt",[0x04]="Exit",[0x06]="LoadExecPS2",[0x07]="ExecPS2",[0x0a]="AddSbusIntcHandler",
    [0x0b]="RemoveSbusIntcHandler",[0x0c]="Interrupt2Iop",[0x0d]="SetVTLBRefillHandler",[0x0e]="SetVCommonHandler",
    [0x0f]="SetVInterruptHandler",[0x10]="AddIntcHandler",[0x11]="RemoveIntcHandler",[0x12]="AddDmacHandler",
    [0x13]="RemoveDmacHandler",[0x14]="EnableIntc",[0x15]="DisableIntc",[0x16]="EnableDmac",[0x17]="DisableDmac",
    [0x18]="SetAlarm(k)",[0x19]="ReleaseAlarm(k)",[0x1a]="iEnableIntc",[0x1b]="iDisableIntc",[0x1c]="iEnableDmac",
    [0x1d]="iDisableDmac",[0x20]="CreateThread",[0x21]="DeleteThread",[0x22]="StartThread",[0x23]="ExitThread",
    [0x24]="ExitDeleteThread",[0x25]="TerminateThread",[0x26]="iTerminateThread",[0x27]="DisableDispatchThread",
    [0x28]="EnableDispatchThread",[0x29]="ChangeThreadPriority",[0x2a]="iChangeThreadPriority",
    [0x2b]="RotateThreadReadyQueue",[0x2c]="iRotateThreadReadyQueue",[0x2d]="ReleaseWaitThread",
    [0x2e]="iReleaseWaitThread",[0x2f]="GetThreadId",[0x30]="ReferThreadStatus",[0x31]="iReferThreadStatus",
    [0x32]="SleepThread",[0x33]="WakeupThread",[0x34]="iWakeupThread",[0x35]="CancelWakeupThread",
    [0x36]="iCancelWakeupThread",[0x37]="SuspendThread",[0x38]="iSuspendThread",[0x39]="ResumeThread",
    [0x3a]="iResumeThread",[0x3b]="JoinThread",[0x3c]="SetupThread",[0x3d]="SetupHeap",[0x3e]="EndOfHeap",
    [0x40]="CreateSema",[0x41]="DeleteSema",[0x42]="SignalSema",[0x43]="iSignalSema",[0x44]="WaitSema",
    [0x45]="PollSema",[0x46]="iPollSema",[0x47]="ReferSemaStatus",[0x48]="iReferSemaStatus",
    [0x4a]="SetOsdConfigParam",[0x4b]="GetOsdConfigParam",[0x4c]="GetGsHParam",[0x4d]="GetGsVParam",
    [0x4e]="SetGsHParam",[0x4f]="SetGsVParam",[0x55]="PutTLBEntry",[0x56]="SetTLBEntry",[0x57]="GetTLBEntry",
    [0x58]="ProbeTLBEntry",[0x59]="ExpandScratchPad",[0x5a]="kCopy",[0x5b]="kCall",
    [0x5c]="EnableIntcHandler",[0x5d]="DisableIntcHandler",[0x5e]="EnableDmacHandler",[0x5f]="DisableDmacHandler",
    [0x60]="KSeg0",[0x61]="EnableCache",[0x62]="DisableCache",[0x63]="GetCop0",[0x64]="FlushCache",
    [0x66]="CpuConfig",[0x67]="iGetCop0",[0x68]="iFlushCache",[0x6a]="iCpuConfig",[0x6b]="sceSifStopDma",
    [0x6c]="SetCPUTimerHandler",[0x6d]="SetCPUTimer",[0x6e]="SetOsdConfigParam2",[0x6f]="GetOsdConfigParam2",
    [0x70]="GsGetIMR",[0x71]="GsPutIMR",[0x72]="SetPgifHandler",[0x73]="SetVSyncFlag",[0x74]="SetSyscall",
    [0x75]="_print",[0x76]="sceSifDmaStat",[0x77]="sceSifSetDma",[0x78]="sceSifSetDChain",[0x79]="sceSifSetReg",
    [0x7a]="sceSifGetReg",[0x7b]="ExecOSD",[0x7c]="Deci2Call",[0x7d]="PSMode",[0x7e]="MachineType",
    [0x7f]="GetMemorySize",[0xfc]="SetAlarm",[0xfd]="iSetAlarm",[0xfe]="ReleaseAlarm",[0xff]="iReleaseAlarm",
};

/* ------------------------------------------------------------ threads */
enum { TS_RUN = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_WAITSUSPEND = 12, TS_DORMANT = 16 };
enum { WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2 };
#define MAX_THREADS 256
#define MAX_SEMAS 256

typedef struct {
    int used, status, wait_type, wait_id, prio, init_prio, wakeups;
    u32 entry, stack, stack_size, gp, attr, option, arg;
    u64 ready_seq;
    R5900Context *ctx;
    void *fiber;
    jmp_buf base;
    int started, restart;
} Thread;

typedef struct { int used, count, max, init, waiters; u32 attr, option; } Sema;

static Thread g_th[MAX_THREADS];
static Sema   g_sema[MAX_SEMAS];
static int    g_cur = 1;               /* running thread id */
static u64    g_seq;
static int    g_dispatch_disabled;
static int    g_in_intr;
static int    g_need_resched;
static void  *g_main_fiber;

static void make_ready(int id) {
    g_th[id].status = TS_READY;
    g_th[id].ready_seq = ++g_seq;
    if (!g_in_intr && g_th[id].prio < g_th[g_cur].prio) g_need_resched = 1;
    if (g_in_intr) g_need_resched = 1;
}

static int pick_next(void) {
    int best = 0;
    for (int i = 1; i < MAX_THREADS; i++) {
        Thread *t = &g_th[i];
        if (!t->used || (t->status != TS_READY && t->status != TS_RUN)) continue;
        if (!best || t->prio < g_th[best].prio ||
            (t->prio == g_th[best].prio && t->ready_seq < g_th[best].ready_seq)) best = i;
    }
    return best;
}

static void deliver_interrupts(R5900Context *base, int idle);
static double next_event_time(void);

static void switch_to(int id) {
    if (id == g_cur) { g_th[id].status = TS_RUN; return; }
    if (g_th[g_cur].status == TS_RUN) { g_th[g_cur].status = TS_READY; g_th[g_cur].ready_seq = ++g_seq; }
    vu0_copy_state(g_th[id].ctx, g_th[g_cur].ctx);   /* VU0 registers belong to the CPU, not the thread */
    g_cur = id;
    g_th[id].status = TS_RUN;
    SwitchToFiber(g_th[id].fiber);
    /* resumed: possibly told to restart from scratch (TerminateThread + StartThread) */
    Thread *me = &g_th[g_cur];
    if (me->restart) { me->restart = 0; longjmp(me->base, 1); }
}

/* Run the highest-priority ready thread; if none, idle (deliver interrupts) until one is. */
static void schedule(void) {
    g_need_resched = 0;
    if (g_dispatch_disabled && g_th[g_cur].status == TS_RUN) return;
    for (;;) {
        int n = pick_next();
        if (n) { switch_to(n); return; }
        /* everyone is waiting: sleep until the next interrupt source fires */
        double wait = next_event_time() - now_sec();
        if (wait > 0.0005) Sleep((DWORD)(wait * 1000.0));
        deliver_interrupts(g_th[g_cur].ctx, 1);
    }
}

static void block_current(int wait_type, int wait_id) {
    Thread *t = &g_th[g_cur];
    t->status = TS_WAIT; t->wait_type = wait_type; t->wait_id = wait_id;
    schedule();
}

static void WINAPI thread_fiber(void *param) {
    int id = (int)(intptr_t)param;
    Thread *t = &g_th[id];
    setjmp(t->base);   /* ExitThread / restart-after-terminate land here */
    for (;;) {
        Thread *me = &g_th[id];
        if (me->status == TS_DORMANT) { schedule(); continue; }   /* wait for StartThread */
        R5900Context *c = me->ctx;
        u8 *ram = c->ram, *spr = c->spr;
        /* Fresh CPU registers, but VU0's registers are shared hardware: switch_to() just copied the
         * current VU0 state in here, and wiping it would erase VU0 constants for every thread. */
        static R5900Context vu0_keep;
        vu0_copy_state(&vu0_keep, c);
        memset(c, 0, sizeof *c);
        vu0_copy_state(c, &vu0_keep);
        c->ram = ram; c->spr = spr;
        c->vf[0].f[3] = 1.0f;
        c->r[4].ud[0] = (u64)(s32)me->arg;
        c->r[28].ud[0] = (u64)(s32)me->gp;
        c->r[29].ud[0] = (u64)(s32)(me->stack + me->stack_size - 0x2a0);
        c->r[31].ud[0] = 0;
        ps2_call(c, me->entry);
        me->status = TS_DORMANT;   /* returning from the entry function == ExitThread */
    }
}

static int new_thread(u32 param) {
    int id;
    for (id = 2; id < MAX_THREADS && g_th[id].used; id++) {}
    if (id >= MAX_THREADS) return -1;
    Thread *t = &g_th[id];
    memset(t, 0, sizeof *t);
    t->used = 1; t->status = TS_DORMANT;
    t->entry = MEM_R32(param + 4); t->stack = MEM_R32(param + 8); t->stack_size = MEM_R32(param + 12);
    t->gp = MEM_R32(param + 16); t->init_prio = t->prio = (int)MEM_R32(param + 20);
    t->attr = MEM_R32(param + 28); t->option = MEM_R32(param + 32);
    t->ctx = calloc(1, sizeof(R5900Context));
    t->ctx->ram = ps2_ram; t->ctx->spr = ps2_spr;
    t->fiber = CreateFiberEx(64 << 10, 8 << 20, FIBER_FLAG_FLOAT_SWITCH, thread_fiber, (void *)(intptr_t)id);
    if (!t->fiber) ps2_die("CreateFiberEx failed");
    return id;
}

static int valid_thread(int id) { return id > 0 && id < MAX_THREADS && g_th[id].used; }

/* ------------------------------------------------------------ interrupts */
typedef struct { int used, cause; u32 handler, arg, gp; int enabled, is2; } Handler;
#define MAX_HANDLERS 64
static Handler g_intc_h[MAX_HANDLERS], g_dmac_h[MAX_HANDLERS];
static u32 g_intc_mask;
extern u32 g_dstat_cis, g_dstat_cim;     /* DMAC D_STAT (hw.c) */
#define g_dmac_mask g_dstat_cim
void hw_vsync(void);
static u32 g_intc_pending;
static int g_intr_enabled = 1;   /* COP0 Status.EIE (EI/DI) */

typedef struct { int used; double when; u32 handler, arg, gp; u16 ticks; } Alarm;
#define MAX_ALARMS 64
static Alarm g_alarm[MAX_ALARMS];

static double g_next_vblank;
static u32 g_vsync_count;

#define INTR_STACK_TOP 0x000ff000u
static R5900Context g_ictx;

static void call_handler(R5900Context *base, u32 handler, u32 gp, u32 a0, u32 a1, u32 a2) {
    g_ictx = *base;
    g_ictx.r[4].ud[0] = (u64)(s32)a0; g_ictx.r[5].ud[0] = (u64)(s32)a1; g_ictx.r[6].ud[0] = (u64)(s32)a2;
    g_ictx.r[28].ud[0] = (u64)(s32)gp;
    g_ictx.r[29].ud[0] = INTR_STACK_TOP;
    g_ictx.r[31].ud[0] = 0;
    ps2_call(&g_ictx, handler);
    vu0_copy_state(base, &g_ictx);
}

static void raise_intc(R5900Context *base, int cause) {
    g_intc_pending |= 1u << cause;
    if (!(g_intc_mask & (1u << cause))) return;
    for (int i = 0; i < MAX_HANDLERS; i++) {
        Handler *h = &g_intc_h[i];
        if (!h->used || !h->enabled || h->cause != cause) continue;
        if (h->is2) call_handler(base, h->handler, h->gp, cause, h->arg, 0);
        else        call_handler(base, h->handler, h->gp, cause, 0, 0);
    }
}

static void raise_dmac(R5900Context *base, int ch) {
    if (!(g_dmac_mask & (1u << ch))) return;
    for (int i = 0; i < MAX_HANDLERS; i++) {
        Handler *h = &g_dmac_h[i];
        if (!h->used || !h->enabled || h->cause != ch) continue;
        call_handler(base, h->handler, h->gp, ch, h->arg, 0);
    }
}

void ps2_raise_dmac(R5900Context *c, int ch) {   /* for DMA emulation code */
    int was = g_in_intr; g_in_intr = 1; raise_dmac(c, ch); g_in_intr = was;
}

/* Hardware models request an INTC interrupt; delivered at the next interrupt point. */
static u32 g_async_intc;
void kernel_request_intc(int cause) {   /* may be called from the GS worker thread */
    InterlockedOr((volatile LONG *)&g_async_intc, (LONG)(1u << cause));
    ps2_poll_budget = 0;
}

/* ---- timers (0x10000000 + n*0x800): COUNT, MODE, COMP, HOLD */
typedef struct { u32 mode, comp, hold; double base_t; u32 base_count; double next_irq; } Timer;
static Timer g_tim[4];
static double timer_rate(int n) {
    switch (g_tim[n].mode & 3) {
    case 0: return BUSCLK; case 1: return BUSCLK / 16; case 2: return BUSCLK / 256; default: return HSYNC_HZ;
    }
}
static u32 timer_count(int n) {
    if (!(g_tim[n].mode & 0x80)) return g_tim[n].base_count;      /* CUE off: stopped */
    double ticks = (now_sec() - g_tim[n].base_t) * timer_rate(n);
    return (u32)(g_tim[n].base_count + (u64)ticks) & 0xffff;
}
static void timer_rearm(int n) {
    Timer *t = &g_tim[n];
    t->next_irq = 0;
    if (!(t->mode & 0x80) || !(t->mode & 0x300)) return;
    u32 cnt = timer_count(n);
    u32 target = (t->mode & 0x100) ? t->comp : 0x10000;
    u32 dist = target > cnt ? target - cnt : 0x10000 - cnt + target;
    t->next_irq = now_sec() + dist / timer_rate(n);
}

static double next_event_time(void) {
    double t = g_next_vblank;
    for (int i = 0; i < MAX_ALARMS; i++) if (g_alarm[i].used && g_alarm[i].when < t) t = g_alarm[i].when;
    for (int i = 0; i < 4; i++) if (g_tim[i].next_irq && g_tim[i].next_irq < t) t = g_tim[i].next_irq;
    return t;
}

static void deliver_interrupts(R5900Context *base, int idle) {
    if (g_in_intr || (!g_intr_enabled && !idle)) return;
    g_in_intr = 1;
    double t = now_sec();
    if (t >= g_next_vblank) {
        g_vsync_count++;
        hw_vsync();
        { void pad_vsync(u32); pad_vsync(g_vsync_count); }
        { /* PS2_TELEPORT=vsync,x,y,z: at that vsync move Eragon (ClEragonObj, class info 0x38b408) by writing his
           * scene node's translation (node = obj+468, translation at node+64) and his position (obj+0x70) */
          static int at = -2; static float tp[3];
          if (at == -2) { const char *e = getenv("PS2_TELEPORT"); at = -1; if (e && sscanf(e, "%d,%f,%f,%f", &at, &tp[0], &tp[1], &tp[2]) != 4) at = -1; }
          if (at >= 0 && (int)g_vsync_count >= at) {
              at = -1;
              for (u32 o = 0x400000; o + 0x900 < (32u << 20); o += 16) {
                  u32 cls; memcpy(&cls, ps2_ram + o + 8, 4);
                  if (cls != 0x38b408) continue;
                  u32 node; memcpy(&node, ps2_ram + o + 468, 4); node &= 0x1FFFFFF;
                  if (node && node + 80 < (32u << 20)) memcpy(ps2_ram + node + 64, tp, 12);
                  memcpy(ps2_ram + o + 0x70, tp, 12);
                  fprintf(stderr, "[teleport] Eragon obj %08x node %08x -> %.3f %.3f %.3f\n", o, node, tp[0], tp[1], tp[2]);
              }
          } }
        { /* PS2_RAMDUMP_AT=N[,M...]: write ramdump_<vsync>.bin at those vsyncs */
          static char spec[256]; static int init;
          if (!init) { init = 1; const char *e = getenv("PS2_RAMDUMP_AT"); if (e) snprintf(spec, sizeof spec, ",%s,", e); }
          if (spec[0]) { char key[32]; snprintf(key, sizeof key, ",%u,", g_vsync_count);
              if (strstr(spec, key)) { char fn[64]; snprintf(fn, sizeof fn, "ramdump_%u.bin", g_vsync_count);
                  FILE *d = fopen(fn, "wb"); if (d) { fwrite(ps2_ram, 1, 32u << 20, d); fclose(d); } } } }
        raise_intc(base, 2);   /* VBLANK_START */
        raise_intc(base, 3);   /* VBLANK_END (delivered together for now) */
        g_next_vblank += 1.0 / VSYNC_HZ;
        if (g_next_vblank < t) g_next_vblank = t + 1.0 / VSYNC_HZ;
    }
    for (int i = 0; i < MAX_ALARMS; i++) {
        Alarm *a = &g_alarm[i];
        if (a->used && t >= a->when) {
            a->used = 0;
            /* SDK alarm handler: void handler(int id, u16 time, void *arg) */
            call_handler(base, a->handler, a->gp, i + 1, a->ticks, a->arg);
        }
    }
    for (u32 req = g_async_intc, cause = 0; req && cause < 15; cause++) {
        if (!(req & (1u << cause))) continue;
        InterlockedAnd((volatile LONG *)&g_async_intc, ~(LONG)(1u << cause));
        raise_intc(base, (int)cause);
    }
    iop_pump();   /* next IOP->EE SIF packet, if the EE consumed the previous one */
    for (u32 pend = g_dstat_cis & g_dstat_cim, ch = 0; pend && ch < 10; ch++) {
        if (!(pend & (1u << ch))) continue;
        g_dstat_cis &= ~(1u << ch);          /* kernel acknowledges before calling handlers */
        raise_dmac(base, (int)ch);
    }
    for (int n = 0; n < 4; n++) {
        if (g_tim[n].next_irq && t >= g_tim[n].next_irq) {
            g_tim[n].mode |= (g_tim[n].mode & 0x100) ? 0x400 : 0x800;   /* EQUF / OVFF */
            if (g_tim[n].mode & 0x40) { g_tim[n].base_count = 0; g_tim[n].base_t = t; }  /* ZRET */
            timer_rearm(n);
            raise_intc(base, 9 + n);
        }
    }
    g_in_intr = 0;
}

int ps2_poll_budget = 20000;
volatile u32 ps2_last_loop;

/* Host thread: every 3 s report which guest loop / thread is running (finds hangs). */
static DWORD WINAPI watchdog(void *p) {
    (void)p;
    u32 prev = 0; int same = 0;
    for (;;) {
        Sleep(3000);
        u32 cur = ps2_last_loop;
        same = (cur == prev) ? same + 1 : 0;
        R5900Context *tc = g_th[g_cur].ctx;
        fprintf(stderr, "[watch] t=%.0fs thread=%d loop=0x%08x%s vsyncs=%u a0=%08x a1=%08x ra=%08x\n", now_sec(), g_cur, cur,
                same ? " (same)" : "", g_vsync_count, tc->r[4].uw[0], tc->r[5].uw[0], tc->r[31].uw[0]);
        { /* echo new lines of the game's debug console (ring of 200 x 128 chars at 0x3BDB50 + 64) */
          static int seen[200]; static u32 sig[200];
          const u8 *con = ps2_ram + 0x3bdb50 + 64;
          for (int i = 0; i < 200; i++) {
              const char *s = (const char *)con + i * 128;
              u32 h = 2166136261u; for (int k = 0; k < 128 && s[k]; k++) h = (h ^ (u8)s[k]) * 16777619u;
              if (s[0] && (!seen[i] || sig[i] != h)) {
                  seen[i] = 1; sig[i] = h; fprintf(stderr, "[game] %.128s\n", s);
                  extern int g_frame_no_pub; extern int g_respawn_frame;
                  if (strstr(s, "Loading respawn ID") && !g_respawn_frame) g_respawn_frame = g_frame_no_pub;
              }
          } }
        { void gif_report(void); void vif_report(void); void vu_report(void);
          if (getenv("PS2_GFXSTATS")) { gif_report(); vif_report(); vu_report(); } }
        for (int i = 1; i < MAX_THREADS; i++) if (g_th[i].used)
            fprintf(stderr, "   thread %d prio=%d status=%d wait=%d/%d ra=%08x\n", i, g_th[i].prio, g_th[i].status,
                    g_th[i].wait_type, g_th[i].wait_id, g_th[i].ctx->r[31].uw[0]);
        if (same == 1) {   /* stuck: snapshot RAM for offline inspection */
            FILE *d = fopen("ramdump.bin", "wb");
            if (d) { fwrite(ps2_ram, 1, 32u << 20, d); fclose(d); }
        }
        prev = cur;
    }
    return 0;
}
/* PS2_SCRIPTLOG=1: log script-method calls (diagnostic hook placed in generated code) */
static const char *guest_str(u32 a) {
    a &= 0x1ffffff;
    if (a < 0x100000 || a >= (32u << 20)) return NULL;
    const char *s = (const char *)ps2_ram + a;
    for (int i = 0; i < 64; i++) { if (!s[i]) return i ? s : NULL; if ((u8)s[i] < 32 || (u8)s[i] > 126) return NULL; }
    return NULL;
}
static struct { u32 fn; const char *name; } g_sau[2048]; static int g_nsau;
int g_scriptlog = -1;
void dbg_script_native(u32 target) {   /* called from ps2_call for every indirect call when enabled */
    static unsigned n; static u32 last;
    for (int i = 0; i < g_nsau; i++) if (g_sau[i].fn == target) {
        if (target == last || n > 200000) return;
        last = target; n++;
        fprintf(stderr, "[sau] v%u %s\n", g_vsync_count, g_sau[i].name);
        return;
    }
}
void dbg_script_ret(u32 target, R5900Context *c) {   /* return values of selected script calls */
    static unsigned n;
    for (int i = 0; i < g_nsau; i++) if (g_sau[i].fn == target) {
        const char *nm = g_sau[i].name;
        if ((strstr(nm, "Player") || strstr(nm, "Respawn")) && n++ < 200)
            fprintf(stderr, "[sau] v%u %s -> v0=%08x v1=%08x\n", g_vsync_count, nm, c->r[2].uw[0], c->r[3].uw[0]);
        return;
    }
}
void dbg_script_call(R5900Context *c, int which) {
    static int on = -1; static unsigned n; static char last[160];
    if (on < 0) on = getenv("PS2_SCRIPTLOG") != NULL;
    g_scriptlog = on;
    if (on && which == 1 && g_nsau < 2048) {
        const char *nm = guest_str(c->r[4].uw[0]); u32 fn = c->r[5].uw[0];
        if (nm && fn >= 0x100000 && fn < 0x600000) {
            int k; for (k = 0; k < g_nsau && g_sau[k].fn != fn; k++) {}
            if (k == g_nsau) { g_sau[g_nsau].fn = fn; g_sau[g_nsau].name = _strdup(nm); g_nsau++; }
        }
        return;
    }
    if (!on || n > 20000) return;
    const char *a = guest_str(c->r[4].uw[0]), *b = guest_str(c->r[5].uw[0]);
    char line[160];
    snprintf(line, sizeof line, "%d %s %s", which, a ? a : "-", b ? b : "-");
    if (!strcmp(line, last)) return;
    strcpy(last, line); n++;
    fprintf(stderr, "[script] v%u %s a2=%08x\n", g_vsync_count, line, c->r[6].uw[0]);
}

void ps2_poll(R5900Context *c) {
    (void)c;
    ps2_poll_budget = 20000;
    deliver_interrupts(g_th[g_cur].ctx, 0);
    if (g_need_resched && !g_in_intr) schedule();
}

/* ------------------------------------------------------------ COP0 */
u32 ps2_cop0_read(R5900Context *c, int reg) {
    if (reg == 9) return (u32)(u64)(now_sec() * 294912000.0);   /* Count at CPU clock */
    if (reg == 12) return (c->cop0[12] & ~0x10001u) | (g_intr_enabled ? 0x10001u : 0);   /* EIE|IE live */
    if (reg == 15) return 0x2e20;                                 /* PRId: R5900 */
    return c->cop0[reg];
}
void ps2_cop0_write(R5900Context *c, int reg, u32 v) {
    c->cop0[reg] = v;
    if (reg == 12) g_intr_enabled = (v & 0x10001) == 0x10001;
}
void ps2_cop0_op(R5900Context *c, u32 pc, u32 w) {
    (void)c; (void)pc;
    switch (w & 63) {
    case 56: g_intr_enabled = 1; break;   /* EI */
    case 57: g_intr_enabled = 0; break;   /* DI */
    default: break;                        /* TLB ops / ERET: flat memory, no-op */
    }
}
int ps2_cop0_cond(R5900Context *c) { (void)c; return 1; }

/* ------------------------------------------------------------ IO registers owned by the kernel side */
static int kernel_io_read_reg(u32 p, u32 *out);
int kernel_io_read32(u32 a, u32 *out) {   /* sub-word reads shift the 32-bit register */
    u32 p = a & 0x1fffffff, sub = p & 3;
    if (!kernel_io_read_reg(p & ~3u, out)) return 0;
    *out >>= sub * 8;
    return 1;
}
static int kernel_io_read_reg(u32 p, u32 *out) {
    if (p >= 0x10000000 && p < 0x10002000 && (p & 0x7ff) < 0x40) {
        int n = (p >> 11) & 3; Timer *t = &g_tim[n];
        switch (p & 0x30) {
        case 0x00: *out = timer_count(n); return 1;
        case 0x10: *out = t->mode | (n == 3 ? 0x100 : 0);   /* T3 "in use" so SDK InitAlarm skips its patch */
                   return 1;
        case 0x20: *out = t->comp; return 1;
        case 0x30: *out = t->hold; return 1;
        }
    }
    if (p == 0x1000f000) { *out = g_intc_pending; return 1; }   /* I_STAT */
    if (p == 0x1000f010) { *out = g_intc_mask; return 1; }      /* I_MASK */
    return 0;
}
int kernel_io_write32(u32 a, u32 v) {
    u32 p = a & 0x1fffffff;
    if (p >= 0x10000000 && p < 0x10002000 && (p & 0x7ff) < 0x40) {
        int n = (p >> 11) & 3; Timer *t = &g_tim[n];
        switch (p & 0x30) {
        case 0x00: t->base_count = v & 0xffff; t->base_t = now_sec(); break;
        case 0x10: { u32 cnt = timer_count(n);
                     t->mode = (t->mode & ~0x3ffu & ~(v & 0xc00)) | (v & 0x3ff);   /* write 1 clears EQUF/OVFF */
                     t->base_count = cnt; t->base_t = now_sec(); } break;
        case 0x20: t->comp = v & 0xffff; break;
        case 0x30: t->hold = v & 0xffff; break;
        }
        timer_rearm(n);
        return 1;
    }
    if (p == 0x1000f000) { g_intc_pending &= ~v; return 1; }
    if (p == 0x1000f010) { g_intc_mask ^= v; return 1; }
    return 0;
}

/* ------------------------------------------------------------ syscalls */
#define SYSCALL_TABLE 0x00000800u
static u32 *syscall_slot(u32 n) { return (u32 *)(ps2_ram + SYSCALL_TABLE) + (n & 0xff); }
static u32 g_heap_end;

static int add_handler(Handler *tbl, int cause, u32 handler, u32 arg, u32 gp, int is2) {
    for (int i = 0; i < MAX_HANDLERS; i++) if (!tbl[i].used) {
        tbl[i] = (Handler){1, cause, handler, arg, gp, 1, is2};
        return i + 1;
    }
    return -1;
}

static int sema_wake_one(int sid) {
    int best = 0;
    for (int i = 1; i < MAX_THREADS; i++) {
        Thread *t = &g_th[i];
        if (t->used && t->status == TS_WAIT && t->wait_type == WAIT_SEMA && t->wait_id == sid &&
            (!best || t->ready_seq < g_th[best].ready_seq)) best = i;   /* FIFO */
    }
    if (!best) return 0;
    g_th[best].wait_type = WAIT_NONE;
    g_th[best].ctx->r[2].ud[0] = (u64)sid;
    g_sema[sid].waiters--;
    make_ready(best);
    return 1;
}

static void write_thread_status(int id, u32 p) {
    Thread *t = &g_th[id];
    MEM_W32(p + 0, t->status); MEM_W32(p + 4, t->entry); MEM_W32(p + 8, t->stack);
    MEM_W32(p + 12, t->stack_size); MEM_W32(p + 16, t->gp); MEM_W32(p + 20, t->init_prio);
    MEM_W32(p + 24, t->prio); MEM_W32(p + 28, t->attr); MEM_W32(p + 32, t->option);
    MEM_W32(p + 36, t->wait_type); MEM_W32(p + 40, t->wait_id); MEM_W32(p + 44, t->wakeups);
}

void ps2_syscall(R5900Context *c, u32 pc) {
    s32 n = c->r[3].sw[0];
    u32 num = (u32)(n < 0 ? -n : n) & 0xff;
    u32 a0 = c->r[4].uw[0], a1 = c->r[5].uw[0], a2 = c->r[6].uw[0], a3 = c->r[7].uw[0];
    s64 ret = 0;
    int self = g_cur;

    static int trace = -1;
    if (trace < 0) trace = getenv("PS2_TRACE") != NULL;
    if (trace && num != 0x64 && num != 0x32 && num != 0x33 && num != 0x34 && num != 0x2f)   /* skip per-frame noise */
        fprintf(stderr, "[sys] %s%s(%08x, %08x, %08x, %08x) pc=%08x t=%d\n", n < 0 ? "i:" : "",
                k_names[num] ? k_names[num] : "?", a0, a1, a2, a3, pc, g_cur);

    u32 user = *syscall_slot(num);
    if (user && user < 0x80000000u) {
        u64 ra = c->r[31].ud[0];
        c->r[31].ud[0] = pc + 4;
        ps2_call(c, user);
        c->r[31].ud[0] = ra;
        return;
    }
    /* user >= 0x80000000: SDK kernel-resident patch (alarm/TLB); we implement those natively below */

    switch (num) {
    case 0x74: *syscall_slot(a0) = a1; break;                                  /* SetSyscall */
    case 0x5a: case 0x5b: ret = 0; break;                                      /* kCopy/kCall without a guest handler */
    case 0x02: break;                                                          /* SetGsCrt */
    case 0x04: ps2_die("guest Exit(%d)", (int)a0); break;
    case 0x3c: {                                                               /* SetupThread */
        g_th[1].gp = a0;
        c->r[28].ud[0] = (u64)(s32)a0;
        u32 top = (a1 == 0xffffffffu) ? 0x02000000u : a1 + a2;
        g_th[1].stack = top - a2; g_th[1].stack_size = a2;
        ret = (s32)(top - 0x2a0);
        break; }
    case 0x3d: g_heap_end = (a1 == 0xffffffffu) ? g_th[1].stack : a0 + a1; ret = g_heap_end; break;
    case 0x3e: ret = g_heap_end; break;

    /* ---- interrupts */
    case 0x10: ret = add_handler(g_intc_h, a0, a1, a3, c->r[28].uw[0], 0); break;    /* AddIntcHandler(cause,h,next) */
    case 0x11: if (a1 >= 1 && a1 <= MAX_HANDLERS) g_intc_h[a1 - 1].used = 0; break;
    case 0x12: ret = add_handler(g_dmac_h, a0, a1, a3, c->r[28].uw[0], 0); break;    /* AddDmacHandler(ch,h,next,arg) */
    case 0x13: if (a1 >= 1 && a1 <= MAX_HANDLERS) g_dmac_h[a1 - 1].used = 0; break;
    case 0x14: case 0x1a: ret = !(g_intc_mask & (1u << a0)) ? 1 : 0; g_intc_mask |= 1u << a0; break;
    case 0x15: case 0x1b: ret = (g_intc_mask >> a0) & 1; g_intc_mask &= ~(1u << a0); break;
    case 0x16: case 0x1c: ret = !(g_dmac_mask & (1u << a0)) ? 1 : 0; g_dmac_mask |= 1u << a0; break;
    case 0x17: case 0x1d: ret = (g_dmac_mask >> a0) & 1; g_dmac_mask &= ~(1u << a0); break;
    case 0x5c: if (a1 >= 1 && a1 <= MAX_HANDLERS) g_intc_h[a1 - 1].enabled = 1; break;
    case 0x5d: if (a1 >= 1 && a1 <= MAX_HANDLERS) g_intc_h[a1 - 1].enabled = 0; break;
    case 0x5e: if (a1 >= 1 && a1 <= MAX_HANDLERS) g_dmac_h[a1 - 1].enabled = 1; break;
    case 0x5f: if (a1 >= 1 && a1 <= MAX_HANDLERS) g_dmac_h[a1 - 1].enabled = 0; break;

    /* ---- alarms (SDK: SetAlarm(u16 hsyncs, handler, arg)) */
    case 0xfc: case 0xfd: case 0x18: case 0x1e: {
        int i; for (i = 0; i < MAX_ALARMS && g_alarm[i].used; i++) {}
        if (i == MAX_ALARMS) { ret = -1; break; }
        g_alarm[i] = (Alarm){1, now_sec() + (a0 & 0xffff) / HSYNC_HZ, a1, a2, c->r[28].uw[0], (u16)a0};
        ret = i + 1; break; }
    case 0xfe: case 0xff: case 0x19: case 0x1f:
        if (a0 >= 1 && a0 <= MAX_ALARMS && g_alarm[a0 - 1].used) { g_alarm[a0 - 1].used = 0; ret = a0; }
        else ret = -1;
        break;

    /* ---- threads */
    case 0x20: ret = new_thread(a0); break;
    case 0x21: if (valid_thread(a0) && (int)a0 != g_cur && g_th[a0].status == TS_DORMANT) {
                   DeleteFiber(g_th[a0].fiber); free(g_th[a0].ctx); g_th[a0].used = 0; ret = 0; } else ret = -1;
               break;
    case 0x22:                                                                  /* StartThread(id,arg) */
        if (!valid_thread(a0) || g_th[a0].status != TS_DORMANT) { ret = -1; break; }
        g_th[a0].arg = a1; g_th[a0].prio = g_th[a0].init_prio; g_th[a0].wakeups = 0;
        if (g_th[a0].started) g_th[a0].restart = 1;
        g_th[a0].started = 1;
        make_ready(a0); ret = 0; break;
    case 0x23: case 0x24:                                                       /* Exit(Delete)Thread */
        g_th[g_cur].status = TS_DORMANT;
        longjmp(g_th[g_cur].base, 1);   /* fiber loop marks dormant + reschedules; main thread has no base */
    case 0x25: case 0x26:                                                       /* TerminateThread */
        if (!valid_thread(a0) || (int)a0 == g_cur) { ret = -1; break; }
        if (g_th[a0].status == TS_WAIT && g_th[a0].wait_type == WAIT_SEMA) g_sema[g_th[a0].wait_id].waiters--;
        g_th[a0].status = TS_DORMANT; g_th[a0].wait_type = WAIT_NONE; ret = 0; break;
    case 0x27: g_dispatch_disabled = 1; break;
    case 0x28: g_dispatch_disabled = 0; break;
    case 0x29: case 0x2a: {                                                     /* ChangeThreadPriority(id,prio) */
        int id = a0 ? (int)a0 : g_cur;
        if (!valid_thread(id)) { ret = -1; break; }
        ret = g_th[id].prio; g_th[id].prio = (int)a1;
        if (g_th[id].status == TS_READY) g_th[id].ready_seq = ++g_seq;
        g_need_resched = 1; break; }
    case 0x2b: case 0x2c: {                                                     /* RotateThreadReadyQueue(prio) */
        if (g_th[g_cur].prio == (int)a0) { g_th[g_cur].status = TS_READY; g_th[g_cur].ready_seq = ++g_seq; }
        else for (int i = 1; i < MAX_THREADS; i++)
            if (g_th[i].used && g_th[i].status == TS_READY && g_th[i].prio == (int)a0) { g_th[i].ready_seq = ++g_seq; break; }
        g_need_resched = 1; break; }
    case 0x2d: case 0x2e:                                                       /* ReleaseWaitThread */
        if (valid_thread(a0) && g_th[a0].status == TS_WAIT) {
            if (g_th[a0].wait_type == WAIT_SEMA) g_sema[g_th[a0].wait_id].waiters--;
            g_th[a0].wait_type = WAIT_NONE; g_th[a0].ctx->r[2].ud[0] = (u64)-418;   /* KE_RELEASE_WAIT */
            make_ready(a0); ret = 0;
        } else ret = -1;
        break;
    case 0x2f: ret = g_cur; break;
    case 0x30: case 0x31: { int id = a0 ? (int)a0 : g_cur;
        if (!valid_thread(id)) { ret = -1; break; }
        if (a1) write_thread_status(id, a1);
        ret = g_th[id].status; break; }
    case 0x32:                                                                  /* SleepThread */
        if (g_th[g_cur].wakeups > 0) { g_th[g_cur].wakeups--; ret = 0; break; }
        c->r[2].ud[0] = 0;
        block_current(WAIT_SLEEP, 0);
        return;
    case 0x33: case 0x34:                                                       /* WakeupThread */
        if (!valid_thread(a0)) { ret = -1; break; }
        if (g_th[a0].status == TS_WAIT && g_th[a0].wait_type == WAIT_SLEEP) {
            g_th[a0].wait_type = WAIT_NONE; make_ready(a0);
        } else g_th[a0].wakeups++;
        ret = 0; break;
    case 0x35: case 0x36: { int id = a0 ? (int)a0 : g_cur;
        if (!valid_thread(id)) { ret = -1; break; }
        ret = g_th[id].wakeups; g_th[id].wakeups = 0; break; }
    case 0x37: case 0x38: case 0x39: case 0x3a: ret = 0; break;                 /* Suspend/Resume: TODO */

    /* ---- semaphores */
    case 0x40: { int s; for (s = 1; s < MAX_SEMAS && g_sema[s].used; s++) {}
        if (s == MAX_SEMAS) { ret = -1; break; }
        g_sema[s] = (Sema){1, (int)MEM_R32(a0 + 8), (int)MEM_R32(a0 + 4), (int)MEM_R32(a0 + 8), 0,
                           MEM_R32(a0 + 16), MEM_R32(a0 + 20)};
        ret = s; break; }
    case 0x41: if (a0 < MAX_SEMAS && g_sema[a0].used) {
                   while (g_sema[a0].waiters > 0 && sema_wake_one(a0)) {}
                   g_sema[a0].used = 0; ret = a0; } else ret = -1;
               break;
    case 0x42: case 0x43:                                                       /* (i)SignalSema */
        if (a0 >= MAX_SEMAS || !g_sema[a0].used) { ret = -1; break; }
        if (!sema_wake_one(a0)) g_sema[a0].count++;
        ret = a0; break;
    case 0x44:                                                                  /* WaitSema */
        if (a0 >= MAX_SEMAS || !g_sema[a0].used) { ret = -1; break; }
        if (g_sema[a0].count > 0) { g_sema[a0].count--; ret = a0; break; }
        g_sema[a0].waiters++;
        g_th[g_cur].ready_seq = ++g_seq;   /* FIFO position among waiters */
        block_current(WAIT_SEMA, a0);
        return;                            /* v0 set by the waker */
    case 0x45: case 0x46:                                                       /* (i)PollSema */
        if (a0 >= MAX_SEMAS || !g_sema[a0].used) { ret = -1; break; }
        if (g_sema[a0].count > 0) { g_sema[a0].count--; ret = a0; } else ret = -419;
        break;
    case 0x47: case 0x48:
        if (a0 >= MAX_SEMAS || !g_sema[a0].used) { ret = -1; break; }
        MEM_W32(a1 + 0, g_sema[a0].count); MEM_W32(a1 + 4, g_sema[a0].max); MEM_W32(a1 + 8, g_sema[a0].init);
        MEM_W32(a1 + 12, g_sema[a0].waiters); MEM_W32(a1 + 16, g_sema[a0].attr); MEM_W32(a1 + 20, g_sema[a0].option);
        ret = a0; break;

    /* ---- misc */
    case 0x4a: break;                                                           /* SetOsdConfigParam */
    case 0x4b: MEM_W32(a0, 1u << 16); break;                                    /* GetOsdConfigParam: English, 4:3 */
    case 0x6e: break;
    case 0x6f: MEM_W32(a0, 0); break;
    case 0x55: case 0x56: case 0x57: case 0x58: ret = a0; break;                /* TLB patch: flat memory */
    case 0x59: ret = 0; break;                                                  /* ExpandScratchPad */
    case 0x60: case 0x61: case 0x62: case 0x64: case 0x68: break;               /* caches */
    case 0x63: case 0x67: ret = (s32)ps2_cop0_read(c, a0 & 31); break;
    case 0x66: case 0x6a: break;
    case 0x70: ret = 0x7f00; break;                                             /* GsGetIMR */
    case 0x71: break;                                                           /* GsPutIMR */
    case 0x73: break;                                                           /* SetVSyncFlag */
    case 0x75: break;                                                           /* _print */
    case 0x7d: ret = 0; break;
    case 0x7e: ret = 0; break;                                                  /* MachineType: retail */
    case 0x7f: ret = 32 << 20; break;
    case 0x7c: ret = 0; break;                                                  /* Deci2Call: no debugger */

    /* ---- SIF (EE<->IOP): HLE IOP in iop.c */
    case 0x79: break;                                                           /* sceSifSetReg */
    case 0x7a: ret = iop_sif_get_reg(a0); break;                                /* sceSifGetReg */
    case 0x76: ret = -1; break;                                                 /* sceSifDmaStat: done */
    case 0x77: ret = iop_sif_set_dma(a0, a1); break;                            /* sceSifSetDma */
    case 0x78: break;                                                           /* sceSifSetDChain */

    default:
        fprintf(stderr, "[syscall] unhandled %s%s (0x%02x) a0=%08x a1=%08x a2=%08x a3=%08x pc=%08x thread=%d\n",
                n < 0 ? "i:" : "", k_names[num] ? k_names[num] : "?", num, a0, a1, a2, a3, pc, g_cur);
        break;
    }
    (void)self;
    c->r[2].ud[0] = (u64)ret;   /* c is the interrupt context when called from a handler */
    if (g_need_resched && !g_in_intr && n >= 0) schedule();
}

/* ------------------------------------------------------------ init */
void kernel_init(R5900Context *main_ctx) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_qpc_to_sec = 1.0 / (double)f.QuadPart;
    QueryPerformanceCounter(&g_qpc0);
    g_next_vblank = 1.0 / VSYNC_HZ;
    iop_init();
    g_main_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    Thread *t = &g_th[1];
    t->used = 1; t->status = TS_RUN; t->prio = t->init_prio = 0; t->started = 1;
    t->ctx = main_ctx; t->fiber = g_main_fiber;
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    { void profiler_start(void); profiler_start(); }
}

/* main() does `if (setjmp(*kernel_main_jmpbuf())) kernel_main_exited();` before booting,
 * so ExitThread on the main thread has a live frame to unwind to. */
jmp_buf *kernel_main_jmpbuf(void) { return &g_th[1].base; }
void kernel_main_exited(void) {
    g_th[1].status = TS_DORMANT;
    for (;;) schedule();
}

R5900Context *kernel_current_ctx(void) { return g_th[g_cur].ctx; }
