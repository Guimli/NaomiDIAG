#include "reloc.h"
#include "progress.h"

extern char reloc_blk_start[], reloc_blk_end[];

void (*p_ram_fill_fast)(u32 *, u32, u32);
u32  (*p_ram_verify_fast)(u32 *, u32, u32, u32 *);
void (*p_ram_prng_fill_fast)(u32 *, u32, prng_ctx *);
void (*p_ram_prng_verify_fast)(const u32 *, u32, prng_ctx *);
u32  (*p_ram_find_pat_fast)(const u32 *, u32, find_ctx *);
u32  (*p_ram_find_prng_fast)(const u32 *, u32, find_ctx *);
void (*p_sha1_pio_blocks)(u32 *, u32, u32 *);
u32  (*p_crc32_rom_block)(const u32 *, u32, u32);
void (*p_g1_unlock)(void);
void (*p_g1_unlock_v)(u32);
void (*p_sha1_mem_blocks)(u32 *, u32, u32 *, const u32 *);
void (*p_dimm_fill_seq_fast)(u32 *, u32, u32, u32);
void (*p_dimm_check_seq_fast)(const u32 *, u32, dimm_chk_ctx *);
u32  (*p_g1_dma_run)(const g1_dma_job *);

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
    p_ram_find_pat_fast    = ram_find_pat_fast;
    p_ram_find_prng_fast   = ram_find_prng_fast;
    p_sha1_pio_blocks      = sha1_pio_blocks;
    p_crc32_rom_block      = crc32_rom_block;
    p_g1_unlock            = g1_unlock;
    p_g1_unlock_v          = g1_unlock_v;
    p_sha1_mem_blocks      = sha1_mem_blocks;
    p_dimm_fill_seq_fast   = dimm_fill_seq_fast;
    p_dimm_check_seq_fast  = dimm_check_seq_fast;
    p_g1_dma_run           = g1_dma_run;
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
    p_ram_find_pat_fast    = (u32 (*)(const u32 *, u32, find_ctx *))
        (cached + ((u32)(char *)ram_find_pat_fast - (u32)reloc_blk_start));
    p_ram_find_prng_fast   = (u32 (*)(const u32 *, u32, find_ctx *))
        (cached + ((u32)(char *)ram_find_prng_fast - (u32)reloc_blk_start));
    p_sha1_pio_blocks      = (void (*)(u32 *, u32, u32 *))
        (cached + ((u32)(char *)sha1_pio_blocks - (u32)reloc_blk_start));
    p_crc32_rom_block      = (u32 (*)(const u32 *, u32, u32))
        (cached + ((u32)(char *)crc32_rom_block - (u32)reloc_blk_start));
    p_g1_unlock            = (void (*)(void))
        (cached + ((u32)(char *)g1_unlock - (u32)reloc_blk_start));
    p_g1_unlock_v          = (void (*)(u32))
        (cached + ((u32)(char *)g1_unlock_v - (u32)reloc_blk_start));
    p_sha1_mem_blocks      = (void (*)(u32 *, u32, u32 *, const u32 *))
        (cached + ((u32)(char *)sha1_mem_blocks - (u32)reloc_blk_start));
    p_dimm_fill_seq_fast   = (void (*)(u32 *, u32, u32, u32))
        (cached + ((u32)(char *)dimm_fill_seq_fast - (u32)reloc_blk_start));
    p_dimm_check_seq_fast  = (void (*)(const u32 *, u32, dimm_chk_ctx *))
        (cached + ((u32)(char *)dimm_check_seq_fast - (u32)reloc_blk_start));
    p_g1_dma_run           = (u32 (*)(const g1_dma_job *))
        (cached + ((u32)(char *)g1_dma_run - (u32)reloc_blk_start));
    g_p2_dest = p2_dest;
    g_active = 1;
    return 1;
}

/* .ramtext: linked at 0x8C700000 (P1, cached; linker.ld), stored in the EPROM
 * at __ramtext_load. Written through the P1 alias: write-through, so memory
 * and any operand-cache line of the area agree -- the code's literal pools
 * are read through the operand cache. Then ICI, from P2 like reloc_install. */
extern char _ramtext_start[], _ramtext_end[], _ramtext_load[];   /* __ramtext_* in linker.ld */

u32 ramtext_install(void)
{
    const volatile u32 *src = (const volatile u32 *)_ramtext_load;
    volatile u32 *dst = (volatile u32 *)_ramtext_start;
    u32 n = (u32)(_ramtext_end - _ramtext_start) / 4;

    if (((u32)dst & 0x1FFFFFFFu) != RAMTEXT_PHYS)
        return 0;                   /* linker.ld and reloc.h disagree */
    for (u32 i = 0; i < n; i++)
        dst[i] = src[i];
    for (u32 i = 0; i < n; i++)
        if (dst[i] != src[i])
            return 0;
    CCR = 0x000009A1;               /* ICI, OC kept (stack and .bss are there) */
    for (volatile int i = 0; i < 8; i++)
        ;
    return 1;
}

/* Filled after the link by tools/textreloc.py: 'TRL1', count, .text size,
 * then the word offsets (from the start of .text) of every literal holding
 * a .text address. The placeholder ('PTRL', capacity) is what the tool
 * looks for; left as it is, text_install refuses. */
typedef struct {
    u32 magic, count, size;
    u16 off[TEXTRELOC_MAX];
} textreloc_t;
const textreloc_t textreloc_tab = { 0x4C525450u, TEXTRELOC_MAX, 0, { 0 } };

/* the first and past-the-last byte of .text (crt0.S, linker.ld) */
extern char text_lo[] __asm__("_start"), text_hi[] __asm__("__text_end");

u32 text_size(void)
{
    return (u32)(text_hi - text_lo);
}

static u32 g_text_p1, g_text_delta;     /* the copy in use, 0 = none */
/* the EPROM original, recorded while running from it: in the copy, the
 * address of text_lo is one of the shifted ones and names the copy itself */
static u32 g_text_rom, g_text_words;

u32 text_install(u32 p2_dest)
{
    const volatile textreloc_t *tr = &textreloc_tab;
    u32 ts = (u32)text_lo, n = text_size() / 4;
    if (tr->magic != 0x314C5254u || tr->size != text_size() ||
        tr->count > TEXTRELOC_MAX || (p2_dest & 0x1Fu))
        return 0;
    /* Through the P1 alias: write-through, so memory and any operand-cache
     * line of the window agree (the copy's literal pools are read through
     * the operand cache). */
    u32 p1 = 0x80000000u | (p2_dest & 0x1FFFFFFFu);
    const volatile u32 *src = (const volatile u32 *)ts;
    volatile u32 *dst = (volatile u32 *)p1;
    for (u32 i = 0; i < n; i++)
        dst[i] = src[i];
    for (u32 i = 0; i < n; i++)
        if (dst[i] != src[i])
            return 0;
    u32 delta = p1 - ts, te = ts + text_size();
    for (u32 k = 0; k < tr->count; k++) {
        u32 i = tr->off[k];
        u32 w = src[i];
        if (i >= n || w < ts || w >= te)
            return 0;                   /* the table does not fit this image */
        dst[i] = w + delta;
        if (dst[i] != w + delta)
            return 0;
    }
    CCR = 0x000009A1;                   /* ICI, from P2; OC kept (stack, .bss) */
    for (volatile int i = 0; i < 8; i++)
        ;
    g_text_p1 = p1;
    g_text_delta = delta;
    g_text_rom = ts;
    g_text_words = n;
    return delta;
}

u32 text_verify(u32 *addr, u32 *expect, u32 *got)
{
    if (!g_text_p1)
        return 1;
    /* through P2: the cache would answer with the clean copy it holds */
    const volatile u32 *src = (const volatile u32 *)g_text_rom;
    const volatile u32 *cp = (const volatile u32 *)(g_text_p1 | 0x20000000u);
    const volatile textreloc_t *tr = &textreloc_tab;
    u32 n = g_text_words;               /* not text_size(): see g_text_rom */
    for (u32 i = 0; i < n; i++) {
        u32 want = src[i], have = cp[i];
        if (have == want)
            continue;
        u32 shifted = 0;
        for (u32 k = 0; k < tr->count && !shifted; k++)
            shifted = tr->off[k] == i;
        if (shifted && have == want + g_text_delta)
            continue;                   /* a code address, shifted on purpose */
        *addr = (g_text_p1 | 0x20000000u) + i * 4;
        *expect = shifted ? want + g_text_delta : want;
        *got = have;
        return 0;
    }
    return 1;
}
