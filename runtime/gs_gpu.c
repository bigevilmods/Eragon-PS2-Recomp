/* Direct3D 11 compute back end for the GS rasterizer: see gs_gpu.h. Used only from the GS thread. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include "gs_gpu.h"
#include "gs_gpu_hlsl.h"   /* g_gs_hlsl (generated from gs_gpu.hlsl) */

extern u8 *gs_vram_ptr(void);
extern void gs_fill_swizzle_tables(u32 *out, int n);

#define VRAM_BYTES (4u << 20)
#define PAGE_BYTES 8192u
#define NPAGES 512

static int g_state = -1;                 /* -1 untried, 0 off, 1 on */
u64 g_gpu_downloads;          /* readbacks that had to wait for the GPU */
double g_gpu_wait_s[4];       /* time blocked: 0 download, 1 dynamic buffer Map, 2 upload, 3 dispatch */
static double qnow(void) { static double f; LARGE_INTEGER t; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = 1.0 / (double)q.QuadPart; } QueryPerformanceCounter(&t); return (double)t.QuadPart * f; }
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11Buffer *g_vram_buf, *g_staging, *g_vram_copy;
static ID3D11UnorderedAccessView *g_vram_uav;
static ID3D11ShaderResourceView *g_copy_srv;
static ID3D11ComputeShader *g_cs, *g_cs_scatter;
static ID3D11Buffer *g_tbl_buf; static ID3D11ShaderResourceView *g_tbl_srv;
static u8 g_cpu_newer[NPAGES], g_gpu_newer[NPAGES];

static u32 *g_scat; static u32 g_nscat, g_scat_cap;   /* pending scatter writes (word, value, mask, 0) */

static int fail(const char *what, HRESULT hr) {
    fprintf(stderr, "[gpu] %s failed (0x%08lx); using the software rasterizer\n", what, (unsigned long)hr);
    g_state = 0;
    return 0;
}

/* Ring buffers: each submission appends with MAP_WRITE_NO_OVERWRITE and the shader reads from the base
 * offsets passed in a constant buffer, so the driver does not have to rename a buffer per dispatch
 * (thousands of WRITE_DISCARD maps per second stall). The ring is discarded only when it wraps. */
typedef struct { ID3D11Buffer *buf; ID3D11ShaderResourceView *srv; u32 cap, stride, pos; } Ring;
static Ring g_r_states, g_r_prims, g_r_tiles, g_r_idx, g_r_clut, g_r_scat;
static int g_no_overwrite;   /* MapNoOverwriteOnDynamicBufferSRV */
static ID3D11Buffer *g_cbuf;  /* bases: state, prim, tile, idx, clut, scatter, scatter count, 0 */

static int ring_write(Ring *b, u32 stride, u32 min_cap, const void *data, u32 count, u32 *base) {
    if (!count) { *base = 0; return 1; }
    if (!b->buf || b->cap < count || b->stride != stride) {
        if (b->srv) ID3D11ShaderResourceView_Release(b->srv);
        if (b->buf) ID3D11Buffer_Release(b->buf);
        u32 cap = count * 4 > min_cap ? count * 4 : min_cap;
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = cap * stride; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = stride;
        HRESULT hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &b->buf);
        if (FAILED(hr)) return fail("CreateBuffer(ring)", hr);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = DXGI_FORMAT_UNKNOWN; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sd.Buffer.NumElements = cap;
        hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)b->buf, &sd, &b->srv);
        if (FAILED(hr)) return fail("CreateShaderResourceView(ring)", hr);
        b->cap = cap; b->stride = stride; b->pos = cap;   /* forces a DISCARD map first */
    }
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!g_no_overwrite || b->pos + count > b->cap) { mode = D3D11_MAP_WRITE_DISCARD; b->pos = 0; }
    D3D11_MAPPED_SUBRESOURCE m;
    double t0 = qnow();
    HRESULT hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b->buf, 0, mode, 0, &m);
    g_gpu_wait_s[1] += qnow() - t0;
    if (FAILED(hr)) return fail("Map(ring)", hr);
    memcpy((u8 *)m.pData + (size_t)b->pos * stride, data, (size_t)count * stride);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)b->buf, 0);
    *base = b->pos; b->pos += count;
    return 1;
}

static void set_bases(u32 sb, u32 pb, u32 tb, u32 ib, u32 cb, u32 scb, u32 scn) {
    static int dbg = -1; if (dbg < 0) { const char *e = getenv("PS2_GPU_DBG"); dbg = e ? atoi(e) : 0; }
    u32 c[8] = {sb, pb, tb, ib, cb, scb, scn, (u32)dbg};
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_cbuf, 0, NULL, c, 0, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(g_ctx, 0, 1, &g_cbuf);
}

/* compile (or load from the on-disk cache, keyed by source hash + entry) one compute shader entry */
static int make_cs(const char *entry, ID3D11ComputeShader **out) {
    u64 sh = 1469598103934665603ull;
    for (const char *s = g_gs_hlsl; *s; s++) sh = (sh ^ (u8)*s) * 1099511628211ull;
    char cache[96]; snprintf(cache, sizeof cache, "gs_gpu_%016llx_%s.cso", (unsigned long long)sh, entry);
    void *bytes = NULL; size_t nbytes = 0;
    FILE *cf_ = fopen(cache, "rb");
    if (cf_) {
        fseek(cf_, 0, SEEK_END); nbytes = (size_t)ftell(cf_); fseek(cf_, 0, SEEK_SET);
        bytes = malloc(nbytes);
        if (fread(bytes, 1, nbytes, cf_) != nbytes) { free(bytes); bytes = NULL; }
        fclose(cf_);
    }
    if (!bytes) {
        ID3DBlob *code = NULL, *err = NULL;
        LARGE_INTEGER c0, c1, cf; QueryPerformanceCounter(&c0);
        HRESULT hr = D3DCompile(g_gs_hlsl, strlen(g_gs_hlsl), "gs_gpu.hlsl", NULL, NULL, entry, "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
        QueryPerformanceCounter(&c1); QueryPerformanceFrequency(&cf);
        fprintf(stderr, "[gpu] shader '%s' compiled in %.1f s\n", entry, (double)(c1.QuadPart - c0.QuadPart) / cf.QuadPart);
        if (FAILED(hr)) {
            if (err) fprintf(stderr, "[gpu] shader errors:\n%s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
            return fail("D3DCompile", hr);
        }
        nbytes = ID3D10Blob_GetBufferSize(code);
        bytes = malloc(nbytes); memcpy(bytes, ID3D10Blob_GetBufferPointer(code), nbytes);
        ID3D10Blob_Release(code);
        FILE *wf = fopen(cache, "wb"); if (wf) { fwrite(bytes, 1, nbytes, wf); fclose(wf); }
    }
    HRESULT hr = ID3D11Device_CreateComputeShader(g_dev, bytes, nbytes, NULL, out);
    free(bytes);
    if (FAILED(hr)) return fail("CreateComputeShader", hr);
    return 1;
}

static int init(void) {
    const char *e = getenv("PS2_GPU");
    if (e && *e == '0') { g_state = 0; return 0; }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);
    /* VRAM: raw UAV buffer + staging copy for downloads */
    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = VRAM_BYTES; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA init_data = {gs_vram_ptr(), 0, 0};
    hr = ID3D11Device_CreateBuffer(g_dev, &bd, &init_data, &g_vram_buf);
    if (FAILED(hr)) return fail("CreateBuffer(vram)", hr);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {0};
    ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = VRAM_BYTES / 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    hr = ID3D11Device_CreateUnorderedAccessView(g_dev, (ID3D11Resource *)g_vram_buf, &ud, &g_vram_uav);
    if (FAILED(hr)) return fail("CreateUnorderedAccessView", hr);
    /* copy of VRAM for draws that sample their own render target (read as before the dispatch) */
    D3D11_BUFFER_DESC cb = {0};
    cb.ByteWidth = VRAM_BYTES; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    hr = ID3D11Device_CreateBuffer(g_dev, &cb, NULL, &g_vram_copy);
    if (FAILED(hr)) return fail("CreateBuffer(vram copy)", hr);
    D3D11_SHADER_RESOURCE_VIEW_DESC cs = {0};
    cs.Format = DXGI_FORMAT_R32_TYPELESS; cs.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    cs.BufferEx.NumElements = VRAM_BYTES / 4; cs.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_vram_copy, &cs, &g_copy_srv);
    if (FAILED(hr)) return fail("CreateShaderResourceView(vram copy)", hr);
    D3D11_BUFFER_DESC sb = {0};
    sb.ByteWidth = VRAM_BYTES; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateBuffer(g_dev, &sb, NULL, &g_staging);
    if (FAILED(hr)) return fail("CreateBuffer(staging)", hr);
    /* swizzle tables */
    u32 tbl[1216]; gs_fill_swizzle_tables(tbl, 1216);
    D3D11_BUFFER_DESC tb = {0};
    tb.ByteWidth = sizeof tbl; tb.Usage = D3D11_USAGE_IMMUTABLE; tb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; tb.StructureByteStride = 4;
    D3D11_SUBRESOURCE_DATA td = {tbl, 0, 0};
    hr = ID3D11Device_CreateBuffer(g_dev, &tb, &td, &g_tbl_buf);
    if (FAILED(hr)) return fail("CreateBuffer(tables)", hr);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_UNKNOWN; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sd.Buffer.NumElements = 1216;
    hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_tbl_buf, &sd, &g_tbl_srv);
    if (FAILED(hr)) return fail("CreateShaderResourceView(tables)", hr);
    if (!make_cs("main", &g_cs) || !make_cs("scatter", &g_cs_scatter)) return 0;
    D3D11_BUFFER_DESC cbd = {0};
    cbd.ByteWidth = 32; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(g_dev, &cbd, NULL, &g_cbuf);
    if (FAILED(hr)) return fail("CreateBuffer(constants)", hr);
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt = {0};
    if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof opt)))
        g_no_overwrite = opt.MapNoOverwriteOnDynamicBufferSRV;
    if (getenv("PS2_GPU_DISCARD")) g_no_overwrite = 0;
    fprintf(stderr, "[gpu] ring buffers %s\n", g_no_overwrite ? "with NO_OVERWRITE" : "with DISCARD only");
    IDXGIDevice *dxgi = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxgi)) && SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &ad))
        && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
        fprintf(stderr, "[gpu] GS rasterizer on %ls\n", desc.Description);
    if (ad) IDXGIAdapter_Release(ad);
    if (dxgi) IDXGIDevice_Release(dxgi);
    g_state = 1;
    return 1;
}

int gsgpu_enabled(void) {
    if (g_state < 0) init();
    return g_state == 1;
}

/* ---- coherence */
static void page_range(u32 block_lo, u32 block_hi, u32 *p0, u32 *p1) {
    *p0 = block_lo / 32; *p1 = (block_hi + 31) / 32;
    if (*p0 > NPAGES) *p0 = NPAGES;
    if (*p1 > NPAGES) *p1 = NPAGES;
}

void gsgpu_scatter_flush(void);
static void download_pages(u32 p0, u32 p1) {
    int any = 0;
    for (u32 p = p0; p < p1 && !any; p++) any = g_gpu_newer[p];
    if (!any) return;
    any = 0;
    gsgpu_scatter_flush();   /* pending scattered writes are part of the GPU copy */
    for (u32 p = p0; p < p1; p++) {
        if (!g_gpu_newer[p]) continue;
        u32 q = p; while (q < p1 && g_gpu_newer[q]) q++;
        D3D11_BOX box = {p * PAGE_BYTES, 0, 0, q * PAGE_BYTES, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_staging, 0, p * PAGE_BYTES, 0, 0, (ID3D11Resource *)g_vram_buf, 0, &box);
        any = 1; p = q - 1;
    }
    if (!any) return;
    D3D11_MAPPED_SUBRESOURCE m;
    double t0 = qnow();
    HRESULT hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &m);
    g_gpu_wait_s[0] += qnow() - t0; g_gpu_downloads++;
    if (FAILED(hr)) return;
    u8 *vram = gs_vram_ptr();
    for (u32 p = p0; p < p1; p++)
        if (g_gpu_newer[p]) { memcpy(vram + p * PAGE_BYTES, (u8 *)m.pData + p * PAGE_BYTES, PAGE_BYTES); g_gpu_newer[p] = 0; }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_staging, 0);
}

void gsgpu_sync_cpu(u32 block_lo, u32 block_hi) {
    if (g_state != 1) return;
    u32 p0, p1; page_range(block_lo, block_hi, &p0, &p1);
    download_pages(p0, p1);
}
void gsgpu_sync_cpu_all(void) { if (g_state == 1) download_pages(0, NPAGES); }

void gsgpu_cpu_wrote(u32 block_lo, u32 block_hi) {
    if (g_state != 1) return;
    u32 p0, p1; page_range(block_lo, block_hi, &p0, &p1);
    download_pages(p0, p1);   /* partial page writes must not lose GPU data (callers sync first anyway) */
    for (u32 p = p0; p < p1; p++) g_cpu_newer[p] = 1;
}

/* ---- scatter writes: CPU-side writes into GPU-newest pages, applied on the GPU (no readback) */
int gsgpu_page_gpu_newer(u32 page) { return g_state == 1 && page < NPAGES && g_gpu_newer[page]; }

void gsgpu_scatter(u32 word, u32 value, u32 mask) {
    if (g_nscat == g_scat_cap) { g_scat_cap = g_scat_cap ? g_scat_cap * 2 : 4096; g_scat = realloc(g_scat, (size_t)g_scat_cap * 16); }
    u32 *e = g_scat + g_nscat++ * 4;
    e[0] = word; e[1] = value; e[2] = mask; e[3] = 0;
}

/* ---- GPU timing (PS2_GPUTIME=1): timestamp pairs per operation, grouped in disjoint-query sets that are
 * read back a few sets later (never waiting). g_gpu_ms[cat] accumulates GPU milliseconds per category:
 * 0 texture snapshots, 1 rasterizer dispatches, 2 scatter dispatches, 3 uploads, 4 whole span of each set. */
enum { TQ_COPY, TQ_MAIN, TQ_SCAT, TQ_UPLOAD, TQ_SPAN, TQ_NCAT };
double g_gpu_ms[TQ_NCAT]; u64 g_gpu_ms_ops[TQ_NCAT];
#define TQ_SETS 6
#define TQ_PER 256
typedef struct { ID3D11Query *disj, *ts[TQ_PER * 2]; u8 cat[TQ_PER]; int n, open, pending; } TSet;
static TSet g_tq[TQ_SETS]; static int g_tq_cur, g_tq_on = -1;
static void tq_collect(TSet *s) {
    if (!s->pending) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)s->disj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    s->pending = 0;
    if (dj.Disjoint || !dj.Frequency) return;
    UINT64 first = 0, last = 0;
    for (int i = 0; i < s->n; i++) {
        UINT64 a, b;
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)s->ts[2 * i], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)s->ts[2 * i + 1], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (b > a) { g_gpu_ms[s->cat[i]] += (double)(b - a) * 1000.0 / (double)dj.Frequency; g_gpu_ms_ops[s->cat[i]]++; }
        if (!i) first = a;
        last = b;
    }
    if (last > first) { g_gpu_ms[TQ_SPAN] += (double)(last - first) * 1000.0 / (double)dj.Frequency; g_gpu_ms_ops[TQ_SPAN]++; }
}
static int tq_begin(void) {   /* returns the slot to pass to tq_end, or -1 */
    if (g_tq_on < 0) g_tq_on = getenv("PS2_GPUTIME") != NULL;
    if (!g_tq_on) return -1;
    TSet *s = &g_tq[g_tq_cur];
    if (!s->disj) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Device_CreateQuery(g_dev, &qd, &s->disj);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (int i = 0; i < TQ_PER * 2; i++) ID3D11Device_CreateQuery(g_dev, &qd, &s->ts[i]);
    }
    if (!s->open) {
        tq_collect(s);
        if (s->pending) return -1;   /* results not back yet: skip timing this op */
        ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)s->disj);
        s->open = 1; s->n = 0;
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)s->ts[2 * s->n]);
    return s->n;
}
static void tq_end(int slot, int cat) {
    if (slot < 0) return;
    TSet *s = &g_tq[g_tq_cur];
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)s->ts[2 * slot + 1]);
    s->cat[slot] = (u8)cat;
    if (++s->n == TQ_PER) {   /* close the set; the next op starts the next one */
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)s->disj);
        s->open = 0; s->pending = 1;
        g_tq_cur = (g_tq_cur + 1) % TQ_SETS;
        for (int i = 0; i < TQ_SETS; i++) tq_collect(&g_tq[i]);
    }
}

void gsgpu_scatter_flush(void) {
    if (g_state != 1 || !g_nscat) return;
    u32 scb;
    if (!ring_write(&g_r_scat, 16, 65536, g_scat, g_nscat, &scb)) return;
    set_bases(0, 0, 0, 0, 0, scb, g_nscat);
    ID3D11DeviceContext_CSSetShader(g_ctx, g_cs_scatter, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(g_ctx, 7, 1, &g_r_scat.srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &g_vram_uav, NULL);
    int tq = tq_begin();
    ID3D11DeviceContext_Dispatch(g_ctx, (g_nscat + 63) / 64, 1, 1);
    tq_end(tq, TQ_SCAT);
    g_nscat = 0;
}

void gsgpu_cpu_overwrite_page(u32 page) {   /* the CPU rewrites this whole page: no need to fetch it first */
    if (g_state != 1 || page >= NPAGES) return;
    g_gpu_newer[page] = 0;
    g_cpu_newer[page] = 1;
}

void gsgpu_gpu_will_write(u32 block_lo, u32 block_hi) {
    if (g_state != 1) return;
    u32 p0, p1; page_range(block_lo, block_hi, &p0, &p1);
    for (u32 p = p0; p < p1; p++) g_gpu_newer[p] = 1;
}

static void upload_pages(void) {
    u8 *vram = gs_vram_ptr();
    for (u32 p = 0; p < NPAGES; p++) {
        if (!g_cpu_newer[p]) continue;
        u32 q = p; while (q < NPAGES && g_cpu_newer[q]) { g_cpu_newer[q] = 0; q++; }
        D3D11_BOX box = {p * PAGE_BYTES, 0, 0, q * PAGE_BYTES, 1, 1};
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_vram_buf, 0, &box, vram + p * PAGE_BYTES, 0, 0);
        p = q - 1;
    }
}

/* PS2_GPU_COHERCHECK=1: compare every page the tracking calls CPU-current with the GPU copy
 * (pages marked GPU-newer are skipped); reports pages that differ */
void gsgpu_check_coherence(int frame) {
    if (g_state != 1) return;
    gsgpu_scatter_flush();
    upload_pages();
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_staging, (ID3D11Resource *)g_vram_buf);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const u8 *vram = gs_vram_ptr();
    int bad = 0; char list[512]; int ll = 0; list[0] = 0;
    for (u32 p = 0; p < NPAGES; p++) {
        if (g_gpu_newer[p]) continue;
        if (memcmp(vram + p * PAGE_BYTES, (const u8 *)m.pData + p * PAGE_BYTES, PAGE_BYTES)) {
            u32 nd = 0; for (u32 w = 0; w < PAGE_BYTES / 4; w++) nd += ((const u32 *)(vram + p * PAGE_BYTES))[w] != ((const u32 *)((const u8 *)m.pData + p * PAGE_BYTES))[w];
            if (ll < 480) ll += snprintf(list + ll, sizeof list - ll, " %u(%u)", p, nd);
            bad++;
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_staging, 0);
    fprintf(stderr, "[cohere] f%d: %d stale CPU pages%s\n", frame, bad, list);
}

/* ---- asynchronous snapshots (display readout without waiting for the GPU) */
#define NSNAP 3
static ID3D11Buffer *g_snap[NSNAP];
int gsgpu_snap_begin(int slot, u32 block_lo, u32 block_hi) {
    if (g_state != 1 || slot < 0 || slot >= NSNAP) return 0;
    if (!g_snap[slot]) {
        D3D11_BUFFER_DESC sb = {0};
        sb.ByteWidth = VRAM_BYTES; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &sb, NULL, &g_snap[slot]))) return 0;
    }
    gsgpu_scatter_flush();
    upload_pages();   /* the GPU copy becomes the newest for every page */
    u32 lo = block_lo * 256, hi = block_hi * 256;
    if (hi > VRAM_BYTES) hi = VRAM_BYTES;
    if (lo >= hi) return 0;
    D3D11_BOX box = {lo, 0, 0, hi, 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_snap[slot], 0, lo, 0, 0, (ID3D11Resource *)g_vram_buf, 0, &box);
    ID3D11DeviceContext_Flush(g_ctx);
    return 1;
}
/* NULL while the copy is still in flight (unless wait) */
const u8 *gsgpu_snap_map(int slot, int wait) {
    D3D11_MAPPED_SUBRESOURCE m;
    double t0 = qnow();
    HRESULT hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_snap[slot], 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (wait) g_gpu_wait_s[0] += qnow() - t0;
    return SUCCEEDED(hr) ? (const u8 *)m.pData : NULL;
}
void gsgpu_snap_unmap(int slot) { ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_snap[slot], 0); }

/* ---- submission */
void gsgpu_submit(const GState *st, int nst, const GPrim *pr, int npr, const u32 *tiles, int ntiles,
                  const u32 *idx, int nidx, const u32 *cluts, int ncluts, const u32 *copy_ranges, int ncopy) {
    if (g_state != 1 || !ntiles) return;
    double t0 = qnow();
    gsgpu_scatter_flush();
    { int any = 0; for (u32 p = 0; p < NPAGES && !any; p++) any = g_cpu_newer[p];
      int tq = any ? tq_begin() : -1; upload_pages(); tq_end(tq, TQ_UPLOAD); }
    g_gpu_wait_s[2] += qnow() - t0;
    static int nocopy = -1; if (nocopy < 0) nocopy = getenv("PS2_GPU_NOCOPY") != NULL;   /* measurement only: wrong output */
    for (int i = 0; i < ncopy && !nocopy; i++) {   /* snapshot only the textures that self-overlapping draws sample */
        u32 lo = copy_ranges[i * 2] * 256, hi = copy_ranges[i * 2 + 1] * 256;
        if (hi > VRAM_BYTES) hi = VRAM_BYTES;
        if (lo >= hi) continue;
        { extern u64 g_copy_bytes; g_copy_bytes += hi - lo; }
        D3D11_BOX box = {lo, 0, 0, hi, 1, 1};
        int tq = tq_begin();
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_vram_copy, 0, lo, 0, 0, (ID3D11Resource *)g_vram_buf, 0, &box);
        tq_end(tq, TQ_COPY);
    }
    u32 sb, pb, tb, ib, cb = 0;
    /* states and primitives as flat word streams (the shader stages them in groupshared memory) */
    if (!ring_write(&g_r_states, 4, 16384 * (sizeof(GState) / 4), st, (u32)nst * (sizeof(GState) / 4), &sb)) return;
    if (!ring_write(&g_r_prims, 4, 65536 * (sizeof(GPrim) / 4), pr, (u32)npr * (sizeof(GPrim) / 4), &pb)) return;
    if (!ring_write(&g_r_tiles, 16, 65536, tiles, (u32)ntiles, &tb)) return;
    if (!ring_write(&g_r_idx, 4, 1u << 20, idx, (u32)nidx, &ib)) return;
    if (ncluts && !ring_write(&g_r_clut, 4, 768 * 1024, cluts, (u32)ncluts * 768, &cb)) return;
    if (!g_r_clut.buf) { u32 z[768] = {0}, d; if (!ring_write(&g_r_clut, 4, 768 * 1024, z, 768, &d)) return; }   /* bind something */
    set_bases(sb, pb, tb, ib, cb, 0, 0);
    ID3D11ShaderResourceView *srvs[7] = {g_r_states.srv, g_r_prims.srv, g_r_tiles.srv, g_r_idx.srv, g_r_clut.srv, g_tbl_srv, g_copy_srv};
    ID3D11DeviceContext_CSSetShader(g_ctx, g_cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(g_ctx, 0, 7, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &g_vram_uav, NULL);
    t0 = qnow();
    int tq = tq_begin();
    ID3D11DeviceContext_Dispatch(g_ctx, (UINT)ntiles, 1, 1);
    tq_end(tq, TQ_MAIN);
    g_gpu_wait_s[3] += qnow() - t0;
}
