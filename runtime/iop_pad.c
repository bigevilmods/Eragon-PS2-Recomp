/* PADMAN replacement (RPC 0x80000100 / 0x80000101) for the game's libpad (EE 0x116E00-0x118100).
 *
 * scePadPortOpen registers a 256-byte EE buffer: two 128-byte frames, double-buffered by a frame
 * counter. libpad reads (offsets verified in this game's libpad):
 *   +0   data[32]      button/analog data       +80  modeTable[4] (u16)
 *   +88  frame         newer frame wins         +96  length (bytes of data)
 *   +101 modeCurId     +104 nrOfModes  +105 modeCurOffs  +106 nrOfActuators  +107 numActComb
 *   +112 state (6 = STABLE)                     +113 reqState (0 = COMPLETE, 2 = BUSY)
 * Every vsync we write a DualShock 2 frame (mode 0x73) from keyboard, XInput or a script. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iop.h"

void iop_ee_write(u32 ee, const void *src, u32 n);

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

static struct { u32 area; int open; u32 frame; } g_port[2];

/* button bits (active low): byte 2 then byte 3 */
enum {
    B_SELECT = 1 << 0, B_L3 = 1 << 1, B_R3 = 1 << 2, B_START = 1 << 3, B_UP = 1 << 4, B_RIGHT = 1 << 5,
    B_DOWN = 1 << 6, B_LEFT = 1 << 7, B_L2 = 1 << 8, B_R2 = 1 << 9, B_L1 = 1 << 10, B_R1 = 1 << 11,
    B_TRIANGLE = 1 << 12, B_CIRCLE = 1 << 13, B_CROSS = 1 << 14, B_SQUARE = 1 << 15
};

/* ---- XInput (loaded dynamically so no SDK lib is needed) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGamepad;
typedef struct { DWORD dwPacketNumber; XGamepad Gamepad; } XState;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState *);
static XInputGetState_t g_xinput;

static void xinput_load(void) {
    static int tried; if (tried++) return;
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) g_xinput = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
}

/* ---- scripted input: PS2_PADSCRIPT="start@900:10,cross@1500:5" (button@vsync:frames) */
static struct { u32 mask; u32 at, len; } g_script[64];
static int g_nscript = -1;
static void script_load(void) {
    g_nscript = 0;
    const char *s = getenv("PS2_PADSCRIPT");
    if (!s) return;
    static const struct { const char *n; u32 m; } names[] = {
        {"start", B_START}, {"select", B_SELECT}, {"cross", B_CROSS}, {"circle", B_CIRCLE}, {"square", B_SQUARE},
        {"triangle", B_TRIANGLE}, {"up", B_UP}, {"down", B_DOWN}, {"left", B_LEFT}, {"right", B_RIGHT},
        {"l1", B_L1}, {"r1", B_R1}, {"l2", B_L2}, {"r2", B_R2},
        /* analog stick pseudo-buttons (bits above the 16 button bits) */
        {"lup", 1u << 16}, {"ldown", 1u << 17}, {"lleft", 1u << 18}, {"lright", 1u << 19},
        {"rup", 1u << 20}, {"rdown", 1u << 21}, {"rleft", 1u << 22}, {"rright", 1u << 23}};
    char buf[1024]; snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, ","); tok && g_nscript < 64; tok = strtok(NULL, ",")) {
        char name[32] = {0}; unsigned at = 0, len = 10;
        if (sscanf(tok, "%31[a-z0-9]@%u:%u", name, &at, &len) < 2) continue;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcmp(names[i].n, name)) { g_script[g_nscript].mask = names[i].m; g_script[g_nscript].at = at; g_script[g_nscript].len = len; g_nscript++; }
    }
}

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void read_input(u32 vsync, u16 *buttons, u8 sticks[4]) {
    u32 b = 0;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0x80;
    /* keyboard, only while our window is in front */
    extern HWND win_hwnd(void);
    HWND w = win_hwnd();
    if (w && GetForegroundWindow() == w) {
        if (key(VK_RETURN)) b |= B_START;   if (key(VK_BACK)) b |= B_SELECT;
        if (key(VK_UP)) b |= B_UP;          if (key(VK_DOWN)) b |= B_DOWN;
        if (key(VK_LEFT)) b |= B_LEFT;      if (key(VK_RIGHT)) b |= B_RIGHT;
        if (key('K')) b |= B_CROSS;         if (key('L')) b |= B_CIRCLE;
        if (key('J')) b |= B_SQUARE;        if (key('I')) b |= B_TRIANGLE;
        if (key('Q')) b |= B_L1;            if (key('E')) b |= B_R1;
        if (key('1')) b |= B_L2;            if (key('3')) b |= B_R2;
        if (key('W')) sticks[3] = 0x00;     if (key('S')) sticks[3] = 0xFF;   /* left stick */
        if (key('A')) sticks[2] = 0x00;     if (key('D')) sticks[2] = 0xFF;
    }
    /* XInput pad 0 */
    xinput_load();
    XState xs;
    if (g_xinput && g_xinput(0, &xs) == 0) {
        WORD x = xs.Gamepad.wButtons;
        if (x & 0x0001) b |= B_UP;   if (x & 0x0002) b |= B_DOWN;  if (x & 0x0004) b |= B_LEFT;  if (x & 0x0008) b |= B_RIGHT;
        if (x & 0x0010) b |= B_START; if (x & 0x0020) b |= B_SELECT; if (x & 0x0040) b |= B_L3; if (x & 0x0080) b |= B_R3;
        if (x & 0x0100) b |= B_L1;   if (x & 0x0200) b |= B_R1;
        if (x & 0x1000) b |= B_CROSS; if (x & 0x2000) b |= B_CIRCLE; if (x & 0x4000) b |= B_SQUARE; if (x & 0x8000) b |= B_TRIANGLE;
        if (xs.Gamepad.bLeftTrigger > 64) b |= B_L2;
        if (xs.Gamepad.bRightTrigger > 64) b |= B_R2;
        sticks[0] = (u8)((xs.Gamepad.sThumbRX + 32768) >> 8);          /* rx */
        sticks[1] = (u8)(255 - ((xs.Gamepad.sThumbRY + 32768) >> 8));  /* ry (PS2: down = 0xFF) */
        sticks[2] = (u8)((xs.Gamepad.sThumbLX + 32768) >> 8);          /* lx */
        sticks[3] = (u8)(255 - ((xs.Gamepad.sThumbLY + 32768) >> 8));  /* ly */
    }
    if (g_nscript < 0) script_load();
    for (int i = 0; i < g_nscript; i++)
        if (vsync >= g_script[i].at && vsync < g_script[i].at + g_script[i].len) b |= g_script[i].mask;
    if (b & (1u << 16)) sticks[3] = 0x00;  if (b & (1u << 17)) sticks[3] = 0xFF;
    if (b & (1u << 18)) sticks[2] = 0x00;  if (b & (1u << 19)) sticks[2] = 0xFF;
    if (b & (1u << 20)) sticks[1] = 0x00;  if (b & (1u << 21)) sticks[1] = 0xFF;
    if (b & (1u << 22)) sticks[0] = 0x00;  if (b & (1u << 23)) sticks[0] = 0xFF;
    *buttons = (u16)b;
}

/* called every vsync (kernel.c) */
void pad_vsync(u32 vsync) {
    u16 buttons; u8 sticks[4];
    int any = g_port[0].open || g_port[1].open;
    if (!any) return;
    read_input(vsync, &buttons, sticks);
    for (int p = 0; p < 2; p++) {
        if (!g_port[p].open) continue;
        u8 f[128];
        memset(f, 0, sizeof f);
        if (p == 0) {
            u16 low = (u16)~buttons;
            f[0] = 0x00; f[1] = 0x73;                     /* ok, DualShock analog, 3 halfwords */
            f[2] = (u8)low; f[3] = (u8)(low >> 8);
            f[4] = sticks[0]; f[5] = sticks[1]; f[6] = sticks[2]; f[7] = sticks[3];
            for (int i = 8; i < 32; i++) f[i] = 0;
            u32 len = 32; memcpy(f + 96, &len, 4);
            u16 modes[4] = {0x41, 0x73, 0, 0}; memcpy(f + 80, modes, 8);
            f[101] = 0x73; f[104] = 2; f[105] = 1; f[106] = 2; f[107] = 1;
            f[112] = 6;                                   /* STABLE */
            f[113] = 0;                                   /* request COMPLETE */
        } else {
            memset(f, 0xFF, 32);
            f[112] = 0;                                   /* port 2: DISCONNECTED */
        }
        g_port[p].frame++;
        memcpy(f + 88, &g_port[p].frame, 4);
        iop_ee_write(g_port[p].area + (g_port[p].frame & 1) * 128, f, 128);
    }
}

/* libpad RPC: buffer word 0 = command, 1 = port, 2 = slot, 3 = result, 4 = argument (EE area) */
static void pad_call(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv) {
    (void)insz; (void)ee_recv;
    u32 cmd = rd32(in), port = rd32(in + 4) & 1, slot = rd32(in + 8);
    memcpy(out, in, *outsz < 128 ? *outsz : 128);
    u32 result = 1;
    static int logged;
    if (logged++ < 20) fprintf(stderr, "[pad] rpc fn %u cmd %u port %u slot %u arg %08x\n", fno, cmd, port, slot, rd32(in + 16));
    if (cmd == 1 && slot == 0) {                   /* PORT OPEN: register the EE pad area */
        g_port[port].area = rd32(in + 16);
        g_port[port].open = 1;
        g_port[port].frame = 1;
    } else if (cmd == 2) {                         /* PORT CLOSE */
        g_port[port].open = 0;
    }
    memcpy(out + 12, &result, 4);
}

const IopServer iop_server_pad0 = {0x80000100u, "padman", pad_call};
const IopServer iop_server_pad1 = {0x80000101u, "padman-ext", pad_call};
