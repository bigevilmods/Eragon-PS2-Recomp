/* AUDIO.IRX (Stormfront SfAudio) replacement: SFIOP type-1 batch commands, played through a host mixer.
 *
 * Command (24 bytes): {type 1, size 24, sub, a, b, c}; s = cmd+8: s[0] = sub, s[1] = handle, s[2] = EE cue id,
 * s[3] = value (layout from the IRX handlers at 0x1B1C.., dispatch table at IRX 0xB7A8).
 *   1 master volume (s[1] = L | R<<16)   4 wave properties for the next ALLOCATE_CUE_WAVE:
 *     {channels, sample rate, size in bytes, SPU address}; stereo waves store the channels one after the other
 *   5-8 SEGMENT_PROPERTY_FILENAME0..3 (24-byte name pieces)   9/10/11 ALLOCATE_CUE_WAVE/_SEGMENT/SEGMENT_SEGMENT
 *   12 FREE_CUE  13 REVERB  14 PRIORITY  15 ENVELOPE (ADSR1 | ADSR2<<16)  16 VOLUME (L | R<<16, signed 14-bit)
 *   17 SAMPLE_RATE  18 GET_CUE_SAMPLE_RATE  19 GET_CUE_PLAY_TIME  20 IS_CUE_IDLE  21 KEY_ON  22 KEY_OFF  23 STOP
 *   24 PAUSE (s[3])  25 MUTE (s[3])  26 GET_CUE_SEGMENT_STATUS  29 SEGMENT_SEEK
 * Replies (type 1, size 24, body r = entry+8):
 *   1/2/3 allocated {code, id, handle, -, result}   6 idle {6, id, handle, idle}   7 play time {7, id, handle, time}
 *   8 sample rate {8, id, rate}   9 segment status {9, id, handle, -, status}
 * Segment status values (IRX 0x1500): 7 busy/opening, 5 ready, 10|pos<<16 playing, 11 finished, 4 no cue.
 *
 * Sound effects live in emulated SPU RAM (g_spu_ram): the EE manages a region it gets from RPC fn 0/2 and loads
 * bank wave data there through FILE.IRX (iop_file.c). Waves are PS2 ADPCM and follow the SPU block flags
 * (loop start / repeat / end). Segments (.SEG music, ambience and speech) are streamed from the disc: 16 KB header
 * ("seg"/"ps2", word 6 rate, 7 samples, 8 looping, 9 channels), then 16 KB chunks holding 16384/channels bytes
 * of each channel in turn. Everything is mixed to 48 kHz stereo and played through WinMM.
 * PS2_NOSOUND=1 mutes, PS2_AUDIOLOG=1 logs commands, PS2_SFXDUMP=1 writes the mix to sfx_audio.raw. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "iop.h"
#include "iop_sfiop.h"

#define SPU_RAM (2u << 20)
u8 g_spu_ram[SPU_RAM];
/* region the EE manages for banks (RPC fn 0/2 replies): above the SPU's reserved low area */
#define SPU_EE_BASE 0x10000u
#define SPU_EE_SIZE 0x1E0000u

extern char g_game_root[];

/* ------------------------------------------------------------------ ADPCM */
static const int k_coef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
static void adpcm_block(const u8 *b, s16 out[28], int *h1, int *h2) {
    int shift = b[0] & 15, filt = b[0] >> 4;
    if (filt > 4) filt = 4;
    if (shift > 12) shift = 9;
    int c1 = k_coef[filt][0], c2 = k_coef[filt][1], s1 = *h1, s2 = *h2;
    for (int i = 0; i < 28; i++) {
        int n = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        if (n >= 8) n -= 16;
        int v = ((n << 12) >> shift) + ((s1 * c1 + s2 * c2 + 32) >> 6);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        out[i] = (s16)v; s2 = s1; s1 = v;
    }
    *h1 = s1; *h2 = s2;
}

/* ------------------------------------------------------------------ voices */
typedef struct {
    int active, paused, muted, releasing, finished;
    int segment;                 /* 0 wave from SPU RAM, 1 streamed segment */
    int nch, rate;
    /* wave: per-channel SPU byte address of the next block and loop start */
    u32 addr[2], loop[2]; int ended[2];
    /* segment */
    FILE *f; u32 blk, nblk; int looping; u8 chunk[16384]; int chunk_no; u32 interleave;
    /* decode + resample */
    int h1[2], h2[2]; s16 dec[2][28]; int di;
    s16 prev[2], cur[2]; double frac; double step;
    float vl, vr, env, rel;      /* volumes (1.0 = 0x3FFF), envelope, release decrement per frame */
    u64 played;                  /* source samples played */
} Voice;

typedef struct { int used; u32 id, handle, type; char name[100]; int nch, rate; u32 size, addr; Voice v; int keyed; } Cue;

static CRITICAL_SECTION g_mx;
static Cue g_cue[128];
static char g_name[4][24];
static u32 g_wprop[4];           /* sub 4: channels, rate, size, address */
static u32 g_next_handle = 0x1000;
static int g_log = -1;
static float g_master_l = 1.0f, g_master_r = 1.0f;

static float vol14(s16 v) { return (float)v / 16383.0f; }

/* next 16-byte ADPCM block of channel ch; returns 0 at the end of the sound */
static int next_block(Voice *v, int ch, u8 out[16]) {
    if (!v->segment) {
        if (v->ended[ch]) return 0;
        u32 a = v->addr[ch] & (SPU_RAM - 1);
        memcpy(out, g_spu_ram + a, 16);
        u8 fl = out[1];
        if (fl & 4) v->loop[ch] = a;                       /* loop start */
        if (fl & 1) {                                      /* loop end: repeat or stop */
            if (fl & 2) v->addr[ch] = v->loop[ch];
            else v->ended[ch] = 1;
        } else v->addr[ch] = a + 16;
        return 1;
    }
    if (v->blk >= v->nblk) return 0;
    u32 per = v->interleave / 16;                          /* blocks per channel per chunk */
    int chunk = (int)(v->blk / per);
    if (chunk != v->chunk_no) {
        memset(v->chunk, 0, sizeof v->chunk);
        if (v->f) { fseek(v->f, 0x4000 + (long)chunk * 16384, SEEK_SET); if (!fread(v->chunk, 1, 16384, v->f)) { v->blk = v->nblk; return 0; } }
        v->chunk_no = chunk;
    }
    memcpy(out, v->chunk + ch * v->interleave + (v->blk % per) * 16, 16);
    return 1;
}

/* advance one source sample (both channels); returns 0 when the voice has finished */
static int next_sample(Voice *v) {
    if (v->di >= 28) {
        int any = 0;
        for (int ch = 0; ch < v->nch; ch++) {
            u8 b[16];
            if (next_block(v, ch, b)) { adpcm_block(b, v->dec[ch], &v->h1[ch], &v->h2[ch]); any = 1; }
            else memset(v->dec[ch], 0, sizeof v->dec[ch]);
        }
        if (v->segment) {
            v->blk++;
            if (v->blk >= v->nblk && v->looping) { v->blk = 0; v->h1[0] = v->h2[0] = v->h1[1] = v->h2[1] = 0; }
        }
        if (!any) return 0;
        v->di = 0;
    }
    for (int ch = 0; ch < v->nch; ch++) { v->prev[ch] = v->cur[ch]; v->cur[ch] = v->dec[ch][v->di]; }
    if (v->nch == 1) { v->prev[1] = v->prev[0]; v->cur[1] = v->cur[0]; }
    v->di++; v->played++;
    return 1;
}

static void voice_stop(Voice *v) {
    if (v->f) { fclose(v->f); v->f = NULL; }
    v->active = 0; v->finished = 1;
}

/* mix n stereo frames into acc */
static void voice_mix(Voice *v, float *acc, int n) {
    for (int i = 0; i < n; i++) {
        while (v->frac >= 1.0) {
            v->frac -= 1.0;
            if (!next_sample(v)) { voice_stop(v); return; }
        }
        if (v->releasing) { v->env -= v->rel; if (v->env <= 0) { voice_stop(v); return; } }
        float t = (float)v->frac;
        float l = v->prev[0] + (v->cur[0] - v->prev[0]) * t, r = v->prev[1] + (v->cur[1] - v->prev[1]) * t;
        if (!v->muted) { acc[2 * i] += l * v->vl * v->env; acc[2 * i + 1] += r * v->vr * v->env; }
        v->frac += v->step;
    }
}

static int voice_start_wave(Voice *v, int nch, int rate, u32 size, u32 addr) {
    memset(v, 0, sizeof *v);
    v->nch = nch < 1 ? 1 : nch > 2 ? 2 : nch;
    v->rate = rate > 0 ? rate : 22050;
    u32 per = size / (u32)v->nch;
    for (int ch = 0; ch < v->nch; ch++) { v->addr[ch] = addr + ch * per; v->loop[ch] = v->addr[ch]; }
    v->di = 28; v->frac = 1.0; v->step = v->rate / 48000.0; v->env = 1.0f; v->vl = v->vr = 1.0f;
    v->active = 1;
    return 1;
}

static FILE *open_segment(const char *name, char *path, size_t pathsz) {
    char rel[128];
    snprintf(rel, sizeof rel, "%s", name);
    size_t n = strlen(rel);
    while (n && (rel[n - 1] == '.' || rel[n - 1] == ' ')) rel[--n] = 0;
    if (!strchr(rel, '.')) strncat(rel, ".SEG", sizeof rel - strlen(rel) - 1);
    snprintf(path, pathsz, "%s\\AUDIO\\%s", g_game_root, rel);
    return fopen(path, "rb");
}

static int voice_start_segment(Voice *v, const char *name) {
    memset(v, 0, sizeof *v);
    char path[512];
    FILE *f = open_segment(name, path, sizeof path);
    u32 h[16] = {0};
    if (!f || fread(h, 4, 16, f) != 16 || memcmp(h, "seg", 4)) {
        if (f) fclose(f);
        if (g_log > 0) fprintf(stderr, "[audio] segment \"%s\" not found (%s)\n", name, path);
        v->finished = 1;
        return 0;
    }
    v->segment = 1; v->f = f;
    v->rate = (int)h[6]; v->nch = h[9] == 1 ? 1 : 2; v->looping = h[8] != 0;
    v->interleave = 16384u / (u32)v->nch;
    v->nblk = (h[7] + 27) / 28;
    v->chunk_no = -1; v->di = 28; v->frac = 1.0; v->step = v->rate / 48000.0; v->env = 1.0f; v->vl = v->vr = 1.0f;
    v->active = 1;
    return 1;
}

/* ------------------------------------------------------------------ output */
#define MIX_FRAMES 960            /* 20 ms at 48 kHz */
#define MIX_BUFS 4
static HWAVEOUT g_wo;
static WAVEHDR g_wh[MIX_BUFS];
static s16 g_wbuf[MIX_BUFS][MIX_FRAMES * 2];

static DWORD WINAPI mixer_thread(void *p) {
    (void)p;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[audio] waveOutOpen failed: no game audio\n");
        return 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    static FILE *dump; if (getenv("PS2_SFXDUMP")) dump = fopen("sfx_audio.raw", "wb");
    for (int i = 0; i < MIX_BUFS; i++) {
        g_wh[i].lpData = (LPSTR)g_wbuf[i]; g_wh[i].dwBufferLength = sizeof g_wbuf[i];
        waveOutPrepareHeader(g_wo, &g_wh[i], sizeof g_wh[i]);
        g_wh[i].dwFlags |= WHDR_DONE;
    }
    static float acc[MIX_FRAMES * 2];
    for (;;) {
        int filled = 0;
        for (int i = 0; i < MIX_BUFS; i++) {
            if (!(g_wh[i].dwFlags & WHDR_DONE)) continue;
            memset(acc, 0, sizeof acc);
            EnterCriticalSection(&g_mx);
            for (int c = 0; c < 128; c++) {
                Voice *v = &g_cue[c].v;
                if (g_cue[c].used && v->active && !v->paused) voice_mix(v, acc, MIX_FRAMES);
            }
            float ml = g_master_l, mr = g_master_r;
            LeaveCriticalSection(&g_mx);
            s16 *o = g_wbuf[i];
            for (int k = 0; k < MIX_FRAMES; k++) {
                float l = acc[2 * k] * ml, r = acc[2 * k + 1] * mr;
                o[2 * k] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                o[2 * k + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
            if (dump) fwrite(o, 4, MIX_FRAMES, dump);
            g_wh[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_wh[i], sizeof g_wh[i]);
            filled = 1;
        }
        if (!filled) WaitForSingleObject(ev, 50);
    }
}

static void audio_init(void) {
    static int done; if (done) return; done = 1;
    InitializeCriticalSection(&g_mx);
    if (g_log < 0) g_log = getenv("PS2_AUDIOLOG") != NULL;
    if (!getenv("PS2_NOSOUND")) CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL);
}

/* ------------------------------------------------------------------ batch commands */
static Cue *find(u32 id) {
    for (int i = 0; i < 128; i++) if (g_cue[i].used && g_cue[i].id == id) return &g_cue[i];
    return NULL;
}

static u32 *reply(u32 code, u32 id) {
    u8 *e = sfiop_response(SFIOP_TYPE_AUDIO, 24);
    if (!e) return NULL;
    u32 *r = (u32 *)(e + 8);
    r[0] = code; r[1] = id;
    return r;
}

void sfiop_audio_command(const u8 *cmd) {
    audio_init();
    u32 s[5]; memcpy(s, cmd + 8, 20);
    u32 sub = s[0], id = s[2];
    EnterCriticalSection(&g_mx);
    Cue *c = find(id);
    Voice *v = c ? &c->v : NULL;
    switch (sub) {
    case 1: g_master_l = vol14((s16)(s[1] & 0xFFFF)); g_master_r = vol14((s16)(s[1] >> 16)); break;
    case 4: g_wprop[0] = s[1]; g_wprop[1] = s[2]; g_wprop[2] = s[3]; g_wprop[3] = s[4]; break;
    case 5: case 6: case 7: case 8: memcpy(g_name[sub - 5], cmd + 12, 24); break;
    case 9: case 10: case 11: {
        if (!c) { for (int i = 0; i < 128; i++) if (!g_cue[i].used) { c = &g_cue[i]; break; } }
        if (!c) break;
        if (c->v.f) fclose(c->v.f);
        memset(c, 0, sizeof *c);
        c->used = 1; c->id = id; c->type = sub; c->handle = g_next_handle++;
        if (sub == 9) { c->nch = (int)g_wprop[0]; c->rate = (int)g_wprop[1]; c->size = g_wprop[2]; c->addr = g_wprop[3]; }
        else {
            memcpy(c->name, g_name, 96); c->name[96] = 0;
            for (int i = 0; i < 96; i++) if ((u8)c->name[i] < 32 || (u8)c->name[i] > 126) { c->name[i] = 0; break; }
        }
        c->v.vl = c->v.vr = 1.0f;
        if (g_log > 0) {
            if (sub == 9) fprintf(stderr, "[audio] alloc cue %u wave ch %d rate %d size %#x spu %#x\n", id, c->nch, c->rate, c->size, c->addr);
            else fprintf(stderr, "[audio] alloc cue %u segment \"%s\"\n", id, c->name);
        }
        memset(g_name, 0, sizeof g_name);
        if (id) { u32 *r = reply(sub - 8, id); if (r) { r[2] = c->handle; r[4] = 1; } }
        break; }
    case 12: if (c) { if (c->v.f) fclose(c->v.f); memset(c, 0, sizeof *c); } break;
    case 15: break;                                                      /* envelope: release handled at key-off */
    case 16: if (c) { c->v.vl = vol14((s16)(s[3] & 0xFFFF)); c->v.vr = vol14((s16)(s[3] >> 16)); } break;
    case 17: if (c && s[3]) { c->rate = (int)s[3]; if (!c->v.segment) c->v.step = s[3] / 48000.0; } break;
    case 18: { u32 *r = reply(8, id); if (r) r[2] = c ? (u32)(c->v.segment ? c->v.rate : c->rate) : 48000; break; }
    case 19: { u32 *r = reply(7, id); if (r) { r[2] = c ? c->handle : 0; r[3] = v ? (u32)v->played : 0; } break; }
    case 20: { u32 *r = reply(6, id); if (r) { r[2] = c ? c->handle : 0; r[3] = !(v && v->active); } break; }
    case 21: if (c) {
        float vl = c->v.vl, vr = c->v.vr; int muted = c->v.muted;
        if (c->v.f) fclose(c->v.f);
        if (c->type == 9) voice_start_wave(&c->v, c->nch, c->rate, c->size, c->addr);
        else voice_start_segment(&c->v, c->name);
        c->v.vl = vl; c->v.vr = vr; c->v.muted = muted; c->keyed = 1;
        if (g_log > 0) fprintf(stderr, "[audio] key on cue %u (%s) vol %.2f %.2f\n", id, c->type == 9 ? "wave" : c->name, vl, vr);
    } break;
    case 22: if (v && v->active) { v->releasing = 1; v->rel = 1.0f / (48000 * 0.05f); } break;   /* ~50 ms release */
    case 23: if (v) voice_stop(v); break;
    case 24: if (v) v->paused = s[3] != 0; break;
    case 25: if (v) v->muted = s[3] != 0; break;
    case 26: {
        u32 status = 4;
        if (c) {
            if (v->active) { u32 pos = v->rate ? (u32)(v->played * 10 / (u64)v->rate) & 0xFFFF : 0; status = 10 | (pos << 16); }
            else if (v->finished) status = 11;
            else status = 5;
        }
        u32 *r = reply(9, id);
        if (r) { r[2] = c ? c->handle : 0; r[4] = status; }
        break; }
    case 29: if (v && v->segment) {                                    /* seek: s[3] = sample position */
        u32 b = s[3] / 28; if (b < v->nblk) { v->blk = b; v->di = 28; v->h1[0] = v->h2[0] = v->h1[1] = v->h2[1] = 0; }
    } break;
    default: break;
    }
    LeaveCriticalSection(&g_mx);
    if (g_log > 0 && sub != 26 && sub != 19 && sub != 20) fprintf(stderr, "[audio] sub %u id %u (%08x %08x %08x)\n", sub, id, s[1], s[3], s[4]);
}

/* RPC sid 0xFF03. fn 0 = init: the EE (ClAudio, 0x263840) checks reply word 0 == 36 (IRX version); words 1/2 are
 * the size and base of the SPU RAM region the EE manages for banks, word 3 free IOP RAM. fn 2 re-allocates that
 * region (level change): words 1/2 again. */
static void audio_call(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv) {
    (void)in; (void)ee_recv;
    audio_init();
    if (g_log > 0) fprintf(stderr, "[audio] rpc fn %u send=%u recv=%u\n", fno, insz, *outsz);
    if (fno == 0 && *outsz >= 16) {
        u32 r[4] = {36, SPU_EE_SIZE, SPU_EE_BASE, 1u << 20};
        memcpy(out, r, sizeof r);
    } else if (fno == 2 && *outsz >= 12) {
        u32 r[2] = {SPU_EE_SIZE, SPU_EE_BASE};   /* words 1/2 only, as the IRX does */
        memcpy(out + 4, r, sizeof r);
    }
}

const IopServer iop_server_audio = {0x0000ff03u, "SfAudio", audio_call};
