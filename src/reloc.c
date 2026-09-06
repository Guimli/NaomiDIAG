#include "reloc.h"
#include "progress.h"

extern char reloc_blk_start[], reloc_blk_end[];

void (*p_ram_fill_fast)(u32 *, u32, u32);
u32  (*p_ram_verify_fast)(u32 *, u32, u32, u32 *);
void (*p_ram_prng_fill_fast)(u32 *, u32, prng_ctx *);
void (*p_ram_prng_verify_fast)(const u32 *, u32, prng_ctx *);

static u32 g_active;
static u32 g_p2_dest;        /* uncached address of the relocated block */

u32 reloc_active(void) { return g_active; }

u32 reloc_verify(u32 *off, u32 *expect, u32 *got)
{
    if (!g_active)
        return 1;                       /* nothing was relocated */

    /* Read the copy through P2, its uncached address. Reading it through the
     * window it executes from would be answered out of the cache, which is
     * exactly the clean copy we are trying to look past -- the check would
     * then be incapable of ever failing. */
    const volatile u8 *src = (const volatile u8 *)reloc_blk_start;
    const volatile u8 *dst = (const volatile u8 *)g_p2_dest;
    u32 len = (u32)(reloc_blk_end - reloc_blk_start);

    for (u32 i = 0; i < len; i++) {
        u8 a = src[i], b = dst[i];
        if (a != b) {
            *off = i;
            *expect = a;
            *got = b;
            reloc_init();               /* back to ROM: nothing runs from it */
            return 0;
        }
    }
    return 1;
}

void reloc_init(void)
{
    p_ram_fill_fast        = ram_fill_fast;
    p_ram_verify_fast      = ram_verify_fast;
    p_ram_prng_fill_fast   = ram_prng_fill_fast;
    p_ram_prng_verify_fast = ram_prng_verify_fast;
    g_active = 0;
}

u32 reloc_install(u32 p2_dest)
{
    const volatile u8 *src = (const volatile u8 *)reloc_blk_start;
    volatile u8 *dst = (volatile u8 *)p2_dest;
    u32 len = (u32)(reloc_blk_end - reloc_blk_start);

    for (u32 i = 0; i < len; i++)
        dst[i] = src[i];
    for (u32 i = 0; i < len; i++)
        if (dst[i] != src[i])
            return 0;               /* the copy did not stick: stay in ROM */

    /* Invalidate the instruction cache before executing what we just wrote
     * through the uncached alias. ICI only: the value keeps OCI clear on
     * purpose, because the operand cache is configured as RAM and holds this
     * program's stack and .bss -- invalidating it here would destroy both.
     * Written from P2, as the architecture requires. */
    CCR = 0x000009A1;
    for (volatile int i = 0; i < 8; i++)
        ;

    u32 cached = 0x80000000u | (p2_dest & 0x1FFFFFFFu);
    u32 off_fill    = (u32)(char *)ram_fill_fast        - (u32)reloc_blk_start;
    u32 off_verify  = (u32)(char *)ram_verify_fast      - (u32)reloc_blk_start;
    u32 off_pfill   = (u32)(char *)ram_prng_fill_fast   - (u32)reloc_blk_start;
    u32 off_pverify = (u32)(char *)ram_prng_verify_fast - (u32)reloc_blk_start;

    void (*f_fill)(u32 *, u32, u32)   = (void (*)(u32 *, u32, u32))
                                        (cached + off_fill);
    u32  (*f_verify)(u32 *, u32, u32, u32 *) =
        (u32 (*)(u32 *, u32, u32, u32 *))(cached + off_verify);

    /* First execution from RAM, and it has to prove two things: that the
     * board will run cached code from SDRAM at all, and that the copy
     * computes the same answers. The scratch area is inside the window we
     * have already tested, well clear of the code itself. */
    volatile u32 *scratch = (volatile u32 *)(p2_dest + RELOC_SCRATCH);
    u32 dodd = 0;
    f_fill((u32 *)scratch, 8, 0xA5A5F00Du);         /* 128 words */
    if (f_verify((u32 *)scratch, 16, 0xA5A5F00Du, &dodd) != 0 || dodd != 0)
        return 0;                                   /* must see them equal */
    /* index 70 is an EVEN-address word, so the difference must land in the
     * even mask and nowhere else -- which also proves the parity split works */
    scratch[70] ^= 0x00000040u;
    dodd = 0;
    if (f_verify((u32 *)scratch, 16, 0xA5A5F00Du, &dodd) != 0x00000040u)
        return 0;                                   /* must see it, exactly */
    if (dodd != 0)
        return 0;                                   /* and on the right half */

    p_ram_fill_fast        = f_fill;
    p_ram_verify_fast      = f_verify;
    p_ram_prng_fill_fast   = (void (*)(u32 *, u32, prng_ctx *))
                             (cached + off_pfill);
    p_ram_prng_verify_fast = (void (*)(const u32 *, u32, prng_ctx *))
                             (cached + off_pverify);
    g_p2_dest = p2_dest;
    g_active = 1;
    return 1;
}
