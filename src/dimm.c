/* DIMM board (G1 bus): mailbox probe (read-only) and, when the operator
 * asks for it, a destructive SDRAM cell test over the GD-DMA path. */
#include "dimm.h"
#include "ramtest.h"        /* crc32_word */
#include "timer.h"          /* timer_ticks, TIMER_HZ */
#include "progress.h"       /* progress_tick, progress_aborted */

void dimm_probe(dimm_info *di)
{
    di->command = DIMM_COMMAND;
    di->offsetl = DIMM_OFFSETL;
    di->paraml  = DIMM_PARAML;
    di->paramh  = DIMM_PARAMH;
    di->status  = DIMM_STATUS;
    /* all-ones on the command register = no board driving the bus */
    di->present = (di->command != 0xFFFF);
}

/* ------------------------------------------------------------------------- */
/* GD-DMA to the DIMM SDRAM.                                                  */

/* 32 KB per transfer: large enough to amortise the setup, small enough that
 * one block's worth of scratch stays well inside main SDRAM. */
#define DIMM_DMA_BLK    0x00008000u

/* System-RAM scratch for the DMA endpoints, 4 MB into main SDRAM so it clears
 * the game area at 0 and the code (which runs from ROM / OC-RAM, never here).
 * Two blocks: the source we write, and the destination we read back into.
 * Accessed through the P2 uncached alias so the CPU sees what the DMA landed
 * without a cache flush; SB_GDSTAR gets the matching physical address. */
#define SCRATCH_PHYS_W  0x0C400000u
#define SCRATCH_PHYS_R  (SCRATCH_PHYS_W + DIMM_DMA_BLK)
#define P2(phys)        ((volatile u32 *)(0xA0000000u | (phys)))

/* Wait for the GD-DMA to go idle. 0 = idle, 1 = timed out (~1 s): nothing on
 * the bus completed the transfer, i.e. no DIMM or its firmware is not up. */
static u32 gdst_wait(void)
{
    u32 t0 = timer_ticks();
    while (SB_GDST & 1)
        if (timer_ticks() - t0 > TIMER_HZ)
            return 1;
    return 0;
}

/* One transfer. dir: 0 = DIMM->system RAM, 1 = system RAM->DIMM.
 * Register order and address encoding replicate the BIOS transfer routines. */
static u32 dimm_dma(u32 dimm_addr, u32 sysram_phys, u32 len, u32 dir)
{
    if (gdst_wait())
        return 1;
    REG32(0xA05F7000) = 0;
    REG32(0xA05F7004) = 0;
    REG32(0xA05F7010) = dimm_addr & 0xFFFF;
    REG32(0xA05F700C) = (dimm_addr >> 16) | 0x8000;
    REG32(0xA05F7014) = len >> 3;
    SB_GDSTAR = sysram_phys;
    SB_GDLEN  = len;
    SB_GDDIR  = dir;
    SB_GDEN   = 1;
    SB_GDST   = 1;
    return gdst_wait();
}

u32 dimm_scratch_ok(void)
{
    volatile u32 *w = P2(SCRATCH_PHYS_W);
    volatile u32 *r = P2(SCRATCH_PHYS_R);
    /* A defective scratch would fabricate DIMM failures. Prove it holds both
     * a pattern and its complement, at the two block bases the test uses. */
    w[0] = 0xA5A5A5A5u; r[0] = 0x5A5A5A5Au;
    if (w[0] != 0xA5A5A5A5u || r[0] != 0x5A5A5A5Au)
        return 0;
    w[DIMM_DMA_BLK/4 - 1] = 0x0F0F0F0Fu;
    if (w[DIMM_DMA_BLK/4 - 1] != 0x0F0F0F0Fu)
        return 0;
    return 1;
}

void dimm_mem_test(u32 span, u32 pattern, dimm_mem_result *r)
{
    volatile u32 *wbuf = P2(SCRATCH_PHYS_W);
    volatile u32 *rbuf = P2(SCRATCH_PHYS_R);
    const u32 nw = DIMM_DMA_BLK / 4;

    r->blocks = 0;
    r->errors = 0;
    r->badbits = 0;
    r->timeout = 0;
    r->crc_w = 0xFFFFFFFFu;
    r->crc_r = 0xFFFFFFFFu;
    r->first_addr = 0;
    r->first_exp = 0;
    r->first_got = 0;

    if (!dimm_scratch_ok())
        return;                         /* r->blocks == 0 flags "not run" */

    for (u32 i = 0; i < nw; i++)
        wbuf[i] = pattern;              /* constant source for every block */

    for (u32 off = 0; off + DIMM_DMA_BLK <= span; off += DIMM_DMA_BLK) {
        progress_tick(off);
        if (progress_aborted())
            break;

        for (u32 i = 0; i < nw; i++)
            rbuf[i] = ~pattern;         /* poison so a dead read shows up  */

        if (dimm_dma(off, SCRATCH_PHYS_W, DIMM_DMA_BLK, 1) ||    /* write */
            dimm_dma(off, SCRATCH_PHYS_R, DIMM_DMA_BLK, 0)) {    /* read  */
            r->timeout = 1;
            return;
        }

        for (u32 i = 0; i < nw; i++) {
            u32 got = rbuf[i];
            r->crc_w = crc32_word(r->crc_w, pattern);
            r->crc_r = crc32_word(r->crc_r, got);
            if (got != pattern) {
                r->badbits |= got ^ pattern;
                if (r->errors == 0) {
                    r->first_addr = off + i * 4;
                    r->first_exp = pattern;
                    r->first_got = got;
                }
                r->errors++;
            }
        }
        r->blocks++;
    }
}
