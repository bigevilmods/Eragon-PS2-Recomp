/* Native replacements for IOP RPC servers. */
#include <stdio.h>
#include <stdlib.h>
#include "iop.h"

u32 g_bink_cid = 11;   /* SIF user command used by Bink's RAD_IOP_SIDE (set from its load args) */

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static void wr32(u8 *p, u32 v) { memcpy(p, &v, 4); }

/* ---- 0x80000003: IOP heap (sceSifAllocIopHeap / sceSifFreeIopHeap / sceSifLoadIopHeap) */
static void heap_call(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv) {
    (void)insz; (void)ee_recv;
    switch (fno) {
    case 1: wr32(out, iop_alloc(rd32(in))); break;               /* alloc(size) -> addr */
    case 2: wr32(out, 0); break;                                  /* free(addr) */
    case 3: fprintf(stderr, "[iop] LoadIopHeap(\"%s\") not implemented\n", (const char *)in + 4); wr32(out, 0); break;
    default: fprintf(stderr, "[iop] heap fn %u\n", fno); wr32(out, 0); break;
    }
    if (*outsz > 4) *outsz = 4;
}
static const IopServer s_heap = {0x80000003u, "iopheap", heap_call};

/* ---- 0x80000006: LOADFILE (sceSifLoadModule etc.). Modules are replaced natively,
 *      so loading "succeeds" with a fake module id. */
static void loadfile_call(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv) {
    (void)insz; (void)ee_recv;
    static int next_id = 10;
    if (fno == 255) {                            /* version query: SDK compares with "3000" (0x3576A4) */
        memcpy(out, "3000", 4);
        if (*outsz > 4) *outsz = 4;
        return;
    }
    const char *path = (const char *)in + 8;   /* {int arg_len; int ?; char path[252]; char args[252]} */
    if (fno == 6) {
        /* Load from an IOP memory buffer (word 0 = IOP address of the uploaded module image). */
        u32 addr = rd32(in);
        const u8 *img = iop_ptr(addr);
        int is_rad = 0;
        for (u32 i = 0; i + 12 < 0x2000; i++) if (!memcmp(img + i, "RAD_IOP_SIDE", 12)) { is_rad = 1; break; }
        fprintf(stderr, "[iop] loadfile buffer @%08x%s req:", addr, is_rad ? " = Bink RAD_IOP_SIDE" : "");
        for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", rd32(in + 4 * i));
        fprintf(stderr, " | args@260: %08x %08x \"%.32s\"\n", rd32(in + 260), rd32(in + 264), (const char *)in + 260);
        if (is_rad) {
            /* RAD_IOP_SIDE announces itself with opt=3 at startup (IRX 0x4F4) on its SIF user command,
             * which is the first load argument (decimal string at +260; default 11). */
            void iop_send_user_cmd_opt(u32, u32, const void *, u32, u32, const void *, u32);
            extern u32 g_bink_cid;
            const char *arg = (const char *)in + 260;
            g_bink_cid = (rd32(in + 4) && arg[0] >= '0' && arg[0] <= '9') ? (u32)atoi(arg) : 11;
            iop_send_user_cmd_opt(g_bink_cid, 3, NULL, 0, 0, NULL, 0);
            void bink_iop_started(void);
            bink_iop_started();
        }
        wr32(out, next_id++); wr32(out + 4, 0);
        if (*outsz > 8) *outsz = 8;
        return;
    }
    fprintf(stderr, "[iop] loadfile fn=%u \"%.64s\"\n", fno, path);
    wr32(out, next_id++);                        /* result: module id (>0 = success) */
    wr32(out + 4, 0);                            /* module start result */
    if (*outsz > 8) *outsz = 8;
}
static const IopServer s_loadfile = {0x80000006u, "loadfile", loadfile_call};

extern const IopServer iop_server_mcserv, iop_server_sfiop, iop_server_audio, iop_server_file, iop_server_pad0, iop_server_pad1;
const IopServer *const iop_servers[] = {
    &s_heap, &s_loadfile, &iop_server_mcserv, &iop_server_sfiop, &iop_server_audio, &iop_server_file, &iop_server_pad0, &iop_server_pad1, NULL };
