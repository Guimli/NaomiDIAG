/* SDRAM (BSC) initialisation, replicated from the original Naomi BIOS
 * boot code at 0xA0000440 (epr-21576h). Sequence:
 *   BCR/WCR setup, refresh timer setup, MCR without MRSET, SDRAM mode
 *   register write (address-encoded), refresh burst wait, MCR with MRSET,
 *   second mode write, second refresh wait.
 * Runs entirely from ROM with an OC-RAM stack; SDRAM is untouched by
 * anything else until ramtest.c has validated it. */
#include "sdram.h"

static void refresh_wait(void)
{
    RFCR = RFCR_VAL;                 /* reset refresh counter (0xA4xx key) */
    while ((RFCR & 0x03FF) <= 8)
        ;
}

static void mode_set(u32 mcr_val)
{
    MCR = mcr_val & 0xBFFFFFFB;      /* MRSET=0, RFSH=0 */
    REG8(SDMR3_ADDR) = 0;            /* mode register set (address-encoded) */
    MCR = mcr_val & 0xBFFFFFFF;      /* MRSET=0, RFSH on */
    refresh_wait();
    MCR = mcr_val & 0xFFFFFFFB;      /* MRSET=1, RFSH=0 */
    REG8(SDMR3_ADDR) = 0;
    RTCSR = RTCSR_VAL;
    RTCOR = RTCOR_VAL;
    RTCNT = RTCNT_VAL;
    MCR = mcr_val;                   /* final: MRSET=1, RFSH=1 */
    refresh_wait();
}

u32 sdram_init(void)
{
    BCR1  = BCR1_VAL;
    BCR2  = BCR2_VAL;
    WCR1  = WCR1_VAL;
    WCR2  = WCR2_VAL;
    WCR3  = WCR3_VAL;
    RTCSR = RTCSR_VAL;
    RTCOR = RTCOR_VAL;
    RTCNT = RTCNT_VAL;

    mode_set(MCR_32MB);

    /* 16MB boards mirror the first 16MB at +16MB (JinGasa trick):
     * clear the mirror address, write a magic at the base address,
     * and see whether it shows up in the mirror. */
    volatile u32 *base   = (volatile u32 *)(SDRAM_P2_BASE + 0x50000);
    volatile u32 *mirror = (volatile u32 *)(SDRAM_P2_BASE + SDRAM_16MB + 0x50000);
    *mirror = 0;
    *base   = 0x424D3631;            /* "16MB" */
    if (*mirror == 0x424D3631) {
        mode_set(MCR_16MB);
        return SDRAM_16MB;
    }
    return SDRAM_32MB;
}
