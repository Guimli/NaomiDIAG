#include "board.h"

#define SH4_VERSION REG32(0xFF000030)
#define HOLLY_ID    REG32(0xA05F8000)
#define HOLLY_REV   REG32(0xA05F8004)
#define ELAN_ID     REG32(0xA8800000)   /* P2 view of 0x08800000 */
#define ELAN_REV    REG32(0xA8800004)

void board_detect(board_info *b)
{
    b->sh4_ver   = SH4_VERSION;
    b->holly_id  = HOLLY_ID;
    b->holly_rev = HOLLY_REV;
    b->elan_id   = ELAN_ID;
    b->elan_rev  = ELAN_REV;

    if ((b->elan_id & 0xFFFF0000) == 0xE1AD0000)
        b->type = BOARD_NAOMI2;
    else if (b->holly_id == 0x17FD11DB)
        b->type = BOARD_NAOMI1;
    else
        b->type = BOARD_UNKNOWN;
}
