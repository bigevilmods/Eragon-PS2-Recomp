/* Software GS: 4 MB local memory with real swizzled layouts, host->local / local->local
 * transfers, register file, rasterizer (points, lines, triangles, strips, fans, sprites),
 * texturing (CT32/24/16, T8/T4 with CLUT, wrap modes), alpha test/blend, Z buffer, scissor,
 * and display readout. Accuracy-first, not speed-first. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gs.h"
#include "gs_gpu.h"
/* GPU readbacks per call site (0 CLUT, 1 CPU fallback batch, 2 partial upload, 3 local->local, 4 display) */
u64 g_rb_site[5];
extern u64 g_gpu_downloads;
#define RB(site, call) do { u64 _b = g_gpu_downloads; call; g_rb_site[site] += g_gpu_downloads - _b; } while (0)

void kernel_request_intc(int cause);
static void xyzf2(u64 v, int draw);
void gs_flush(void);

/* ------------------------------------------------------------------ memory layouts */
static u8 *g_vram;   /* 4 MB */
#define VRAM_MASK32 0xFFFFF
#define VRAM_MASK16 0x1FFFFF
#define VRAM_MASK8  0x3FFFFF

static const u8 blk32[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const u8 blkz32[4][8] = {{24,25,28,29,8,9,12,13},{26,27,30,31,10,11,14,15},{16,17,20,21,0,1,4,5},{18,19,22,23,2,3,6,7}};
static const u8 col32[8][8] = {
    {0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15},{16,17,20,21,24,25,28,29},{18,19,22,23,26,27,30,31},
    {32,33,36,37,40,41,44,45},{34,35,38,39,42,43,46,47},{48,49,52,53,56,57,60,61},{50,51,54,55,58,59,62,63}};
static const u8 blk16[8][4] = {{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const u8 blk16s[8][4] = {{0,2,16,18},{1,3,17,19},{8,10,24,26},{9,11,25,27},{4,6,20,22},{5,7,21,23},{12,14,28,30},{13,15,29,31}};
static const u8 blkz16[8][4] = {{24,26,16,18},{25,27,17,19},{28,30,20,22},{29,31,21,23},{8,10,0,2},{9,11,1,3},{12,14,4,6},{13,15,5,7}};
static const u8 blkz16s[8][4] = {{24,26,8,10},{25,27,9,11},{16,18,0,2},{17,19,1,3},{28,30,12,14},{29,31,13,15},{20,22,4,6},{21,23,5,7}};
static const u8 col16[8][16] = {
    {0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27},{4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31},
    {32,34,40,42,48,50,56,58,33,35,41,43,49,51,57,59},{36,38,44,46,52,54,60,62,37,39,45,47,53,55,61,63},
    {64,66,72,74,80,82,88,90,65,67,73,75,81,83,89,91},{68,70,76,78,84,86,92,94,69,71,77,79,85,87,93,95},
    {96,98,104,106,112,114,120,122,97,99,105,107,113,115,121,123},{100,102,108,110,116,118,124,126,101,103,109,111,117,119,125,127}};
static const u8 blk8[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const u8 col8[16][16] = {
    {0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54},{8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62},
    {33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23},{41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31},
    {96,100,112,116,64,68,80,84,98,102,114,118,66,70,82,86},{104,108,120,124,72,76,88,92,106,110,122,126,74,78,90,94},
    {65,69,81,85,97,101,113,117,67,71,83,87,99,103,115,119},{73,77,89,93,105,109,121,125,75,79,91,95,107,111,123,127},
    {128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182},{136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190},
    {161,165,177,181,129,133,145,149,163,167,179,183,131,135,147,151},{169,173,185,189,137,141,153,157,171,175,187,191,139,143,155,159},
    {224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214},{232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222},
    {193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247},{201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255}};
static const u8 blk4[8][4] = {{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const u16 col4[16][32] = {
    {0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110},
    {16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126},
    {65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47},
    {81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63},
    {192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174},
    {208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190},
    {129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239},
    {145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255},
    {256,264,288,296,320,328,352,360,258,266,290,298,322,330,354,362,260,268,292,300,324,332,356,364,262,270,294,302,326,334,358,366},
    {272,280,304,312,336,344,368,376,274,282,306,314,338,346,370,378,276,284,308,316,340,348,372,380,278,286,310,318,342,350,374,382},
    {321,329,353,361,257,265,289,297,323,331,355,363,259,267,291,299,325,333,357,365,261,269,293,301,327,335,359,367,263,271,295,303},
    {337,345,369,377,273,281,305,313,339,347,371,379,275,283,307,315,341,349,373,381,277,285,309,317,343,351,375,383,279,287,311,319},
    {448,456,480,488,384,392,416,424,450,458,482,490,386,394,418,426,452,460,484,492,388,396,420,428,454,462,486,494,390,398,422,430},
    {464,472,496,504,400,408,432,440,466,474,498,506,402,410,434,442,468,476,500,508,404,412,436,444,470,478,502,510,406,414,438,446},
    {385,393,417,425,449,457,481,489,387,395,419,427,451,459,483,491,389,397,421,429,453,461,485,493,391,399,423,431,455,463,487,495},
    {401,409,433,441,465,473,497,505,403,411,435,443,467,475,499,507,405,413,437,445,469,477,501,509,407,415,439,447,471,479,503,511}};

enum { PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A, PSMT8 = 0x13, PSMT4 = 0x14,
       PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C, PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A };

static inline u32 addr32(u32 bp, u32 bw, u32 x, u32 y, const u8 (*blk)[8]) {
    u32 page = (x >> 6) + (y >> 5) * bw;
    return (bp * 64 + page * 2048 + blk[(y >> 3) & 3][(x >> 3) & 7] * 64 + col32[y & 7][x & 7]) & VRAM_MASK32;
}
static inline u32 addr16(u32 bp, u32 bw, u32 x, u32 y, const u8 (*blk)[4]) {
    u32 page = (x >> 6) + (y >> 6) * bw;
    return (bp * 128 + page * 4096 + blk[(y >> 3) & 7][(x >> 4) & 3] * 128 + col16[y & 7][x & 15]) & VRAM_MASK16;
}
static inline u32 addr8(u32 bp, u32 bw, u32 x, u32 y) {
    u32 page = (x >> 7) + (y >> 6) * ((bw + 1) >> 1);
    return (bp * 256 + page * 8192 + blk8[(y >> 4) & 3][(x >> 4) & 7] * 256 + col8[y & 15][x & 15]) & VRAM_MASK8;
}
static inline u32 addr4(u32 bp, u32 bw, u32 x, u32 y) {   /* nibble index */
    u32 page = (x >> 7) + (y >> 7) * ((bw + 1) >> 1);
    return (bp * 512 + page * 16384 + blk4[(y >> 4) & 7][(x >> 5) & 3] * 512 + col4[y & 15][x & 31]) & 0x7FFFFF;
}

#define V32(i) (((u32 *)g_vram)[i])
#define V16(i) (((u16 *)g_vram)[i])

/* raw read/write in a format (value is the stored bits; 24-bit formats keep top byte) */
static u32 vram_read(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: return V32(addr32(bp, bw, x, y, blk32));
    case PSMZ32: case PSMZ24: return V32(addr32(bp, bw, x, y, blkz32));
    case PSMCT16: return V16(addr16(bp, bw, x, y, blk16));
    case PSMCT16S: return V16(addr16(bp, bw, x, y, blk16s));
    case PSMZ16: return V16(addr16(bp, bw, x, y, blkz16));
    case PSMZ16S: return V16(addr16(bp, bw, x, y, blkz16s));
    case PSMT8: return g_vram[addr8(bp, bw, x, y)];
    case PSMT4: { u32 n = addr4(bp, bw, x, y); return (g_vram[n >> 1] >> ((n & 1) * 4)) & 15; }
    }
    return 0;
}
static void vram_write(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v) {
    switch (psm) {
    case PSMCT32: V32(addr32(bp, bw, x, y, blk32)) = v; break;
    case PSMCT24: { u32 *p = &V32(addr32(bp, bw, x, y, blk32)); *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break; }
    case PSMT8H:  { u32 *p = &V32(addr32(bp, bw, x, y, blk32)); *p = (*p & 0x00FFFFFFu) | (v << 24); break; }
    case PSMT4HL: { u32 *p = &V32(addr32(bp, bw, x, y, blk32)); *p = (*p & 0xF0FFFFFFu) | ((v & 15) << 24); break; }
    case PSMT4HH: { u32 *p = &V32(addr32(bp, bw, x, y, blk32)); *p = (*p & 0x0FFFFFFFu) | ((v & 15) << 28); break; }
    case PSMZ32: V32(addr32(bp, bw, x, y, blkz32)) = v; break;
    case PSMZ24: { u32 *p = &V32(addr32(bp, bw, x, y, blkz32)); *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break; }
    case PSMCT16: V16(addr16(bp, bw, x, y, blk16)) = (u16)v; break;
    case PSMCT16S: V16(addr16(bp, bw, x, y, blk16s)) = (u16)v; break;
    case PSMZ16: V16(addr16(bp, bw, x, y, blkz16)) = (u16)v; break;
    case PSMZ16S: V16(addr16(bp, bw, x, y, blkz16s)) = (u16)v; break;
    case PSMT8: g_vram[addr8(bp, bw, x, y)] = (u8)v; break;
    case PSMT4: { u32 n = addr4(bp, bw, x, y); u8 *p = &g_vram[n >> 1];
                  *p = (n & 1) ? (u8)((*p & 0x0F) | ((v & 15) << 4)) : (u8)((*p & 0xF0) | (v & 15)); break; }
    }
}

/* ------------------------------------------------------------------ registers */
typedef struct { float x, y; u32 z; int r, g, b, a; float s, t, q, u, v; u8 fog; } Vtx;

typedef struct {
    u64 tex0, tex1, clamp, xyoffset, scissor, alpha, test, fba, frame, zbuf;
} Ctx;

static struct {
    u64 prim, rgbaq, st, uv, fog, prmodecont, prmode, texclut, texa, fogcol, dimx, dthe, colclamp, pabe, scanmsk;
    u64 bitbltbuf, trxpos, trxreg, trxdir;
    Ctx ctx[2];
    float q;          /* internal Q from ST */
    Vtx vq[3]; int nv; int prim_count;  /* vertex queue */
    /* CLUT */
    u32 clut32[256]; u16 clut16[512]; u32 cbp0, cbp1;
    /* transfer */
    int xfer_active; u32 tx, ty;
    u8 xfer_carry[16]; int xfer_ncarry;
    /* privileged */
    u64 pmode, smode1, smode2, srfsh, synch1, synch2, syncv, dispfb[2], display[2], extbuf, extdata, extwrite, bgcolor;
    u64 csr, imr, busdir, siglblid;
} G;

static int g_trace_gs = -1;
double g_gs_time;   /* seconds spent in GIF/VIF/GS processing (gif.c/vif.c accumulate) */

void gs_init(void) {
    if (!g_vram) g_vram = calloc(1, 4u << 20);
    memset(&G, 0, sizeof G);
    G.csr = 0x551B0000ull | 0x2;   /* revision/id + FINISH */
    G.imr = 0x7F00;
    G.prmodecont = 1;
    g_trace_gs = getenv("PS2_TRACE_GS") != NULL;
}

/* ------------------------------------------------------------------ CLUT */
static u32 g_clut_gen = 1;            /* bumped whenever clut_load() reloads the CLUT (texture cache key) */
static int gpu_on(void);
static int batch_writes(u32 lo, u32 hi);
static u32 psm_blocks(u32 psm, u32 bw, u32 rows);
static int batch_pages_hit(u32 lo, u32 hi);
/* debugging hotkeys (window.c): F9 capture, F10 switch GPU/CPU rasterizer */
volatile LONG g_hk_capture, g_hk_toggle_gpu;
static volatile int g_disp_drawn;   /* a primitive targeted the displayed frame buffer since the last readout */
static FILE *g_cap_fp; static int g_cap_frame = -1; static char g_cap_bmp[96];
/* GPU: the last CLUT load was a GPU-drawn 256-entry CT32 CLUT at g_clutvram_cbp (read from VRAM by the shader);
 * g_batch_clutvram: the pending batch has draws that read it, so draws writing its blocks must flush first */
static int g_clutvram, g_batch_clutvram; static u32 g_clutvram_cbp;
/* GPU: VRAM words (bit per 32-bit word) that a transfer rewrote completely on the CPU copy as well,
 * since the GPU last drew into their page: the CPU copy of those words is current without a readback */
static u64 g_fresh[(4u << 20) / 4 / 64];
static void fresh_clear_page(u32 page) { if (page < 512) memset(&g_fresh[page * 32], 0, 32 * sizeof(u64)); }
static struct { u32 bp, psm; int x0, y0, x1, y1; } g_page_mark[512];   /* diagnostics: last draw target marking each page */
static void clut_load(u64 tex0) {
    u32 cld = (tex0 >> 61) & 7, cbp = (tex0 >> 37) & 0x3FFF, cpsm = (tex0 >> 51) & 15, csm = (tex0 >> 55) & 1;
    u32 csa = (tex0 >> 56) & 31, psm = (tex0 >> 20) & 63;
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: G.cbp0 = cbp; break;
    case 3: G.cbp1 = cbp; break;
    case 4: if (G.cbp0 == cbp) return; G.cbp0 = cbp; break;
    case 5: if (G.cbp1 == cbp) return; G.cbp1 = cbp; break;
    default: return;
    }
    int n = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16;
    g_clutvram = 0;
    u32 cbw = (G.texclut & 63), cou = (G.texclut >> 6) & 63, cov = (G.texclut >> 12) & 1023;
    static int verify = -1; if (verify < 0) verify = getenv("PS2_GPUVERIFY") != NULL;
    if (gpu_on() && !verify) {
        /* GPU: the batch keeps CLUT snapshots, so only flush if pending draws write the CLUT source;
         * then make the CPU copy current (the GPU may have drawn it) */
        /* blocks the CLUT occupies: CSM1 (csm 0) 256 entries = 16x16 texels = 4 CT32 blocks or 2 CT16 blocks,
         * 16 entries = 8x2 = 1 block; CSM2 rows: conservative */
        u32 hi = csm ? cbp + psm_blocks(PSMCT32, cbw ? cbw : 1, cov + 16)
                     : cbp + (n == 256 ? ((cpsm == PSMCT32 || cpsm == PSMCT24) ? 4 : 2) : 1);
        if (batch_pages_hit(cbp, hi)) gs_flush();
        static int clog = -1; if (clog < 0) clog = getenv("PS2_CLUTLOG") ? 400 : 0;
        if (clog > 0 && gsgpu_page_gpu_newer(cbp / 32)) {
            clog--;
            fprintf(stderr, "[clut] cbp %05x cpsm %x csm %u psm %02x n %d  page %u marked by bp %05x psm %02x rect %d,%d..%d,%d\n",
                    cbp, cpsm, csm, psm, n, cbp / 32, g_page_mark[cbp / 32 & 511].bp, g_page_mark[cbp / 32 & 511].psm,
                    g_page_mark[cbp / 32 & 511].x0, g_page_mark[cbp / 32 & 511].y0, g_page_mark[cbp / 32 & 511].x1, g_page_mark[cbp / 32 & 511].y1);
        }
        int fresh = cpsm == PSMCT32 || cpsm == PSMCT24;   /* every CLUT word rewritten on the CPU since the GPU drew there? */
        for (int i = 0; i < n && fresh; i++) {
            u32 x, y;
            if (csm) { x = cou * 16 + i; y = cov; }
            else if (n == 256) { u32 j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); x = j & 15; y = j >> 4; }
            else { x = i & 7; y = i >> 3; }
            u32 w = addr32(cbp, csm ? cbw : 1, x, y, blk32);
            fresh = (g_fresh[(w >> 6) & 16383] >> (w & 63)) & 1;
        }
        /* a 256-entry CT32 CLUT the GPU drew (render-to-CLUT): 8-bit textures read it from VRAM on the GPU
         * (GSF_CLUTVRAM) instead of waiting for a readback; other cases still read back */
        g_clutvram = 0;
        if (!fresh && (cpsm == PSMCT32) && !csm && n == 256 && gsgpu_page_gpu_newer(cbp / 32) && !getenv("PS2_NOCLUTVRAM")) {
            g_clutvram = 1; g_clutvram_cbp = cbp;
        } else if (!fresh) {
            static int rl = -1; if (rl < 0) rl = getenv("PS2_CLUTLOG") ? 60 : 0;
            if (rl > 0 && (gsgpu_page_gpu_newer(cbp / 32) || gsgpu_page_gpu_newer((hi - 1) / 32))) {
                rl--; fprintf(stderr, "[clutrb] cbp %05x hi %05x cpsm %x csm %u psm %02x n %d cld %u newer %d/%d\n", cbp, hi, cpsm, csm, psm, n, cld,
                              gsgpu_page_gpu_newer(cbp / 32), gsgpu_page_gpu_newer((hi - 1) / 32)); }
            RB(0, gsgpu_sync_cpu(cbp, hi));
        }
    } else {
        gs_flush();   /* pending draws may have written the CLUT source */
        if (gpu_on()) gsgpu_sync_cpu(cbp, cbp + psm_blocks(PSMCT32, csm ? (cbw ? cbw : 1) : 1, (csm ? cov : 0) + 16));
    }
    for (int i = 0; i < n; i++) {
        u32 x, y;
        if (csm) { x = cou * 16 + i; y = cov; }
        else if (n == 256) { u32 j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); x = j & 15; y = j >> 4; }
        else { x = i & 7; y = i >> 3; }
        u32 bw = csm ? cbw : 1;
        if (cpsm == PSMCT32 || cpsm == PSMCT24) {
            u32 v = vram_read(PSMCT32, cbp, bw, x, y);
            G.clut32[(n == 16 ? (csa & 15) * 16 : 0) + i] = v;
        } else {
            u32 v = vram_read(cpsm == 2 ? PSMCT16 : PSMCT16S, cbp, bw, x, y);
            G.clut16[(n == 16 ? csa * 16 : 0) + i] = (u16)v;
        }
    }
    { /* advance the CLUT generation only if the contents actually changed (CLD=1 reloads are common) */
      static u64 last = 0;
      u64 h = 1469598103934665603ull;
      const u8 *p = (const u8 *)G.clut32; for (size_t i = 0; i < sizeof G.clut32; i++) h = (h ^ p[i]) * 1099511628211ull;
      p = (const u8 *)G.clut16; for (size_t i = 0; i < sizeof G.clut16; i++) h = (h ^ p[i]) * 1099511628211ull;
      if (h != last) { last = h; g_clut_gen++; } }
}

/* ------------------------------------------------------------------ draw state snapshot */
typedef struct {
    Ctx cxs; const Ctx *cx;
    u64 texa, colclamp, pabe;
    u32 fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmsk;
    int sx0, sx1, sy0, sy1;
    int tme, abe, fst, fge, iip;
    u32 test;
    int f32, z32, need_dst, ztst, ate;
    /* texture facts */
    u32 tbp, tbw, tpsm, cpsm, csa, tcc, tfx, wms, wmt, minu, maxu, minv, maxv;
    int tw, th, bilinear;
    float twf, thf;
    const u32 *tlin;    /* decoded linear RGBA copy of the texture (texture cache), or NULL */
    u32 clut_snap;      /* GPU path: index of the batch CLUT snapshot */
    u32 clut_vram;      /* GPU path: 1 + cbp when the CLUT is read from VRAM (GSF_CLUTVRAM), else 0 */
} GsDraw;

/* ------------------------------------------------------------------ VRAM write tracking
 * VRAM is tracked in 8 KB pages. Anything that writes VRAM bumps the generation of the pages it can
 * touch (transfers at start, draws when their batch is flushed); g_vram_epoch changes on every bump so
 * a cached texture can skip its page check when nothing was written since it was validated. */
static u32 g_page_gen[512];
static u32 g_vram_epoch = 1;
static void vram_touch(u32 block_lo, u32 block_hi) {   /* block = 256 bytes; pages are 32 blocks */
    u32 p0 = block_lo / 32, p1 = (block_hi + 31) / 32;
    if (p1 > 512) p1 = 512;
    for (u32 p = p0; p < p1; p++) g_page_gen[p]++;
    g_vram_epoch++;
}
/* blocks a PSM rectangle of width bw (64-pixel units) and height h rows can reach from bp */
static u32 psm_blocks(u32 psm, u32 bw, u32 rows) {
    u32 page_h = (psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24 || (psm & 0x0F) == 0x0B) ? 32
               : (psm == PSMT4 ? 128 : 64);
    u32 pages_per_row = psm == PSMT8 || psm == PSMT4 ? (bw + 1) / 2 : bw;
    if (!pages_per_row) pages_per_row = 1;
    return ((rows + page_h - 1) / page_h + 1) * pages_per_row * 32;
}

/* 16-bit -> RGBA8 with TEXA alpha expansion */
static inline u32 c16_to_32(const GsDraw *d, u32 v) {
    u32 r = (v & 0x1F) << 3, g = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
    u32 aem = (d->texa >> 15) & 1, ta0 = d->texa & 0xFF, ta1 = (d->texa >> 32) & 0xFF;
    u32 a = (v & 0x8000) ? ta1 : ((aem && !(v & 0x7FFF)) ? 0 : ta0);
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* ------------------------------------------------------------------ texture fetch */
static u32 tex_texel(const GsDraw *d, int u, int v) {
    u32 tbp = d->tbp, tbw = d->tbw, psm = d->tpsm;
    u32 aem = (d->texa >> 15) & 1, ta0 = d->texa & 0xFF;
    switch (psm) {
    case PSMCT32: return V32(addr32(tbp, tbw, u, v, blk32));
    case PSMCT24: { u32 c = V32(addr32(tbp, tbw, u, v, blk32)) & 0xFFFFFF; return c | ((aem && !c) ? 0 : ta0 << 24); }
    case PSMCT16: case PSMCT16S: return c16_to_32(d, vram_read(psm, tbp, tbw, u, v));
    /* Z buffers used as textures (depth effects): same bits, read with the Z swizzle */
    case PSMZ32: return V32(addr32(tbp, tbw, u, v, blkz32));
    case PSMZ24: { u32 c = V32(addr32(tbp, tbw, u, v, blkz32)) & 0xFFFFFF; return c | ((aem && !c) ? 0 : ta0 << 24); }
    case PSMZ16: case PSMZ16S: return c16_to_32(d, vram_read(psm, tbp, tbw, u, v));
    case PSMT8: case PSMT8H: case PSMT4: case PSMT4HL: case PSMT4HH: {
        u32 idx;
        if (psm == PSMT8) idx = g_vram[addr8(tbp, tbw, u, v)];
        else if (psm == PSMT4) idx = vram_read(PSMT4, tbp, tbw, u, v);
        else { u32 w = V32(addr32(tbp, tbw, u, v, blk32)); idx = psm == PSMT8H ? w >> 24 : psm == PSMT4HL ? (w >> 24) & 15 : w >> 28; }
        int is4 = psm != PSMT8 && psm != PSMT8H;
        if (d->cpsm == PSMCT32 || d->cpsm == PSMCT24) return G.clut32[(is4 ? (d->csa & 15) * 16 : 0) + idx];
        return c16_to_32(d, G.clut16[(is4 ? d->csa * 16 : 0) + idx]);
    }
    }
    return 0xFFFF00FFu;
}

/* ------------------------------------------------------------------ texture cache
 * Textures are decoded (swizzle, CLUT, TEXA) into linear RGBA once and reused until a page they live
 * in is written. Lookups happen on the GS thread at enqueue time; workers only read the buffers. */
static u64 g_batch_seq = 1;           /* id of the batch currently being filled */
typedef struct {
    u64 tex0_key; u32 texa_key, clut_gen; int tw, th;
    u32 p0, p1, gen_sum, epoch;       /* page range and generation sum at decode time */
    u64 used_batch, lru;
    u32 *buf; size_t cap;
} TexEntry;
#define TEXC_N 64
static TexEntry g_texc[TEXC_N];
static u64 g_texc_clock;
static int g_texc_on = -1;
static u64 g_texc_hits, g_texc_builds, g_flush_count, g_flush_prims;
u64 g_view_flushes, g_copies, g_tile_pairs, g_gpu_tiles, g_copy_bytes;

static u32 tex_texel(const GsDraw *d, int u, int v);
void gs_flush(void);
/* VRAM block ranges the pending batch may write (frame buffers, Z buffers), kept separate so that a
 * texture lying between a frame and a Z buffer is not mistaken for a render target */
/* Each range also records its view (format, base, width): on the GPU, the same memory written through two
 * different views lands in different screen tiles, which run in parallel, so such draws must not share a batch. */
typedef struct { u32 lo, hi, psm, bp, bw; } BlkRange;
#define NBW 16
static BlkRange g_bw[NBW]; static int g_nbw;
static int batch_writes(u32 lo, u32 hi) {
    for (int i = 0; i < g_nbw; i++) if (lo < g_bw[i].hi && hi > g_bw[i].lo) return 1;
    return 0;
}
/* GPU: 8 KB pages the pending batch may write (scissor rectangle of each target, page-exact) */
static u8 g_batch_pages[512]; static int g_batch_pages_any;
static void batch_mark_pages(u32 bp, u32 bw, u32 psm, int sx0, int sx1, int sy0, int sy1) {
    u32 pw = 64, ph = (psm == 0x00 || psm == 0x01 || psm == 0x30 || psm == 0x31) ? 32 : 64;
    u32 cols = bw ? bw : 10;
    if (sx0 < 0) sx0 = 0; if (sy0 < 0) sy0 = 0;
    if (sx1 < sx0 || sy1 < sy0) return;
    for (u32 pr = (u32)sy0 / ph; pr <= (u32)sy1 / ph; pr++)
        for (u32 pc = (u32)sx0 / pw; pc <= (u32)sx1 / pw; pc++) {
            u32 page = bp / 32 + pr * cols + pc;
            if (bp % 32) { if (page + 1 < 512) g_batch_pages[page + 1] = 1; }   /* unaligned base: spans two pages */
            if (page < 512) {
                g_batch_pages[page] = 1;
                g_page_mark[page].bp = bp; g_page_mark[page].psm = psm;
                g_page_mark[page].x0 = sx0; g_page_mark[page].y0 = sy0; g_page_mark[page].x1 = sx1; g_page_mark[page].y1 = sy1;
            }
        }
    g_batch_pages_any = 1;
}
static int batch_pages_hit(u32 lo, u32 hi) {   /* may the pending batch write blocks [lo, hi)? */
    if (!g_batch_pages_any) return 0;
    for (u32 p = lo / 32; p <= (hi - 1) / 32 && p < 512; p++) if (g_batch_pages[p]) return 1;
    return 0;
}

/* exact block extent of a render target drawn at rows 0..sy1 (scissor) with width fbw (64-pixel units) */
static u32 target_blocks_x(u32 psm, u32 fbw, int sx1, int sy1) {
    u32 page_h = (psm == 0x00 || psm == 0x01 || psm == 0x30 || psm == 0x31) ? 32 : 64;   /* 32-bit vs 16-bit pages */
    u32 bw = fbw ? fbw : 10;
    u32 rows = (u32)(sy1 < 0 ? 0 : sy1) + 1;
    u32 pages = (rows + page_h - 1) / page_h * bw;
    u32 last = (u32)(sy1 < 0 ? 0 : sy1) / page_h * bw + (u32)(sx1 < 0 ? 0 : sx1) / 64 + 1;   /* x beyond the width spills over */
    return (pages > last ? pages : last) * 32;
}
/* the batch writes this range through a different pixel format (swizzle)? Such draws must not share a
 * GPU batch: one view's pixels land in other tiles of the other view, which run in parallel. */
static int same_view(const BlkRange *r, u32 psm, u32 bp, u32 bw) { return r->psm == psm && r->bp == bp && r->bw == bw; }
/* would writing [lo, hi) through this view race with pending writes (or not fit in the table)? */
static int batch_view_conflict(u32 lo, u32 hi, u32 psm, u32 bp, u32 bw) {
    int fits = g_nbw < NBW;
    for (int i = 0; i < g_nbw; i++) {
        int same = same_view(&g_bw[i], psm, bp, bw);
        if (lo < g_bw[i].hi && hi > g_bw[i].lo && !same) return 1;
        if (same && lo <= g_bw[i].hi && hi >= g_bw[i].lo) fits = 1;
    }
    return !fits;
}
static void batch_add_write(u32 lo, u32 hi, u32 psm, u32 bp, u32 bw) {
    for (int i = 0; i < g_nbw; i++)
        if (lo <= g_bw[i].hi && hi >= g_bw[i].lo && same_view(&g_bw[i], psm, bp, bw)) {   /* overlapping or adjacent: merge */
            if (lo < g_bw[i].lo) g_bw[i].lo = lo;
            if (hi > g_bw[i].hi) g_bw[i].hi = hi;
            return;
        }
    if (g_nbw < NBW) { BlkRange r = {lo, hi, psm, bp, bw}; g_bw[g_nbw++] = r; return; }
    /* out of slots (only on the CPU path, where views cannot race): widen the last one */
    if (lo < g_bw[NBW - 1].lo) g_bw[NBW - 1].lo = lo;
    if (hi > g_bw[NBW - 1].hi) g_bw[NBW - 1].hi = hi;
}

static const u32 *texc_lookup(const GsDraw *d) {
    if (g_texc_on < 0) g_texc_on = getenv("PS2_NOTEXCACHE") == NULL;
    if (!g_texc_on || d->tw * d->th > 512 * 512) return NULL;
    int paletted = d->tpsm == PSMT8 || d->tpsm == PSMT8H || d->tpsm == PSMT4 || d->tpsm == PSMT4HL || d->tpsm == PSMT4HH;
    int uses_texa = d->tpsm == PSMCT24 || d->tpsm == PSMCT16 || d->tpsm == PSMCT16S || d->tpsm == PSMZ24 || d->tpsm == PSMZ16 ||
                    d->tpsm == PSMZ16S || (paletted && d->cpsm != PSMCT32 && d->cpsm != PSMCT24);
    /* key: TBP, TBW, PSM, TW, TH, CPSM, CSA (TCC/TFX/CLD do not affect texel values) */
    u64 key = (d->tbp) | ((u64)d->tbw << 14) | ((u64)d->tpsm << 20) | ((u64)d->tw << 26) | ((u64)d->th << 38) |
              (paletted ? ((u64)d->cpsm << 50) | ((u64)d->csa << 54) : 0);
    u32 texa_key = uses_texa ? (u32)(d->texa & 0xFF) | (u32)((d->texa >> 15) & 1) << 8 | (u32)((d->texa >> 32) & 0xFF) << 16 : 0;
    u32 clut_gen = paletted ? g_clut_gen : 0;
    u32 p0 = d->tbp / 32, p1 = (d->tbp + psm_blocks(d->tpsm, d->tbw, (u32)d->th) + 31) / 32;
    if (p1 > 512) p1 = 512;
    /* pending draws in this batch may write the texture: draw them first */
    if (batch_writes(d->tbp, p1 * 32)) gs_flush();
    TexEntry *e = NULL, *victim = NULL;
    for (int i = 0; i < TEXC_N; i++) {
        TexEntry *t = &g_texc[i];
        if (t->buf && t->tex0_key == key && t->texa_key == texa_key && t->clut_gen == clut_gen) { e = t; break; }
        if (!victim || t->lru < victim->lru) victim = t;
    }
    if (e) {
        if (e->epoch == g_vram_epoch) goto hit;
        u32 sum = 0; for (u32 p = e->p0; p < e->p1; p++) sum += g_page_gen[p];
        if (sum == e->gen_sum) { e->epoch = g_vram_epoch; goto hit; }
    } else e = victim;
    /* (re)decode; if a pending batch still reads this entry's old contents, draw it first */
    if (e->used_batch == g_batch_seq) gs_flush();
    size_t need = (size_t)d->tw * d->th;
    if (e->cap < need) { free(e->buf); e->buf = malloc(need * 4); e->cap = need; }
    for (int v = 0; v < d->th; v++) for (int u = 0; u < d->tw; u++) e->buf[(size_t)v * d->tw + u] = tex_texel(d, u, v);
    e->tex0_key = key; e->texa_key = texa_key; e->clut_gen = clut_gen; e->tw = d->tw; e->th = d->th;
    e->p0 = p0; e->p1 = p1;
    { u32 sum = 0; for (u32 p = p0; p < p1; p++) sum += g_page_gen[p]; e->gen_sum = sum; }
    e->epoch = g_vram_epoch;
    g_texc_builds++;
hit:
    e->lru = ++g_texc_clock; e->used_batch = g_batch_seq;
    g_texc_hits++;
    return e->buf;
}

static inline int wrap_coord(int c, int size, u32 wm, u32 mn, u32 mx) {
    switch (wm) {
    case 0: return c & (size - 1);
    case 1: return c < 0 ? 0 : c >= size ? size - 1 : c;
    case 2: return c < (int)mn ? (int)mn : c > (int)mx ? (int)mx : c;
    default: return (int)(((u32)c & mn) | mx);     /* region repeat: MINU = mask, MAXU = fix */
    }
}

/* texel from the decoded copy when the coordinate is inside it, else from VRAM */
static inline u32 texel_at(const GsDraw *d, int u, int v) {
    if (d->tlin && (unsigned)u < (unsigned)d->tw && (unsigned)v < (unsigned)d->th) return d->tlin[v * d->tw + u];
    return tex_texel(d, u, v);
}

static inline u32 tex_sample(const GsDraw *d, float fu, float fv) {
    if (!d->bilinear) {
        int iu = fu >= 0 ? (int)fu : (int)floorf(fu), iv = fv >= 0 ? (int)fv : (int)floorf(fv);
        if (d->tlin && d->wms == 0 && d->wmt == 0) return d->tlin[(iv & (d->th - 1)) * d->tw + (iu & (d->tw - 1))];
        if (d->tpsm == PSMCT32 && d->wms < 2 && d->wmt < 2 && (unsigned)iu < (unsigned)d->tw && (unsigned)iv < (unsigned)d->th)
            return V32(addr32(d->tbp, d->tbw, iu, iv, blk32));
        return texel_at(d, wrap_coord(iu, d->tw, d->wms, d->minu, d->maxu), wrap_coord(iv, d->th, d->wmt, d->minv, d->maxv));
    }
    fu -= 0.5f; fv -= 0.5f;
    int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
    float au = fu - u0, av = fv - v0;
    int ua = wrap_coord(u0, d->tw, d->wms, d->minu, d->maxu), ub = wrap_coord(u0 + 1, d->tw, d->wms, d->minu, d->maxu);
    int va = wrap_coord(v0, d->th, d->wmt, d->minv, d->maxv), vb = wrap_coord(v0 + 1, d->th, d->wmt, d->minv, d->maxv);
    u32 c0 = texel_at(d, ua, va), c1 = texel_at(d, ub, va), c2 = texel_at(d, ua, vb), c3 = texel_at(d, ub, vb);
    if (c0 == c1 && c0 == c2 && c0 == c3) return c0;
    /* fixed point (8-bit weights), two channels at a time in 32-bit lanes */
    u32 fu8 = (u32)(au * 256.0f), fv8 = (u32)(av * 256.0f);
    if (fu8 > 256) fu8 = 256; if (fv8 > 256) fv8 = 256;
    u32 rb0 = c0 & 0x00FF00FFu, rb1 = c1 & 0x00FF00FFu, rb2 = c2 & 0x00FF00FFu, rb3 = c3 & 0x00FF00FFu;
    u32 ag0 = (c0 >> 8) & 0x00FF00FFu, ag1 = (c1 >> 8) & 0x00FF00FFu, ag2 = (c2 >> 8) & 0x00FF00FFu, ag3 = (c3 >> 8) & 0x00FF00FFu;
    u32 rbt = (rb0 * (256 - fu8) + rb1 * fu8) >> 8 & 0x00FF00FFu, rbb = (rb2 * (256 - fu8) + rb3 * fu8) >> 8 & 0x00FF00FFu;
    u32 agt = (ag0 * (256 - fu8) + ag1 * fu8) >> 8 & 0x00FF00FFu, agb = (ag2 * (256 - fu8) + ag3 * fu8) >> 8 & 0x00FF00FFu;
    u32 rb = (rbt * (256 - fv8) + rbb * fv8 + 0x00800080u) >> 8 & 0x00FF00FFu;
    u32 ag = (agt * (256 - fv8) + agb * fv8 + 0x00800080u) >> 8 & 0x00FF00FFu;
    return rb | (ag << 8);
}

/* ------------------------------------------------------------------ pixel pipeline */
static void setup_draw(GsDraw *d) {
    u64 p = (G.prmodecont & 1) ? G.prim : G.prmode;
    int ctxt = (p >> 9) & 1;
    d->cxs = G.ctx[ctxt];
    d->cx = &d->cxs;
    const Ctx *c = d->cx;
    d->texa = G.texa; d->colclamp = G.colclamp; d->pabe = G.pabe;
    d->iip = (p >> 3) & 1; d->tme = (p >> 4) & 1; d->fge = (p >> 5) & 1; d->abe = (p >> 6) & 1; d->fst = (p >> 8) & 1;
    d->fbp = (u32)(c->frame & 0x1FF) * 32; d->fbw = (c->frame >> 16) & 63; d->fpsm = (c->frame >> 24) & 63;
    d->fbmsk = (u32)(c->frame >> 32);
    d->zbp = (u32)(c->zbuf & 0x1FF) * 32; d->zpsm = 0x30 | ((c->zbuf >> 24) & 15); d->zmsk = (c->zbuf >> 32) & 1;
    d->sx0 = c->scissor & 0x7FF; d->sx1 = (c->scissor >> 16) & 0x7FF;
    d->sy0 = (c->scissor >> 32) & 0x7FF; d->sy1 = (c->scissor >> 48) & 0x7FF;
    d->test = (u32)c->test;
    d->f32 = d->fpsm == PSMCT32 || d->fpsm == PSMCT24;
    d->z32 = d->zpsm == PSMZ32 || d->zpsm == PSMZ24;
    d->ate = (d->test & 1) && ((d->test >> 1) & 7) != 1;
    d->ztst = ((d->test >> 16) & 1) ? (int)((d->test >> 17) & 3) : -1;
    d->need_dst = d->abe || d->fbmsk || ((d->test >> 14) & 1) || !d->f32 || d->fpsm == PSMCT24 || d->ate;
    u64 t0 = c->tex0, cl = c->clamp;
    d->tbp = t0 & 0x3FFF; d->tbw = (t0 >> 14) & 63; d->tpsm = (t0 >> 20) & 63;
    d->tw = 1 << ((t0 >> 26) & 15); d->th = 1 << ((t0 >> 30) & 15);
    d->twf = (float)d->tw; d->thf = (float)d->th;
    d->tcc = (t0 >> 34) & 1; d->tfx = (t0 >> 35) & 3; d->cpsm = (t0 >> 51) & 15; d->csa = (t0 >> 56) & 31;
    d->wms = cl & 3; d->wmt = (cl >> 2) & 3; d->minu = (cl >> 4) & 1023; d->maxu = (cl >> 14) & 1023;
    d->minv = (cl >> 24) & 1023; d->maxv = (cl >> 34) & 1023;
    d->bilinear = (c->tex1 >> 5) & 1;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void put_pixel_slow(const GsDraw *d, int x, int y, u32 z, int r, int g, int b, int a);
static inline void put_pixel(const GsDraw *d, int x, int y, u32 z, int r, int g, int b, int a) {
    if (x < d->sx0 || x > d->sx1 || y < d->sy0 || y > d->sy1) return;
    if (!d->need_dst && d->f32 && (d->ztst < 0 || (d->z32 && d->ztst >= 1))) {
        /* fast path: 32-bit frame, no blend/mask/alpha test; 32-bit Z or no Z test */
        if (d->ztst == 0) return;
        u32 zi = 0;
        if (d->ztst >= 1) {
            u32 zmax = d->zpsm == PSMZ32 ? 0xFFFFFFFFu : 0xFFFFFFu;
            if (z > zmax) z = zmax;
            zi = addr32(d->zbp, d->fbw, x, y, blkz32);
            if (d->ztst >= 2) { u32 zd = V32(zi) & zmax; if (d->ztst == 2 ? z < zd : z <= zd) return; }
        }
        if (d->colclamp & 1) { r = clamp255(r); g = clamp255(g); b = clamp255(b); } else { r &= 255; g &= 255; b &= 255; }
        if (d->cx->fba & 1) a |= 0x80;
        V32(addr32(d->fbp, d->fbw, x, y, blk32)) = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)(a & 255) << 24);
        if (d->ztst >= 1 && !d->zmsk) {
            if (d->zpsm == PSMZ32) V32(zi) = z; else V32(zi) = (V32(zi) & 0xFF000000u) | (z & 0xFFFFFF);
        }
        return;
    }
    put_pixel_slow(d, x, y, z, r, g, b, a);
}

/* 32-bit frame (CT32/CT24) with 32/24-bit Z or no Z test: same rules as the generic path below, but
 * each swizzled address is computed once and reused for test, blend and write. */
static void put_pixel32(const GsDraw *d, int x, int y, u32 z, int r, int g, int b, int a) {
    u32 test = d->test;
    int zwrite = !d->zmsk, fwrite = 1, awrite = 1;
    if (test & 1) {
        u32 atst = (test >> 1) & 7, aref = (test >> 4) & 0xFF;
        int pass;
        switch (atst) {
        case 0: pass = 0; break; case 1: pass = 1; break;
        case 2: pass = a < (int)aref; break; case 3: pass = a <= (int)aref; break;
        case 4: pass = a == (int)aref; break; case 5: pass = a >= (int)aref; break;
        case 6: pass = a > (int)aref; break; default: pass = a != (int)aref; break;
        }
        if (!pass) {
            switch ((test >> 12) & 3) {
            case 0: return;
            case 1: zwrite = 0; break;
            case 2: fwrite = 0; break;
            case 3: zwrite = 0; awrite = 0; break;
            }
        }
    }
    u32 *zp = NULL, zmax = d->zpsm == PSMZ32 ? 0xFFFFFFFFu : 0xFFFFFF;
    if (z > zmax) z = zmax;
    if ((test >> 16) & 1) {
        u32 zt = (test >> 17) & 3;
        if (zt == 0) return;
        zp = &V32(addr32(d->zbp, d->fbw, x, y, blkz32));
        if (zt >= 2) { u32 zd = *zp & zmax; if (zt == 2 ? z < zd : z <= zd) return; }
    } else zwrite = 0;
    u32 *fp = &V32(addr32(d->fbp, d->fbw, x, y, blk32));
    u32 dv = *fp;
    if ((test >> 14) & 1) { if ((int)((dv >> 31) & 1) != (int)((test >> 15) & 1)) return; }
    if (fwrite) {
        int dr = dv & 255, dg = (dv >> 8) & 255, db = (dv >> 16) & 255, da = d->fpsm == PSMCT24 ? 0x80 : (int)(dv >> 24);
        int pabe_skip = (d->pabe & 1) && !(a & 0x80);
        if (d->abe && !pabe_skip) {
            u64 al = d->cx->alpha;
            int A = al & 3, B = (al >> 2) & 3, C = (al >> 4) & 3, D = (al >> 6) & 3, fix = (al >> 32) & 255;
            int cc = C == 0 ? a : C == 1 ? da : fix;
            int sr = r, sg = g, sb = b;
            #define BLEND1(cs, cd) ((((A == 0 ? cs : A == 1 ? cd : 0) - (B == 0 ? cs : B == 1 ? cd : 0)) * cc >> 7) + (D == 0 ? cs : D == 1 ? cd : 0))
            r = BLEND1(sr, dr); g = BLEND1(sg, dg); b = BLEND1(sb, db);
            #undef BLEND1
        }
        if (d->colclamp & 1) { r = clamp255(r); g = clamp255(g); b = clamp255(b); }
        else { r &= 255; g &= 255; b &= 255; }
        if (d->cx->fba & 1) a |= 0x80;
        if (!awrite) a = da;
        u32 nv = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)(a & 255) << 24);
        nv = (nv & ~d->fbmsk) | (dv & d->fbmsk);
        if (d->fpsm == PSMCT24) nv = (dv & 0xFF000000u) | (nv & 0xFFFFFF);
        *fp = nv;
    }
    if (zwrite && zp) *zp = d->zpsm == PSMZ32 ? z : (*zp & 0xFF000000u) | (z & 0xFFFFFF);
}

static void put_pixel_slow(const GsDraw *d, int x, int y, u32 z, int r, int g, int b, int a) {
    if (d->f32 && (d->zpsm == PSMZ32 || d->zpsm == PSMZ24 || !((d->test >> 16) & 1))) { put_pixel32(d, x, y, z, r, g, b, a); return; }
    u32 test = d->test;
    int zwrite = !d->zmsk, fwrite = 1, awrite = 1;
    if (test & 1) {
        u32 atst = (test >> 1) & 7, aref = (test >> 4) & 0xFF;
        int pass;
        switch (atst) {
        case 0: pass = 0; break; case 1: pass = 1; break;
        case 2: pass = a < (int)aref; break; case 3: pass = a <= (int)aref; break;
        case 4: pass = a == (int)aref; break; case 5: pass = a >= (int)aref; break;
        case 6: pass = a > (int)aref; break; default: pass = a != (int)aref; break;
        }
        if (!pass) {
            switch ((test >> 12) & 3) {
            case 0: return;
            case 1: zwrite = 0; break;            /* FB_ONLY */
            case 2: fwrite = 0; break;            /* ZB_ONLY */
            case 3: zwrite = 0; awrite = 0; break;/* RGB_ONLY */
            }
        }
    }
    u32 zmax = d->zpsm == PSMZ32 ? 0xFFFFFFFFu : d->zpsm == PSMZ24 ? 0xFFFFFF : 0xFFFF;
    if (z > zmax) z = zmax;
    if ((test >> 16) & 1) {
        u32 zt = (test >> 17) & 3;
        if (zt == 0) return;
        if (zt >= 2) {
            u32 zd = vram_read(d->zpsm, d->zbp, d->fbw, x, y) & zmax;
            if (zt == 2 ? z < zd : z <= zd) return;
        }
    } else zwrite = 0;
    if ((test >> 14) & 1) {
        u32 dv = vram_read(d->fpsm, d->fbp, d->fbw, x, y);
        int dbit = (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S) ? (dv >> 15) & 1 : (dv >> 31) & 1;
        if (dbit != (int)((test >> 15) & 1)) return;
    }
    if (fwrite) {
        u32 dv = vram_read(d->fpsm, d->fbp, d->fbw, x, y);
        int dr, dg, db, da;
        if (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S) {
            dr = (dv & 0x1F) << 3; dg = ((dv >> 5) & 0x1F) << 3; db = ((dv >> 10) & 0x1F) << 3; da = (dv & 0x8000) ? 0x80 : 0;
        } else { dr = dv & 255; dg = (dv >> 8) & 255; db = (dv >> 16) & 255; da = d->fpsm == PSMCT24 ? 0x80 : dv >> 24; }
        int pabe_skip = (d->pabe & 1) && !(a & 0x80);
        if (d->abe && !pabe_skip) {
            u64 al = d->cx->alpha;
            int A = al & 3, B = (al >> 2) & 3, C = (al >> 4) & 3, D = (al >> 6) & 3, fix = (al >> 32) & 255;
            int cs[3] = {r, g, b}, cd[3] = {dr, dg, db}, out[3];
            int cc = C == 0 ? a : C == 1 ? da : fix;
            for (int i = 0; i < 3; i++) {
                int va = A == 0 ? cs[i] : A == 1 ? cd[i] : 0;
                int vb = B == 0 ? cs[i] : B == 1 ? cd[i] : 0;
                int vd = D == 0 ? cs[i] : D == 1 ? cd[i] : 0;
                out[i] = (((va - vb) * cc) >> 7) + vd;
            }
            r = out[0]; g = out[1]; b = out[2];
        }
        if (d->colclamp & 1) { r = clamp255(r); g = clamp255(g); b = clamp255(b); }
        else { r &= 255; g &= 255; b &= 255; }
        if (d->cx->fba & 1) a |= 0x80;
        if (!awrite) a = da;
        u32 nv;
        if (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S)
            nv = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a & 0x80) ? 0x8000 : 0);
        else nv = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)(a & 255) << 24);
        u32 msk = d->fbmsk;
        if (d->fpsm == PSMCT16 || d->fpsm == PSMCT16S)
            msk = ((msk >> 3) & 0x1F) | ((msk >> 6) & 0x3E0) | ((msk >> 9) & 0x7C00) | ((msk >> 16) & 0x8000);
        nv = (nv & ~msk) | (dv & msk);
        vram_write(d->fpsm, d->fbp, d->fbw, x, y, nv);
    }
    if (zwrite) vram_write(d->zpsm, d->zbp, d->fbw, x, y, z);
}

/* fragment counters per raster worker (index = worker id), for PS2_GFXSTATS */
static volatile u64 g_frag_count[16][8];   /* [worker][0] = fragments shaded; padded to separate cache lines */
static __declspec(thread) int t_worker_id;

/* shade one fragment: texture + TFX */
static inline void shade(const GsDraw *d, int x, int y, u32 z, float r, float g, float b, float a, float s, float t, float q, float u, float v) {
    g_frag_count[t_worker_id][0]++;
    /* Early Z: a fragment that fails the depth test is discarded whatever the alpha test says, so test
     * before the (expensive) texture fetch. Workers own disjoint rows, so reading Z here is safe. */
    if (d->ztst >= 2 && d->z32) {
        u32 zmax = d->zpsm == PSMZ32 ? 0xFFFFFFFFu : 0xFFFFFFu, zz = z > zmax ? zmax : z;
        u32 zd = V32(addr32(d->zbp, d->fbw, x, y, blkz32)) & zmax;
        if (d->ztst == 2 ? zz < zd : zz <= zd) return;
    }
    int R = (int)r, Gc = (int)g, B = (int)b, A = (int)a;
    if (d->tme) {
        float fu, fv;
        if (d->fst) { fu = u; fv = v; }
        else { float iq = q != 0.0f ? 1.0f / q : 0.0f; fu = s * iq * d->twf; fv = t * iq * d->thf; }
        u32 tc = tex_sample(d, fu, fv);
        int tr = tc & 255, tg = (tc >> 8) & 255, tb = (tc >> 16) & 255, ta = tc >> 24;
        switch (d->tfx) {
        case 0: R = (tr * R) >> 7; Gc = (tg * Gc) >> 7; B = (tb * B) >> 7; A = d->tcc ? (ta * A) >> 7 : A; break;
        case 1: R = tr; Gc = tg; B = tb; A = d->tcc ? ta : A; break;
        case 2: R = ((tr * R) >> 7) + A; Gc = ((tg * Gc) >> 7) + A; B = ((tb * B) >> 7) + A; A = d->tcc ? ta + A : A; break;
        default: R = ((tr * R) >> 7) + A; Gc = ((tg * Gc) >> 7) + A; B = ((tb * B) >> 7) + A; A = d->tcc ? ta : A; break;
        }
        R = clamp255(R); Gc = clamp255(Gc); B = clamp255(B); A = clamp255(A);
    }
    put_pixel(d, x, y, z, R, Gc, B, A);
}

/* ------------------------------------------------------------------ primitives (stripe-parallel)
 * Worker `bi` of `nb` owns rows with ((y >> 3) % nb) == bi. */
#define MINE(y) ((((unsigned)(y) >> 3) % (unsigned)nb) == (unsigned)bi)

static void draw_sprite(const GsDraw *dp, const Vtx *v0, const Vtx *v1, int bi, int nb) {
    const GsDraw *d = dp;
    float x0 = v0->x, x1 = v1->x, y0 = v0->y, y1 = v1->y;
    float s0 = v0->s, s1 = v1->s, t0 = v0->t, t1 = v1->t, u0 = v0->u, u1 = v1->u, w0 = v0->v, w1 = v1->v;
    if (x0 > x1) { float t; t = x0; x0 = x1; x1 = t; t = s0; s0 = s1; s1 = t; t = u0; u0 = u1; u1 = t; }
    if (y0 > y1) { float t; t = y0; y0 = y1; y1 = t; t = t0; t0 = t1; t1 = t; t = w0; w0 = w1; w1 = t; }
    int ix0 = (int)ceilf(x0), ix1 = (int)ceilf(x1), iy0 = (int)ceilf(y0), iy1 = (int)ceilf(y1);
    if (ix0 < d->sx0) ix0 = d->sx0;
    if (iy0 < d->sy0) iy0 = d->sy0;
    if (ix1 > d->sx1 + 1) ix1 = d->sx1 + 1;
    if (iy1 > d->sy1 + 1) iy1 = d->sy1 + 1;
    float dx = x1 - x0, dy = y1 - y0;
    if (dx <= 0 || dy <= 0) return;
    float ds = (s1 - s0) / dx, du = (u1 - u0) / dx, dt = (t1 - t0) / dy, dw = (w1 - w0) / dy;
    float r = (float)v1->r, g = (float)v1->g, b = (float)v1->b, a = (float)v1->a;
    float sx0 = s0 + ds * (ix0 - x0), ux0 = u0 + du * (ix0 - x0);
    for (int y = iy0; y < iy1; y++) {
        if (!MINE(y)) continue;
        float t = t0 + dt * (y - y0), w = w0 + dw * (y - y0);
        float s = sx0, u = ux0;
        for (int x = ix0; x < ix1; x++, s += ds, u += du)
            shade(d, x, y, v1->z, r, g, b, a, s, t, v1->q, u, w);
    }
}

static void draw_triangle(const GsDraw *d, const Vtx *a, const Vtx *b, const Vtx *c, int bi, int nb) {
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0.0f) return;
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    int x0 = (int)ceilf(minx), x1 = (int)ceilf(maxx) - 1, y0 = (int)ceilf(miny), y1 = (int)ceilf(maxy) - 1;
    if (x0 < d->sx0) x0 = d->sx0;
    if (y0 < d->sy0) y0 = d->sy0;
    if (x1 > d->sx1) x1 = d->sx1;
    if (y1 > d->sy1) y1 = d->sy1;
    float ia = 1.0f / area;
    const Vtx *flat = c;   /* flat shading uses the last vertex */
    float dw0dx = -(c->y - b->y) * ia, dw1dx = -(a->y - c->y) * ia;
    float dw2dx = -(dw0dx + dw1dx);
    for (int y = y0; y <= y1; y++) {
        if (!MINE(y)) continue;
        float py = (float)y, px0 = (float)x0;
        float w0 = ((b->x - px0) * (c->y - py) - (c->x - px0) * (b->y - py)) * ia;
        float w1 = ((c->x - px0) * (a->y - py) - (a->x - px0) * (c->y - py)) * ia;
        /* span: each barycentric is linear in x (w = w_x0 + dw * (x - x0)) and must stay >= -1e-6, which
         * bounds x from one side; loop only over the intersection (+1 pixel margin), testing as before */
        {
            float lo = 0.0f, hi = (float)(x1 - x0);
            const float c3[3] = {w0, w1, 1.0f - w0 - w1}, d3[3] = {dw0dx, dw1dx, dw2dx};
            for (int k = 0; k < 3; k++) {
                if (d3[k] > 0.0f) { float t = (-1e-6f - c3[k]) / d3[k]; if (t > lo) lo = t; }
                else if (d3[k] < 0.0f) { float t = (-1e-6f - c3[k]) / d3[k]; if (t < hi) hi = t; }
                else if (c3[k] < -1e-6f) { lo = 1.0f; hi = 0.0f; }
            }
            if (lo > hi + 1.0f) continue;
            int sx = (int)lo - 1; if (sx < 0) sx = 0;
            int ex = (int)hi + 1; if (ex > x1 - x0) ex = x1 - x0;
            if (sx > ex) continue;
            if (sx > 0) { w0 += dw0dx * sx; w1 += dw1dx * sx; }
            int xe = x0 + ex;
            for (int x = x0 + sx; x <= xe; x++, w0 += dw0dx, w1 += dw1dx) {
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
            u32 z = (u32)(w0 * a->z + w1 * b->z + w2 * (double)c->z);
            float r, g, bb, al;
            if (d->iip) { r = w0 * a->r + w1 * b->r + w2 * c->r; g = w0 * a->g + w1 * b->g + w2 * c->g;
                          bb = w0 * a->b + w1 * b->b + w2 * c->b; al = w0 * a->a + w1 * b->a + w2 * c->a; }
            else { r = (float)flat->r; g = (float)flat->g; bb = (float)flat->b; al = (float)flat->a; }
            shade(d, x, y, z, r, g, bb, al,
                  w0 * a->s + w1 * b->s + w2 * c->s, w0 * a->t + w1 * b->t + w2 * c->t, w0 * a->q + w1 * b->q + w2 * c->q,
                  w0 * a->u + w1 * b->u + w2 * c->u, w0 * a->v + w1 * b->v + w2 * c->v);
            }
        }
    }
}

static void draw_line(const GsDraw *d, const Vtx *a, const Vtx *b, int bi, int nb) {
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy));
    if (n <= 0) n = 1;
    for (int i = 0; i < n; i++) {
        float f = (float)i / n;
        int y = (int)(a->y + dy * f);
        if (!MINE(y)) continue;
        const Vtx *cv = d->iip ? NULL : b;
        shade(d, (int)(a->x + dx * f), y, (u32)(a->z + (b->z - (double)a->z) * f),
              cv ? cv->r : a->r + (b->r - a->r) * f, cv ? cv->g : a->g + (b->g - a->g) * f,
              cv ? cv->b : a->b + (b->b - a->b) * f, cv ? cv->a : a->a + (b->a - a->a) * f,
              a->s + (b->s - a->s) * f, a->t + (b->t - a->t) * f, a->q + (b->q - a->q) * f,
              a->u + (b->u - a->u) * f, a->v + (b->v - a->v) * f);
    }
}

/* ------------------------------------------------------------------ draw batch + workers */
typedef struct { GsDraw d; Vtx v[3]; int type; } DrawCmd;   /* type: 0 point, 1 line, 3 triangle, 6 sprite */
#define BATCH_MAX 4096
static DrawCmd *g_batch;
static int g_nbatch;
static u32 g_batch_fb_lo = 0xFFFFFFFF, g_batch_fb_hi;   /* block range written by the batch */

static int g_nworkers = 1;
static HANDLE g_work_start[16], g_work_done;
static volatile LONG g_work_left;

static void run_batch(int bi, int nb) {
    for (int i = 0; i < g_nbatch; i++) {
        DrawCmd *c = &g_batch[i];
        switch (c->type) {
        case 0: if (MINE((int)c->v[0].y)) shade(&c->d, (int)c->v[0].x, (int)c->v[0].y, c->v[0].z, (float)c->v[0].r, (float)c->v[0].g,
                    (float)c->v[0].b, (float)c->v[0].a, c->v[0].s, c->v[0].t, c->v[0].q, c->v[0].u, c->v[0].v); break;
        case 1: draw_line(&c->d, &c->v[0], &c->v[1], bi, nb); break;
        case 3: draw_triangle(&c->d, &c->v[0], &c->v[1], &c->v[2], bi, nb); break;
        case 6: draw_sprite(&c->d, &c->v[0], &c->v[1], bi, nb); break;
        }
    }
}

static DWORD WINAPI raster_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    t_worker_id = id;
    for (;;) {
        WaitForSingleObject(g_work_start[id], INFINITE);
        run_batch(id, g_nworkers);
        if (InterlockedDecrement(&g_work_left) == 0) SetEvent(g_work_done);
    }
    return 0;
}

static void workers_init(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 1;           /* GS thread + workers; the EE often waits on the GS */
    const char *e = getenv("PS2_GS_THREADS");
    if (e) n = atoi(e);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    g_nworkers = n;
    g_batch = calloc(BATCH_MAX, sizeof(DrawCmd));
    g_work_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (int i = 1; i < n; i++) {           /* worker 0 is the GS thread itself */
        g_work_start[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, raster_worker, (void *)(intptr_t)i, 0, NULL);
    }
}

/* ------------------------------------------------------------------ GPU back end (gs_gpu.c) */
static int g_gpu = -1;   /* decided on first use, on the GS thread */
static int gpu_on(void) { if (g_gpu < 0) g_gpu = gsgpu_enabled(); return g_gpu; }
u8 *gs_vram_ptr(void) { if (!g_vram) g_vram = calloc(1, 4u << 20); return g_vram; }
void gs_fill_swizzle_tables(u32 *o, int n) {   /* layout = T_* offsets in gs_gpu.hlsl */
    memset(o, 0, (size_t)n * 4);
    for (int i = 0; i < 32; i++) { o[0 + i] = blk32[i / 8][i % 8]; o[32 + i] = blkz32[i / 8][i % 8]; }
    for (int i = 0; i < 64; i++) o[64 + i] = col32[i / 8][i % 8];
    for (int i = 0; i < 32; i++) { o[128 + i] = blk16[i / 4][i % 4]; o[160 + i] = blk16s[i / 4][i % 4];
                                   o[192 + i] = blkz16[i / 4][i % 4]; o[224 + i] = blkz16s[i / 4][i % 4]; }
    for (int i = 0; i < 128; i++) o[256 + i] = col16[i / 16][i % 16];
    for (int i = 0; i < 32; i++) o[384 + i] = blk8[i / 8][i % 8];
    for (int i = 0; i < 256; i++) o[416 + i] = col8[i / 16][i % 16];
    for (int i = 0; i < 32; i++) o[672 + i] = blk4[i / 4][i % 4];
    for (int i = 0; i < 512; i++) o[704 + i] = col4[i / 32][i % 32];
}

/* CLUT snapshots used by the pending batch (the GPU path keeps several CLUTs per batch, so a CLUT
 * reload does not have to end the batch) */
#define MAX_BCLUT 64
static u32 g_bclut[MAX_BCLUT][768]; static int g_nbclut; static u32 g_bclut_gen;
static u32 batch_clut(void) {
    if (g_nbclut && g_bclut_gen == g_clut_gen) return (u32)g_nbclut - 1;
    if (g_nbclut == MAX_BCLUT) gs_flush();
    u32 *c = g_bclut[g_nbclut];
    memcpy(c, G.clut32, 256 * 4);
    for (int i = 0; i < 512; i++) c[256 + i] = G.clut16[i];
    g_bclut_gen = g_clut_gen;
    return (u32)g_nbclut++;
}

static int gpu_supported(const GsDraw *d) {
    return d->fpsm == PSMCT32 || d->fpsm == PSMCT24 || d->fpsm == PSMCT16 || d->fpsm == PSMCT16S;
}

static void gstate_from(GState *s, const GsDraw *d) {
    memset(s, 0, sizeof *s);
    s->fbp = d->fbp; s->fbw = d->fbw; s->fpsm = d->fpsm; s->fbmsk = d->fbmsk;
    s->zbp = d->zbp; s->zpsm = d->zpsm; s->zmsk = d->zmsk; s->test = d->test;
    s->sx0 = d->sx0; s->sx1 = d->sx1; s->sy0 = d->sy0; s->sy1 = d->sy1;
    s->flags = (d->tme ? GSF_TME : 0) | (d->abe ? GSF_ABE : 0) | (d->fst ? GSF_FST : 0) | (d->iip ? GSF_IIP : 0) |
               ((d->colclamp & 1) ? GSF_COLCLAMP : 0) | ((d->pabe & 1) ? GSF_PABE : 0) | ((d->cx->fba & 1) ? GSF_FBA : 0);
    s->alpha = (u32)(d->cx->alpha & 0xFF); s->afix = (u32)((d->cx->alpha >> 32) & 255);
    if (d->tme) {
        s->flags |= (d->bilinear ? GSF_BILINEAR : 0) | (d->tcc ? GSF_TCC : 0);
        s->tbp = d->tbp; s->tbw = d->tbw; s->tpsm = d->tpsm; s->tw = (u32)d->tw; s->th = (u32)d->th; s->tfx = d->tfx;
        s->wms = d->wms; s->wmt = d->wmt; s->minu = d->minu; s->maxu = d->maxu; s->minv = d->minv; s->maxv = d->maxv;
        s->cpsm = d->cpsm; s->csa = d->csa;
        s->texa = (u32)(d->texa & 0xFF) | (u32)((d->texa >> 15) & 1) << 8 | (u32)((d->texa >> 32) & 0xFF) << 16;
        s->clut = d->clut_snap;
        if (d->clut_vram) { s->flags |= GSF_CLUTVRAM; s->clut = d->clut_vram - 1; }
        /* sampling its own frame/Z buffer: read the pre-dispatch copy (as the GS texture cache would) */
        u32 tlo = d->tbp, thi = d->tbp + psm_blocks(d->tpsm, d->tbw, (u32)d->th);
        u32 flo = d->fbp, fhi = d->fbp + target_blocks_x(d->fpsm, d->fbw, d->sx1, d->sy1);
        u32 zlo = d->zbp, zhi = d->zbp + target_blocks_x(d->zpsm, d->fbw, d->sx1, d->sy1);
        if ((tlo < fhi && thi > flo) || (d->ztst >= 1 && !d->zmsk && tlo < zhi && thi > zlo)) s->flags |= GSF_TEXCOPY;
    }
}

/* Convert the batch to GPU records, bin primitives into 16x16 tiles (in submission order) and submit. */
static void gpu_flush_batch(void) {
    static GState *st; static GPrim *pr; static u16 (*rect)[4]; static u32 *tcount, *tfirst, *touched, *tiles, *idx; static u32 idx_cap;
    if (!st) {
        st = malloc(sizeof(GState) * BATCH_MAX); pr = malloc(sizeof(GPrim) * BATCH_MAX); rect = malloc(8 * BATCH_MAX);
        tcount = calloc(16384, 4); tfirst = calloc(16384, 4); touched = malloc(16384 * 4); tiles = malloc(16384 * 16);
        idx_cap = 65536; idx = malloc(idx_cap * 4);
    }
    int nst = 0, npr = 0, ntouched = 0; u32 total = 0;
    for (int i = 0; i < g_nbatch; i++) {
        const DrawCmd *c = &g_batch[i];
        const GsDraw *d = &c->d;
        int nv = c->type == 3 ? 3 : c->type == 0 ? 1 : 2;
        float x0 = c->v[0].x, x1 = c->v[0].x, y0 = c->v[0].y, y1 = c->v[0].y;
        for (int k = 1; k < nv; k++) {
            x0 = fminf(x0, c->v[k].x); x1 = fmaxf(x1, c->v[k].x); y0 = fminf(y0, c->v[k].y); y1 = fmaxf(y1, c->v[k].y);
        }
        /* covered pixels, by the shader's rules: triangles/sprites [ceil(min), ceil(max) - 1],
         * points (int)x, lines between the truncated endpoints (floor is conservative) */
        int ix0, ix1, iy0, iy1;
        if (c->type == 3 || c->type == 6) { ix0 = (int)ceilf(x0); ix1 = (int)ceilf(x1) - 1; iy0 = (int)ceilf(y0); iy1 = (int)ceilf(y1) - 1; }
        else if (c->type == 0) { ix0 = ix1 = (int)x0; iy0 = iy1 = (int)y0; }
        else { ix0 = (int)floorf(x0); ix1 = (int)floorf(x1) + 1; iy0 = (int)floorf(y0); iy1 = (int)floorf(y1) + 1; }
        if (ix0 < d->sx0) ix0 = d->sx0; if (iy0 < d->sy0) iy0 = d->sy0;
        if (ix1 > d->sx1) ix1 = d->sx1; if (iy1 > d->sy1) iy1 = d->sy1;
        if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0; if (ix1 > 2047) ix1 = 2047; if (iy1 > 2047) iy1 = 2047;
        if (ix0 > ix1 || iy0 > iy1) continue;
        if (c->type == 3) {
            float area = (c->v[1].x - c->v[0].x) * (c->v[2].y - c->v[0].y) - (c->v[2].x - c->v[0].x) * (c->v[1].y - c->v[0].y);
            if (area == 0.0f) continue;
        }
        GState s; gstate_from(&s, d);
        if (!nst || memcmp(&st[nst - 1], &s, sizeof s)) st[nst++] = s;
        GPrim *p = &pr[npr];
        p->type = (u32)c->type; p->state = (u32)nst - 1;
        p->bb0 = (u32)ix0 | (u32)iy0 << 16; p->bb1 = (u32)ix1 | (u32)iy1 << 16;
        for (int k = 0; k < 3; k++) {
            const Vtx *v = &c->v[k < nv ? k : 0];
            float *o = p->v + k * 12;
            o[0] = v->x; o[1] = v->y; memcpy(&o[2], &v->z, 4);
            o[3] = (float)v->r; o[4] = (float)v->g; o[5] = (float)v->b; o[6] = (float)v->a;
            o[7] = v->s; o[8] = v->t; o[9] = v->q; o[10] = v->u; o[11] = v->v;
        }
        u16 tx0 = (u16)(ix0 >> 4), tx1 = (u16)(ix1 >> 4), ty0 = (u16)(iy0 >> 4), ty1 = (u16)(iy1 >> 4);
        rect[npr][0] = tx0; rect[npr][1] = ty0; rect[npr][2] = tx1; rect[npr][3] = ty1;
        for (u32 ty = ty0; ty <= ty1; ty++) for (u32 tx = tx0; tx <= tx1; tx++) {
            u32 t = ty * 128 + tx;
            if (!tcount[t]++) touched[ntouched++] = t;
            total++;
        }
        npr++;
    }
    if (!npr) return;
    u32 off = 0;
    for (int k = 0; k < ntouched; k++) {
        u32 t = touched[k];
        tiles[k * 4 + 0] = t; tiles[k * 4 + 1] = off; tiles[k * 4 + 2] = tcount[t]; tiles[k * 4 + 3] = 0;
        tfirst[t] = off; off += tcount[t]; tcount[t] = 0;
    }
    if (total > idx_cap) { idx_cap = total * 2; free(idx); idx = malloc((size_t)idx_cap * 4); }
    for (int p = 0; p < npr; p++)
        for (u32 ty = rect[p][1]; ty <= rect[p][3]; ty++) for (u32 tx = rect[p][0]; tx <= rect[p][2]; tx++) {
            u32 t = ty * 128 + tx;
            idx[tfirst[t] + tcount[t]++] = (u32)p;
        }
    for (int k = 0; k < ntouched; k++) tcount[touched[k]] = 0;
    /* texture pages that self-overlapping draws sample (snapshotted before the dispatch): only the pages under
     * each primitive's texel bounding box (vertex texel coordinates bound the interior, also with perspective);
     * wrapping that can reach the whole texture, or q <= 0, falls back to the whole texture */
    static u8 cpages[512]; int anyc = 0;
    for (int p = 0; p < npr; p++) {
        const GState *s = &st[pr[p].state];
        if (!(s->flags & GSF_TEXCOPY)) continue;
        anyc = 1;
        int nv = pr[p].type == 3 ? 3 : pr[p].type == 0 ? 1 : 2;
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        static int fullcopy = -1; if (fullcopy < 0) fullcopy = getenv("PS2_GPU_FULLCOPY") != NULL;
        int whole = fullcopy;
        for (int k = 0; k < nv; k++) {
            const float *o = pr[p].v + k * 12;
            float uu, vv;
            if (s->flags & GSF_FST) { uu = o[10]; vv = o[11]; }
            else { if (!(o[9] > 0.0f)) { whole = 1; break; } uu = o[7] / o[9] * (float)s->tw; vv = o[8] / o[9] * (float)s->th; }
            u0 = fminf(u0, uu); u1 = fmaxf(u1, uu); v0 = fminf(v0, vv); v1 = fmaxf(v1, vv);
        }
        int tu0 = (int)floorf(u0) - 1, tu1 = (int)ceilf(u1) + 1, tv0 = (int)floorf(v0) - 1, tv1 = (int)ceilf(v1) + 1;   /* bilinear margin */
        int tw = (int)s->tw, th = (int)s->th;
        if (!whole) {
            if (s->wms == 0 || s->wms == 3) { if (tu0 < 0 || tu1 >= tw || s->wms == 3) { tu0 = 0; tu1 = tw - 1; } }
            else if (s->wms == 2) { tu0 = tu0 < (int)s->minu ? (int)s->minu : tu0; tu1 = tu1 > (int)s->maxu ? (int)s->maxu : tu1; }
            if (s->wmt == 0 || s->wmt == 3) { if (tv0 < 0 || tv1 >= th || s->wmt == 3) { tv0 = 0; tv1 = th - 1; } }
            else if (s->wmt == 2) { tv0 = tv0 < (int)s->minv ? (int)s->minv : tv0; tv1 = tv1 > (int)s->maxv ? (int)s->maxv : tv1; }
        }
        if (whole) { tu0 = 0; tu1 = tw - 1; tv0 = 0; tv1 = th - 1; }
        if (tu0 < 0) tu0 = 0; if (tv0 < 0) tv0 = 0; if (tu1 > tw - 1) tu1 = tw - 1; if (tv1 > th - 1) tv1 = th - 1;
        if (tu0 > tu1 || tv0 > tv1) continue;
        u32 psm = s->tpsm, pw, ph;
        switch (psm) {
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
        case PSMT8: pw = 128; ph = 64; break;
        case PSMT4: pw = 128; ph = 128; break;
        default: pw = 64; ph = 32; break;
        }
        u32 bw = s->tbw ? s->tbw : 1, cols = pw == 128 ? (bw + 1) / 2 : bw;
        for (u32 py = (u32)tv0 / ph; py <= (u32)tv1 / ph; py++)
            for (u32 px = (u32)tu0 / pw; px <= (u32)tu1 / pw; px++) {
                u32 page = s->tbp / 32 + py * cols + px;
                if (page < 512) cpages[page] = 1;
                if ((s->tbp & 31) && page + 1 < 512) cpages[page + 1] = 1;   /* unaligned base spans two pages */
            }
    }
    u32 cr[512]; int ncr = 0;
    if (anyc)
        for (u32 pg = 0; pg < 512; pg++) {
            if (!cpages[pg]) continue;
            u32 q = pg; while (q < 512 && cpages[q]) cpages[q++] = 0;
            cr[ncr * 2] = pg * 32; cr[ncr * 2 + 1] = q * 32; ncr++;
            pg = q - 1;
        }
    { extern u64 g_copies, g_tile_pairs, g_gpu_tiles; g_copies += ncr; g_tile_pairs += total; g_gpu_tiles += (u64)ntouched; }
    gsgpu_submit(st, nst, pr, npr, tiles, ntouched, idx, (int)total, &g_bclut[0][0], g_nbclut ? g_nbclut : 1, cr, ncr);
}

/* Execute and empty the batch. Called on the GS thread before anything that depends on drawn pixels. */
static u64 g_huge_prims;
static double g_flush_s;
void gs_flush(void) {
    if (!g_nbatch) return;
    LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
    int on_gpu = gpu_on();
    if (on_gpu) for (int i = 0; i < g_nbatch && on_gpu; i++) on_gpu = gpu_supported(&g_batch[i].d);
    static int verify = -1; if (verify < 0) verify = getenv("PS2_GPUVERIFY") != NULL;
    extern int g_respawn_frame, g_frame_no_pub;
    if (on_gpu && verify && g_respawn_frame && g_frame_no_pub > g_respawn_frame + 300) {
        /* PS2_GPUVERIFY=1: rasterize the batch on the CPU and on the GPU from the same VRAM and compare */
        static u8 *vbak, *vcpu; static u64 nflush, nbad;
        if (!vbak) { vbak = malloc(4u << 20); vcpu = malloc(4u << 20); }
        static int full = -1; if (full < 0) full = getenv("PS2_GPUVERIFY_FULLSYNC") != NULL;
        if (full) gsgpu_gpu_will_write(0, 16384);   /* distrust page tracking: start from the whole GPU copy */
        gsgpu_sync_cpu_all();
        memcpy(vbak, g_vram, 4u << 20);
        run_batch(0, 1);
        memcpy(vcpu, g_vram, 4u << 20); memcpy(g_vram, vbak, 4u << 20);
        gpu_flush_batch();
        for (int i = 0; i < g_nbw; i++) gsgpu_gpu_will_write(g_bw[i].lo, g_bw[i].hi);
        gsgpu_sync_cpu_all();
        nflush++;
        u32 ndiff = 0, first = 0;
        static int vx0 = -2; if (vx0 == -2) { const char *e = getenv("PS2_GPUVERIFY_X0"); vx0 = e ? atoi(e) : -1; }
        if (vx0 >= 0) {   /* only frame-buffer-0 pixels with x >= X0 (640x448, fbw 10) */
            for (int y = 0; y < 448; y++) for (int x = vx0; x < 640; x++) {
                u32 w = addr32(0, 10, x, y, blk32);
                if (((u32 *)vcpu)[w] != ((u32 *)g_vram)[w]) { if (!ndiff) first = w; ndiff++; }
            }
        } else
        for (u32 w = 0; w < (1u << 20); w++) if (((u32 *)vcpu)[w] != ((u32 *)g_vram)[w]) { if (!ndiff) first = w; ndiff++; }
        static int vmax = -1; if (vmax < 0) { const char *e = getenv("PS2_GPUVERIFY_MAX"); vmax = e ? atoi(e) : 25; }
        if (ndiff && nbad++ < (u64)vmax) {
            fprintf(stderr, "[gpuverify] flush %llu (%d prims): %u words differ, first at word %05x (block %04x): cpu %08x gpu %08x\n",
                    (unsigned long long)nflush, g_nbatch, ndiff, first, first / 64, ((u32 *)vcpu)[first], ((u32 *)g_vram)[first]);
            /* find the pixel: try each draw's frame buffer (CT32) for an (x,y) mapping to that word */
            int fx = -1, fy = -1; const GsDraw *fd = NULL;
            for (int i = 0; i < g_nbatch && fx < 0; i++) {
                const GsDraw *d = &g_batch[i].d;
                for (int y = 0; y < 1024 && fx < 0; y++) for (int x = 0; x < (int)(d->fbw ? d->fbw : 10) * 64; x++)
                    if (addr32(d->fbp, d->fbw, x, y, blk32) == first || addr32(d->zbp, d->fbw, x, y, blkz32) == first) { fx = x; fy = y; fd = d; break; }
            }
            fprintf(stderr, "   pixel %d,%d (%s)\n", fx, fy, fd ? (addr32(fd->fbp, fd->fbw, fx, fy, blk32) == first ? "frame" : "z") : "?");
            if (fd && nbad <= 4) {   /* all differing pixels of that frame buffer, as row runs */
                int shown = 0;
                for (int y = 0; y < 512 && shown < 40; y++) {
                    int run0 = -1;
                    for (int x = 0; x <= 640; x++) {
                        int dif = x < 640 && ((u32 *)vcpu)[addr32(fd->fbp, fd->fbw, x, y, blk32)] != ((u32 *)g_vram)[addr32(fd->fbp, fd->fbw, x, y, blk32)];
                        if (dif && run0 < 0) run0 = x;
                        if (!dif && run0 >= 0) { if (shown++ < 40) fprintf(stderr, "     y %d x %d..%d\n", y, run0, x - 1); run0 = -1; }
                    }
                }
            }
            if (nbad <= (u64)vmax) {   /* replay one primitive at a time on both sides to find where they diverge */
                DrawCmd *orig = g_batch; int n = g_nbatch;
                static u8 *vc, *vg; if (!vc) { vc = malloc(4u << 20); vg = malloc(4u << 20); }
                memcpy(vg, g_vram, 4u << 20);                /* GPU full-batch result */
                memcpy(vc, vbak, 4u << 20);                  /* CPU side state */
                memcpy(g_vram, vbak, 4u << 20); gsgpu_cpu_wrote(0, 16384);   /* GPU side state */
                for (int i = 0; i < n; i++) {
                    u32 before = ((u32 *)vc)[first];
                    g_batch = orig + i; g_nbatch = 1;
                    u8 *save = g_vram; g_vram = vc; run_batch(0, 1); g_vram = save;
                    gpu_flush_batch(); gsgpu_gpu_will_write(0, 16384); gsgpu_sync_cpu_all();
                    u32 c = ((u32 *)vc)[first], gq = ((u32 *)g_vram)[first];
                    if (c != gq) {
                        const DrawCmd *d = orig + i;
                        fprintf(stderr, "   diverges at prim %d: before %08x -> cpu %08x gpu %08x; type %d scissor %d..%d,%d..%d fbmsk %08x test %x alpha %llx "
                                "tme %d tfx %u tpsm %02x tbp %05x tw %d th %d fst %d bil %d | v0 %.2f,%.2f uv %.1f,%.1f st %g,%g q %g | v1 %.2f,%.2f uv %.1f,%.1f c1 %d,%d,%d,%d\n",
                                i, before, c, gq, d->type, d->d.sx0, d->d.sx1, d->d.sy0, d->d.sy1, d->d.fbmsk, d->d.test,
                                (unsigned long long)d->d.cx->alpha, d->d.tme, d->d.tfx, d->d.tpsm, d->d.tbp, d->d.tw, d->d.th, d->d.fst, d->d.bilinear,
                                d->v[0].x, d->v[0].y, d->v[0].u, d->v[0].v, d->v[0].s, d->v[0].t, d->v[0].q,
                                d->v[1].x, d->v[1].y, d->v[1].u, d->v[1].v, d->v[1].r, d->v[1].g, d->v[1].b, d->v[1].a);
                        break;
                    }
                }
                fprintf(stderr, "   pixel after: cpu full %08x, cpu one-by-one %08x, gpu full %08x, gpu one-by-one %08x\n",
                        ((u32 *)vcpu)[first], ((u32 *)vc)[first], ((u32 *)vg)[first], ((u32 *)g_vram)[first]);
                { /* distinct targets in the batch */
                    struct { u32 fbp, fbw, fpsm, fbmsk, zbp, zpsm, zw, tme, tbp, tpsm; int n, first; } t[16]; int nt = 0;
                    for (int i = 0; i < n; i++) {
                        const GsDraw *d = &orig[i].d;
                        u32 zw = d->ztst >= 1 && !d->zmsk;
                        int k; for (k = 0; k < nt; k++)
                            if (t[k].fbp == d->fbp && t[k].fbw == d->fbw && t[k].fpsm == d->fpsm && t[k].fbmsk == d->fbmsk && t[k].zbp == d->zbp &&
                                t[k].zpsm == d->zpsm && t[k].zw == zw && t[k].tme == (u32)d->tme && t[k].tbp == d->tbp && t[k].tpsm == d->tpsm) break;
                        if (k == nt && nt < 16) { t[k].fbp = d->fbp; t[k].fbw = d->fbw; t[k].fpsm = d->fpsm; t[k].fbmsk = d->fbmsk; t[k].zbp = d->zbp;
                                                  t[k].zpsm = d->zpsm; t[k].zw = zw; t[k].tme = d->tme; t[k].tbp = d->tbp; t[k].tpsm = d->tpsm;
                                                  t[k].n = 0; t[k].first = i; nt++; }
                        if (k < 16) t[k].n++;
                    }
                    for (int k = 0; k < nt; k++)
                        fprintf(stderr, "   target: fbp %05x fbw %u fpsm %02x fbmsk %08x zbp %05x zpsm %02x zwrite %u tme %u tbp %05x tpsm %02x: %d prims from #%d\n",
                                t[k].fbp, t[k].fbw, t[k].fpsm, t[k].fbmsk, t[k].zbp, t[k].zpsm, t[k].zw, t[k].tme, t[k].tbp, t[k].tpsm, t[k].n, t[k].first);
                }
                memcpy(g_vram, vg, 4u << 20); gsgpu_cpu_wrote(0, 16384);   /* keep the GPU's full-batch result */
                g_batch = orig; g_nbatch = n;
            }
            int shown = 0;
            for (int i = 0; i < g_nbatch && shown < 6; i++) {
                const DrawCmd *c = &g_batch[i];
                int nv = c->type == 3 ? 3 : c->type == 0 ? 1 : 2;
                float x0 = c->v[0].x, x1 = x0, y0 = c->v[0].y, y1 = y0;
                for (int k = 1; k < nv; k++) { x0 = fminf(x0, c->v[k].x); x1 = fmaxf(x1, c->v[k].x); y0 = fminf(y0, c->v[k].y); y1 = fmaxf(y1, c->v[k].y); }
                if (fx < x0 - 1 || fx > x1 + 1 || fy < y0 - 1 || fy > y1 + 1) continue;
                shown++;
                fprintf(stderr, "   prim %d type %d fbp %05x fpsm %02x fbmsk %08x zbp %05x zpsm %02x test %x tme %d tfx %u tcc %u tpsm %02x tbp %05x abe %d alpha %llx "
                        "fba %llx | %.1f,%.1f..%.1f,%.1f c0 %d,%d,%d,%d c1 %d,%d,%d,%d z %08x\n",
                        i, c->type, c->d.fbp, c->d.fpsm, c->d.fbmsk, c->d.zbp, c->d.zpsm, c->d.test, c->d.tme, c->d.tfx, c->d.tcc, c->d.tpsm, c->d.tbp,
                        c->d.abe, (unsigned long long)c->d.cx->alpha, (unsigned long long)c->d.cx->fba, x0, y0, x1, y1,
                        c->v[0].r, c->v[0].g, c->v[0].b, c->v[0].a, c->v[1].r, c->v[1].g, c->v[1].b, c->v[1].a, c->v[nv - 1].z);
            }
        }
        if (nflush % 5000 == 0) fprintf(stderr, "[gpuverify] %llu flushes, %llu with differences\n", (unsigned long long)nflush, (unsigned long long)nbad);
    } else if (on_gpu) {
        static int obo = -1; if (obo < 0) obo = getenv("PS2_GPU_ONEBYONE") != NULL;   /* diagnostic: one dispatch per primitive */
        if (obo) {
            DrawCmd *orig = g_batch; int n = g_nbatch;
            for (int i = 0; i < n; i++) { g_batch = orig + i; g_nbatch = 1; gpu_flush_batch(); }
            g_batch = orig; g_nbatch = n;
        } else
        gpu_flush_batch();
        if (g_batch_pages_any)
            for (u32 p = 0; p < 512; p++) if (g_batch_pages[p]) { gsgpu_gpu_will_write(p * 32, p * 32 + 32); fresh_clear_page(p); }
    } else {
        if (gpu_on()) RB(1, gsgpu_sync_cpu_all());   /* fallback: the CPU rasterizer needs current VRAM */
        if (g_nworkers > 1) {
            g_work_left = g_nworkers - 1;
            for (int i = 1; i < g_nworkers; i++) SetEvent(g_work_start[i]);
            run_batch(0, g_nworkers);
            WaitForSingleObject(g_work_done, INFINITE);
        } else run_batch(0, 1);
        if (gpu_on()) for (int i = 0; i < g_nbw; i++) gsgpu_cpu_wrote(g_bw[i].lo, g_bw[i].hi);
    }
    g_nbclut = 0; g_batch_clutvram = 0;
    if (g_batch_pages_any) { memset(g_batch_pages, 0, sizeof g_batch_pages); g_batch_pages_any = 0; }
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    g_flush_s += (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
    g_flush_count++; g_flush_prims += (u64)g_nbatch;
    for (int i = 0; i < g_nbw; i++) vram_touch(g_bw[i].lo, g_bw[i].hi);   /* pages the batch may have written */
    g_nbw = 0;
    g_batch_seq++;
    g_nbatch = 0;
    g_batch_fb_lo = 0xFFFFFFFF; g_batch_fb_hi = 0;
}

extern int g_frame_no_pub;
static void enqueue(int type, const Vtx *a, const Vtx *b, const Vtx *c) {
    if (!g_batch) workers_init();
    { /* PS2_FASTTEST=1 (with PS2_SHOT=N): only rasterize the 20 vsyncs before each screenshot, so a whole
       * game frame (several vsyncs, several passes) is drawn before the capture */
      static int ft = -1, shot; if (ft < 0) { ft = getenv("PS2_FASTTEST") != NULL; const char *e = getenv("PS2_SHOT"); shot = e ? atoi(e) : 0; }
      if (ft && shot > 20 && (shot - g_frame_no_pub % shot) % shot > 20) { G.prim_count++; return; } }
    GsDraw d; setup_draw(&d);
    { /* PS2_DRAWLOG=N: log every primitive of frame N */
      static int fr = -2, n; if (fr == -2) { const char *e = getenv("PS2_DRAWLOG"); fr = e ? atoi(e) : -1; }
      static int last_f = -1, cnt;
      if (fr >= 0 && g_frame_no_pub != last_f) {
          if (last_f >= fr - 200 && last_f <= fr + 200 && last_f % 10 == 0) fprintf(stderr, "[drawcount] f%d %d\n", last_f, cnt);
          last_f = g_frame_no_pub; cnt = 0;
      }
      cnt++;
      { /* oversized primitives (likely broken vertices) */
        static int huge_n;
        const Vtx *vv[3] = {a, b, c};
        float x0 = a->x, x1 = a->x, y0 = a->y, y1 = a->y;
        for (int k = 1; k < 3 && vv[k]; k++) {
            if (vv[k]->x < x0) x0 = vv[k]->x; if (vv[k]->x > x1) x1 = vv[k]->x;
            if (vv[k]->y < y0) y0 = vv[k]->y; if (vv[k]->y > y1) y1 = vv[k]->y;
        }
        if ((x1 - x0 > 1024 || y1 - y0 > 1024) && type != 6) {
            g_huge_prims++;
            if (fr == -1 && g_frame_no_pub > 5000 && huge_n++ < 12) {
                u64 p = (G.prmodecont & 1) ? G.prim : G.prmode; int cx = (p >> 9) & 1;
                extern int g_gif_cur_path;
                u64 off = G.ctx[cx].xyoffset;
                fprintf(stderr, "[huge] f%d path%d t%d prim=%03llx frame=%llx off=%llx", g_frame_no_pub, g_gif_cur_path, type,
                        (unsigned long long)G.prim, (unsigned long long)G.ctx[cx].frame, (unsigned long long)off);
                for (int k = 0; k < 3 && vv[k]; k++)
                    fprintf(stderr, " | raw %04x,%04x z=%08x q=%g", (unsigned)(vv[k]->x * 16 + (off & 0xFFFF)),
                            (unsigned)(vv[k]->y * 16 + ((off >> 32) & 0xFFFF)), (unsigned)vv[k]->z, vv[k]->q);
                fputc('\n', stderr);
            }
        } }
      { /* PS2_REGIONLOG=x0,y0,x1,y1: after the respawn, log state of PATH1 STQ-textured triangles centred in that box */
        static int rl = -1, rx0, ry0, rx1, ry1, rn; extern int g_respawn_frame, g_gif_cur_path;
        if (rl < 0) { const char *e = getenv("PS2_REGIONLOG"); rl = e && sscanf(e, "%d,%d,%d,%d", &rx0, &ry0, &rx1, &ry1) == 4; }
        if (rl && type == 3 && g_gif_cur_path == 1 && g_respawn_frame && g_frame_no_pub > g_respawn_frame + 250 && rn < 12) {
            float cx = (a->x + b->x + c->x) / 3, cy = (a->y + b->y + c->y) / 3;
            u64 p = (G.prmodecont & 1) ? G.prim : G.prmode; int cx_ = (p >> 9) & 1;
            if (cx >= rx0 && cx <= rx1 && cy >= ry0 && cy <= ry1 && ((G.prim >> 4) & 1) && !((G.prim >> 8) & 1)) {
                rn++;
                fprintf(stderr, "[region] f%d prim=%03llx frame=%llx zbuf=%llx test=%llx alpha=%llx tex0=%llx tex1=%llx clamp=%llx fba=%llx texa=%llx\n"
                        "         v %.1f,%.1f z=%08x c=%02x%02x%02x%02x stq=%g,%g,%g | %.1f,%.1f z=%08x | %.1f,%.1f z=%08x\n",
                        g_frame_no_pub, (unsigned long long)G.prim, (unsigned long long)G.ctx[cx_].frame, (unsigned long long)G.ctx[cx_].zbuf,
                        (unsigned long long)G.ctx[cx_].test, (unsigned long long)G.ctx[cx_].alpha, (unsigned long long)G.ctx[cx_].tex0,
                        (unsigned long long)G.ctx[cx_].tex1, (unsigned long long)G.ctx[cx_].clamp, (unsigned long long)G.ctx[cx_].fba,
                        (unsigned long long)G.texa, a->x, a->y, (unsigned)a->z, (unsigned)a->r, (unsigned)a->g, (unsigned)a->b, (unsigned)a->a,
                        a->s, a->t, a->q, b->x, b->y, (unsigned)b->z, c->x, c->y, (unsigned)c->z);
            }
        } }
      { static int cl = -2, cn; if (cl == -2) { const char *e = getenv("PS2_COPYLOG"); cl = e ? atoi(e) : -1; }
        if (cl >= 0 && d.fbp == 0x2300 && g_frame_no_pub >= cl && cn < 4000 && (a->x == 0 || cn < 20)) {
            cn++;
            u64 p = (G.prmodecont & 1) ? G.prim : G.prmode; int cx = (p >> 9) & 1;
            fprintf(stderr, "[copy] f%d t%d prim=%03llx tex0=%llx zbuf=%llx rgbaq=%llx z=%x,%x test=%llx | %.0f,%.0f c=%02x%02x%02x%02x uv=%.0f,%.0f | %.0f,%.0f uv=%.0f,%.0f c2=%02x%02x%02x%02x\n",
                    g_frame_no_pub, type, (unsigned long long)G.prim, (unsigned long long)G.ctx[cx].tex0, (unsigned long long)G.ctx[cx].zbuf,
                    (unsigned long long)G.rgbaq, (unsigned)a->z, b ? (unsigned)b->z : 0,
                    (unsigned long long)G.ctx[cx].test, a->x, a->y,
                    (unsigned)a->r, (unsigned)a->g, (unsigned)a->b, (unsigned)a->a, a->u, a->v,
                    b ? b->x : 0, b ? b->y : 0, b ? b->u : 0, b ? b->v : 0,
                    b ? (unsigned)b->r : 0, b ? (unsigned)b->g : 0, b ? (unsigned)b->b : 0, b ? (unsigned)b->a : 0);
        } }
      if (fr >= 0 && g_frame_no_pub == fr && n < 6000) {
          n++;
          u64 p = (G.prmodecont & 1) ? G.prim : G.prmode; int cx = (p >> 9) & 1;
          fprintf(stderr, "[draw] t%d prim=%03llx frame=%llx zbuf=%llx test=%llx alpha=%llx tex0=%llx", type,
                  (unsigned long long)G.prim, (unsigned long long)G.ctx[cx].frame, (unsigned long long)G.ctx[cx].zbuf,
                  (unsigned long long)G.ctx[cx].test, (unsigned long long)G.ctx[cx].alpha, (unsigned long long)G.ctx[cx].tex0);
          const Vtx *vv[3] = {a, b, c};
          for (int k = 0; k < 3 && vv[k]; k++)
              fprintf(stderr, " | %.1f,%.1f z=%08x c=%02x%02x%02x%02x q=%g", vv[k]->x, vv[k]->y, (unsigned)vv[k]->z,
                      (unsigned)vv[k]->r, (unsigned)vv[k]->g, (unsigned)vv[k]->b, (unsigned)vv[k]->a, vv[k]->q);
          fputc('\n', stderr);
      }
      if (g_cap_fp && g_frame_no_pub >= g_cap_frame && g_frame_no_pub < g_cap_frame + 4) {   /* F9 capture: 4 vsyncs, full state */
          u64 p = (G.prmodecont & 1) ? G.prim : G.prmode; int cx = (p >> 9) & 1;
          extern int g_gif_cur_path;
          fprintf(g_cap_fp, "f%d t%d path%d prim=%03llx frame=%llx zbuf=%llx test=%llx alpha=%llx tex0=%llx tex1=%llx clamp=%llx texa=%llx fba=%llx scissor=%llx",
                  g_frame_no_pub, type, g_gif_cur_path, (unsigned long long)G.prim, (unsigned long long)G.ctx[cx].frame, (unsigned long long)G.ctx[cx].zbuf,
                  (unsigned long long)G.ctx[cx].test, (unsigned long long)G.ctx[cx].alpha, (unsigned long long)G.ctx[cx].tex0,
                  (unsigned long long)G.ctx[cx].tex1, (unsigned long long)G.ctx[cx].clamp, (unsigned long long)G.texa,
                  (unsigned long long)G.ctx[cx].fba, (unsigned long long)G.ctx[cx].scissor);
          const Vtx *vv[3] = {a, b, c};
          for (int k = 0; k < 3 && vv[k]; k++)
              fprintf(g_cap_fp, " | %.2f,%.2f z=%08x c=%02x%02x%02x%02x stq=%g,%g,%g uv=%.2f,%.2f", vv[k]->x, vv[k]->y, (unsigned)vv[k]->z,
                      (unsigned)vv[k]->r, (unsigned)vv[k]->g, (unsigned)vv[k]->b, (unsigned)vv[k]->a, vv[k]->s, vv[k]->t, vv[k]->q, vv[k]->u, vv[k]->v);
          fputc('\n', g_cap_fp);
      } }
    /* render-to-texture: sampling a buffer drawn earlier in this batch needs those pixels first */
    if (d.tme && d.tbp >= g_batch_fb_lo && d.tbp < g_batch_fb_hi) gs_flush();
    if (g_nbatch == BATCH_MAX) gs_flush();
    /* render-to-texture (including Z buffers sampled as textures): earlier draws in this batch may still
     * be writing the texture, and raster workers own different rows, so draw them first */
    if (d.tme && batch_writes(d.tbp, d.tbp + psm_blocks(d.tpsm, d.tbw, (u32)d.th))) gs_flush();
    if (gpu_on()) {
        u32 flo = d.fbp, fhi = d.fbp + target_blocks_x(d.fpsm, d.fbw, d.sx1, d.sy1);
        /* draws in this batch read a GPU-drawn CLUT from VRAM: one that rewrites its blocks must wait for them */
        if (g_batch_clutvram) {
            u32 c0 = g_clutvram_cbp, c1 = g_clutvram_cbp + 4;
            u32 zlo = d.zbp, zhi = d.zbp + target_blocks_x(d.zpsm, d.fbw, d.sx1, d.sy1);
            if ((flo < c1 && fhi > c0) || (d.ztst >= 1 && !d.zmsk && zlo < c1 && zhi > c0)) gs_flush();
        }
        d.clut_vram = 0;
        if (d.tme && g_clutvram && (d.tpsm == PSMT8 || d.tpsm == PSMT8H) && d.cpsm == PSMCT32) {
            d.clut_vram = 1 + g_clutvram_cbp; g_batch_clutvram = 1;
        }
        if (batch_view_conflict(flo, fhi, d.fpsm, d.fbp, d.fbw) ||
            (d.ztst >= 1 && !d.zmsk && batch_view_conflict(d.zbp, d.zbp + target_blocks_x(d.zpsm, d.fbw, d.sx1, d.sy1), d.zpsm, d.zbp, d.fbw)))
        { extern u64 g_view_flushes; g_view_flushes++; gs_flush(); }   /* same memory written through another swizzle: separate GPU batches */
        d.tlin = NULL; if (d.tme) d.clut_snap = batch_clut();   /* GPU samples VRAM directly */
    }
    else d.tlin = d.tme ? texc_lookup(&d) : NULL;
    if (d.tme) { /* texture formats tex_texel() cannot decode (they sample as magenta) */
        static int seen[64]; u32 p = d.tpsm & 63;
        int ok = p == PSMCT32 || p == PSMCT24 || p == PSMCT16 || p == PSMCT16S || p == PSMT8 || p == PSMT8H || p == PSMT4 || p == PSMT4HL || p == PSMT4HH ||
                 p == PSMZ32 || p == PSMZ24 || p == PSMZ16 || p == PSMZ16S;
        if (!ok && seen[p]++ < 3)
            fprintf(stderr, "[gs] texture with unsupported PSM 0x%02x: tbp %05x tbw %u %dx%d fbp %05x (frame %d)\n", p, d.tbp, d.tbw, d.tw, d.th, d.fbp, g_frame_no_pub);
    }
    { /* PS2_AREALOG=1: per render target, primitives and covered area (clipped to scissor) over 3 vsyncs */
      static int on = -1; if (on < 0) on = getenv("PS2_AREALOG") != NULL;
      extern int g_respawn_frame;
      if (on && g_respawn_frame && g_frame_no_pub > g_respawn_frame + 400) {
          static int start = -1, done; static struct { u32 fbp, fbw, psm; u64 n, area; u64 tex, blend; } t[32]; static int nt;
          if (start < 0) start = g_frame_no_pub;
          if (!done && g_frame_no_pub < start + 3) {
              float x0, x1, y0, y1; const Vtx *vv[3] = {a, b, c};
              x0 = x1 = a->x; y0 = y1 = a->y;
              for (int k = 1; k < 3 && vv[k]; k++) { x0 = fminf(x0, vv[k]->x); x1 = fmaxf(x1, vv[k]->x); y0 = fminf(y0, vv[k]->y); y1 = fmaxf(y1, vv[k]->y); }
              x0 = fmaxf(x0, (float)d.sx0); y0 = fmaxf(y0, (float)d.sy0); x1 = fminf(x1, (float)d.sx1 + 1); y1 = fminf(y1, (float)d.sy1 + 1);
              double ar = (x1 > x0 && y1 > y0) ? (double)(x1 - x0) * (y1 - y0) : 0;
              if (type == 3) {   /* triangle: exact area */
                  ar = fabs((b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y)) / 2;
                  double bb = (x1 > x0 && y1 > y0) ? (double)(x1 - x0) * (y1 - y0) : 0; if (ar > bb) ar = bb;
              }
              int k; for (k = 0; k < nt && !(t[k].fbp == d.fbp && t[k].fbw == d.fbw && t[k].psm == d.fpsm); k++) {}
              if (k == nt && nt < 32) { t[k].fbp = d.fbp; t[k].fbw = d.fbw; t[k].psm = d.fpsm; nt++; }
              if (k < 32) { t[k].n++; t[k].area += (u64)ar; t[k].tex += d.tme; t[k].blend += d.abe; }
          } else if (!done) {
              done = 1;
              for (int k = 0; k < nt; k++)
                  fprintf(stderr, "[area] fbp %05x fbw %2u psm %02x: %7llu prims (%llu textured, %llu blended), %8.2f Mpixels\n",
                          t[k].fbp, t[k].fbw, t[k].psm, t[k].n, t[k].tex, t[k].blend, t[k].area / 1e6);
          }
      } }
    { int circ = (G.pmode & 2) ? 1 : 0; if (d.fbp == (u32)(G.dispfb[circ] & 0x1FF) * 32) g_disp_drawn = 1; }
    DrawCmd *cmd = &g_batch[g_nbatch++];
    { extern u32 g_fbp_hist[8][2]; for (int i = 0; i < 8; i++) {
        if (g_fbp_hist[i][1] && g_fbp_hist[i][0] == d.fbp) { g_fbp_hist[i][1]++; break; }
        if (!g_fbp_hist[i][1]) { g_fbp_hist[i][0] = d.fbp; g_fbp_hist[i][1] = 1; break; } } }
    cmd->d = d; cmd->d.cx = &cmd->d.cxs;
    cmd->type = type;
    cmd->v[0] = *a; if (b) cmd->v[1] = *b; if (c) cmd->v[2] = *c;
    u32 lo = d.fbp, hi = d.fbp + (d.fbw ? d.fbw : 10) * 512;
    if (lo < g_batch_fb_lo) g_batch_fb_lo = lo;
    if (hi > g_batch_fb_hi) g_batch_fb_hi = hi;
    batch_add_write(d.fbp, d.fbp + target_blocks_x(d.fpsm, d.fbw, d.sx1, d.sy1), d.fpsm, d.fbp, d.fbw);
    if (d.ztst >= 1 && !d.zmsk) batch_add_write(d.zbp, d.zbp + target_blocks_x(d.zpsm, d.fbw, d.sx1, d.sy1), d.zpsm, d.zbp, d.fbw);   /* Z writes too */
    if (gpu_on()) {   /* pages the primitive's bounding box (within the scissor) covers, for GPU/CPU coherence */
        float fx0 = a->x, fx1 = a->x, fy0 = a->y, fy1 = a->y;
        if (b) { fx0 = fminf(fx0, b->x); fx1 = fmaxf(fx1, b->x); fy0 = fminf(fy0, b->y); fy1 = fmaxf(fy1, b->y); }
        if (c) { fx0 = fminf(fx0, c->x); fx1 = fmaxf(fx1, c->x); fy0 = fminf(fy0, c->y); fy1 = fmaxf(fy1, c->y); }
        int x0 = d.sx0, x1 = d.sx1, y0 = d.sy0, y1 = d.sy1;
        /* covered pixels start at >= floor(min); triangles/sprites stop at ceil(max)-1, points/lines at (int)max */
        float ex1 = (type == 3 || type == 6) ? ceilf(fx1) - 1 : floorf(fx1), ey1 = (type == 3 || type == 6) ? ceilf(fy1) - 1 : floorf(fy1);
        if (floorf(fx0) > x0) x0 = (int)floorf(fx0); if (ex1 < x1) x1 = (int)ex1;
        if (floorf(fy0) > y0) y0 = (int)floorf(fy0); if (ey1 < y1) y1 = (int)ey1;
        if (x0 <= x1 && y0 <= y1) {
            static u32 lb, lz, lpsm, lzpsm, lw; static int lx0 = 1, lx1 = 0, ly0, ly1, lseq = -1;
            /* skip repeats (same targets, box inside the last marked one, same batch) */
            if (!(lseq == g_batch_seq && lb == d.fbp && lw == d.fbw && lpsm == d.fpsm && lz == ((d.ztst >= 1 && !d.zmsk) ? d.zbp : ~0u) &&
                  lzpsm == d.zpsm && x0 >= lx0 && x1 <= lx1 && y0 >= ly0 && y1 <= ly1)) {
                batch_mark_pages(d.fbp, d.fbw, d.fpsm, x0, x1, y0, y1);
                if (d.ztst >= 1 && !d.zmsk) batch_mark_pages(d.zbp, d.fbw, d.zpsm, x0, x1, y0, y1);
                /* remember the scissor-wide page-aligned box covering this one */
                lseq = g_batch_seq; lb = d.fbp; lw = d.fbw; lpsm = d.fpsm; lz = (d.ztst >= 1 && !d.zmsk) ? d.zbp : ~0u; lzpsm = d.zpsm;
                lx0 = x0 & ~63; lx1 = x1 | 63; ly0 = y0 & ~31; ly1 = y1 | 31;
            }
        }
    }
}

static void vertex_kick(u32 X, u32 Y, u32 z, int fog, int draw) {
    u64 p = G.prim;
    int ctxt = (((G.prmodecont & 1) ? p : G.prmode) >> 9) & 1;
    u64 off = G.ctx[ctxt].xyoffset;
    Vtx v;
    v.x = ((float)X - (float)(off & 0xFFFF)) / 16.0f;
    v.y = ((float)Y - (float)((off >> 32) & 0xFFFF)) / 16.0f;
    v.z = z;
    v.r = G.rgbaq & 255; v.g = (G.rgbaq >> 8) & 255; v.b = (G.rgbaq >> 16) & 255; v.a = (G.rgbaq >> 24) & 255;
    u32 qb = (u32)(G.rgbaq >> 32); memcpy(&v.q, &qb, 4);
    u32 sb = (u32)G.st, tb = (u32)(G.st >> 32); memcpy(&v.s, &sb, 4); memcpy(&v.t, &tb, 4);
    v.u = (float)(G.uv & 0x3FFF) / 16.0f; v.v = (float)((G.uv >> 16) & 0x3FFF) / 16.0f;
    v.fog = (u8)fog;
    int type = p & 7;
    G.vq[G.nv < 3 ? G.nv : 2] = v;
    if (G.nv < 3) G.nv++;
    switch (type) {
    case 0: if (draw) enqueue(0, &v, NULL, NULL); G.nv = 0; break;
    case 1: if (G.nv == 2) { if (draw) enqueue(1, &G.vq[0], &G.vq[1], NULL); G.nv = 0; } break;
    case 2: if (G.nv == 2) { if (draw) enqueue(1, &G.vq[0], &G.vq[1], NULL); G.vq[0] = G.vq[1]; G.nv = 1; } break;
    case 3: if (G.nv == 3) { if (draw) enqueue(3, &G.vq[0], &G.vq[1], &G.vq[2]); G.nv = 0; } break;
    case 4: if (G.nv == 3) { if (draw) enqueue(3, &G.vq[0], &G.vq[1], &G.vq[2]); G.vq[0] = G.vq[1]; G.vq[1] = G.vq[2]; G.nv = 2; } break;
    case 5: if (G.nv == 3) { if (draw) enqueue(3, &G.vq[0], &G.vq[1], &G.vq[2]); G.vq[1] = G.vq[2]; G.nv = 2; } break;
    case 6: if (G.nv == 2) { if (draw) enqueue(6, &G.vq[0], &G.vq[1], NULL); G.nv = 0; } break;
    default: G.nv = 0; break;
    }
    G.prim_count++;
}

/* ------------------------------------------------------------------ transfers */
static u64 g_xfer_count, g_xfer_bytes_ok, g_xfer_bytes_dropped;
static u8 g_xfer_scat[512]; static int g_xfer_any_scat;   /* GPU: pages the current transfer updates by scatter */

/* VRAM word and bit mask a pixel of `psm` occupies (for scatter writes) */
static void vram_word_mask(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 *word, u32 *mask) {
    switch (psm) {
    case PSMCT32: *word = addr32(bp, bw, x, y, blk32); *mask = ~0u; return;
    case PSMCT24: *word = addr32(bp, bw, x, y, blk32); *mask = 0xFFFFFFu; return;
    case PSMZ32: *word = addr32(bp, bw, x, y, blkz32); *mask = ~0u; return;
    case PSMZ24: *word = addr32(bp, bw, x, y, blkz32); *mask = 0xFFFFFFu; return;
    case PSMT8H: *word = addr32(bp, bw, x, y, blk32); *mask = 0xFF000000u; return;
    case PSMT4HL: *word = addr32(bp, bw, x, y, blk32); *mask = 0x0F000000u; return;
    case PSMT4HH: *word = addr32(bp, bw, x, y, blk32); *mask = 0xF0000000u; return;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
        u32 h = addr16(bp, bw, x, y, psm == PSMCT16 ? blk16 : psm == PSMCT16S ? blk16s : psm == PSMZ16 ? blkz16 : blkz16s);
        *word = h >> 1; *mask = 0xFFFFu << ((h & 1) * 16); return; }
    case PSMT8: { u32 b = addr8(bp, bw, x, y); *word = b >> 2; *mask = 0xFFu << ((b & 3) * 8); return; }
    case PSMT4: { u32 n = addr4(bp, bw, x, y); *word = n >> 3; *mask = 0xFu << ((n & 7) * 4); return; }
    }
    *word = 0; *mask = 0;
}
char g_seq[256]; int g_seq_n;
extern int g_frame_no_pub;
static void xfer_start(void) {
    gs_flush();
    u32 dir = G.trxdir & 3;
    static int xstats = -1; if (xstats < 0) xstats = getenv("PS2_GFXSTATS") != NULL;
    { static int t0 = -2; if (t0 == -2) { const char *e = getenv("PS2_TRACE_DRAW"); t0 = e ? atoi(e) : -1; }
      if (t0 >= 0 && g_frame_no_pub >= t0 && g_frame_no_pub < t0 + 1)
          fprintf(stderr, "[xfer] dir=%u BITBLTBUF=%016llx TRXPOS=%016llx TRXREG=%016llx\n", dir, (unsigned long long)G.bitbltbuf,
                  (unsigned long long)G.trxpos, (unsigned long long)G.trxreg); }
    { /* tally transfer shapes */
      static struct { u64 key; u64 n; } tally[64]; static int nt;
      u64 key = (G.bitbltbuf >> 32) ^ ((G.trxreg & 0xFFF) << 40) ^ (((G.trxreg >> 32) & 0xFFF) << 52) ^ ((u64)dir << 62);
      int i; for (i = 0; i < nt && tally[i].key != key; i++) {}
      if (i == nt && nt < 64) { tally[nt].key = key; tally[nt].n = 0; nt++; }
      if (i < 64) tally[i].n++;
      static int calls; if (getenv("PS2_XFERTALLY") && ++calls % 2000 == 0) {
          fprintf(stderr, "[xfer-tally] after %d transfers:\n", calls);
          for (int k = 0; k < nt; k++) fprintf(stderr, "   dbp=%04llx dbw=%llu dpsm=%02llx w=%llu h=%llu dir=%llu  x%llu\n",
              tally[k].key & 0x3FFF, (tally[k].key >> 16) & 63, (tally[k].key >> 24) & 63, (tally[k].key >> 40) & 0xFFF,
              (tally[k].key >> 52) & 0x3FF, tally[k].key >> 62, (unsigned long long)tally[k].n); } }
    if (xstats && g_xfer_count++ < 12)
        fprintf(stderr, "[gs] TRXDIR %u: BITBLTBUF=%016llx TRXPOS=%016llx TRXREG=%016llx\n", dir,
                (unsigned long long)G.bitbltbuf, (unsigned long long)G.trxpos, (unsigned long long)G.trxreg);
    G.tx = G.ty = 0; G.xfer_ncarry = 0;
    G.xfer_active = (dir == 0);
    if (dir == 0 || dir == 2) {   /* host->local / local->local: destination pages change */
        u32 dbp = (G.bitbltbuf >> 32) & 0x3FFF, dbw = (G.bitbltbuf >> 48) & 63, dpsm = (G.bitbltbuf >> 56) & 63;
        u32 dsy = (G.trxpos >> 48) & 0x7FF, h = (G.trxreg >> 32) & 0xFFF;
        vram_touch(dbp, dbp + psm_blocks(dpsm, dbw, dsy + h));
        if (dir == 0 && gpu_on()) {
            /* GPU: pages the transfer rectangle covers completely are simply overwritten (no readback);
             * partially covered pages whose newest copy is on the GPU get the transfer's pixels as masked
             * word writes applied on the GPU (scatter); other pages are written here and uploaded */
            gsgpu_scatter_flush();
            if (g_xfer_any_scat) { memset(g_xfer_scat, 0, sizeof g_xfer_scat); g_xfer_any_scat = 0; }
            u32 dsx = (G.trxpos >> 32) & 0x7FF, w = G.trxreg & 0xFFF;
            u32 pw = (dpsm == PSMT8 || dpsm == PSMT4) ? 128 : 64;
            u32 ph = (dpsm == PSMCT32 || dpsm == PSMCT24 || dpsm == PSMZ32 || dpsm == PSMZ24 || dpsm == PSMT8H ||
                      dpsm == PSMT4HL || dpsm == PSMT4HH) ? 32 : dpsm == PSMT4 ? 128 : 64;
            u32 cols = (dpsm == PSMT8 || dpsm == PSMT4) ? (dbw + 1) / 2 : dbw;
            if (!cols) cols = 1;
            for (u32 py = dsy / ph; py <= (dsy + (h ? h - 1 : 0)) / ph; py++)
                for (u32 px = dsx / pw; px <= (dsx + (w ? w - 1 : 0)) / pw; px++) {
                    u32 page = dbp / 32 + py * cols + px;
                    int full = dbp % 32 == 0 && px < cols && px * pw >= dsx && (px + 1) * pw <= dsx + w && py * ph >= dsy && (py + 1) * ph <= dsy + h;
                    if (page >= 512) continue;
                    if (full) gsgpu_cpu_overwrite_page(page);
                    else if (gsgpu_page_gpu_newer(page)) g_xfer_scat[page] = 1, g_xfer_any_scat = 1;   /* scatter, no readback */
                    else RB(2, gsgpu_cpu_wrote(page * 32, page * 32 + 32));
                }
        }
    }
    if (dir == 0 && ((G.bitbltbuf >> 32) & 0x3FFF) == 0x3480 && (G.trxreg & 0xFFF) == 640 && g_seq_n < 200) g_seq[g_seq_n++] = 'U';
    if (g_seq_n == 200) { g_seq[200] = 0; fprintf(stderr, "[seq] %s\n", g_seq); g_seq_n++; }
    if (dir == 2) {   /* local -> local */
        u32 sbp = G.bitbltbuf & 0x3FFF, sbw = (G.bitbltbuf >> 16) & 63, spsm = (G.bitbltbuf >> 24) & 63;
        u32 dbp = (G.bitbltbuf >> 32) & 0x3FFF, dbw = (G.bitbltbuf >> 48) & 63, dpsm = (G.bitbltbuf >> 56) & 63;
        u32 ssx = G.trxpos & 0x7FF, ssy = (G.trxpos >> 16) & 0x7FF, dsx = (G.trxpos >> 32) & 0x7FF, dsy = (G.trxpos >> 48) & 0x7FF;
        u32 w = G.trxreg & 0xFFF, h = (G.trxreg >> 32) & 0xFFF;
        u32 shi = sbp + psm_blocks(spsm, sbw, ssy + h), dhi = dbp + psm_blocks(dpsm, dbw, dsy + h);
        RB(3, (gsgpu_sync_cpu(sbp, shi), gsgpu_sync_cpu(dbp, dhi)));   /* GPU: copy on the CPU with current data */
        for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++)
            vram_write(dpsm, dbp, dbw, (dsx + x) & 2047, (dsy + y) & 2047, vram_read(spsm, sbp, sbw, (ssx + x) & 2047, (ssy + y) & 2047));
        gsgpu_cpu_wrote(dbp, dhi);
    } else if (dir == 1) {
        static int once; if (!once++) fprintf(stderr, "[gs] local->host transfer not implemented\n");
    }
}

static void xfer_pixel(u32 v) {
    u32 dbp = (G.bitbltbuf >> 32) & 0x3FFF, dbw = (G.bitbltbuf >> 48) & 63, dpsm = (G.bitbltbuf >> 56) & 63;
    u32 dsx = (G.trxpos >> 32) & 0x7FF, dsy = (G.trxpos >> 48) & 0x7FF;
    u32 w = G.trxreg & 0xFFF, h = (G.trxreg >> 32) & 0xFFF;
    if (!G.xfer_active || G.ty >= h) return;
    u32 px = (dsx + G.tx) & 2047, py = (dsy + G.ty) & 2047;
    vram_write(dpsm, dbp, dbw, px, py, v);
    if (g_xfer_any_scat) {   /* GPU-newest page: forward the written bits to the GPU copy */
        u32 word, mask; vram_word_mask(dpsm, dbp, dbw, px, py, &word, &mask);
        if (mask && g_xfer_scat[(word >> 11) & 511]) {
            gsgpu_scatter(word, V32(word), mask);
            if (mask == ~0u) g_fresh[(word >> 6) & 16383] |= 1ull << (word & 63);
        }
    }
    if (++G.tx >= w) { G.tx = 0; if (++G.ty >= h) G.xfer_active = 0; }
}

void gs_image_data(const u8 *data, u32 bytes) {
    if (!G.xfer_active) { g_xfer_bytes_dropped += bytes; return; }
    g_xfer_bytes_ok += bytes;
    u32 dpsm = (G.bitbltbuf >> 56) & 63;
    for (u32 i = 0; i < bytes && G.xfer_active;) {
        switch (dpsm) {
        case PSMCT32: case PSMZ32: { u32 v; memcpy(&v, data + i, 4); xfer_pixel(v); i += 4; break; }
        case PSMCT24: case PSMZ24: {
            G.xfer_carry[G.xfer_ncarry++] = data[i++];
            if (G.xfer_ncarry == 3) { xfer_pixel(G.xfer_carry[0] | (G.xfer_carry[1] << 8) | (G.xfer_carry[2] << 16)); G.xfer_ncarry = 0; }
            break; }
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: { u16 v; memcpy(&v, data + i, 2); xfer_pixel(v); i += 2; break; }
        case PSMT8: case PSMT8H: xfer_pixel(data[i++]); break;
        case PSMT4: case PSMT4HL: case PSMT4HH: xfer_pixel(data[i] & 15); xfer_pixel(data[i] >> 4); i++; break;
        default: i = bytes; break;
        }
    }
}

/* ------------------------------------------------------------------ register writes */
static struct { int reg; u64 v; } g_hist[24]; static int g_hist_i;
static void hist_dump(void) {
    for (int k = 0; k < 24; k++) { int j = (g_hist_i + k) % 24; fprintf(stderr, "      reg %02x = %016llx\n", g_hist[j].reg, (unsigned long long)g_hist[j].v); }
}
void gs_write_reg(int reg, u64 v) {
    g_hist[g_hist_i].reg = reg; g_hist[g_hist_i].v = v; g_hist_i = (g_hist_i + 1) % 24;
    if (g_trace_gs > 0) fprintf(stderr, "[gs] reg %02x = %016llx\n", reg, (unsigned long long)v);
    switch (reg) {
    case 0x00: G.prim = v; G.nv = 0; break;
    case 0x01: G.rgbaq = v; break;
    case 0x02: G.st = v; break;
    case 0x03: G.uv = v; break;
    case 0x04: xyzf2(v, 1); break;
    case 0x05: vertex_kick((u32)v & 0xFFFF, (u32)(v >> 16) & 0xFFFF, (u32)(v >> 32), 0, 1); break;
    case 0x06: case 0x07: G.ctx[reg - 6].tex0 = v; clut_load(v); break;
    case 0x08: case 0x09: G.ctx[reg - 8].clamp = v; break;
    case 0x0A: G.fog = v; break;
    case 0x0C: xyzf2(v, 0); break;
    case 0x0D: vertex_kick((u32)v & 0xFFFF, (u32)(v >> 16) & 0xFFFF, (u32)(v >> 32), 0, 0); break;
    case 0x14: case 0x15: G.ctx[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: {   /* TEX2: update PSM/CLUT fields of TEX0 */
        Ctx *c = &G.ctx[reg - 0x16];
        u64 mask = (63ull << 20) | (0x7FFFFFFull << 37);
        c->tex0 = (c->tex0 & ~mask) | (v & mask);
        clut_load(c->tex0);
        break; }
    case 0x18: case 0x19: G.ctx[reg - 0x18].xyoffset = v; break;
    case 0x1A: G.prmodecont = v; break;
    case 0x1B: G.prmode = v; break;
    case 0x1C: G.texclut = v; break;
    case 0x22: G.scanmsk = v; break;
    case 0x3B: G.texa = v; break;
    case 0x3D: G.fogcol = v; break;
    case 0x3F: break;   /* TEXFLUSH */
    case 0x40: case 0x41: G.ctx[reg - 0x40].scissor = v; break;
    case 0x42: case 0x43: G.ctx[reg - 0x42].alpha = v; break;
    case 0x44: G.dimx = v; break;
    case 0x45: G.dthe = v; break;
    case 0x46: G.colclamp = v; break;
    case 0x47: case 0x48: G.ctx[reg - 0x47].test = v; break;
    case 0x49: G.pabe = v; break;
    case 0x4A: case 0x4B: G.ctx[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: G.ctx[reg - 0x4C].frame = v; break;
    case 0x4E: case 0x4F: G.ctx[reg - 0x4E].zbuf = v; break;
    case 0x50: G.bitbltbuf = v; break;
    case 0x51: G.trxpos = v; break;
    case 0x52: G.trxreg = v; break;
    case 0x53: G.trxdir = v; xfer_start(); break;
    case 0x54: { u8 b[8]; memcpy(b, &v, 8); gs_image_data(b, 8); break; }
    case 0x60: { /* SIGNAL: SIGID = (SIGID & ~mask) | (id & mask) */
        gs_flush();
        u32 id = (u32)v, mask = (u32)(v >> 32), sig = (u32)G.siglblid;
        G.siglblid = (G.siglblid & 0xFFFFFFFF00000000ull) | ((sig & ~mask) | (id & mask));
        InterlockedOr64((volatile LONG64 *)&G.csr, 1);
        if (!(G.imr & 0x100)) kernel_request_intc(0);
        break; }
    case 0x61:   /* FINISH: everything before it has been drawn */
        gs_flush();
        InterlockedOr64((volatile LONG64 *)&G.csr, 2);
        if (!(G.imr & 0x200)) kernel_request_intc(0);
        break;
    case 0x62: { /* LABEL: LBLID = (LBLID & ~mask) | (id & mask) */
        gs_flush();
        u32 id = (u32)v, mask = (u32)(v >> 32), lbl = (u32)(G.siglblid >> 32);
        G.siglblid = (G.siglblid & 0xFFFFFFFFull) | ((u64)((lbl & ~mask) | (id & mask)) << 32);
        break; }
    default: break;
    }
}

/* XYZF2 needs Z (24 bits at 32..55) and F (56..63) */
static void xyzf2(u64 v, int draw) { vertex_kick((u32)v & 0xFFFF, (u32)(v >> 16) & 0xFFFF, (u32)((v >> 32) & 0xFFFFFF), (int)(v >> 56), draw); }

/* PACKED descriptors (GIF): q = 16 bytes */
void gs_write_packed(int reg, const u8 *q) {
    u64 lo, hi; memcpy(&lo, q, 8); memcpy(&hi, q + 8, 8);
    g_hist[g_hist_i].reg = 0x100 | reg; g_hist[g_hist_i].v = lo ^ (hi << 32); g_hist_i = (g_hist_i + 1) % 24;
    switch (reg) {
    case 0x0: G.prim = lo & 0x7FF; G.nv = 0; break;
    case 0x1: {
        u64 r = lo & 255, g = (lo >> 32) & 255, b = hi & 255, a = (hi >> 32) & 255;
        u32 qb; memcpy(&qb, &G.q, 4);
        G.rgbaq = r | (g << 8) | (b << 16) | (a << 24) | ((u64)qb << 32);
        break; }
    case 0x2: { G.st = lo; u32 qb = (u32)hi; memcpy(&G.q, &qb, 4); break; }
    case 0x3: G.uv = (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16); break;
    case 0x4: {   /* XYZF2 packed: X 0-15, Y 32-47, Z 68-91, F 100-107, ADC bit 111 */
        u32 z = (u32)((hi >> 4) & 0xFFFFFF); int f = (int)((hi >> 36) & 255);
        int adc = (hi >> 47) & 1;
        vertex_kick((u32)lo & 0xFFFF, (u32)(lo >> 32) & 0xFFFF, z, f, !adc);
        break; }
    case 0x5: {
        int adc = (hi >> 47) & 1;
        vertex_kick((u32)lo & 0xFFFF, (u32)(lo >> 32) & 0xFFFF, (u32)hi, 0, !adc);
        break; }
    case 0xA: G.fog = ((hi >> 36) & 255) << 56; break;
    default: gs_write_reg(reg, lo); break;
    }
}

/* ------------------------------------------------------------------ privileged registers */
u64 gs_priv_read(u32 addr) {
    switch (addr & 0x1FFFFFFF) {
    case 0x12001000: return G.csr;
    case 0x12001010: return G.imr;
    case 0x12001040: return 0;
    case 0x12001080: return G.siglblid;
    }
    return 0;
}

void gs_priv_write(u32 addr, u64 v) {
    switch (addr & 0x1FFFFFFF) {
    case 0x12000000: G.pmode = v; break;
    case 0x12000010: G.smode1 = v; break;
    case 0x12000020: G.smode2 = v; break;
    case 0x12000070: G.dispfb[0] = v; break;
    case 0x12000080: G.display[0] = v; break;
    case 0x12000090: G.dispfb[1] = v; break;
    case 0x120000A0: G.display[1] = v; break;
    case 0x120000E0: G.bgcolor = v; break;
    case 0x12001000:
        InterlockedAnd64((volatile LONG64 *)&G.csr, ~(LONG64)(v & 0x1F));   /* write 1 clears SIGNAL/FINISH/HSINT/VSINT/EDWINT */
        if (v & 0x200) InterlockedAnd64((volatile LONG64 *)&G.csr, ~(LONG64)0x1F);   /* RESET */
        break;
    case 0x12001010: G.imr = v; break;
    case 0x12001080: G.siglblid = v; break;
    default: break;
    }
}

/* ------------------------------------------------------------------ display */
static u32 g_fb[1024 * 1024];
u32 g_fbp_hist[8][2];
static int g_frame_no;
int g_frame_no_pub;
static volatile LONG g_disp_req;   /* GPU mode: a display readout is pending (served on the GS thread) */
static void display_readout(int frame_no);
static void display_readout_p(int frame_no, u64 pmode, u64 smode2, u64 fb, u64 dp);

/* GS thread: serve a pending display request (GPU mode). Returns 1 if one was pending. */
/* Asynchronous: the displayed pages are copied to a staging slot and presented once the GPU has
 * finished them (normally by the next vsync), so the GS thread never waits for the GPU here.
 * PS2_GPU_SYNCDISPLAY=1 reads the display back synchronously instead. */
static struct { int busy, frame; u64 pmode, smode2, fb, dp; } g_dq[3];
static int g_dq_head, g_dq_n;
static void dq_present(int wait) {
    while (g_dq_n) {
        int s = g_dq_head;
        const u8 *mem = gsgpu_snap_map(s, wait);
        if (!mem) return;
        u8 *save = g_vram;
        g_vram = (u8 *)mem;   /* display_readout reads VRAM through g_vram (only the GS thread touches it in GPU mode) */
        display_readout_p(g_dq[s].frame, g_dq[s].pmode, g_dq[s].smode2, g_dq[s].fb, g_dq[s].dp);
        g_vram = save;
        gsgpu_snap_unmap(s);
        g_dq[s].busy = 0; g_dq_head = (g_dq_head + 1) % 3; g_dq_n--;
        wait = 0;
    }
}
const char *gs_renderer_name(void) { return g_gpu == 1 ? "GPU" : g_gpu == 0 ? "software" : "-"; }
static void vram_touch(u32 lo, u32 hi);
/* GS thread: F10 switches between the GPU and CPU rasterizers (VRAM handed over), F9 writes a capture:
 * capture/NN_ram.bin (EE RAM), NN_vram.raw (GS memory), NN_info.txt, NN_draws.txt (every primitive of
 * the next frame with full state) and NN_frame.bmp (that frame). */
static void gs_hotkeys(void) {
    { /* the same, scripted: PS2_CAPTURE_AT=frame, PS2_TOGGLE_GPU_AT=frame */
      static int cap_at = -2, tog_at = -2;
      if (cap_at == -2) { const char *e = getenv("PS2_CAPTURE_AT"); cap_at = e ? atoi(e) : -1; e = getenv("PS2_TOGGLE_GPU_AT"); tog_at = e ? atoi(e) : -1; }
      if (cap_at >= 0 && g_frame_no_pub >= cap_at) { cap_at = -1; InterlockedExchange(&g_hk_capture, 1); }
      if (tog_at >= 0 && g_frame_no_pub >= tog_at) { tog_at = -1; InterlockedExchange(&g_hk_toggle_gpu, 1); } }
    if (InterlockedExchange(&g_hk_toggle_gpu, 0)) {
        gs_flush();
        if (g_gpu == 1) {
            while (g_dq_n) dq_present(1);
            gsgpu_sync_cpu_all();
            g_gpu = 0;
            vram_touch(0, 16384);
            fprintf(stderr, "[gs] F10: software rasterizer\n");
        } else if (gsgpu_enabled()) {
            gsgpu_cpu_wrote(0, 16384);   /* the GPU copy is stale: upload everything */
            g_gpu = 1;
            fprintf(stderr, "[gs] F10: GPU rasterizer\n");
        }
    }
    if (g_cap_fp && g_frame_no_pub > g_cap_frame + 3) { fclose(g_cap_fp); g_cap_fp = NULL; fprintf(stderr, "[gs] F9: capture done\n"); }
    if (InterlockedExchange(&g_hk_capture, 0) && !g_cap_fp) {
        gs_flush();
        if (g_gpu == 1) gsgpu_sync_cpu_all();
        CreateDirectoryA("capture", NULL);
        static int idx; char fn[96]; FILE *f;
        for (;; idx++) { snprintf(fn, sizeof fn, "capture/%02d_info.txt", idx); if (GetFileAttributesA(fn) == INVALID_FILE_ATTRIBUTES) break; }
        snprintf(fn, sizeof fn, "capture/%02d_ram.bin", idx);
        if ((f = fopen(fn, "wb"))) { fwrite(ps2_ram, 1, 32u << 20, f); fclose(f); }
        snprintf(fn, sizeof fn, "capture/%02d_vram.raw", idx);
        if ((f = fopen(fn, "wb"))) { fwrite(g_vram, 1, 4u << 20, f); fclose(f); }
        snprintf(fn, sizeof fn, "capture/%02d_info.txt", idx);
        if ((f = fopen(fn, "w"))) {
            fprintf(f, "frame %d renderer %s\n", g_frame_no_pub, g_gpu == 1 ? "gpu" : "cpu");
            fprintf(f, "pmode %llx dispfb1 %llx dispfb2 %llx display1 %llx\n", (unsigned long long)G.pmode,
                    (unsigned long long)G.dispfb[0], (unsigned long long)G.dispfb[1], (unsigned long long)G.display[0]);
            for (u32 o = 0x400000; o + 0x900 < (32u << 20); o += 16) {   /* ClEragonObj (class info 0x38b408 at +8) */
                u32 cls; memcpy(&cls, ps2_ram + o + 8, 4);
                if (cls != 0x38b408) continue;
                float p[3]; memcpy(p, ps2_ram + o + 0x70, 12);
                fprintf(f, "eragon obj %08x pos %.3f %.3f %.3f\n", o, p[0], p[1], p[2]);
            }
            fclose(f);
        }
        snprintf(fn, sizeof fn, "capture/%02d_draws.txt", idx);
        g_cap_fp = fopen(fn, "w");
        g_cap_frame = g_frame_no_pub + 1;
        snprintf(g_cap_bmp, sizeof g_cap_bmp, "capture/%02d_frame.bmp", idx);
        fprintf(stderr, "[gs] F9: capture %02d (frame %d)\n", idx, g_cap_frame);
        idx++;
    }
}

int gs_service_display(void) {
    gs_hotkeys();
    if (!InterlockedExchange(&g_disp_req, 0)) { if (g_dq_n) dq_present(0); return 0; }
    gs_flush();
    { static int cc = -1; if (cc < 0) cc = getenv("PS2_GPU_COHERCHECK") != NULL;
      if (cc && g_frame_no % 200 == 0) { void gsgpu_check_coherence(int); gsgpu_check_coherence(g_frame_no); } }
    int circ = (G.pmode & 2) ? 1 : 0;
    u64 fb = G.dispfb[circ];
    u32 fbp = (u32)(fb & 0x1FF) * 32, fbw = (fb >> 9) & 63, psm = (fb >> 15) & 31, dby = (fb >> 43) & 0x7FF;
    u32 hi = fbp + psm_blocks(psm, fbw ? fbw : 10, dby + 1024);
    static int sync = -1; if (sync < 0) sync = getenv("PS2_GPU_SYNCDISPLAY") != NULL;
    if (!sync) {
        if (g_dq_n == 3) dq_present(1);   /* all slots in flight: wait for the oldest */
        int s = (g_dq_head + g_dq_n) % 3;
        if (gsgpu_snap_begin(s, fbp, hi)) {
            g_dq[s].busy = 1; g_dq[s].frame = g_frame_no; g_dq[s].pmode = G.pmode; g_dq[s].smode2 = G.smode2;
            g_dq[s].fb = fb; g_dq[s].dp = G.display[circ];
            g_dq_n++;
            dq_present(0);
            return 1;
        }
    }
    RB(4, gsgpu_sync_cpu(fbp, hi));
    display_readout(g_frame_no);
    return 1;
}
int gs_display_pending(void) { return g_disp_req != 0; }

void gs_vsync(void) {
    InterlockedOr64((volatile LONG64 *)&G.csr, 0x8);       /* VSINT */
    InterlockedXor64((volatile LONG64 *)&G.csr, 0x2000);   /* FIELD */
    g_frame_no++; g_frame_no_pub = g_frame_no;
    { static int lo = -2, hi; if (lo == -2) { const char *e = getenv("PS2_FBLOG"); lo = e ? atoi(e) : -1; hi = lo + 120; }
      if (lo >= 0 && g_frame_no >= lo && g_frame_no < hi) {
          fprintf(stderr, "[fb] f%d pmode=%llx dispfb1=%llx dispfb2=%llx draws:", g_frame_no, (unsigned long long)G.pmode,
                  (unsigned long long)G.dispfb[0], (unsigned long long)G.dispfb[1]);
          for (int i = 0; i < 8 && g_fbp_hist[i][1]; i++) fprintf(stderr, " fbp%x x%u", g_fbp_hist[i][0], g_fbp_hist[i][1]);
          fputc('\n', stderr);
      }
      memset(g_fbp_hist, 0, sizeof g_fbp_hist); }
    if (getenv("PS2_GFXSTATS") && g_frame_no % 120 == 0)
        fprintf(stderr, "[gs] f%d pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx prims=%d frame1=%llx frame2=%llx\n",
                g_frame_no, (unsigned long long)G.pmode, (unsigned long long)G.smode2, (unsigned long long)G.dispfb[0],
                (unsigned long long)G.display[0], (unsigned long long)G.dispfb[1], (unsigned long long)G.display[1],
                G.prim_count, (unsigned long long)G.ctx[0].frame, (unsigned long long)G.ctx[1].frame),
        fprintf(stderr, "[gs]   transfers %llu, image bytes %llu, GS time %.1f s, raster %.1f s, huge prims %llu, texcache %llu hits %llu builds, "
                "%llu flushes (%.1f prims each)\n",
                (unsigned long long)g_xfer_count, (unsigned long long)g_xfer_bytes_ok, g_gs_time, g_flush_s, (unsigned long long)g_huge_prims,
                (unsigned long long)g_texc_hits, (unsigned long long)g_texc_builds, (unsigned long long)g_flush_count,
                g_flush_count ? (double)g_flush_prims / g_flush_count : 0.0);
    if (getenv("PS2_GFXSTATS") && g_frame_no % 120 == 0) {
        static u64 last_frags; static double last_raster;
        u64 f = 0; for (int i = 0; i < 16; i++) f += g_frag_count[i][0];
        double dr = g_flush_s - last_raster;
        fprintf(stderr, "[gs]   fragments: %.2f M per vsync, %.1f Mfrag/s of raster time; GPU view-conflict flushes %llu, VRAM copies %llu\n",
                (double)(f - last_frags) / 120 / 1e6, dr > 0 ? (double)(f - last_frags) / dr / 1e6 : 0.0,
                (unsigned long long)g_view_flushes, (unsigned long long)g_copies);
        { extern u64 g_gpu_downloads; extern double g_gpu_wait_s[4];
          fprintf(stderr, "[gs]   readbacks by site: clut %llu fallback %llu partial-upload %llu local-copy %llu display %llu\n", (unsigned long long)g_rb_site[0], (unsigned long long)g_rb_site[1], (unsigned long long)g_rb_site[2], (unsigned long long)g_rb_site[3], (unsigned long long)g_rb_site[4]);
          fprintf(stderr, "[gs]   GPU waits: %llu readbacks %.1f s, dynamic Map %.1f s, uploads %.1f s, dispatch %.1f s\n",
                  (unsigned long long)g_gpu_downloads, g_gpu_wait_s[0], g_gpu_wait_s[1], g_gpu_wait_s[2], g_gpu_wait_s[3]); }
        { static u64 lp, lt, lc, lb; fprintf(stderr, "[gs]   GPU work: %.1f k tile-prim pairs, %.1f k tiles, %.1f texture copies (%.2f MB) per vsync\n",
                                     (double)(g_tile_pairs - lp) / 120e3, (double)(g_gpu_tiles - lt) / 120e3,
                                     (double)(g_copies - lc) / 120, (double)(g_copy_bytes - lb) / 120 / 1048576.0);
          lp = g_tile_pairs; lt = g_gpu_tiles; lc = g_copies; lb = g_copy_bytes; }
        { extern double g_gpu_ms[5]; extern u64 g_gpu_ms_ops[5];   /* PS2_GPUTIME=1 */
          static double lm[5]; static u64 lo[5];
          if (g_gpu_ms_ops[4] > lo[4]) {
              const char *nm[5] = {"copies", "raster", "scatter", "uploads", "span"};
              fprintf(stderr, "[gs]   GPU time per 120 vsyncs (sampled):");
              for (int k = 0; k < 5; k++) fprintf(stderr, " %s %.0f ms (%llu ops)", nm[k], g_gpu_ms[k] - lm[k], (unsigned long long)(g_gpu_ms_ops[k] - lo[k]));
              fputc('\n', stderr);
              for (int k = 0; k < 5; k++) { lm[k] = g_gpu_ms[k]; lo[k] = g_gpu_ms_ops[k]; }
          } }
        last_frags = f; last_raster = g_flush_s;
    }
    { static int vdump = -1; if (vdump < 0) { const char *e = getenv("PS2_VRAMDUMP"); vdump = e ? atoi(e) : 0; }
      if (vdump && g_frame_no == vdump) {   /* whole VRAM as a 1024-wide CT32 surface (bw 16) */
          FILE *f = fopen("vram.raw", "wb");
          for (int y = 0; y < 1024; y++) for (int x = 0; x < 1024; x++) { u32 v = vram_read(PSMCT32, 0, 16, x, y); fwrite(&v, 4, 1, f); }
          fclose(f); } }
    if (g_gpu == 1) {   /* GPU: the GS thread reads the display out once it has drawn what is queued */
        void gif_wake_gs(void);
        InterlockedExchange(&g_disp_req, 1);
        gif_wake_gs();
        return;
    }
    display_readout(g_frame_no);
}

/* Read the displayed buffer out of VRAM, present it, and save screenshots (PS2_SHOT). */
static void display_readout_p(int frame_no, u64 pmode, u64 smode2, u64 fb, u64 dp);
static void display_readout(int frame_no) {
    int circ = (G.pmode & 2) ? 1 : 0;
    display_readout_p(frame_no, G.pmode, G.smode2, G.dispfb[circ], G.display[circ]);
}
/* game frames = flips of the displayed buffer (DISPFB changes); read by the overlay in window.c */
volatile LONG64 g_gs_flips;
volatile LONG g_gs_frame_max_us;
static void display_readout_p(int frame_no, u64 pmode, u64 smode2, u64 fb, u64 dp) {
    int g_frame_no = frame_no;
    { /* a new game frame: the displayed buffer changed, or something was drawn into it since the last vsync
       * (Eragon keeps DISPFB fixed and copies each finished frame into the front buffer) */
      static u64 last_fb = ~0ull; static LARGE_INTEGER last_t;
      if (fb != last_fb || g_disp_drawn) {
          InterlockedIncrement64(&g_gs_flips);
          LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
          if (last_t.QuadPart) {   /* longest frame since the overlay last read it (microseconds) */
              LONG us = (LONG)((t.QuadPart - last_t.QuadPart) * 1000000 / f.QuadPart);
              if (us > g_gs_frame_max_us) InterlockedExchange(&g_gs_frame_max_us, us);
          }
          last_t = t;
      }
      last_fb = fb; g_disp_drawn = 0; }
    struct { u64 smode2; } G = {smode2};   /* shadows the register file: use the captured values */
    if (!(pmode & 3)) { win_present(NULL, 0, 0); return; }
    u32 fbp = (fb & 0x1FF) * 32, fbw = (fb >> 9) & 63, psm = (fb >> 15) & 31, dbx = (fb >> 32) & 0x7FF, dby = (fb >> 43) & 0x7FF;
    u32 magh = ((dp >> 23) & 15) + 1, magv = ((dp >> 27) & 3) + 1;
    int w = (int)((((dp >> 32) & 0xFFF) + 1) / magh), h = (int)((((dp >> 44) & 0x7FF) + 1) / magv);
    int interlaced_frame = (G.smode2 & 1) && !(G.smode2 & 2);
    if (interlaced_frame) h /= 2;                     /* DH counts field lines in frame mode? keep half */
    if ((G.smode2 & 3) == 1) h *= 2;                  /* interlaced FRAME mode: full-height buffer */
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { w = 640; h = 448; }
    if (fbw == 0) fbw = (w + 63) / 64;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        u32 v = vram_read(psm, fbp, fbw, dbx + x, dby + y);
        u32 c;
        if (psm == PSMCT16 || psm == PSMCT16S) c = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19);
        else c = v & 0xFFFFFF;
        g_fb[y * w + x] = c;
    }
    static int dump_every = -1;
    if (dump_every < 0) { const char *e = getenv("PS2_SHOT"); dump_every = e ? atoi(e) : 0; }
    int cap = g_cap_bmp[0] && g_frame_no > g_cap_frame && g_frame_no <= g_cap_frame + 4;   /* F9 capture: 4 frames */
    if ((dump_every > 0 && g_frame_no % dump_every == 0) || cap) {
        char name[128]; snprintf(name, sizeof name, "frames/f%05d.bmp", g_frame_no);
        if (cap) { snprintf(name, sizeof name, "%.*s_%d.bmp", (int)strlen(g_cap_bmp) - 4, g_cap_bmp, g_frame_no - g_cap_frame); }
        CreateDirectoryA("frames", NULL);
        FILE *f = fopen(name, "wb");
        if (f) {
            u32 rowb = (u32)w * 3, pad = (4 - rowb % 4) % 4, size = 54 + (rowb + pad) * h;
            u8 hdr[54] = {'B', 'M'};
            memcpy(hdr + 2, &size, 4); u32 off = 54; memcpy(hdr + 10, &off, 4);
            u32 ihs = 40; memcpy(hdr + 14, &ihs, 4); memcpy(hdr + 18, &w, 4); s32 nh = -h; memcpy(hdr + 22, &nh, 4);
            u16 planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
            fwrite(hdr, 1, 54, f);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) { u32 c = g_fb[y * w + x]; u8 px[3] = {(u8)(c >> 16), (u8)(c >> 8), (u8)c}; fwrite(px, 1, 3, f); }
                fwrite("\0\0\0", 1, pad, f);
            }
            fclose(f);
        }
    }
    win_present(g_fb, w, h);
}
