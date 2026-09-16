/* Non-destructive DIMM flash identify over the G1 PIO. See dimm_flash.h.
 * Ported faithfully from Sega's DIMM FIRMWARE UPDATE program: the flash-mode
 * control words, the AMD read-ID sequence, and the address encoding are the
 * ones that program used. Only the read side is built here on purpose. */
#include "dimm_flash.h"

/* Set the flash address on the G1 PIO (flash-mode). The high word keeps the
 * flash-enable bits primed by dimm_flash_enter(); the 16-bit write here sets
 * (addr>>16) | 0x3000, matching the updater's per-access sequence. */
static void fl_addr(u32 addr)
{
    G1_OFFSETL = (u16)addr;
    G1_OFFSETH = (u16)((addr >> 16) | DIMM_FLASH_ADDRFLAG);
}

static void dimm_flash_enter(void)
{
    /* setup1: enter flash access mode (32-bit control write) */
    G1_CTRL32 = DIMM_FLASH_MODE;
}

static void dimm_flash_exit(void)
{
    /* leave flash mode: return the G1 window to normal ROM/SDRAM access */
    G1_CTRL32 = 0;
}

u16 dimm_flash_read16(u32 addr)
{
    fl_addr(addr);
    return G1_DATA16;
}

/* AMD read-ID: unlock (0xAAA<-AA, 0x554<-55), 0xAAA<-90, then read
 * manufacturer at word 0 and device at word 2; 0xF0 anywhere resets to
 * array-read. Commands are byte-duplicated for the 16-bit path. */
static void fl_write16(u32 addr, u16 val)
{
    fl_addr(addr);
    G1_DATA16 = val;
}

void dimm_flash_identify(dimm_flash_id *fi)
{
    fi->entered = 0;
    fi->mfr = 0xFFFF;
    fi->dev = 0xFFFF;
    fi->id_ok = 0;

    dimm_flash_enter();
    fi->entered = 1;

    /* read-reset first, so we start from a known array-read state */
    fl_write16(0x000000, 0xF0F0);

    /* AMD read-ID entry */
    fl_write16(0x000AAA, 0xAAAA);
    fl_write16(0x000554, 0x5555);
    fl_write16(0x000AAA, 0x9090);

    fi->mfr = dimm_flash_read16(0x000000);
    fi->dev = dimm_flash_read16(0x000002);

    /* back to array read */
    fl_write16(0x000000, 0xF0F0);

    /* A window that answers all-ones at both words is not a flash in ID mode
     * (no DIMM, mode switch refused, or a stuck bus). */
    fi->id_ok = !(fi->mfr == 0xFFFF && fi->dev == 0xFFFF);

    dimm_flash_exit();
}
