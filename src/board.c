#include "board.h"
#include "config.h"

void board_detect(board_info *b)
{
    b->sh4_ver = REG32(0xFF000030);
    b->holly_id = REG32(0xA05F8000);
    b->holly_rev = REG32(0xA05F8004);
    b->elan_id = 0;

    /* CLXB windows are disabled at boot; VRAM independence cannot identify
     * the board. Auto mode expects absent Elan to return open bus, not an
     * exception. A fixed Naomi 1 selection avoids that unpopulated window. */
#if CFG_BOARD_MODEL != 1
    b->elan_id = REG32(0xA8800000);
#endif
    if (CFG_BOARD_MODEL == 2 || b->elan_id == 0xE1AD0000) {
        b->type = BOARD_NAOMI2;
    } else if (b->holly_id == 0x17FD11DB) {
        b->type = BOARD_NAOMI1;
    } else {
        b->type = BOARD_UNKNOWN;
    }
}
