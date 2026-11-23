/* Host runtime: memory, ELF data load, dispatch, MMIO fallback, boot.
 * Kernel (syscalls/threads/interrupts/timers/COP0) lives in kernel.c. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>
#include "ps2_runtime.h"

u8 *ps2_ram, *ps2_spr;
char g_game_root[1024] = ".";   /* extracted disc folder = directory of the ELF passed on the command line */
static R5900Context g_ctx;

void kernel_init(R5900Context *main_ctx);
jmp_buf *kernel_main_jmpbuf(void);
void kernel_main_exited(void);
int kernel_io_read32(u32 a, u32 *out);
int kernel_io_write32(u32 a, u32 v);

void ps2_die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr); exit(1);
}

static void dump_ram(void) {
    FILE *d = fopen("ramdump.bin", "wb");   /* for offline analysis with tools/probe.py */
    if (d) { fwrite(ps2_ram, 1, 32u << 20, d); fclose(d); }
}

/* ------------------------------------------------------------ store watch (PS2_WATCH) */
u32 g_watch_qw[2];
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
void ps2_watch_hit(u32 a, const void *val, int size) {
    extern int g_frame_no_pub;
    static int from = -2, n; if (from == -2) { const char *e = getenv("PS2_WATCH_FROM"); from = e ? atoi(e) : 0; }
    if (g_frame_no_pub < from || n >= 400) return;
    n++;
    static int sym; if (!sym) { sym = 1; SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME); SymInitialize(GetCurrentProcess(), NULL, TRUE); }
    void *frames[24]; USHORT nf = RtlCaptureStackBackTrace(1, 24, frames, NULL);
    char names[640]; int nl = 0; names[0] = 0;
    for (int i = 0; i < nf && nl < 600; i++) {   /* the call chain, skipping the MEM_W* helpers */
        char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *s = (SYMBOL_INFO *)buf; memset(buf, 0, sizeof buf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 127; DWORD64 disp = 0;
        const char *name = SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &disp, s) ? s->Name : "?";
        if (!strncmp(name, "MEM_W", 5) || !strcmp(name, "ps2_call")) continue;
        nl += snprintf(names + nl, sizeof names - nl, "%s%s", nl ? " < " : "", name);
    }
    u32 w[4] = {0}; memcpy(w, val, size > 16 ? 16 : size);
    float f[4]; memcpy(f, w, 16);
    fprintf(stderr, "[wstore] f%d %08x <- %d bytes %08x %08x %08x %08x (%g %g %g) by %s\n", g_frame_no_pub, a, size,
            w[0], w[1], w[2], w[3], f[0], f[1], f[2], names);
}

/* ------------------------------------------------------------ dispatch */
static const PS2FuncEntry *find_func(u32 addr) {
    unsigned lo = 0, hi = ps2_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (ps2_func_table[mid].start <= addr) lo = mid + 1; else hi = mid;
    }
    return lo ? &ps2_func_table[lo - 1] : NULL;
}

void ps2_call(R5900Context *c, u32 addr) {
    { extern int g_scriptlog; extern void dbg_script_native(u32); if (g_scriptlog > 0) dbg_script_native(addr); }
    const PS2FuncEntry *f = find_func(addr);
    if (!f || f->start != addr) {
        /* Entry into the middle of a recompiled function: add the address to
         * recomp/extra_entries.txt, then rerun tools/ps2recomp.py. */
        dump_ram();
        ps2_die("ps2_call: no function starts at 0x%08x (nearest 0x%08x, ra=0x%08x); RAM saved to ramdump.bin",
                addr, f ? f->start : 0, c->r[31].uw[0]);
    }
    c->pc = addr;
    f->fn(c);
    { extern int g_scriptlog; extern void dbg_script_ret(u32, R5900Context *); if (g_scriptlog > 0) dbg_script_ret(addr, c); }
}

void ps2_jump(R5900Context *c, u32 addr) { ps2_call(c, addr); }

void ps2_break(R5900Context *c, u32 pc, u32 code) { dump_ram(); ps2_die("break 0x%x at 0x%08x (ra=0x%08x)", code, pc, c->r[31].uw[0]); }
void ps2_trap(R5900Context *c, u32 pc) { ps2_die("trap at 0x%08x (ra=0x%08x)", pc, c->r[31].uw[0]); }
void ps2_invalid(R5900Context *c, u32 pc, u32 w) { (void)c; ps2_die("invalid insn %08x at 0x%08x", w, pc); }
void ps2_unimpl(R5900Context *c, u32 pc, u32 w, const char *what) {
    dump_ram();
    ps2_die("unimplemented %s (%08x) at 0x%08x (ra=0x%08x)", what, w, pc, c->r[31].uw[0]);
}

/* ------------------------------------------------------------ MMIO fallback (TODO: DMAC/GIF/VIF/GS/IPU/SIF) */
static int io_first_time(u32 a, int write) {
    static u8 seen[2][0x10000];
    u32 k = ((a >> 4) ^ (a >> 20)) & 0xffff;
    if (seen[write][k]) return 0;
    seen[write][k] = 1; return 1;
}
int hw_io_read(u32 a, u64 *out);
int hw_io_write(u32 a, u64 v);
int hw_fifo_write(u32 a, const u8 *q);
void gs_init(void);
void win_init(void);
#define IO_STUB(bits, T) \
    T ps2_io_read##bits(u32 a) { T v; memset(&v, 0, sizeof v); u32 k; u64 q; \
        if (hw_io_read(a, &q)) { memcpy(&v, &q, sizeof v < 8 ? sizeof v : 8); return v; } \
        if (kernel_io_read32(a, &k)) { memcpy(&v, &k, sizeof v < 4 ? sizeof v : 4); return v; } \
        if (io_first_time(a, 0)) fprintf(stderr, "[io] r%d %08x\n", bits, a); return v; } \
    void ps2_io_write##bits(u32 a, T v) { u32 k = 0; u64 q = 0; \
        if (sizeof(T) == 16 && hw_fifo_write(a, (const u8 *)&v)) return; \
        memcpy(&k, &v, sizeof v < 4 ? sizeof v : 4); memcpy(&q, &v, sizeof v < 8 ? sizeof v : 8); \
        if (hw_io_write(a, q)) return; \
        if (kernel_io_write32(a, k)) return; \
        if (io_first_time(a, 1)) fprintf(stderr, "[io] w%d %08x = %08x\n", bits, a, k); }
IO_STUB(8, u8) IO_STUB(16, u16) IO_STUB(32, u32) IO_STUB(64, u64) IO_STUB(128, u128)

/* ------------------------------------------------------------ boot */
static void load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) ps2_die("cannot open %s (pass the path to SLUS_213.22)", path);
    u8 hdr[52];
    if (fread(hdr, 1, 52, f) != 52 || memcmp(hdr, "\x7f" "ELF", 4)) ps2_die("%s is not an ELF", path);
    u32 phoff; u16 phentsize, phnum;
    memcpy(&phoff, hdr + 28, 4); memcpy(&phentsize, hdr + 42, 2); memcpy(&phnum, hdr + 44, 2);
    for (unsigned i = 0; i < phnum; i++) {
        u32 ph[8];
        fseek(f, phoff + i * phentsize, SEEK_SET);
        if (fread(ph, 4, 8, f) != 8) ps2_die("bad program header");
        if (ph[0] != 1) continue;
        u32 vaddr = ph[2] & 0x01ffffff;
        if (vaddr + ph[5] > 0x02000000) ps2_die("segment outside RAM");
        memset(ps2_ram + vaddr, 0, ph[5]);
        fseek(f, ph[1], SEEK_SET);
        if (fread(ps2_ram + vaddr, 1, ph[4], f) != ph[4]) ps2_die("short segment read");
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *elf = argc > 1 ? argv[1] : "SLUS_213.22";
    {
        snprintf(g_game_root, sizeof g_game_root, "%s", elf);
        char *sl = strrchr(g_game_root, '\\'), *sl2 = strrchr(g_game_root, '/');
        if (sl2 > sl) sl = sl2;
        if (sl) *sl = 0; else snprintf(g_game_root, sizeof g_game_root, ".");
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    { const char *w = getenv("PS2_WATCH");   /* PS2_WATCH=addr[,addr2] (hex): log stores to those 16-byte blocks */
      if (w) { unsigned a = 0, b = 0; sscanf(w, "%x,%x", &a, &b); g_watch_qw[0] = a & 0x1FFFFFF0u; g_watch_qw[1] = (b ? b : a) & 0x1FFFFFF0u; } }
    ps2_ram = calloc(1, 32u << 20);
    ps2_spr = calloc(1, 16u << 10);
    if (!ps2_ram || !ps2_spr) ps2_die("out of memory");
    load_elf(elf);   /* data segments; code is already compiled in */
    R5900Context *c = &g_ctx;
    c->ram = ps2_ram; c->spr = ps2_spr;
    c->vf[0].f[3] = 1.0f;
    c->r[29].ud[0] = 0x01fff000;  /* crt0 replaces this via SetupThread */
    gs_init();
    win_init();
    kernel_init(c);
    if (setjmp(*kernel_main_jmpbuf())) kernel_main_exited();
    fprintf(stderr, "booting at 0x%08x (%u recompiled functions)\n", ps2_entry_point, ps2_func_count);
    ps2_call(c, ps2_entry_point);
    fprintf(stderr, "main returned\n");
    kernel_main_exited();
    return 0;
}
