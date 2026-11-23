/* HLE IOP interface: SIF transport (iop.c) and native RPC servers (iop_servers.c). */
#pragma once
#include "ps2_runtime.h"

/* One native RPC server. `call` gets the request in IOP memory and fills the reply;
 * *outsz starts as the EE's receive size and may be reduced. `ee_recv` is the EE address
 * the reply will be copied to (some servers also write EE memory directly). */
typedef struct {
    u32 sid;
    const char *name;
    void (*call)(u32 fno, const u8 *in, u32 insz, u8 *out, u32 *outsz, u32 ee_recv);
} IopServer;

extern const IopServer *const iop_servers[];   /* NULL-terminated (iop_servers.c) */
extern u8 *iop_ram;
u8  *iop_ptr(u32 iop_addr);
u32  iop_alloc(u32 size);

void iop_init(void);
void iop_pump(void);
u32  iop_sif_set_dma(u32 list, u32 count);
u32  iop_sif_get_reg(u32 reg);
