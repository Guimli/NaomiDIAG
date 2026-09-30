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

/* The board's transfer count (0x5F7014): 8-byte units as first reversed
 * for the DIMM, 32-byte units in the BIOS cartridge loader. dimm_g1_probe
 * settles it on the real board; until then the DIMM figure stands. */
static u32 g_count32;            /* 0: 8-byte units (no .data here) */

/* One transfer; dir is the raw SB_GDDIR value. Which value reads and which
 * writes is what dimm_g1_probe establishes: the first reversing said 0 =
 * DIMM->RAM, but the BIOS loads cartridges with 1. Register order and
 * address encoding replicate the BIOS transfer routines. */
static u32 dimm_dma_n(u32 dimm_addr, u32 sysram_phys, u32 len, u32 dir,
                      u32 count)
{
    if (gdst_wait())
        return 1;
    REG32(0xA05F7000) = 0;
    REG32(0xA05F7004) = 0;
    REG32(0xA05F7010) = dimm_addr & 0xFFFF;
    REG32(0xA05F700C) = (dimm_addr >> 16) | 0x8000;
    REG32(0xA05F7014) = count;
    SB_GDSTAR = sysram_phys;
    SB_GDLEN  = len;
    SB_GDDIR  = dir;
    SB_GDEN   = 1;
    SB_GDST   = 1;
    return gdst_wait();
}

static u32 dimm_dma(u32 dimm_addr, u32 sysram_phys, u32 len, u32 dir)
{
    return dimm_dma_n(dimm_addr, sysram_phys, len, dir, len >> (g_count32 ? 5 : 3));
}

/* ------------------------------------------------------------------------- */
/* Probe of the G1 DMA with the DIMM, leaving its SDRAM as it found it.       */

#define PROBE_ADDR  0x00100000u         /* 1 MB into the game image */
#define PROBE_LEN   1024u
#define PROBE_NW    (PROBE_LEN / 4)
#define PHYS_A      SCRATCH_PHYS_W                  /* original, by PIO    */
#define PHYS_B      (SCRATCH_PHYS_W + 0x2000u)      /* DMA buffer          */
#define PHYS_D      (SCRATCH_PHYS_W + 0x4000u)      /* re-read, by PIO     */

/* PIO read of the board, auto-advancing; flags as the caller asks */
static void dimm_pio_read(u32 addr, volatile u32 *dst, u32 nw, u16 flags)
{
    REG16(0xA05F7000) = (u16)(((addr >> 16) & 0x1FFF) | flags);
    REG16(0xA05F7004) = (u16)addr;
    for (u32 i = 0; i < nw; i++) {
        u32 lo = REG16(0xA05F7008);
        dst[i] = lo | ((u32)REG16(0xA05F7008) << 16);
    }
}

static u32 same(volatile u32 *a, volatile u32 *b)
{
    for (u32 i = 0; i < PROBE_NW; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static u32 probe_pat(u32 i) { return (i * 0x9E3779B1u) ^ 0xC3A55A3Cu; }

static u32 is_pattern(volatile u32 *b)
{
    for (u32 i = 0; i < PROBE_NW; i++)
        if (b[i] != probe_pat(i))
            return 0;
    return 1;
}

static u32 plausible(volatile u32 *b)
{
    for (u32 i = 1; i < PROBE_NW; i++)
        if (b[i] != b[0])
            return 1;
    return b[0] != 0 && b[0] != 0xFFFFFFFFu;
}

void dimm_g1_probe(dimm_g1_probe_result *r)
{
    volatile u32 *a = P2(PHYS_A), *b = P2(PHYS_B), *d = P2(PHYS_D);
    r->pio_flags = 0;
    r->dir_read = r->dir_write = DP_UNKNOWN;
    r->outcome[0] = r->outcome[1] = DP_NOT_RUN;
    r->restored = r->restore_failed = 0;
    r->cnt_ok[0] = r->cnt_ok[1] = 0;
    r->ticks_32k = 0;
    if (!dimm_scratch_ok())
        return;

    /* 1. the reference: the same 1 KB twice through the PIO port, which
     *    must be stable and not a constant. Auto-advance alone first, then
     *    with the linear-mapping flag the cartridge code uses. */
    static const u16 flags[2] = { 0x8000, 0xA000 };
    for (u32 f = 0; f < 2 && !r->pio_flags; f++) {
        dimm_pio_read(PROBE_ADDR, a, PROBE_NW, flags[f]);
        dimm_pio_read(PROBE_ADDR, d, PROBE_NW, flags[f]);
        if (same(a, d) && plausible(a))
            r->pio_flags = flags[f];
    }
    if (!r->pio_flags)
        return;                         /* nothing to compare against */

    /* 2. each SB_GDDIR value on a buffer holding a pattern the DIMM does
     *    not: the buffer turning into the reference is a read, the DIMM
     *    turning into the pattern is a write -- undone at once by writing
     *    the reference back the same way, and checked. 1 first: the BIOS
     *    reads cartridges with it, so the likely write comes second. */
    for (u32 k = 0; k < 2; k++) {
        u32 dir = k ? 0u : 1u;
        for (u32 i = 0; i < PROBE_NW; i++)
            b[i] = probe_pat(i);
        if (dimm_dma_n(PROBE_ADDR, PHYS_B, PROBE_LEN, dir, PROBE_LEN >> 5)) {
            r->outcome[dir] = DP_TIMEOUT;
            continue;
        }
        dimm_pio_read(PROBE_ADDR, d, PROBE_NW, r->pio_flags);
        u32 dimm_same = same(d, a);
        if (dimm_same && same(b, a)) {
            r->outcome[dir] = DP_READ;
        } else if (dimm_same && is_pattern(b)) {
            r->outcome[dir] = DP_NONE;
        } else {
            r->outcome[dir] = is_pattern(d) ? DP_WRITE : DP_ODD;
            if (!dimm_same) {
                /* put the game data back: the reference, same direction */
                for (u32 i = 0; i < PROBE_NW; i++)
                    b[i] = a[i];
                dimm_dma_n(PROBE_ADDR, PHYS_B, PROBE_LEN, dir, PROBE_LEN >> 5);
                dimm_pio_read(PROBE_ADDR, d, PROBE_NW, r->pio_flags);
                if (same(d, a))
                    r->restored = 1;
                else
                    r->restore_failed = 1;
            }
        }
    }
    if (r->outcome[0] == DP_READ && r->outcome[1] == DP_WRITE)
        r->dir_read = 0, r->dir_write = 1;
    else if (r->outcome[1] == DP_READ && r->outcome[0] == DP_WRITE)
        r->dir_read = 1, r->dir_write = 0;
    else if (r->outcome[1] == DP_READ)
        r->dir_read = 1;
    else if (r->outcome[0] == DP_READ)
        r->dir_read = 0;
    if (r->dir_read == DP_UNKNOWN)
        return;

    /* 3. the count register, both readings of it, on a read */
    static const u32 shifts[2] = { 5, 3 };
    for (u32 c = 0; c < 2; c++) {
        for (u32 i = 0; i < PROBE_NW; i++)
            b[i] = probe_pat(i);
        if (!dimm_dma_n(PROBE_ADDR, PHYS_B, PROBE_LEN, r->dir_read,
                        PROBE_LEN >> shifts[c]) && same(b, a))
            r->cnt_ok[c] = 1;
    }
    if (r->cnt_ok[0] && !r->cnt_ok[1])
        g_count32 = 1;

    /* 4. speed: one 32 KB read */
    u32 t0 = timer_ticks();
    if (!dimm_dma(PROBE_ADDR, SCRATCH_PHYS_R, DIMM_DMA_BLK, r->dir_read))
        r->ticks_32k = timer_ticks() - t0;
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

void dimm_mem_test(u32 span, u32 pattern, dimm_mem_result *r,
                   u32 dir_write, u32 dir_read)
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

        if (dimm_dma(off, SCRATCH_PHYS_W, DIMM_DMA_BLK, dir_write) ||
            dimm_dma(off, SCRATCH_PHYS_R, DIMM_DMA_BLK, dir_read)) {
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
