/* Stormfront SFIOP.IRX replacement (RPC sid 0xFF04 + SIF user commands 0/1).
 *
 * Protocol (from SFIOP.IRX disassembly and the EE "ClIop" client at 0x1D5xxx):
 *   RPC fn 0 (send 4 = EE response buffer address) -> {version=1, mData IOP address, free bytes}
 *   EE -> IOP: EE DMAs {u32 count; Command cmds[count]} to mData, then sends user SIF cmd 0.
 *              Each command is 64 bytes, word0 = type: 0 system, 1 audio, 2 file.
 *   IOP -> EE: responses {u32 count; Response r[count]} (64 bytes each, word0 = type, word1 = size)
 *              are DMA'd to the EE response buffer, then user SIF cmd 1 is sent.
 *              EE dispatches entries by type through its table (0x1D53B0). */
#include <stdio.h>
#include <stdlib.h>
#include "iop.h"
#include "iop_sfiop.h"

void iop_ee_write(u32 ee, const void *src, u32 n);
void iop_send_user_cmd(u32 cid, const void *pkt_extra, u32 extra_len, u32 ee_dest, const void *data, u32 data_len);

static u32 g_mdata;          /* fake IOP address of SFIOP mData */
static u32 g_ee_respbuf;     /* EE address for responses */
static u8  g_resp[4 + 512 * 64];
static u32 g_nresp;
static int g_trace;

u8 *sfiop_response(u32 type, u32 size) {
    if (g_nresp >= 512) { fprintf(stderr, "[sfiop] response overflow\n"); return NULL; }
    u8 *r = g_resp + 4 + g_nresp * 64;
    memset(r, 0, 64);
    memcpy(r, &type, 4); memcpy(r + 4, &size, 4);
    g_nresp++;
    return r;
}

static void flush_responses(void) {
    if (!g_ee_respbuf) return;
    memcpy(g_resp, &g_nresp, 4);
    u32 len = 128 + g_nresp * 64;
    if (len > sizeof g_resp) len = sizeof g_resp;
    iop_send_user_cmd(1, NULL, 0, g_ee_respbuf, g_resp, len);
    g_nresp = 0;
}

/* user SIF command 0: a batch was DMA'd to mData */
void sfiop_batch(void) {
    u8 *m = iop_ptr(g_mdata);
    u32 count; memcpy(&count, m, 4);
    if (count > 1024) { fprintf(stderr, "[sfiop] bad batch count %u\n", count); return; }
    for (u32 i = 0; i < count; i++) {
        const u8 *cmd = m + 4 + i * 64;
        u32 type; memcpy(&type, cmd, 4);
        if (g_trace) {
            const u32 *w = (const u32 *)cmd;
            fprintf(stderr, "[sfiop] cmd type=%u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    type, w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
        }
        switch (type) {
        case SFIOP_TYPE_AUDIO: sfiop_audio_command(cmd); break;
        case SFIOP_TYPE_FILE:  sfiop_file_command(cmd); break;
        default: fprintf(stderr, "[sfiop] unknown command type %u\n", type); break;
        }
    }
    flush_responses();
}

static void sfiop_call(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv) {
    (void)insz; (void)ee_recv;
    switch (fno) {
    case 0: {                                        /* init */
        memcpy(&g_ee_respbuf, in, 4);
        if (!g_mdata) g_mdata = iop_alloc(4 + 1024 * 64 + 0x100);
        u32 r[3] = {1, g_mdata, 1u << 20};
        memcpy(out, r, 12); if (*outsz > 12) *outsz = 12;
        g_trace = getenv("PS2_TRACE") != NULL;
        fprintf(stderr, "[sfiop] init: EE response buffer %08x, mData %08x\n", g_ee_respbuf, g_mdata);
        break; }
    case 2: g_nresp = 0; break;                      /* reset queue */
    case 3: break;                                   /* terminate */
    default: fprintf(stderr, "[sfiop] rpc fn %u\n", fno); break;
    }
}

const IopServer iop_server_sfiop = {0x0000ff04u, "SfIOP", sfiop_call};
