/* PS2_PROFILE=1: sampling profiler for the EE thread (the OS thread that runs all guest fibers).
 * A host thread suspends it every ~1 ms, records RIP, and every 10 s prints the hottest
 * functions (symbolized with DbgHelp; needs the PDB the build produces). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>

#define NBUCKET (1 << 16)
static struct { DWORD64 addr; unsigned n; } g_samp[NBUCKET];
static HANDLE g_target;
static unsigned g_total;

static void report(void) {
    /* aggregate by function start */
    static struct { DWORD64 fn; unsigned n; char name[96]; } fns[4096];
    int nf = 0;
    HANDLE proc = GetCurrentProcess();
    char buf[sizeof(SYMBOL_INFO) + 256];
    for (int i = 0; i < NBUCKET; i++) {
        if (!g_samp[i].n) continue;
        SYMBOL_INFO *s = (SYMBOL_INFO *)buf; memset(buf, 0, sizeof buf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 255;
        DWORD64 disp = 0, fn = g_samp[i].addr; const char *name = "?";
        if (SymFromAddr(proc, g_samp[i].addr, &disp, s)) { fn = s->Address; name = s->Name; }
        if (disp > 0x1000 && g_samp[i].n > 2) {   /* far from its symbol: probably unsymbolized code */
            DWORD64 base = SymGetModuleBase64(proc, g_samp[i].addr);
            fprintf(stderr, "[prof] raw %u samples at %s+0x%llx (module rva 0x%llx)\n", g_samp[i].n, name,
                    (unsigned long long)disp, (unsigned long long)(g_samp[i].addr - base));
        }
        int k; for (k = 0; k < nf && fns[k].fn != fn; k++) {}
        if (k == nf) { if (nf == 4096) continue; fns[nf].fn = fn; fns[nf].n = 0; snprintf(fns[nf].name, sizeof fns[0].name, "%s", name); nf++; }
        fns[k].n += g_samp[i].n;
    }
    fprintf(stderr, "[prof] %u samples, top functions:\n", g_total);
    for (int r = 0; r < 25; r++) {
        int best = -1;
        for (int k = 0; k < nf; k++) if (fns[k].n && (best < 0 || fns[k].n > fns[best].n)) best = k;
        if (best < 0) break;
        fprintf(stderr, "[prof] %5.1f%%  %s\n", 100.0 * fns[best].n / (g_total ? g_total : 1), fns[best].name);
        fns[best].n = 0;
    }
    /* PS2_PROFILE=lines: also the hottest source lines */
    const char *mode = getenv("PS2_PROFILE");
    if (mode && !strcmp(mode, "lines")) {
        static struct { DWORD line; char file[40]; unsigned n; } ln[2048];
        int nl = 0;
        for (int i = 0; i < NBUCKET; i++) {
            if (!g_samp[i].n) continue;
            IMAGEHLP_LINE64 L; memset(&L, 0, sizeof L); L.SizeOfStruct = sizeof L; DWORD disp = 0;
            if (!SymGetLineFromAddr64(proc, g_samp[i].addr, &disp, &L)) continue;
            const char *f = strrchr(L.FileName, '\\'); f = f ? f + 1 : L.FileName;
            int k; for (k = 0; k < nl && !(ln[k].line == L.LineNumber && !strcmp(ln[k].file, f)); k++) {}
            if (k == nl) { if (nl == 2048) continue; ln[nl].line = L.LineNumber; snprintf(ln[nl].file, sizeof ln[0].file, "%s", f); ln[nl].n = 0; nl++; }
            ln[k].n += g_samp[i].n;
        }
        for (int r = 0; r < 30; r++) {
            int best = -1;
            for (int k = 0; k < nl; k++) if (ln[k].n && (best < 0 || ln[k].n > ln[best].n)) best = k;
            if (best < 0) break;
            fprintf(stderr, "[prof]   %5.1f%%  %s:%lu\n", 100.0 * ln[best].n / (g_total ? g_total : 1), ln[best].file, ln[best].line);
            ln[best].n = 0;
        }
    }
    memset(g_samp, 0, sizeof g_samp); g_total = 0;
}

static DWORD WINAPI sampler(void *p) {
    (void)p;
    timeBeginPeriod(1);
    DWORD last = GetTickCount();
    for (;;) {
        Sleep(1);
        if (SuspendThread(g_target) == (DWORD)-1) continue;
        CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_target, &ctx)) {
            DWORD64 a = ctx.Rip;
            { /* samples inside the C runtime's memcpy/memset (leaf code): attribute to the caller instead */
              static DWORD64 lo, hi;
              if (!lo) { HMODULE m = GetModuleHandleA("vcruntime140.dll");
                         if (m) { lo = (DWORD64)m; hi = lo + 0x20000; } else lo = 1; }
              if (a >= lo && a < hi) { DWORD64 ra = *(const DWORD64 *)ctx.Rsp; if (ra) a = ra; } }
            unsigned h = (unsigned)((a >> 2) * 2654435761u) & (NBUCKET - 1);
            for (int probe = 0; probe < 64; probe++, h = (h + 1) & (NBUCKET - 1)) {
                if (g_samp[h].addr == a || !g_samp[h].n) { g_samp[h].addr = a; g_samp[h].n++; break; }
            }
            g_total++;
        }
        ResumeThread(g_target);
        if (GetTickCount() - last > 10000) { report(); last = GetTickCount(); }
    }
    return 0;
}

/* PS2_PROFILE_GS=1: sample the GS thread instead of the EE thread (gif.c registers it) */
void profiler_set_gs_thread(HANDLE h) {
    if (!getenv("PS2_PROFILE_GS")) return;
    HANDLE d; DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_target = d;
}

void profiler_start(void) {
    if (!getenv("PS2_PROFILE")) return;
    if (!g_target) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
}
