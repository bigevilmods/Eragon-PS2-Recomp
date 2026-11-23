/* HLE IOP: SIF command transport + SIF RPC, with native RPC servers.
 *
 * EE -> IOP: sceSifSetDma descriptors {src, dest, size, attr}. A descriptor whose dest is the
 *            IOP command buffer carries a command packet; any other dest lands in fake IOP RAM.
 * IOP -> EE: packets are written one at a time into the EE receive buffer (address from
 *            CHANGE_SADDR) and announced with a SIF0 (DMAC ch5) interrupt; the EE handler
 *            (0x10C6A8 in this game) clears byte 0 when it has consumed the packet.
 * Packet header: { u8 psize; u24 dsize; u32 dest; u32 cid; u32 opt; } */
#include <stdio.h>
#include <stdlib.h>
#include "ps2_runtime.h"
#include "iop.h"
#include "iop_sfiop.h"
extern u32 g_bink_cid;

#define IOP_RAM_SIZE (16u << 20)
#define IOP_CMDBUF   0x00001000u            /* reported as SIF_SYSREG_SUBADDR */
u8 *iop_ram;

extern u32 g_dstat_cis;
static u32 g_ee_pktbuf;                     /* EE receive buffer */
static int g_trace;

/* ---- outgoing packet queue */
#define QMAX 256
static u32 g_q[QMAX][32];
static int g_qhead, g_qtail;

static void send_to_ee(const u32 *pkt) {
    int n = (g_qtail + 1) % QMAX;
    if (n == g_qhead) { fprintf(stderr, "[iop] EE packet queue overflow\n"); return; }
    memcpy(g_q[g_qtail], pkt, (pkt[0] & 0xff) ? (pkt[0] & 0xff) : 16);
    g_qtail = n;
    ps2_poll_budget = 0;
}

/* Called at every interrupt-delivery point: hand the EE the next packet once it consumed the last. */
void iop_pump(void) {
    void bink_iop_tick(void);
    bink_iop_tick();
    if (g_qhead == g_qtail || !g_ee_pktbuf) return;
    u8 *buf = ps2_ptr(g_ee_pktbuf);
    if (!buf || buf[0] != 0 || (g_dstat_cis & (1u << 5))) return;   /* previous not consumed yet */
    u32 *pkt = g_q[g_qhead];
    memcpy(buf, pkt, ((pkt[0] & 0xff) + 15) & ~15u);
    g_qhead = (g_qhead + 1) % QMAX;
    g_dstat_cis |= 1u << 5;                 /* SIF0 DMA complete -> EE sifcmd handler */
}

/* ---- memory helpers between EE and fake IOP RAM */
u8 *iop_ptr(u32 a) { return iop_ram + (a & (IOP_RAM_SIZE - 1)); }
void iop_ee_write(u32 ee, const void *src, u32 n) {
    for (u32 i = 0; i < n; i++) { u8 *p = ps2_ptr(ee + i); if (p) *p = ((const u8 *)src)[i]; }
}
void iop_ee_read(void *dst, u32 ee, u32 n) {
    for (u32 i = 0; i < n; i++) { u8 *p = ps2_ptr(ee + i); ((u8 *)dst)[i] = p ? *p : 0; }
}
#define ee_write iop_ee_write
#define ee_read  iop_ee_read

/* ---- IOP heap (fake addresses; bump allocator) */
static u32 g_iop_brk = 0x00100000u;
u32 iop_alloc(u32 size) {
    u32 a = (g_iop_brk + 63) & ~63u;
    g_iop_brk = a + ((size + 63) & ~63u);
    if (g_iop_brk >= IOP_RAM_SIZE) { fprintf(stderr, "[iop] fake IOP RAM exhausted\n"); g_iop_brk = 0x00100000u; a = g_iop_brk; }
    return a;
}

/* ---- RPC servers */
typedef struct { u32 sid; u32 server_addr, buff; const IopServer *impl; int logged; } Bound;
#define MAX_BOUND 64
static Bound g_bound[MAX_BOUND];
static int g_nbound;

static const IopServer *find_impl(u32 sid) {
    for (const IopServer *const *s = iop_servers; *s; s++) if ((*s)->sid == sid) return *s;
    return NULL;
}

static Bound *bind_server(u32 sid) {
    for (int i = 0; i < g_nbound; i++) if (g_bound[i].sid == sid) return &g_bound[i];
    if (g_nbound == MAX_BOUND) return NULL;
    Bound *b = &g_bound[g_nbound++];
    b->sid = sid;
    b->server_addr = iop_alloc(0x40);
    b->buff = iop_alloc(0x40000);
    b->impl = find_impl(sid);
    fprintf(stderr, "[iop] bind sid 0x%08x -> %s\n", sid, b->impl ? b->impl->name : "UNKNOWN (stub: replies zeros)");
    return b;
}
static Bound *bound_by_server(u32 server) {
    for (int i = 0; i < g_nbound; i++) if (g_bound[i].server_addr == server) return &g_bound[i];
    return NULL;
}

static void rpc_end(const u32 *req, u32 req_cid, u32 server, u32 buff) {
    u32 p[12] = {0};
    p[0] = 48; p[2] = 0x80000008u;          /* SIF_CMD_RPC_END */
    p[4] = req[4]; p[5] = req[5]; p[6] = req[6];   /* rec_id, pkt_addr, rpc_id */
    p[7] = req[7];                          /* client (or receive-data struct) */
    p[8] = req_cid;
    p[9] = server; p[10] = buff; p[11] = buff;
    send_to_ee(p);
}

static void handle_call(const u32 *pkt) {
    u32 client = pkt[7], fno = pkt[8], ssize = pkt[9], recv = pkt[10], rsize = pkt[11], server = pkt[13];
    Bound *b = bound_by_server(server);
    (void)client;
    if (!b) { fprintf(stderr, "[iop] call to unknown server %08x\n", server); rpc_end(pkt, 0x8000000Au, 0, 0); return; }
    u8 *in = iop_ptr(b->buff);
    static u8 out[0x40000];
    memset(out, 0, rsize < sizeof out ? rsize : sizeof out);
    u32 outsz = rsize;
    if (b->impl) {
        b->impl->call(fno, in, ssize, out, &outsz, recv);
    } else if (b->logged++ < 60) {
        fprintf(stderr, "[iop] stub call sid=%08x fn=%u send=%u recv=%u ->%08x\n", b->sid, fno, ssize, rsize, recv);
    }
    if (g_trace) fprintf(stderr, "[iop] call %s fn=%u send=%u recv=%u\n", b->impl ? b->impl->name : "?", fno, ssize, rsize);
    if (recv && outsz) ee_write(recv, out, outsz < sizeof out ? outsz : sizeof out);
    rpc_end(pkt, 0x8000000Au, b->server_addr, b->buff);
}

static void handle_cmd(const u32 *pkt) {
    u32 cid = pkt[2];
    if (g_trace) fprintf(stderr, "[iop] cmd %08x psize=%u\n", cid, pkt[0] & 0xff);
    switch (cid) {
    case 0x80000000u: g_ee_pktbuf = pkt[4]; break;                  /* CHANGE_SADDR */
    case 0x80000001u: break;                                         /* SET_SREG (EE->IOP) */
    case 0x80000002u: {                                              /* INIT_CMD -> answer SET_SREG(0,1) */
        u32 p[6] = {24, 0, 0x80000001u, 0, 0, 1};
        send_to_ee(p); break; }
    case 0x80000003u: {                                              /* RESET (IOP reboot with an image) */
        char path[81] = {0}; memcpy(path, &pkt[6], 80);   /* {hdr; int size; int flag; char arg[80]} */
        fprintf(stderr, "[iop] reset: \"%s\"\n", path);
        break; }
    case 0x80000009u: {                                              /* RPC BIND */
        Bound *b = bind_server(pkt[8]);
        rpc_end(pkt, 0x80000009u, b ? b->server_addr : 0, b ? b->buff : 0);
        break; }
    case 0x8000000Au: handle_call(pkt); break;                       /* RPC CALL */
    case 0x8000000Cu: {                                              /* RPC RDATA (sceSifGetOtherData) */
        u32 src = pkt[8], dest = pkt[9], size = pkt[10];
        ee_write(dest, iop_ptr(src), size);
        rpc_end(pkt, 0x8000000Cu, 0, 0);
        break; }
    case 0x00000000u: sfiop_batch(); break;                         /* SfIOP: batch DMA'd to mData */
    default:
        if (cid == g_bink_cid) {                                     /* Bink RAD_IOP_SIDE (iop_bink.c) */
            void bink_iop_command(const u32 *pkt);
            bink_iop_command(pkt);
            break;
        }
        fprintf(stderr, "[iop] unhandled SIF command %08x\n", cid);
    }
}

/* IOP -> EE user command: optional data DMA to ee_dest, then a packet {hdr, extra}. */
void iop_send_user_cmd_opt(u32 cid, u32 opt, const void *extra, u32 extra_len, u32 ee_dest, const void *data, u32 data_len) {
    if (data && data_len) iop_ee_write(ee_dest, data, data_len);
    u32 p[32] = {0};
    if (extra_len > sizeof p - 16) extra_len = sizeof p - 16;
    p[0] = (16 + extra_len) | (data_len << 8);
    p[1] = ee_dest; p[2] = cid; p[3] = opt;
    if (extra) memcpy(&p[4], extra, extra_len);
    send_to_ee(p);
}
void iop_send_user_cmd(u32 cid, const void *extra, u32 extra_len, u32 ee_dest, const void *data, u32 data_len) {
    if (data && data_len) iop_ee_write(ee_dest, data, data_len);
    u32 p[32] = {0};
    if (extra_len > sizeof p - 16) extra_len = sizeof p - 16;
    p[0] = (16 + extra_len) | (data_len << 8);
    p[1] = ee_dest; p[2] = cid; p[3] = 0;
    if (extra) memcpy(&p[4], extra, extra_len);
    send_to_ee(p);
}

/* sceSifSetDma(SifDmaTransfer_t *list, int count) */
u32 iop_sif_set_dma(u32 list, u32 count) {
    static u32 id = 1;
    for (u32 i = 0; i < count; i++) {
        u32 d[4]; ee_read(d, list + i * 16, 16);
        u32 src = d[0], dest = d[1], size = d[2];
        if ((dest & 0x1fffff) == IOP_CMDBUF) {
            u32 pkt[64] = {0};
            ee_read(pkt, src, size < sizeof pkt ? size : sizeof pkt);
            handle_cmd(pkt);
        } else {
            ee_read(iop_ptr(dest), src, size);
        }
    }
    return id++;
}

u32 iop_sif_get_reg(u32 reg) {
    switch (reg) {
    case 4: return 0x70000;                 /* SMFLAG: SIFINIT | CMDINIT | BOOTEND */
    case 0x80000000u: return IOP_CMDBUF;    /* SUBADDR */
    default: return 0;
    }
}

void iop_init(void) {
    iop_ram = calloc(1, IOP_RAM_SIZE);
    g_trace = getenv("PS2_TRACE") != NULL;
}

