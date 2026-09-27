#ifndef BOARD_H
#define BOARD_H
#include "hw.h"

/* Board identification, read-only, no RAM needed.
 * - SH4 version register (0xFF000030): 0x040205C1 = SH7091
 * - HOLLY ID/revision (0xA05F8000/04): 0x17FD11DB / 0x11 on Naomi 1
 * - Elan T&L chip (Naomi 2 only) ID at 0x08800000: 0xE1AD0000, rev 0x12;
 *   that area is unpopulated on Naomi 1 (open bus).
 * CFG_BOARD_MODEL=1 avoids the Elan probe on a known Naomi 1; =2 selects
 * a known Naomi 2 without requiring a working Elan ID. Auto mode cannot
 * distinguish an absent Elan from one that fails to return its ID. */

typedef enum { BOARD_NAOMI1, BOARD_NAOMI2, BOARD_UNKNOWN } board_type;

typedef struct {
    board_type type;
    u32 sh4_ver;
    u32 holly_id, holly_rev;
    u32 elan_id;
} board_info;

void board_detect(board_info *b);

#endif
