/* Software GS (Graphics Synthesizer). */
#pragma once
#include "ps2_runtime.h"

void gs_init(void);
void gs_write_reg(int reg, u64 v);          /* general register (A+D / REGLIST / PACKED direct) */
void gs_write_packed(int reg, const u8 *q);  /* PACKED-format register descriptor 0..0xD */
void gs_image_data(const u8 *data, u32 bytes); /* IMAGE mode / HWREG host->local data */
u64  gs_priv_read(u32 addr);
void gs_priv_write(u32 addr, u64 v);
void gs_vsync(void);                         /* field/frame flip: present the display */

/* frame presentation (window.c) */
void win_present(const u32 *rgba, int w, int h);
void win_init(void);
