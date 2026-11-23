#pragma once
#include "ps2_runtime.h"

enum { SFIOP_TYPE_SYSTEM = 0, SFIOP_TYPE_AUDIO = 1, SFIOP_TYPE_FILE = 2 };

/* Append a 64-byte response entry (type, size prefilled); returns it for the caller to fill
 * bytes 8..63. Sent to the EE at the end of the current batch. */
u8 *sfiop_response(u32 type, u32 size);
void sfiop_batch(void);

void sfiop_audio_command(const u8 *cmd);   /* iop_audio.c */
void sfiop_file_command(const u8 *cmd);    /* iop_file.c */
