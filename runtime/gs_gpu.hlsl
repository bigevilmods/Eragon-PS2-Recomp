// GS pixel pipeline as a D3D11 compute shader (see gs_gpu.h). A line-for-line port of the software
// rasterizer in gs.c: same swizzled addressing, formats, coverage rules, texturing, tests and blending.

// globallycoherent: a thread re-reads pixels it wrote earlier in the same dispatch (blending over its own
// output); without it AMD's non-coherent L1 can return the value from before the write.
globallycoherent RWByteAddressBuffer vram : register(u0);
// (Reading textures through a second, non-coherent UAV of the same buffer was tried: slower on the Vega 8 driver.)

struct GState {
    uint fbp, fbw, fpsm, fbmsk;
    uint zbp, zpsm, zmsk, test;
    int  sx0, sx1, sy0, sy1;
    uint flags, alpha, afix;
    uint tbp, tbw, tpsm, tw, th, tfx;
    uint wms, wmt, minu, maxu, minv, maxv;
    uint cpsm, csa, texa, clut;
};
// primitives (40 words: type, state, pixel bbox x0|y0<<16, x1|y1<<16, 3 x 12 vertex words) and states
// (31 words, GState order) arrive as flat word streams; a thread group stages them in groupshared memory
#define PRIM_WORDS 40
#define STATE_WORDS 31
StructuredBuffer<uint>   statew : register(t0);
StructuredBuffer<uint>   primw  : register(t1);
StructuredBuffer<uint4>  tiles  : register(t2);
StructuredBuffer<uint>   binidx : register(t3);
StructuredBuffer<uint>   cluts  : register(t4);   // per CLUT: 256 x CT32, then 512 x CT16
StructuredBuffer<uint>   tbl    : register(t5);   // swizzle tables (offsets below, filled by gs.c)
ByteAddressBuffer        vramc  : register(t6);   // VRAM as of the dispatch start (GSF_TEXCOPY draws)
// the inputs live in ring buffers: this submission's records start at these indices
cbuffer Bases : register(b0) { uint b_state, b_prim, b_tile, b_idx, b_clut, b_scat, n_scat, dbg_mode; };

#define T_BLK32 0
#define T_BLKZ32 32
#define T_COL32 64
#define T_BLK16 128
#define T_BLK16S 160
#define T_BLKZ16 192
#define T_BLKZ16S 224
#define T_COL16 256
#define T_BLK8 384
#define T_COL8 416
#define T_BLK4 672
#define T_COL4 704

#define PSMCT32 0x00
#define PSMCT24 0x01
#define PSMCT16 0x02
#define PSMCT16S 0x0A
#define PSMT8 0x13
#define PSMT4 0x14
#define PSMT8H 0x1B
#define PSMT4HL 0x24
#define PSMT4HH 0x2C
#define PSMZ32 0x30
#define PSMZ24 0x31
#define PSMZ16 0x32
#define PSMZ16S 0x3A

#define GSF_TME 1
#define GSF_ABE 2
#define GSF_FST 4
#define GSF_IIP 8
#define GSF_BILINEAR 16
#define GSF_COLCLAMP 32
#define GSF_PABE 64
#define GSF_FBA 128
#define GSF_TCC 256
#define GSF_TEXCOPY 512
#define GSF_CLUTVRAM 1024

// ---------------------------------------------------------------- addressing (word/half/byte/nibble index)
uint addr32(uint bp, uint bw, uint x, uint y, uint blk) {
    uint page = (x >> 6) + (y >> 5) * bw;
    return (bp * 64 + page * 2048 + tbl[blk + ((y >> 3) & 3) * 8 + ((x >> 3) & 7)] * 64 + tbl[T_COL32 + (y & 7) * 8 + (x & 7)]) & 0xFFFFF;
}
uint addr16(uint bp, uint bw, uint x, uint y, uint blk) {
    uint page = (x >> 6) + (y >> 6) * bw;
    return (bp * 128 + page * 4096 + tbl[blk + ((y >> 3) & 7) * 4 + ((x >> 4) & 3)] * 128 + tbl[T_COL16 + (y & 7) * 16 + (x & 15)]) & 0x1FFFFF;
}
uint addr8(uint bp, uint bw, uint x, uint y) {
    uint page = (x >> 7) + (y >> 6) * ((bw + 1) >> 1);
    return (bp * 256 + page * 8192 + tbl[T_BLK8 + ((y >> 4) & 3) * 8 + ((x >> 4) & 7)] * 256 + tbl[T_COL8 + (y & 15) * 16 + (x & 15)]) & 0x3FFFFF;
}
uint addr4(uint bp, uint bw, uint x, uint y) {
    uint page = (x >> 7) + (y >> 7) * ((bw + 1) >> 1);
    return (bp * 512 + page * 16384 + tbl[T_BLK4 + ((y >> 4) & 7) * 4 + ((x >> 5) & 3)] * 512 + tbl[T_COL4 + (y & 15) * 32 + (x & 31)]) & 0x7FFFFF;
}

uint rd32(uint w) { return vram.Load(w * 4); }
void wr32(uint w, uint v) { vram.Store(w * 4, v); }
uint rd16(uint h) { uint w = vram.Load((h >> 1) * 4); return (h & 1) ? (w >> 16) : (w & 0xFFFF); }
void wr16(uint h, uint v) {   // neighbouring 16-bit pixels share a word and may belong to other threads
    uint sh = (h & 1) * 16, o;
    vram.InterlockedAnd((h >> 1) * 4, ~(0xFFFFu << sh), o);
    vram.InterlockedOr((h >> 1) * 4, (v & 0xFFFF) << sh, o);
}

uint vram_read(uint psm, uint bp, uint bw, uint x, uint y) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: return rd32(addr32(bp, bw, x, y, T_BLK32));
    case PSMZ32: case PSMZ24: return rd32(addr32(bp, bw, x, y, T_BLKZ32));
    case PSMCT16: return rd16(addr16(bp, bw, x, y, T_BLK16));
    case PSMCT16S: return rd16(addr16(bp, bw, x, y, T_BLK16S));
    case PSMZ16: return rd16(addr16(bp, bw, x, y, T_BLKZ16));
    case PSMZ16S: return rd16(addr16(bp, bw, x, y, T_BLKZ16S));
    case PSMT8: { uint b = addr8(bp, bw, x, y); return (vram.Load((b >> 2) * 4) >> ((b & 3) * 8)) & 255; }
    case PSMT4: { uint n = addr4(bp, bw, x, y); return (vram.Load((n >> 3) * 4) >> ((n & 7) * 4)) & 15; }
    }
    return 0;
}
void vram_write(uint psm, uint bp, uint bw, uint x, uint y, uint v) {
    switch (psm) {
    case PSMCT32: wr32(addr32(bp, bw, x, y, T_BLK32), v); break;
    case PSMCT24: { uint a = addr32(bp, bw, x, y, T_BLK32); wr32(a, (rd32(a) & 0xFF000000u) | (v & 0xFFFFFF)); break; }
    case PSMZ32: wr32(addr32(bp, bw, x, y, T_BLKZ32), v); break;
    case PSMZ24: { uint a = addr32(bp, bw, x, y, T_BLKZ32); wr32(a, (rd32(a) & 0xFF000000u) | (v & 0xFFFFFF)); break; }
    case PSMCT16: wr16(addr16(bp, bw, x, y, T_BLK16), v); break;
    case PSMCT16S: wr16(addr16(bp, bw, x, y, T_BLK16S), v); break;
    case PSMZ16: wr16(addr16(bp, bw, x, y, T_BLKZ16), v); break;
    case PSMZ16S: wr16(addr16(bp, bw, x, y, T_BLKZ16S), v); break;
    }
}

// texture loads: from the live VRAM, or from the pre-dispatch copy for draws that sample their own target
uint tld(bool c, uint byteaddr) { return c ? vramc.Load(byteaddr) : vram.Load(byteaddr); }
uint tvram_read(bool c, uint psm, uint bp, uint bw, uint x, uint y) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: return tld(c, addr32(bp, bw, x, y, T_BLK32) * 4);
    case PSMZ32: case PSMZ24: return tld(c, addr32(bp, bw, x, y, T_BLKZ32) * 4);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
        uint blk = psm == PSMCT16 ? T_BLK16 : psm == PSMCT16S ? T_BLK16S : psm == PSMZ16 ? T_BLKZ16 : T_BLKZ16S;
        uint h = addr16(bp, bw, x, y, blk); uint w = tld(c, (h >> 1) * 4);
        return (h & 1) ? (w >> 16) : (w & 0xFFFF); }
    case PSMT8: { uint b = addr8(bp, bw, x, y); return (tld(c, (b >> 2) * 4) >> ((b & 3) * 8)) & 255; }
    case PSMT4: { uint n = addr4(bp, bw, x, y); return (tld(c, (n >> 3) * 4) >> ((n & 7) * 4)) & 15; }
    }
    return 0;
}

// ---------------------------------------------------------------- texturing
uint c16_to_32(GState s, uint v) {
    uint r = (v & 0x1F) << 3, g = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
    uint aem = (s.texa >> 8) & 1, ta0 = s.texa & 0xFF, ta1 = (s.texa >> 16) & 0xFF;
    uint a = (v & 0x8000) ? ta1 : ((aem && !(v & 0x7FFF)) ? 0 : ta0);
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint tex_texel(GState s, int u, int v) {
    uint tbp = s.tbp, tbw = s.tbw, psm = s.tpsm;
    uint aem = (s.texa >> 8) & 1, ta0 = s.texa & 0xFF;
    bool cp = (s.flags & GSF_TEXCOPY) != 0 && dbg_mode != 3;   // PS2_GPU_DBG=3: sample live VRAM (diagnostic)
    switch (psm) {
    case PSMCT32: case PSMZ32: return tvram_read(cp, psm, tbp, tbw, u, v);
    case PSMCT24: case PSMZ24: { uint c = tvram_read(cp, psm, tbp, tbw, u, v) & 0xFFFFFF; return c | ((aem && !c) ? 0 : ta0 << 24); }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return c16_to_32(s, tvram_read(cp, psm, tbp, tbw, u, v));
    case PSMT8: case PSMT8H: case PSMT4: case PSMT4HL: case PSMT4HH: {
        uint idx;
        if (psm == PSMT8 || psm == PSMT4) idx = tvram_read(cp, psm, tbp, tbw, u, v);
        else { uint w = tvram_read(cp, PSMCT32, tbp, tbw, u, v); idx = psm == PSMT8H ? w >> 24 : psm == PSMT4HL ? (w >> 24) & 15 : w >> 28; }
        bool is4 = psm != PSMT8 && psm != PSMT8H;
        if (s.flags & GSF_CLUTVRAM) {   // 256-entry CT32 CLUT (CSM1 = csm 0, 16x16 swizzled) read straight from VRAM at s.clut
            uint j = (idx & 0xE7) | ((idx & 0x08) << 1) | ((idx & 0x10) >> 1);
            return rd32(addr32(s.clut, 1, j & 15, j >> 4, T_BLK32));
        }
        uint cb = b_clut + s.clut * 768;
        if (s.cpsm == PSMCT32 || s.cpsm == PSMCT24) return cluts[cb + (is4 ? (s.csa & 15) * 16 : 0) + idx];
        return c16_to_32(s, cluts[cb + 256 + (is4 ? s.csa * 16 : 0) + idx]);
    }
    }
    return 0xFFFF00FFu;
}

int wrap_coord(int c, int size, uint wm, uint mn, uint mx) {
    switch (wm) {
    case 0: return c & (size - 1);
    case 1: return c < 0 ? 0 : c >= size ? size - 1 : c;
    case 2: return c < (int)mn ? (int)mn : c > (int)mx ? (int)mx : c;
    default: return (int)(((uint)c & mn) | mx);
    }
}

uint tex_sample(GState s, float fu, float fv) {
    if (!(s.flags & GSF_BILINEAR)) {
        int iu = (int)floor(fu), iv = (int)floor(fv);
        return tex_texel(s, wrap_coord(iu, s.tw, s.wms, s.minu, s.maxu), wrap_coord(iv, s.th, s.wmt, s.minv, s.maxv));
    }
    fu -= 0.5f; fv -= 0.5f;
    int u0 = (int)floor(fu), v0 = (int)floor(fv);
    float au = fu - u0, av = fv - v0;
    int ua = wrap_coord(u0, s.tw, s.wms, s.minu, s.maxu), ub = wrap_coord(u0 + 1, s.tw, s.wms, s.minu, s.maxu);
    int va = wrap_coord(v0, s.th, s.wmt, s.minv, s.maxv), vb = wrap_coord(v0 + 1, s.th, s.wmt, s.minv, s.maxv);
    uint c0 = tex_texel(s, ua, va), c1 = tex_texel(s, ub, va), c2 = tex_texel(s, ua, vb), c3 = tex_texel(s, ub, vb);
    if (c0 == c1 && c0 == c2 && c0 == c3) return c0;
    uint fu8 = min((uint)(au * 256.0f), 256u), fv8 = min((uint)(av * 256.0f), 256u);
    uint rb0 = c0 & 0x00FF00FFu, rb1 = c1 & 0x00FF00FFu, rb2 = c2 & 0x00FF00FFu, rb3 = c3 & 0x00FF00FFu;
    uint ag0 = (c0 >> 8) & 0x00FF00FFu, ag1 = (c1 >> 8) & 0x00FF00FFu, ag2 = (c2 >> 8) & 0x00FF00FFu, ag3 = (c3 >> 8) & 0x00FF00FFu;
    uint rbt = ((rb0 * (256 - fu8) + rb1 * fu8) >> 8) & 0x00FF00FFu, rbb = ((rb2 * (256 - fu8) + rb3 * fu8) >> 8) & 0x00FF00FFu;
    uint agt = ((ag0 * (256 - fu8) + ag1 * fu8) >> 8) & 0x00FF00FFu, agb = ((ag2 * (256 - fu8) + ag3 * fu8) >> 8) & 0x00FF00FFu;
    uint rb = ((rbt * (256 - fv8) + rbb * fv8 + 0x00800080u) >> 8) & 0x00FF00FFu;
    uint ag = ((agt * (256 - fv8) + agb * fv8 + 0x00800080u) >> 8) & 0x00FF00FFu;
    return rb | (ag << 8);
}

int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// ---------------------------------------------------------------- pixel pipeline (put_pixel_slow in gs.c)
void put_pixel(GState s, int x, int y, uint z, int r, int g, int b, int a) {
    uint test = s.test;
    bool zwrite = !s.zmsk, fwrite = true, awrite = true;
    if (test & 1) {
        uint atst = (test >> 1) & 7; int aref = (int)((test >> 4) & 0xFF);
        bool apass;
        switch (atst) {
        case 0: apass = false; break; case 1: apass = true; break;
        case 2: apass = a < aref; break; case 3: apass = a <= aref; break;
        case 4: apass = a == aref; break; case 5: apass = a >= aref; break;
        case 6: apass = a > aref; break; default: apass = a != aref; break;
        }
        if (!apass) {
            uint af = (test >> 12) & 3;
            if (af == 0) return;
            if (af == 1) zwrite = false;
            else if (af == 2) fwrite = false;
            else { zwrite = false; awrite = false; }
        }
    }
    uint zmax = s.zpsm == PSMZ32 ? 0xFFFFFFFFu : s.zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
    if (z > zmax) z = zmax;
    if ((test >> 16) & 1) {
        uint zt = (test >> 17) & 3;
        if (zt == 0) return;
        if (zt >= 2) {
            uint zd = vram_read(s.zpsm, s.zbp, s.fbw, x, y) & zmax;
            if (zt == 2 ? z < zd : z <= zd) return;
        }
    } else zwrite = false;
    bool f16 = s.fpsm == PSMCT16 || s.fpsm == PSMCT16S;
    if ((test >> 14) & 1) {
        uint dv = vram_read(s.fpsm, s.fbp, s.fbw, x, y);
        uint dbit = f16 ? (dv >> 15) & 1 : (dv >> 31) & 1;
        if (dbit != ((test >> 15) & 1)) return;
    }
    if (fwrite) {
        uint dv = vram_read(s.fpsm, s.fbp, s.fbw, x, y);
        int dr, dg, db, da;
        if (f16) { dr = (dv & 0x1F) << 3; dg = ((dv >> 5) & 0x1F) << 3; db = ((dv >> 10) & 0x1F) << 3; da = (dv & 0x8000) ? 0x80 : 0; }
        else { dr = dv & 255; dg = (dv >> 8) & 255; db = (dv >> 16) & 255; da = s.fpsm == PSMCT24 ? 0x80 : (int)(dv >> 24); }
        bool pabe_skip = (s.flags & GSF_PABE) && !(a & 0x80);
        if ((s.flags & GSF_ABE) && !pabe_skip) {
            uint A = s.alpha & 3, B = (s.alpha >> 2) & 3, C = (s.alpha >> 4) & 3, D = (s.alpha >> 6) & 3;
            int cc = C == 0 ? a : C == 1 ? da : (int)s.afix;
            int cs[3] = {r, g, b}, cd[3] = {dr, dg, db}, o[3];
            [unroll] for (int i = 0; i < 3; i++) {
                int va = A == 0 ? cs[i] : A == 1 ? cd[i] : 0;
                int vb = B == 0 ? cs[i] : B == 1 ? cd[i] : 0;
                int vd = D == 0 ? cs[i] : D == 1 ? cd[i] : 0;
                o[i] = (((va - vb) * cc) >> 7) + vd;
            }
            r = o[0]; g = o[1]; b = o[2];
        }
        if (s.flags & GSF_COLCLAMP) { r = clamp255(r); g = clamp255(g); b = clamp255(b); }
        else { r &= 255; g &= 255; b &= 255; }
        if (s.flags & GSF_FBA) a |= 0x80;
        if (!awrite) a = da;
        uint nv;
        if (f16) nv = ((uint)r >> 3) | (((uint)g >> 3) << 5) | (((uint)b >> 3) << 10) | ((a & 0x80) ? 0x8000 : 0);
        else nv = (uint)r | ((uint)g << 8) | ((uint)b << 16) | ((uint)(a & 255) << 24);
        uint msk = s.fbmsk;
        if (f16) msk = ((msk >> 3) & 0x1F) | ((msk >> 6) & 0x3E0) | ((msk >> 9) & 0x7C00) | ((msk >> 16) & 0x8000);
        nv = (nv & ~msk) | (dv & msk);
        vram_write(s.fpsm, s.fbp, s.fbw, x, y, nv);
    }
    if (zwrite) vram_write(s.zpsm, s.zbp, s.fbw, x, y, z);
}

// shade(): early Z, texture, TFX, then put_pixel
void shade(GState s, int x, int y, uint z, float r, float g, float b, float a, float ss, float tt, float q, float u, float v) {
    uint test = s.test;
    if (((test >> 16) & 1) && ((test >> 17) & 3) >= 2 && (s.zpsm == PSMZ32 || s.zpsm == PSMZ24)) {
        uint zmax = s.zpsm == PSMZ32 ? 0xFFFFFFFFu : 0xFFFFFFu, zz = min(z, zmax);
        uint zd = rd32(addr32(s.zbp, s.fbw, x, y, T_BLKZ32)) & zmax;
        if (((test >> 17) & 3) == 2 ? zz < zd : zz <= zd) return;
    }
    int R = (int)r, G = (int)g, B = (int)b, A = (int)a;
    if (s.flags & GSF_TME) {
        float fu, fv;
        if (s.flags & GSF_FST) { fu = u; fv = v; }
        else { float iq = q != 0.0f ? 1.0f / q : 0.0f; fu = ss * iq * (float)s.tw; fv = tt * iq * (float)s.th; }
        uint tc = tex_sample(s, fu, fv);
        int tr = tc & 255, tg = (tc >> 8) & 255, tb = (tc >> 16) & 255, ta = tc >> 24;
        bool tcc = (s.flags & GSF_TCC) != 0;
        switch (s.tfx) {
        case 0: R = (tr * R) >> 7; G = (tg * G) >> 7; B = (tb * B) >> 7; A = tcc ? (ta * A) >> 7 : A; break;
        case 1: R = tr; G = tg; B = tb; A = tcc ? ta : A; break;
        case 2: R = ((tr * R) >> 7) + A; G = ((tg * G) >> 7) + A; B = ((tb * B) >> 7) + A; A = tcc ? ta + A : A; break;
        default: R = ((tr * R) >> 7) + A; G = ((tg * G) >> 7) + A; B = ((tb * B) >> 7) + A; A = tcc ? ta : A; break;
        }
        R = clamp255(R); G = clamp255(G); B = clamp255(B); A = clamp255(A);
    }
    put_pixel(s, x, y, z, R, G, B, A);
}

// ---------------------------------------------------------------- scatter: masked word writes from transfers
// (CPU writes into pages whose newest copy is on the GPU, applied here instead of reading the page back)
StructuredBuffer<uint4> scat : register(t7);   // (word index, value, mask, 0)
[numthreads(64, 1, 1)]
void scatter(uint3 id : SV_DispatchThreadID) {
    if (id.x >= n_scat) return;
    uint4 e = scat[b_scat + id.x];
    if (e.z == 0) return;
    uint o;
    vram.InterlockedAnd(e.x * 4, ~e.z, o);
    vram.InterlockedOr(e.x * 4, e.y & e.z, o);
}

#define CHUNK 32
groupshared uint gP[CHUNK * PRIM_WORDS];
groupshared uint gS[CHUNK * STATE_WORDS];

GState ld_state(uint o) {
    GState s;
    s.fbp = gS[o + 0]; s.fbw = gS[o + 1]; s.fpsm = gS[o + 2]; s.fbmsk = gS[o + 3];
    s.zbp = gS[o + 4]; s.zpsm = gS[o + 5]; s.zmsk = gS[o + 6]; s.test = gS[o + 7];
    s.sx0 = (int)gS[o + 8]; s.sx1 = (int)gS[o + 9]; s.sy0 = (int)gS[o + 10]; s.sy1 = (int)gS[o + 11];
    s.flags = gS[o + 12]; s.alpha = gS[o + 13]; s.afix = gS[o + 14];
    s.tbp = gS[o + 15]; s.tbw = gS[o + 16]; s.tpsm = gS[o + 17]; s.tw = gS[o + 18]; s.th = gS[o + 19]; s.tfx = gS[o + 20];
    s.wms = gS[o + 21]; s.wmt = gS[o + 22]; s.minu = gS[o + 23]; s.maxu = gS[o + 24]; s.minv = gS[o + 25]; s.maxv = gS[o + 26];
    s.cpsm = gS[o + 27]; s.csa = gS[o + 28]; s.texa = gS[o + 29]; s.clut = gS[o + 30];
    return s;
}

// vertex fields of the staged primitive at gP[pb]
#define VW(k, f) gP[pb + 4 + (k) * 12 + (f)]
#define VX(k) asfloat(VW(k, 0))
#define VY(k) asfloat(VW(k, 1))
#define VZ(k) VW(k, 2)
#define VR(k) asfloat(VW(k, 3))
#define VG(k) asfloat(VW(k, 4))
#define VB(k) asfloat(VW(k, 5))
#define VA(k) asfloat(VW(k, 6))
#define VS(k) asfloat(VW(k, 7))
#define VT(k) asfloat(VW(k, 8))
#define VQ(k) asfloat(VW(k, 9))
#define VU(k) asfloat(VW(k, 10))
#define VV(k) asfloat(VW(k, 11))

[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID, uint li : SV_GroupIndex) {
    uint4 t = tiles[b_tile + gid.x];
    int px = (int)((t.x & 127) * 16 + lid.x), py = (int)((t.x >> 7) * 16 + lid.y);
    float fx = (float)px, fy = (float)py;
    for (uint c0 = 0; c0 < t.z; c0 += CHUNK) {
        uint n = min((uint)CHUNK, t.z - c0);
        GroupMemoryBarrierWithGroupSync();   // the previous chunk is no longer read
        for (uint i = li; i < n * PRIM_WORDS; i += 256) {
            uint j = i / PRIM_WORDS;
            gP[i] = primw[b_prim + binidx[b_idx + t.y + c0 + j] * PRIM_WORDS + (i - j * PRIM_WORDS)];
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint i2 = li; i2 < n * STATE_WORDS; i2 += 256) {
            uint j = i2 / STATE_WORDS;
            gS[i2] = statew[b_state + gP[j * PRIM_WORDS + 1] * STATE_WORDS + (i2 - j * STATE_WORDS)];
        }
        GroupMemoryBarrierWithGroupSync();
    for (uint k = 0; k < n; k++) {
        uint pb = k * PRIM_WORDS;
        uint bb0 = gP[pb + 2], bb1 = gP[pb + 3];   // pixel bbox within the scissor
        if (px < (int)(bb0 & 0xFFFF) || px > (int)(bb1 & 0xFFFF) || py < (int)(bb0 >> 16) || py > (int)(bb1 >> 16)) continue;
        uint ptype = gP[pb];
        GState s = ld_state(k * STATE_WORDS);
        // coverage + fragment inputs per primitive type, then one shade() call (keeps the shader small)
        bool hit = false;
        uint z = 0; float r = 0, g = 0, b = 0, a = 0, ss = 0, tt = 0, q = 0, u = 0, v = 0;
        if (ptype == 3) {                                      // triangle (draw_triangle)
            float area = (VX(1) - VX(0)) * (VY(2) - VY(0)) - (VX(2) - VX(0)) * (VY(1) - VY(0));
            float minx = min(VX(0), min(VX(1), VX(2))), maxx = max(VX(0), max(VX(1), VX(2)));
            float miny = min(VY(0), min(VY(1), VY(2))), maxy = max(VY(0), max(VY(1), VY(2)));
            if (area != 0.0f && px >= (int)ceil(minx) && px <= (int)ceil(maxx) - 1 && py >= (int)ceil(miny) && py <= (int)ceil(maxy) - 1) {
                float ia = 1.0f / area;
                float w0 = ((VX(1) - fx) * (VY(2) - fy) - (VX(2) - fx) * (VY(1) - fy)) * ia;
                float w1 = ((VX(2) - fx) * (VY(0) - fy) - (VX(0) - fx) * (VY(2) - fy)) * ia;
                float w2 = 1.0f - w0 - w1;
                if (w0 >= -1e-6f && w1 >= -1e-6f && w2 >= -1e-6f) {
                    hit = true;
                    z = (uint)(w0 * (float)VZ(0) + w1 * (float)VZ(1) + w2 * (float)VZ(2));
                    if (s.flags & GSF_IIP) {
                        r = w0 * VR(0) + w1 * VR(1) + w2 * VR(2); g = w0 * VG(0) + w1 * VG(1) + w2 * VG(2);
                        b = w0 * VB(0) + w1 * VB(1) + w2 * VB(2); a = w0 * VA(0) + w1 * VA(1) + w2 * VA(2);
                    } else { r = VR(2); g = VG(2); b = VB(2); a = VA(2); }
                    ss = w0 * VS(0) + w1 * VS(1) + w2 * VS(2); tt = w0 * VT(0) + w1 * VT(1) + w2 * VT(2);
                    q = w0 * VQ(0) + w1 * VQ(1) + w2 * VQ(2);
                    u = w0 * VU(0) + w1 * VU(1) + w2 * VU(2); v = w0 * VV(0) + w1 * VV(1) + w2 * VV(2);
                }
            }
        } else if (ptype == 6) {                               // sprite (draw_sprite)
            float x0 = VX(0), x1 = VX(1), y0 = VY(0), y1 = VY(1);
            float s0 = VS(0), s1 = VS(1), t0 = VT(0), t1 = VT(1), u0 = VU(0), u1 = VU(1), w0 = VV(0), w1 = VV(1);
            if (x0 > x1) { float sw; sw = x0; x0 = x1; x1 = sw; sw = s0; s0 = s1; s1 = sw; sw = u0; u0 = u1; u1 = sw; }
            if (y0 > y1) { float sw; sw = y0; y0 = y1; y1 = sw; sw = t0; t0 = t1; t1 = sw; sw = w0; w0 = w1; w1 = sw; }
            int ix0 = (int)ceil(x0), ix1 = (int)ceil(x1), iy0 = (int)ceil(y0), iy1 = (int)ceil(y1);
            float dx = x1 - x0, dy = y1 - y0;
            if (px >= ix0 && px < ix1 && py >= iy0 && py < iy1 && dx > 0 && dy > 0) {
                float ds = (s1 - s0) / dx, du = (u1 - u0) / dx, dt = (t1 - t0) / dy, dw = (w1 - w0) / dy;
                hit = true;
                z = VZ(1); r = VR(1); g = VG(1); b = VB(1); a = VA(1); q = VQ(1);
                ss = s0 + ds * (ix0 - x0) + ds * (px - ix0); u = u0 + du * (ix0 - x0) + du * (px - ix0);
                tt = t0 + dt * (fy - y0); v = w0 + dw * (fy - y0);
            }
        } else if (ptype == 0) {                               // point
            if (px == (int)VX(0) && py == (int)VY(0)) {
                hit = true; z = VZ(0); r = VR(0); g = VG(0); b = VB(0); a = VA(0);
                ss = VS(0); tt = VT(0); q = VQ(0); u = VU(0); v = VV(0);
            }
        } else if (ptype == 1) {                               // line (draw_line: DDA steps; first step on this pixel)
            float dx = VX(1) - VX(0), dy = VY(1) - VY(0);
            int n = (int)max(abs(dx), abs(dy)); if (n <= 0) n = 1;
            bool iip = (s.flags & GSF_IIP) != 0;
            for (int i = 0; i < n && !hit; i++) {
                float f = (float)i / n;
                if ((int)(VX(0) + dx * f) == px && (int)(VY(0) + dy * f) == py) {
                    hit = true;
                    z = (uint)((float)VZ(0) + ((float)VZ(1) - (float)VZ(0)) * f);
                    r = iip ? VR(0) + (VR(1) - VR(0)) * f : VR(1); g = iip ? VG(0) + (VG(1) - VG(0)) * f : VG(1);
                    b = iip ? VB(0) + (VB(1) - VB(0)) * f : VB(1); a = iip ? VA(0) + (VA(1) - VA(0)) * f : VA(1);
                    ss = VS(0) + (VS(1) - VS(0)) * f; tt = VT(0) + (VT(1) - VT(0)) * f; q = VQ(0) + (VQ(1) - VQ(0)) * f;
                    u = VU(0) + (VU(1) - VU(0)) * f; v = VV(0) + (VV(1) - VV(0)) * f;
                }
            }
        }
        if (dbg_mode == 1) continue;                            // PS2_GPU_DBG=1: coverage only
        if (dbg_mode == 2) s.flags &= ~GSF_TME;                 // PS2_GPU_DBG=2: no texturing
        if (hit) shade(s, px, py, z, r, g, b, a, ss, tt, q, u, v);
    }
    }
}
