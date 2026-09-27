/* Detailed readback after a fast verifier detects a mismatch. The ROM-side
 * scan can take minutes on hardware; keep progress and input polling alive. */
#include "pvr.h"
#include "progress.h"
#include "scif.h"
#include "strings.h"

#ifndef VRAM_READ
#define VRAM_READ(a) REG32(a)
#endif

static void note_fail(ram_result *r, u32 addr, u32 exp, u32 got)
{
    u32 diff = exp ^ got;
    r->errors++;
    r->badbits |= diff;
    r->vram_chips |= vram_chip_mask(addr, diff);
    if (addr & 4) r->badbits_o |= diff;
    else r->badbits_e |= diff;
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp[r->nfails] = exp;
        r->fail_got[r->nfails++] = got;
    }
}

void vram_locate(u32 base, u32 n, u32 value, u32 random, ram_result *r)
{
    u32 x = random && !value ? 1 : value;
    scif_puts("\n");
    scif_puts(S_VRAM_SCAN);
    scif_puts(" : ");
    progress_begin(S_VRAM_SCAN, n);
    for (u32 i = 0; i < n; i++) {
        if (!(i & 1023u)) {
            progress_tick(i);
            if (progress_aborted()) return;
        }
        if (random) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
        }
        u32 got = VRAM_READ(base + (i << 2));
        if (got != x) note_fail(r, base + (i << 2), x, got);
    }
    progress_tick(n);
}
