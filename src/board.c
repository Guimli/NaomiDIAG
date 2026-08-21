#include "board.h"

#define SH4_VERSION REG32(0xFF000030)
#define HOLLY_ID    REG32(0xA05F8000)
#define HOLLY_REV   REG32(0xA05F8004)
#define ELAN_ID     REG32(0xA8800000)   /* P2 view of 0x08800000 */
#define ELAN_REV    REG32(0xA8800004)

/* Naomi 1 mirrors its single texture RAM at +0x02000000 (one physical
 * memory seen at 0x04000000 and 0x06000000); on Naomi 2 those are the two
 * distinct VRAMs of the two PVRs. Enable both VRAM controllers (the
 * mirror makes it idempotent on Naomi 1), write different values at the
 * same offset of each window, and see whether the first survives.
 * Scratch cells are saved/restored; the VRAM test runs later anyway. */
static u32 detect_dual_pvr(void)
{
    REG32(0xA05F8008) = 0;              /* master: leave reset, cfg VRAM */
    REG32(0xA05F80A4) = 0x0000001F;
    REG32(0xA05F80A8) = 0x15D1C951;
    REG32(0xA05F80A0) = 0x00000020;
    REG32(0xA25F8008) = 0;              /* slave (mirror-safe on Naomi 1) */
    REG32(0xA25F80A4) = 0x0000001F;
    REG32(0xA25F80A8) = 0x15D1C951;
    REG32(0xA25F80A0) = 0x00000020;

    volatile u32 *a = (volatile u32 *)0xA4000000;   /* PVR-A texture RAM */
    volatile u32 *b = (volatile u32 *)0xA6000000;   /* PVR-B / mirror    */
    u32 sa = *a, sb = *b;
    *a = 0x4E414F31;                    /* "NAO1" */
    *b = 0x4E414F32;                    /* "NAO2" */
    u32 dual = (*a == 0x4E414F31);
    *b = sb;
    *a = sa;
    return dual;
}

void board_detect(board_info *b)
{
    b->sh4_ver   = SH4_VERSION;
    b->holly_id  = HOLLY_ID;
    b->holly_rev = HOLLY_REV;
    b->elan_id   = 0;
    b->elan_rev  = 0;

    /* Decide from the VRAM aliasing probe ALONE. It only ever touches
     * addresses mapped on both boards (on a Naomi 1 the second window is a
     * documented mirror of the first). The Elan sits in an area that is
     * UNPOPULATED on a Naomi 1 -- MAME maps it as 'Unassigned' and returns
     * 0, but on real silicon reading empty space is a blind probe, exactly
     * what this project's own methodology warns against. So the Elan is
     * only read once we already know this is a Naomi 2. */
    b->dual_pvr = detect_dual_pvr();

    if (b->dual_pvr) {
        b->type = BOARD_NAOMI2;
        b->elan_id  = ELAN_ID;          /* safe: populated on this board */
        b->elan_rev = ELAN_REV;
    } else if (b->holly_id == 0x17FD11DB) {
        b->type = BOARD_NAOMI1;
    } else {
        b->type = BOARD_UNKNOWN;
    }
}
