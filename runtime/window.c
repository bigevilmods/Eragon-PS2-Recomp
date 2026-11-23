/* Win32 output window: the GS display is copied here at every vsync and drawn with StretchDIBits. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "gs.h"

static HWND g_hwnd;
static CRITICAL_SECTION g_cs;
static u32 *g_buf;
static int g_w = 640, g_h = 448, g_ready;

/* Debug overlay (F8 toggles, PS2_OVERLAY=0 starts hidden): Eragon's position, vsync and renderer.
 * ClEragonObj is found by its class info (0x38b408 at +8); the address is cached and re-validated. */
static int g_overlay = 1;
static int eragon_pos(float out[3]) {
    extern u8 *ps2_ram;
    static u32 obj; static DWORD last_scan;
    if (!ps2_ram) return 0;
    u32 cls = 0;
    if (obj) memcpy(&cls, ps2_ram + obj + 8, 4);
    if (cls != 0x38b408) {
        obj = 0;
        if (GetTickCount() - last_scan < 1000) return 0;   /* rescan at most once a second */
        last_scan = GetTickCount();
        for (u32 o = 0x400000; o + 0x900 < (32u << 20); o += 16) {
            memcpy(&cls, ps2_ram + o + 8, 4);
            if (cls == 0x38b408) { obj = o; break; }
        }
        if (!obj) return 0;
    }
    memcpy(out, ps2_ram + obj + 0x70, 12);
    return 1;
}
static void draw_overlay(HDC dc) {
    extern int g_frame_no_pub;
    extern volatile LONG64 g_gs_flips;
    extern volatile LONG g_gs_frame_max_us;
    const char *gs_renderer_name(void);
    /* rates over ~0.5 s windows: game FPS (frames shown), average and worst frame time, vsyncs per second */
    static LARGE_INTEGER t0; static LONG64 f0; static int v0;
    static double fps, ft_ms, ft_max_ms, vps;
    LARGE_INTEGER t, fq; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&fq);
    if (!t0.QuadPart) { t0 = t; f0 = g_gs_flips; v0 = g_frame_no_pub; InterlockedExchange(&g_gs_frame_max_us, 0); }
    double dt = (double)(t.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
    if (dt >= 0.5) {
        LONG64 f1 = g_gs_flips; int v1 = g_frame_no_pub;
        fps = (double)(f1 - f0) / dt;
        ft_ms = f1 > f0 ? dt * 1000.0 / (double)(f1 - f0) : 0.0;
        ft_max_ms = InterlockedExchange(&g_gs_frame_max_us, 0) / 1000.0;
        vps = (double)(v1 - v0) / dt;
        t0 = t; f0 = f1; v0 = v1;
    }
    char line[2][200]; float p[3];
    snprintf(line[0], sizeof line[0], " FPS %5.1f   frame %6.1f ms (worst %6.1f ms)   vsync/s %5.1f   %s ", fps, ft_ms, ft_max_ms, vps, gs_renderer_name());
    if (eragon_pos(p)) snprintf(line[1], sizeof line[1], " Eragon  x %.2f  y %.2f  z %.2f   vsync %d   (F8 hides) ", p[0], p[1], p[2], g_frame_no_pub);
    else snprintf(line[1], sizeof line[1], " Eragon: not in a level   vsync %d   (F8 hides) ", g_frame_no_pub);
    HFONT font = (HFONT)GetStockObject(ANSI_FIXED_FONT), old = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(0, 0, 0)); SetTextColor(dc, RGB(255, 255, 80));
    TEXTMETRICA tm; GetTextMetricsA(dc, &tm);
    for (int i = 0; i < 2; i++) TextOutA(dc, 6, 6 + i * (tm.tmHeight + 2), line[i], (int)strlen(line[i]));
    SelectObject(dc, old);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        EnterCriticalSection(&g_cs);
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = g_w; bi.bmiHeader.biHeight = -g_h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        if (g_buf) StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, g_w, g_h, g_buf, &bi, DIB_RGB_COLORS, SRCCOPY);
        LeaveCriticalSection(&g_cs);
        if (g_overlay) draw_overlay(dc);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: {   /* debugging hotkeys, served on the GS thread (gs.c) */
        extern volatile LONG g_hk_capture, g_hk_toggle_gpu;
        if (wp == VK_F9 && !(lp & 0x40000000)) InterlockedExchange(&g_hk_capture, 1);
        if (wp == VK_F10 && !(lp & 0x40000000)) InterlockedExchange(&g_hk_toggle_gpu, 1);
        if (wp == VK_F8 && !(lp & 0x40000000)) { g_overlay = !g_overlay; InvalidateRect(h, NULL, FALSE); }
        break; }
    case WM_CLOSE: ExitProcess(0);
    }
    return DefWindowProcA(h, m, wp, lp);
}

static DWORD WINAPI win_thread(void *p) {
    (void)p;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "ps2recomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, 640 * 3 / 2, 448 * 3 / 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("ps2recomp", "Eragon (PS2 static recompilation)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    g_ready = 1;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

void win_init(void) {
    InitializeCriticalSection(&g_cs);
    g_buf = calloc(1024 * 1024, 4);
    { const char *e = getenv("PS2_OVERLAY"); if (e && *e == '0') g_overlay = 0; }
    if (getenv("PS2_NOWINDOW")) return;
    CreateThread(NULL, 0, win_thread, NULL, 0, NULL);
}

void win_present(const u32 *rgba, int w, int h) {
    if (!g_buf) return;
    EnterCriticalSection(&g_cs);
    if (!rgba) { memset(g_buf, 0, (size_t)g_w * g_h * 4); }
    else {
        g_w = w; g_h = h;
        for (int i = 0; i < w * h; i++) { u32 c = rgba[i]; g_buf[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
    }
    LeaveCriticalSection(&g_cs);
    if (g_ready && g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

HWND win_hwnd(void) { return g_hwnd; }
