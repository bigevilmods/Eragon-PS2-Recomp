#pragma once
#include "ps2_runtime.h"
u32  gif_feed(int path, const u8 *data, u32 nqw, int stop_at_eop);
void gif_report(void);
void vif_feed(int unit, const u8 *data, u32 nqw, int is_dma_tag);
void vu1_start(u32 addr);   /* vu1.c */
void vu0_micro_start(u32 addr);
void gif_path3_dma(const u8 *data, u32 nqw);
void gif_path3_vifmask(int masked);
void gif_path3_modemask(int masked);
void gif_path3_flush(void);
