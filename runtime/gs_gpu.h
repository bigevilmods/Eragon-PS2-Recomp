/* GPU (Direct3D 11 compute) back end for the GS rasterizer.
 *
 * The PS2's 4 MB swizzled local memory lives in a GPU buffer; a compute shader runs the same pixel
 * pipeline as the software rasterizer in gs.c (formats, swizzle, tests, blending, CLUT texturing)
 * directly on it, so render-to-texture and format reinterpretation behave exactly as on the CPU.
 * Primitives are binned into 16x16 tiles; one thread group owns a tile and walks its primitives in
 * submission order, so per-pixel ordering is preserved without locking.
 *
 * Coherence with the CPU copy (g_vram in gs.c) is tracked per 8 KB page: pages the CPU wrote are
 * uploaded before the next dispatch; pages the GPU may have written are downloaded only when a CPU
 * reader (display, CLUT load, transfer, fallback raster) asks for them. */
#pragma once
#include "ps2_runtime.h"

/* Per-draw state; layout shared with the HLSL struct GState (32 uints). */
typedef struct {
    u32 fbp, fbw, fpsm, fbmsk;
    u32 zbp, zpsm, zmsk, test;
    s32 sx0, sx1, sy0, sy1;
    u32 flags;          /* GSF_* */
    u32 alpha, afix;    /* ALPHA A|B<<2|C<<4|D<<6, FIX */
    u32 tbp, tbw, tpsm, tw, th, tfx;
    u32 wms, wmt, minu, maxu, minv, maxv;
    u32 cpsm, csa, texa;   /* texa = TA0 | AEM<<8 | TA1<<16 */
    u32 clut;              /* index of the CLUT snapshot (768 uints each) */
} GState;

enum { GSF_TME = 1, GSF_ABE = 2, GSF_FST = 4, GSF_IIP = 8, GSF_BILINEAR = 16, GSF_COLCLAMP = 32, GSF_PABE = 64,
       GSF_FBA = 128, GSF_TCC = 256,
       GSF_TEXCOPY = 512,     /* texture overlaps this draw's own targets: sample the pre-dispatch VRAM copy */
       GSF_CLUTVRAM = 1024 }; /* 8-bit texture whose CT32 CLUT the GPU drew: read it from VRAM at GState.clut (cbp) */

/* Primitive: type (0 point, 1 line, 3 triangle, 6 sprite), state index, covered pixel box within the
 * scissor (x0 | y0 << 16, x1 | y1 << 16, inclusive), 3 vertices of x, y, z (bits), r, g, b, a, s, t, q, u, v.
 * 40 uints; GState is 31. */
typedef struct { u32 type, state, bb0, bb1; float v[36]; } GPrim;

int  gsgpu_enabled(void);                   /* PS2_GPU=0 disables; initialises on first call */
void gsgpu_sync_cpu(u32 block_lo, u32 block_hi);      /* make g_vram current for these blocks */
void gsgpu_sync_cpu_all(void);
void gsgpu_cpu_wrote(u32 block_lo, u32 block_hi);     /* the CPU changed these blocks */
void gsgpu_gpu_will_write(u32 block_lo, u32 block_hi);/* a submitted batch may write these blocks */
void gsgpu_cpu_overwrite_page(u32 page);              /* the CPU rewrites this whole 8 KB page */
int  gsgpu_page_gpu_newer(u32 page);                  /* the GPU holds the newest copy of this page */
void gsgpu_scatter(u32 word, u32 value, u32 mask);    /* masked write of a VRAM word, applied on the GPU */
void gsgpu_scatter_flush(void);
/* asynchronous VRAM snapshot into staging slot 0..2 (blocks [lo, hi)); map returns NULL while in flight */
int  gsgpu_snap_begin(int slot, u32 block_lo, u32 block_hi);
const u8 *gsgpu_snap_map(int slot, int wait);
void gsgpu_snap_unmap(int slot);
/* tiles: (tile id = ty*128+tx, first index, count, 0) per active tile; idx: primitive indices */
void gsgpu_submit(const GState *st, int nst, const GPrim *pr, int npr, const u32 *tiles, int ntiles,
                  const u32 *idx, int nidx, const u32 *cluts, int ncluts,
                  const u32 *copy_ranges, int ncopy);   /* (block lo, block hi) pairs to snapshot for GSF_TEXCOPY */
