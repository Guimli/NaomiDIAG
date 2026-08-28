/* RAM test engine. All accesses go through P2 (uncached) addresses so
 * every read/write really hits the SDRAM chips. State lives on the
 * OC-RAM stack / in registers only — the RAM under test is never used
 * to hold test code or data.
 *
 * Per user spec, each full test = N_PASSES passes of three patterns
 * (the specified figure is 10; see PASSES in the Makefile):
 *   0x55555555, 0xAAAAAAAA, then a pseudo-random stream (different seed
 *   every pass) whose CRC32 is kept in a CPU register on the write side
 *   and compared with the CRC32 recomputed on the read side. */
#include "ramtest.h"
#include "progress.h"
#include "scif.h"

void ram_result_clear(ram_result *r)
{
    r->errors = 0;
    r->badbits = 0;
    r->badbits_e = 0;
    r->badbits_o = 0;
    r->nfails = 0;
    r->crc_w = 0;
    r->crc_r = 0;
}

static void note_fail(ram_result *r, u32 addr, u32 exp, u32 got)
{
    r->errors++;
    r->badbits |= exp ^ got;
    if (addr & 4)
        r->badbits_o |= exp ^ got;
    else
        r->badbits_e |= exp ^ got;
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp [r->nfails] = exp;
        r->fail_got [r->nfails] = got;
        r->nfails++;
    }
}

u32 ram_test_databus(u32 addr)
{
    volatile u32 *p = (volatile u32 *)addr;
    u32 bad = 0;
    for (u32 bit = 1; bit != 0; bit <<= 1) {
        *p = bit;                       /* walking one  */
        bad |= *p ^ bit;
        *p = ~bit;                      /* walking zero */
        bad |= *p ^ ~bit;
    }
    return bad;
}

u32 ram_test_addrbus(u32 base, u32 size)
{
    volatile u32 *b = (volatile u32 *)base;
    const u32 pat = 0xAAAAAAAA, anti = 0x55555555;
    u32 nwords = size >> 2;
    u32 bad = 0;

    for (u32 off = 1; off < nwords; off <<= 1)
        b[off] = pat;
    b[0] = anti;                        /* stuck-high check */
    for (u32 off = 1; off < nwords; off <<= 1)
        if (b[off] != pat)
            bad |= off << 2;
    b[0] = pat;
    for (u32 test = 1; test < nwords; test <<= 1) {   /* stuck-low / shorts */
        b[test] = anti;
        if (b[0] != pat)
            bad |= test << 2;
        for (u32 off = 1; off < nwords; off <<= 1)
            if (off != test && b[off] != pat)
                bad |= test << 2;
        b[test] = pat;
    }
    return bad;
}

void ram_test_pattern(u32 base, u32 len, u32 pattern, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 n = len >> 2;
    for (u32 i = 0; i < n; i++) {
        if ((i & 1023) == 0)
            progress_tick(i);
        p[i] = pattern;
    }
    for (u32 i = 0; i < n; i++) {
        if ((i & 1023) == 0)
            progress_tick(n + i);
        u32 got = p[i];
        if (got != pattern)
            note_fail(r, base + (i << 2), pattern, got);
    }
}

/* CRC32 (IEEE 0xEDB88320), 4 bits at a time; table lives in ROM. */
static const u32 crc4tab[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

static inline u32 crc32_word(u32 crc, u32 w)
{
    crc ^= w;
    for (int k = 0; k < 8; k++)
        crc = (crc >> 4) ^ crc4tab[crc & 0xF];
    return crc;
}

static inline u32 xorshift32(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void ram_test_prng(u32 base, u32 len, u32 seed, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 n = len >> 2;
    register u32 crc_w = 0xFFFFFFFF;    /* write-side CRC, lives in a register */
    register u32 crc_r = 0xFFFFFFFF;    /* read-side CRC */
    u32 x = seed ? seed : 1;

    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        if ((i & 1023) == 0)
            progress_tick(i);
        p[i] = x;
        crc_w = crc32_word(crc_w, x);
    }
    x = seed ? seed : 1;
    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        if ((i & 1023) == 0)
            progress_tick(n + i);
        u32 got = p[i];
        crc_r = crc32_word(crc_r, got);
        if (got != x)
            note_fail(r, base + (i << 2), x, got);
    }
    r->crc_w = ~crc_w;
    r->crc_r = ~crc_r;
    if (crc_w != crc_r && r->errors == 0) {
        /* CRC caught something the compare loop did not (should not happen,
         * but the register-held CRC is the belt-and-braces the spec asks for) */
        note_fail(r, base, crc_w, crc_r);
    }
}
