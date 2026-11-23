/* FILE.IRX (Stormfront SfFile) replacement: serves files from the extracted disc folder.
 *
 * Batch commands (SFIOP type 2, 36 bytes; s = cmd+8):
 *   s[0] subcommand, s[1] request id (echoed in responses), s[2..] arguments.
 *   1  path fragment: bytes s+4..s+20 -> path[s[5] .. s[5]+s[6]]
 *   3  queue load of `path` into EE memory: size s[2], EE buffer s[3], flags s[4]
 *   12 status snapshot            15 query request status (handle s[2])
 * Responses (SFIOP type 2, size 36, body r = entry+8): r[0] code, r[1] request id, ...
 *   2 global status   3 queued {handle, status}   4 per-stream status   8 status {handle, status, size}
 * Request states (FILE.IRX asserts): 5 QUEUED, 6 LOADING, 7 LOADED; EE treats 2/3/4 as errors.
 * Loads are performed synchronously, so requests go straight to LOADED. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "iop.h"
#include "iop_sfiop.h"

void iop_ee_write(u32 ee, const void *src, u32 n);
extern char g_game_root[];

enum { ST_QUEUED = 5, ST_LOADING = 6, ST_LOADED = 7, ST_NOTFOUND = 3 };

static char g_path[512];
typedef struct { u32 reqid, handle, status, size; } Req;
/* Requests live in a ring indexed by handle: the game never releases them explicitly, and world
 * streaming issues far more than a fixed table can hold. Old completed entries are recycled. */
#define NREQ 4096
static Req g_req[NREQ];
static u32 g_next_handle = 0x100;
static Req *alloc_req(u32 reqid) {
    u32 h = g_next_handle++;
    Req *r = &g_req[h & (NREQ - 1)];
    memset(r, 0, sizeof *r);
    r->reqid = reqid; r->handle = h;
    return r;
}
/* handle -> path, kept after release so later chunk reads (sub 5) can reopen the file */
static char g_hpath[NREQ][128];
static void remember(u32 handle, const char *path) { snprintf(g_hpath[handle & (NREQ - 1)], sizeof g_hpath[0], "%s", path); }
static int g_trace = -1;

static u32 rd(const u8 *p, int word) { u32 v; memcpy(&v, p + word * 4, 4); return v; }

static u32 *resp(u32 code, u32 reqid) {
    u8 *e = sfiop_response(SFIOP_TYPE_FILE, 36);
    if (!e) return NULL;
    u32 *r = (u32 *)(e + 8);
    r[0] = code; r[1] = reqid;
    return r;
}

static Req *find_req(u32 handle) {
    Req *r = &g_req[handle & (NREQ - 1)];
    return r->handle == handle ? r : NULL;
}

/* "wad/boot.wad" -> "<root>\wad\boot.wad" (Windows is case-insensitive). */
static FILE *open_game_file(const char *rel, char *full, size_t n) {
    while (*rel == '/' || *rel == '\\') rel++;
    const char *colon = strchr(rel, ':');        /* strip device prefixes like host0: / cdrom0: */
    if (colon) { rel = colon + 1; while (*rel == '/' || *rel == '\\') rel++; }
    snprintf(full, n, "%s\\%s", g_game_root, rel);
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
    char *semi = strrchr(full, ';'); if (semi) *semi = 0;   /* ISO version suffix ";1" */
    return fopen(full, "rb");
}

static void do_load(u32 reqid, u32 maxsize, u32 ee_buf, u32 flags) {
    char full[1024];
    Req *rq = alloc_req(reqid);
    remember(rq->handle, g_path);
    FILE *f = open_game_file(g_path, full, sizeof full);
    if (!f) {
        fprintf(stderr, "[file] load \"%s\": NOT FOUND (%s)\n", g_path, full);
        rq->status = ST_NOTFOUND;
    } else {
        static u8 buf[1 << 20];
        u32 total = 0;
        while (total < maxsize) {
            u32 want = maxsize - total < sizeof buf ? maxsize - total : (u32)sizeof buf;
            u32 got = (u32)fread(buf, 1, want, f);
            if (!got) break;
            iop_ee_write(ee_buf + total, buf, got);
            total += got;
        }
        fclose(f);
        rq->size = total; rq->status = ST_LOADED;
        fprintf(stderr, "[file] load \"%s\" -> %08x, %u bytes (max %u, flags %x)\n", g_path, ee_buf, total, maxsize, flags);
    }
    u32 *r = resp(3, reqid);
    if (r) { r[2] = rq->handle; r[3] = rq->status; }
    r = resp(8, reqid);
    if (r) { r[2] = rq->handle; r[3] = rq->status; r[4] = rq->size; }
}

void sfiop_file_command(const u8 *cmd) {
    if (g_trace < 0) g_trace = getenv("PS2_TRACE") != NULL;
    const u8 *s = cmd + 8;
    u32 sub = rd(s, 0), reqid = rd(s, 1);
    { static unsigned cnt[32], total; if (sub < 32) cnt[sub]++;
      if (g_trace > 0 && ++total % 500 == 0) {
          fprintf(stderr, "[file] %u batch cmds:", total);
          for (int i = 0; i < 32; i++) if (cnt[i]) fprintf(stderr, " sub%d=%u", i, cnt[i]);
          fputc('\n', stderr); } }
    switch (sub) {
    case 1: {                                            /* path fragment */
        u32 off = rd(s, 5), len = rd(s, 6);
        if (off + len < sizeof g_path) { memcpy(g_path + off, s + 4, len > 16 ? 16 : len); g_path[off + len] = 0; }
        break; }
    case 3: do_load(reqid, rd(s, 2), rd(s, 3), rd(s, 4)); break;
    case 4: {                                            /* two-segment load (IRX 0xC48 -> queue with 2 descriptors) */
        /* seg 1 (type 2): s[2] bytes -> EE s[3]; seg 2 (type 3): next s[4] bytes -> destination s[5] (IOP side) */
        u32 size1 = rd(s, 2), ee = rd(s, 3), size2 = rd(s, 4), dst2 = rd(s, 5);
        char full[1024];
        Req *rq = alloc_req(reqid);
        remember(rq->handle, g_path);
        FILE *f = open_game_file(g_path, full, sizeof full);
        if (!f) { fprintf(stderr, "[file] load2 \"%s\": NOT FOUND (%s)\n", g_path, full); rq->status = ST_NOTFOUND; }
        else {
            static u8 buf[1 << 21];
            u32 n1 = (u32)fread(buf, 1, size1 < sizeof buf ? size1 : sizeof buf, f);
            iop_ee_write(ee, buf, n1);
            u32 n2 = (u32)fread(buf, 1, size2 < sizeof buf ? size2 : sizeof buf, f);
            size_t pl = strlen(g_path);
            if (pl > 4 && !_stricmp(g_path + pl - 4, ".bnk")) {
                /* sound bank: the second part is wave data for SPU RAM at dst2 (the EE's bank region, iop_audio.c) */
                extern u8 g_spu_ram[];
                u32 room = dst2 < (2u << 20) ? (2u << 20) - dst2 : 0;
                memcpy(g_spu_ram + dst2, buf, n2 < room ? n2 : room);
            } else if (dst2) memcpy(iop_ptr(dst2), buf, n2);
            fclose(f);
            rq->size = n1 + n2; rq->status = ST_LOADED;
            fprintf(stderr, "[file] load2 \"%s\": %u bytes -> EE %08x, %u bytes -> %08x  (raw %08x %08x %08x %08x %08x %08x %08x %08x)\n",
                    g_path, n1, ee, n2, dst2, rd(s, 0), rd(s, 1), rd(s, 2), rd(s, 3), rd(s, 4), rd(s, 5), rd(s, 6), rd(s, 7));
        }
        u32 *r = resp(3, reqid);
        if (r) { r[2] = rq->handle; r[3] = rq->status; }
        r = resp(8, reqid);
        if (r) { r[2] = rq->handle; r[3] = rq->status; r[4] = rq->size; }
        break; }
    case 5: {                                            /* chunk read from a loaded file: {handle, size, EE dest, offset} */
        u32 h = rd(s, 2), size = rd(s, 3), ee = rd(s, 4), off = rd(s, 5);
        char full[1024];
        u32 status = ST_NOTFOUND, got = 0;
        const char *hp = g_hpath[h & (NREQ - 1)];
        FILE *f = hp[0] ? open_game_file(hp, full, sizeof full) : NULL;
        if (f) {
            static u8 buf[1 << 21];
            fseek(f, (long)off, SEEK_SET);
            got = (u32)fread(buf, 1, size < sizeof buf ? size : sizeof buf, f);
            iop_ee_write(ee, buf, got);
            fclose(f);
            status = ST_LOADED;
        } else fprintf(stderr, "[file] chunk read: unknown handle %x\n", h);
        char basepath[128]; snprintf(basepath, sizeof basepath, "%s", hp);
        Req *rq = alloc_req(reqid);
        rq->status = status; rq->size = got; remember(rq->handle, basepath);
        u32 *r = resp(3, reqid);
        if (r) { r[2] = rq->handle; r[3] = status; }
        r = resp(8, reqid);
        if (r) { r[2] = rq->handle; r[3] = status; r[4] = got; }
        break; }
    case 2: {                                            /* release handle */
        Req *rq = find_req(rd(s, 1));
        if (rq) memset(rq, 0, sizeof *rq);
        break; }
    case 12: {                                           /* status snapshot */
        u32 *r = resp(2, reqid);
        if (r) { r[1] = 8466; }                          /* IRX writes 8466 here (version/magic) */
        for (u32 i = 0; i < 6; i++) { r = resp(4, i); (void)r; }
        break; }
    case 15: {                                           /* query request status */
        Req *rq = find_req(rd(s, 2));
        if (!rq) { static int n; if (n++ < 20) fprintf(stderr, "[file] status query for unknown handle %x (req %u)\n", rd(s, 2), reqid); }
        u32 *r = resp(8, reqid);
        if (r) { r[2] = rd(s, 2); r[3] = rq ? rq->status : ST_NOTFOUND; r[4] = rq ? rq->size : 0; }
        break; }
    default: {
        static int n;
        if (n++ < 40) {
            const u32 *w = (const u32 *)cmd;
            fprintf(stderr, "[file] unhandled batch sub %u: %08x %08x %08x %08x %08x\n", sub, w[3], w[4], w[5], w[6], w[7]);
        }
        break; }
    }
}

/* RPC side (sid 0xFF01): returns a 768-byte status block. Synchronous-file API not reversed yet. */
static void file_call(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv) {
    (void)out; (void)ee_recv;
    fprintf(stderr, "[file] rpc fn %u send=%u recv=%u", fno, insz, *outsz);
    if (insz >= 8) fprintf(stderr, " in: \"%.40s\"", (const char *)in);
    fputc('\n', stderr);
}

const IopServer iop_server_file = {0x0000ff01u, "SfFile", file_call};

