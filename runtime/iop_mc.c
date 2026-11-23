/* MCSERV (sid 0x80000400) replacement: PS2 memory cards backed by host folders memcard/mc<port>/.
 * Request layouts are taken from this game's libmc (EE 0x118600-0x1197B0):
 *   name param (1044 B): {port, slot, flags, maxent, table*, char name[1024]}
 *   desc param (48 B):   fn-specific, see each case. Result = 4-byte int to the EE.
 * Results follow the SDK sceMcRes* codes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "iop.h"

void iop_ee_write(u32 ee, const void *src, u32 n);
void iop_ee_read(void *dst, u32 ee, u32 n);

enum { MC_OK = 0, MC_CHANGED = -1, MC_NOENTRY = -4, MC_DENIED = -5, MC_NOTEMPTY = -6, MC_UPLIMIT = -7 };

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static int g_seen_card[2];
static char g_cwd[2][1024] = {"/", "/"};

typedef struct { FILE *f; int used; } McFile;
static McFile g_fd[16];

/* mc path ("/DIR/FILE" or relative) -> host path "memcard/mcN/DIR/FILE" */
static void host_path(int port, const char *mcpath, char *out, size_t n) {
    char full[2048];
    if (mcpath[0] == '/') snprintf(full, sizeof full, "%s", mcpath);
    else snprintf(full, sizeof full, "%s%s%s", g_cwd[port & 1], g_cwd[port & 1][strlen(g_cwd[port & 1]) - 1] == '/' ? "" : "/", mcpath);
    snprintf(out, n, "memcard/mc%d%s", port & 1, full);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}

static void ensure_root(int port) {
    char p[64]; _mkdir("memcard"); snprintf(p, sizeof p, "memcard\\mc%d", port & 1); _mkdir(p);
}

static void filetime_to_mc(const FILETIME *ft, u8 out[8]) {
    SYSTEMTIME st; FileTimeToSystemTime(ft, &st);
    out[0] = 0; out[1] = (u8)st.wSecond; out[2] = (u8)st.wMinute; out[3] = (u8)st.wHour;
    out[4] = (u8)st.wDay; out[5] = (u8)st.wMonth; memcpy(out + 6, &st.wYear, 2);
}

/* sceMcTblGetDir entry: create[8] modify[8] size u32, attr u16, resv u16, resv u32, pda u32, name[32] */
static void make_entry(u8 *e, const char *name, int is_dir, u32 size, const FILETIME *ct, const FILETIME *mt) {
    memset(e, 0, 64);
    filetime_to_mc(ct, e); filetime_to_mc(mt, e + 8);
    memcpy(e + 16, &size, 4);
    u16 attr = is_dir ? 0x8427 : 0x8497;
    memcpy(e + 20, &attr, 2);
    strncpy((char *)e + 32, name, 31);
}

static int mc_getdir(int port, const char *pattern, int flags, int maxent, u32 ee_table) {
    if (flags) return 0;                 /* continuation: everything was returned on the first call */
    char hp[2048]; host_path(port, pattern, hp, sizeof hp);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(hp, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (n >= maxent) break;
        u8 e[64];
        make_entry(e, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, fd.nFileSizeLow,
                   &fd.ftCreationTime, &fd.ftLastWriteTime);
        iop_ee_write(ee_table + n * 64, e, 64);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static void mc_call(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv) {
    (void)insz; (void)ee_recv;
    s32 res = 0;
    int port = (int)rd32(in) & 1;
    const char *name = (const char *)in + 20;
    char hp[2048];
    switch (fno) {
    case 0xfe: {                                   /* init: {result, mcserv ver, mcman ver} */
        u32 r[3] = {0, 0x20a, 0x20e}; memcpy(out, r, 12); *outsz = 12; return; }
    case 1: {                                      /* GetInfo: desc {?, port, slot, wantFmt, wantFree, wantType, ?, ee_buf} */
        port = (int)rd32(in + 4) & 1;
        u32 slot = rd32(in + 8), ee_buf = rd32(in + 28);
        ensure_root(port);
        if (slot != 0 || port != 0) { res = -10; break; }   /* only mc0 slot 0 is inserted */
        u8 info[0x94] = {0};
        u32 type = 2, freekb = 8000, fmt = 1;               /* PS2 card, ~8 MB free, formatted */
        memcpy(info + 0, &type, 4); memcpy(info + 4, &freekb, 4); memcpy(info + 0x90, &fmt, 4);
        iop_ee_write(ee_buf, info, sizeof info);
        res = g_seen_card[port] ? MC_OK : MC_CHANGED;       /* first call reports a newly inserted card */
        g_seen_card[port] = 1;
        break; }
    case 2: {                                      /* Open / Mkdir (flags 0x40) */
        u32 flags = rd32(in + 8);
        ensure_root(port);
        host_path(port, name, hp, sizeof hp);
        if (flags == 0x40) {
            res = _mkdir(hp) == 0 ? MC_OK : MC_NOENTRY;
            break;
        }
        int i; for (i = 0; i < 16 && g_fd[i].used; i++) {}
        if (i == 16) { res = MC_UPLIMIT; break; }
        const char *mode = (flags & 0x200) ? ((flags & 3) == 1 ? "rb" : "w+b") : ((flags & 3) == 1 ? "rb" : "r+b");
        FILE *f = fopen(hp, mode);
        if (!f && (flags & 0x200)) f = fopen(hp, "w+b");
        if (!f) { res = MC_NOENTRY; break; }
        g_fd[i].f = f; g_fd[i].used = 1; res = i;
        break; }
    case 3: {                                      /* Close: desc {fd} */
        u32 fd = rd32(in);
        if (fd < 16 && g_fd[fd].used) { fclose(g_fd[fd].f); g_fd[fd].used = 0; res = 0; } else res = MC_DENIED;
        break; }
    case 4: {                                      /* Seek: desc {fd, .., .., .., offset@16, origin@20} */
        u32 fd = rd32(in);
        if (fd >= 16 || !g_fd[fd].used) { res = MC_DENIED; break; }
        fseek(g_fd[fd].f, (long)(s32)rd32(in + 16), (int)rd32(in + 20));
        res = (s32)ftell(g_fd[fd].f);
        break; }
    case 5: {                                      /* Read: desc {fd, .., .., size@12, .., .., buf@24, bounce@28} */
        u32 fd = rd32(in), size = rd32(in + 12), buf = rd32(in + 24), bounce = rd32(in + 28);
        u8 zero[16] = {0};
        iop_ee_write(bounce, zero, 16);           /* no unaligned head/tail: callback copies nothing */
        if (fd >= 16 || !g_fd[fd].used) { res = MC_DENIED; break; }
        static u8 tmp[1 << 20];
        u32 n = (u32)fread(tmp, 1, size < sizeof tmp ? size : sizeof tmp, g_fd[fd].f);
        iop_ee_write(buf, tmp, n);
        res = (s32)n;
        break; }
    case 6: {                                      /* Write: head bytes in desc+32 (count@20), rest @24 (count@12) */
        u32 fd = rd32(in), head = rd32(in + 20), rest = rd32(in + 12), buf = rd32(in + 24);
        if (fd >= 16 || !g_fd[fd].used) { res = MC_DENIED; break; }
        if (head > 16) head = 16;
        fwrite(in + 32, 1, head, g_fd[fd].f);
        static u8 tmp[1 << 20];
        if (rest > sizeof tmp) rest = sizeof tmp;
        iop_ee_read(tmp, buf, rest);
        fwrite(tmp, 1, rest, g_fd[fd].f);
        res = (s32)(head + rest);
        break; }
    case 10: {                                     /* Flush: desc {fd} */
        u32 fd = rd32(in);
        if (fd < 16 && g_fd[fd].used) fflush(g_fd[fd].f);
        res = 0; break; }
    case 12: {                                     /* ChDir: name param; table@16 = EE buffer for old cwd */
        u32 oldbuf = rd32(in + 16);
        if (oldbuf) iop_ee_write(oldbuf, g_cwd[port], (u32)strlen(g_cwd[port]) + 1);
        host_path(port, name, hp, sizeof hp);
        DWORD a = GetFileAttributesA(hp);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { res = MC_NOENTRY; break; }
        if (name[0] == '/') snprintf(g_cwd[port], sizeof g_cwd[port], "%s", name);
        else { size_t l = strlen(g_cwd[port]); snprintf(g_cwd[port] + l, sizeof g_cwd[port] - l, "%s%s", g_cwd[port][l - 1] == '/' ? "" : "/", name); }
        res = 0; break; }
    case 13:                                       /* GetDir: name param {port, slot, flags, maxent, table*} */
        res = mc_getdir(port, name, (int)rd32(in + 8), (int)rd32(in + 12), rd32(in + 16));
        break;
    case 15: {                                     /* Delete */
        host_path(port, name, hp, sizeof hp);
        DWORD a = GetFileAttributesA(hp);
        if (a == INVALID_FILE_ATTRIBUTES) res = MC_NOENTRY;
        else if (a & FILE_ATTRIBUTE_DIRECTORY) res = RemoveDirectoryA(hp) ? 0 : MC_NOTEMPTY;
        else res = DeleteFileA(hp) ? 0 : MC_DENIED;
        break; }
    case 16: ensure_root(port); res = 0; break;    /* Format */
    case 20: res = 0; break;                       /* ChgPrity */
    default:
        fprintf(stderr, "[mc] unhandled fn %u\n", fno);
        res = MC_DENIED;
        break;
    }
    memcpy(out, &res, 4);
    *outsz = 4;
    if (getenv("PS2_TRACE")) fprintf(stderr, "[mc] fn=%u port=%d name=\"%.40s\" -> %d\n", fno, port, (fno == 2 || fno == 12 || fno == 13 || fno == 15) ? name : "", res);
}

const IopServer iop_server_mcserv = {0x80000400u, "mcserv", mc_call};

