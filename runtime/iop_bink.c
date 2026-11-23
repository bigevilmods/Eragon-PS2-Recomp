/* Bink RAD_IOP_SIDE replacement (embedded IRX at EE 0x387240; disassembly in recomp/rad_iop.txt).
 *
 * Channel: one SIF user command id (first load argument, "2" in this game) used both ways,
 * with the message type in the packet's opt field.
 *   EE -> IOP  opt 0 read  {handle, ee_dest, iop_buf, block4k}   (IRX 0x214 -> worker 0x7AC)
 *              opt 1 open  {iop address of path}                  (0x264 -> 0x55C)
 *              opt 2 close {handle}                               (0x290 -> 0x728)
 *              opt 3 heartbeat period in ms (<=6000)              (0x2BC)
 *              opt 4 audio channel {ch, enable, iop_ring(4 KB)}   (0x334)
 *              opt 5 volumes {ch, bvolL, bvolR, volL, volR}       (0x410)
 *   IOP -> EE  opt 0 read done {bytes, error, ee_dest} + 4096 bytes DMA'd to ee_dest
 *              opt 1 open result {ok, handle, error, size}
 *              opt 2 close done        opt 3 alive (startup)        opt 4 heartbeat
 *              opt 5 audio position {ch, iop address of the half being played}
 * cdrom0: paths get handle 0x80000000|lsn and reads use sector = block*2; here LSNs are virtual
 * and map onto the files of the extracted disc. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "iop.h"

void iop_send_user_cmd_opt(u32 cid, u32 opt, const void *extra, u32 extra_len, u32 ee_dest, const void *data, u32 data_len);
extern char g_game_root[];
extern u32 g_bink_cid;

/* ---- virtual disc: LSN ranges -> host files */
typedef struct { u32 lsn, sectors; char path[512]; FILE *f; } VFile;
static VFile g_vf[64];
static int g_nvf;
static u32 g_next_lsn = 0x1000;

static VFile *vfile_by_sector(u32 sector) {
    for (int i = 0; i < g_nvf; i++)
        if (sector >= g_vf[i].lsn && sector < g_vf[i].lsn + g_vf[i].sectors) return &g_vf[i];
    return NULL;
}

static void host_path(const char *p, char *out, size_t n) {
    while (*p == '\\' || *p == '/') p++;
    snprintf(out, n, "%s\\%s", g_game_root, p);
    for (char *q = out; *q; q++) if (*q == '/') *q = '\\';
    char *semi = strrchr(out, ';'); if (semi) *semi = 0;
}

/* host (non-cdrom) handles: small ints */
static FILE *g_fd[16];

static void reply4(u32 opt, u32 a, u32 b, u32 c, u32 d, u32 ee_dest, const void *data, u32 len) {
    u32 w[4] = {a, b, c, d};
    iop_send_user_cmd_opt(g_bink_cid, opt, w, 16, ee_dest, data, len);
}

static void bink_open(u32 iop_path) {
    const char *path = (const char *)iop_ptr(iop_path);
    char hp[1024];
    if (!_strnicmp(path, "cdrom0:", 7)) {
        host_path(path + 7, hp, sizeof hp);
        for (int i = 0; i < g_nvf; i++) if (!_stricmp(g_vf[i].path, hp)) {
            reply4(1, 1, 0x80000000u | g_vf[i].lsn, 0, g_vf[i].sectors * 2048u, 0, NULL, 0);
            return;
        }
        FILE *f = fopen(hp, "rb");
        if (!f || g_nvf == 64) {
            fprintf(stderr, "[bink-iop] open \"%s\": not found (%s)\n", path, hp);
            if (f) fclose(f);
            reply4(1, 0, 0, 19, 0, 0, NULL, 0);   /* sceCdSearchFile failed */
            return;
        }
        fseek(f, 0, SEEK_END); u32 size = (u32)ftell(f);
        VFile *v = &g_vf[g_nvf++];
        v->lsn = g_next_lsn; v->sectors = (size + 2047) / 2048; v->f = f;
        snprintf(v->path, sizeof v->path, "%s", hp);
        g_next_lsn += (v->sectors + 15) & ~15u;      /* keep LSNs even and spaced */
        fprintf(stderr, "[bink-iop] open \"%s\" -> lsn %u, %u bytes\n", path, v->lsn, size);
        reply4(1, 1, 0x80000000u | v->lsn, 0, size, 0, NULL, 0);
        return;
    }
    host_path(path, hp, sizeof hp);
    int fd; for (fd = 3; fd < 16 && g_fd[fd]; fd++) {}
    FILE *f = fd < 16 ? fopen(hp, "rb") : NULL;
    if (!f) { reply4(1, 0, (u32)-1, (u32)-1, 0, 0, NULL, 0); return; }
    g_fd[fd] = f;
    fseek(f, 0, SEEK_END); u32 size = (u32)ftell(f);
    reply4(1, 1, (u32)fd, 0, size, 0, NULL, 0);
}

static void bink_read(u32 handle, u32 ee_dest, u32 block) {
    static u8 buf[4096];
    memset(buf, 0, sizeof buf);
    u32 got = 0, err = 0;
    if (handle & 0x80000000u) {                       /* cdrom: sector = block * 2 */
        u32 sector = block * 2;
        VFile *v = vfile_by_sector(sector);
        if (!v) err = 0x32;
        else {
            fseek(v->f, (long)(sector - v->lsn) * 2048, SEEK_SET);
            fread(buf, 1, 4096, v->f);
            got = 4096;                               /* CD reads always transfer whole sectors */
        }
    } else if (handle < 16 && g_fd[handle]) {
        fseek(g_fd[handle], (long)block * 4096, SEEK_SET);
        got = (u32)fread(buf, 1, 4096, g_fd[handle]);
    } else err = 1;
    reply4(0, got, err, ee_dest, 0, ee_dest, buf, 4096);
    static u32 nreads;
    if ((++nreads & 63) == 0) fprintf(stderr, "[bink-iop] %u reads, last block %u (%.1f MB into file)\n", nreads, block, (block * 2.0 - (vfile_by_sector(block * 2) ? vfile_by_sector(block * 2)->lsn : 0)) * 2048.0 / 1048576.0);
}

/* ---- host audio output (WinMM). Bink's PCM ring uses the SPU2 core-input layout:
 * repeating 1 KB blocks of 256 left samples followed by 256 right samples (s16, 48 kHz). */
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#define WO_BUFS 48
#define WO_FRAMES 512                                /* one 2 KB half = 512 stereo frames */
static HWAVEOUT g_wo;
static WAVEHDR g_wh[WO_BUFS];
static s16 g_wbuf[WO_BUFS][WO_FRAMES * 2];
static int g_wnext;

static void wo_open(void) {
    if (g_wo || getenv("PS2_NOSOUND")) return;
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_wo = NULL; return; }
    for (int i = 0; i < WO_BUFS; i++) {
        g_wh[i].lpData = (LPSTR)g_wbuf[i]; g_wh[i].dwBufferLength = sizeof g_wbuf[i];
        waveOutPrepareHeader(g_wo, &g_wh[i], sizeof g_wh[i]);
        g_wh[i].dwFlags |= WHDR_DONE;
    }
}

static void wo_submit_half(const u8 *half) {
    if (!g_wo) return;
    WAVEHDR *h = &g_wh[g_wnext];
    if (!(h->dwFlags & WHDR_DONE)) return;           /* output is behind: drop rather than block */
    s16 *dst = g_wbuf[g_wnext];
    for (int blk = 0; blk < 2; blk++) {              /* two 1 KB L/R blocks per half */
        const s16 *l = (const s16 *)(half + blk * 1024), *r = l + 256;
        for (int i = 0; i < 256; i++) { dst[(blk * 256 + i) * 2] = l[i]; dst[(blk * 256 + i) * 2 + 1] = r[i]; }
    }
    h->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_wo, h, sizeof *h);
    static FILE *dump; static int dump_init;
    if (!dump_init) { dump_init = 1; if (getenv("PS2_AUDIODUMP")) dump = fopen("bink_audio.raw", "wb"); }
    if (dump) fwrite(dst, 2, WO_FRAMES * 2, dump);   /* 48 kHz s16 stereo, for checking */
    g_wnext = (g_wnext + 1) % WO_BUFS;
}

/* ---- audio: positions are paced by host time */
typedef struct { int on; u32 ring; double next; int half; } BinkVoice;
static BinkVoice g_voice[2];
static double g_hb_period = 6.0, g_hb_next;
static double now_s(void) {
    static LARGE_INTEGER f, t0; LARGE_INTEGER t;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}
#define HALF_SEC (2048.0 / (48000.0 * 4.0))         /* 2 KB of 16-bit stereo at 48 kHz */

void bink_iop_tick(void) {
    double t = now_s();
    for (int ch = 0; ch < 2; ch++) {
        BinkVoice *v = &g_voice[ch];
        if (!v->on || t < v->next) continue;
        v->half ^= 1;
        v->next += HALF_SEC;
        if (v->next < t - 0.1) v->next = t + HALF_SEC;  /* don't burst after a stall */
        wo_submit_half(iop_ptr(v->ring + (v->half ? 2048u : 0u)));   /* this half starts playing */
        u32 w[2] = {(u32)ch, v->ring + (v->half ? 2048u : 0u)};
        iop_send_user_cmd_opt(g_bink_cid, 5, w, 8, 0, NULL, 0);
    }
    if (g_hb_next && t >= g_hb_next) {
        g_hb_next = t + g_hb_period;
        iop_send_user_cmd_opt(g_bink_cid, 4, NULL, 0, 0, NULL, 0);
    }
}

void bink_iop_command(const u32 *pkt) {
    u32 opt = pkt[3];
    static int trace = -1;
    if (trace < 0) trace = getenv("PS2_TRACE") != NULL;
    if (trace) fprintf(stderr, "[bink-iop] cmd %u: %08x %08x %08x %08x %08x\n", opt, pkt[4], pkt[5], pkt[6], pkt[7], pkt[8]);
    switch (opt) {
    case 0: bink_read(pkt[4], pkt[5], pkt[7]); break;
    case 1: bink_open(pkt[4]); break;
    case 2:
        if (pkt[4] < 16 && g_fd[pkt[4]]) { fclose(g_fd[pkt[4]]); g_fd[pkt[4]] = NULL; }
        iop_send_user_cmd_opt(g_bink_cid, 2, NULL, 0, 0, NULL, 0);
        break;
    case 3: {
        u32 ms = pkt[4];
        g_hb_period = (ms && ms <= 6000) ? ms / 1000.0 : 6.0;
        g_hb_next = now_s() + g_hb_period;
        break; }
    case 4: {
        u32 ch = pkt[4] & 1;
        g_voice[ch].on = pkt[5] != 0;
        g_voice[ch].ring = pkt[6];
        g_voice[ch].next = now_s() + HALF_SEC;
        g_voice[ch].half = 1;
        if (g_voice[ch].ring) memset(iop_ptr(g_voice[ch].ring), 0, 4096);
        if (g_voice[ch].on) wo_open();
        fprintf(stderr, "[bink-iop] audio ch%u %s ring %08x\n", ch, g_voice[ch].on ? "on" : "off", g_voice[ch].ring);
        break; }
    case 5: break;                                   /* volumes */
    default: fprintf(stderr, "[bink-iop] unknown cmd %u\n", opt); break;
    }
}

void bink_iop_started(void) { g_hb_next = now_s() + g_hb_period; }
