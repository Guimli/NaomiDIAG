/* Locating the failing words once a fast verify pass has seen a
 * difference, for CPU RAM and VRAM alike.
 *
 * The scan itself is hand-written assembly in the relocated block
 * (ramtest_fast.S): it runs at verify speed and stops only on a word worth
 * recording. The first RAM_MAX_FAILS mismatches are recorded in full, for
 * the detail lines; after that, only one that brings a data bit not yet
 * seen in its 4 MiB half and on its word parity -- which is all a chip
 * attribution depends on, on every memory this ROM names: CPU RAM (data
 * half x parity), VRAM (4 MiB half x data half), Elan RAM (16 MiB half x
 * parity). The rest are counted, not recorded, so the totals stay exact.
 *
 * Before, every bad word went through note_fail() from a C loop in ROM:
 * with a cut data line, a million of them per 4 MB, three and a half
 * minutes per pass on a real Naomi 2. */
#include "pvr.h"
#include "progress.h"
#include "scif.h"
#include "strings.h"

#ifdef VRAM_READ
/* Host harness: the same scan in C over the simulated reads. */
static u32 find_c(u32 addr, u32 n, find_ctx *c, u32 random)
{
    for (u32 i = 0; i < n; i++, addr += 4) {
        if (random) {
            c->x ^= c->x << 13;
            c->x ^= c->x >> 17;
            c->x ^= c->x << 5;
        }
        u32 got = VRAM_READ(addr);
        u32 d = got ^ c->x;
        if (!d)
            continue;
        if (d & ~((addr & 4) ? c->ign_o : c->ign_e)) {
            c->exp = c->x;
            c->got = got;
            return i;
        }
        c->skipped++;
    }
    return n;
}
#define FIND(a, n, c, random) find_c((a), (n), (c), (random))
#else
#include "reloc.h"
#define FIND(a, n, c, random) ((random) ? \
    p_ram_find_prng_fast((const u32 *)(a), (n), (c)) : \
    p_ram_find_pat_fast((const u32 *)(a), (n), (c)))
#endif

static void note_fail(ram_result *r, u32 addr, u32 exp, u32 got)
{
    u32 diff = exp ^ got;
    r->errors++;
    r->badbits |= diff;
    r->vram_chips |= vram_chip_mask(addr, diff);   /* 0 outside the VRAM map */
    if (addr & 4) r->badbits_o |= diff;
    else r->badbits_e |= diff;
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp[r->nfails] = exp;
        r->fail_got[r->nfails++] = got;
    }
}

void ram_locate(u32 base, u32 n, u32 value, u32 random, ram_result *r,
                const char *label)
{
    find_ctx c;
    c.x = random && !value ? 1 : value;
    c.ign_e = c.ign_o = 0;
    c.skipped = 0;
    u32 seen_e = 0, seen_o = 0;

    scif_puts("\n");
    scif_puts(label);
    scif_puts(" : ");
    progress_begin(label, n);
    for (u32 i = 0; i < n; ) {
        progress_tick(i);
        if (progress_aborted()) {
            r->errors += c.skipped;
            return;
        }
        u32 addr = base + (i << 2);
        if (!(addr & 0x003FFFFFu))
            seen_e = seen_o = 0;        /* a new 4 MiB half: other chips */
        u32 chunk = n - i < 1024u ? n - i : 1024u;
        u32 done = 0;
        while (done < chunk) {
            u32 detailed = r->nfails < RAM_MAX_FAILS;
            c.ign_e = detailed ? 0 : seen_e;
            c.ign_o = detailed ? 0 : seen_o;
            done += FIND(addr + (done << 2), chunk - done, &c, random);
            if (done >= chunk)
                break;
            u32 a = addr + (done << 2);
            note_fail(r, a, c.exp, c.got);
            if (a & 4)
                seen_o |= c.exp ^ c.got;
            else
                seen_e |= c.exp ^ c.got;
            done++;
        }
        i += chunk;
    }
    progress_tick(n);
    r->errors += c.skipped;
}

void vram_locate(u32 base, u32 n, u32 value, u32 random, ram_result *r)
{
    ram_locate(base, n, value, random, r, S_VRAM_SCAN);
}
