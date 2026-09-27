/* Separate local readback failures from reproducible address coupling.
 * CPU byte-address bits are not SDRAM multiplexed address pin numbers. */
#include "pvr.h"
#ifndef VRAM_READ
#define VRAM_READ(a) REG32(a)
#define VRAM_WRITE(a, v) (REG32(a) = (v))
#endif

static u32 check_pair(u32 a, u32 b, vram_address_report report)
{
    const u32 pattern[2] = { 0xAAAAAAAAu, 0x55555555u };
    u32 local_bad = 0, errors = 0, coupled = 0;
    vram_address_event first = {0};
    /* Qualify each cell independently before interpreting pair interference. */
    for (u32 cell = 0; cell < 2; cell++) {
        u32 addr = cell ? b : a;
        for (u32 p = 0; p < 66; p++) {
            u32 value = p < 2 ? pattern[p] :
                ((1u << ((p-2)/2)) ^ ((p & 1) ? 0xFFFFFFFFu : 0));
            VRAM_WRITE(addr, value);
            u32 got = VRAM_READ(addr), again = VRAM_READ(addr);
            if (got != value || again != value) {
                vram_address_event e = {addr, addr, value, got, again, 0, 0,
                                        value, again};
                if (report) report(&e);
                local_bad = 1;
            }
        }
    }
    /* Two repetitions, both orders, both polarities. Exact copying of the
     * other cell's pattern in every trial supports coupling, not a pin verdict. */
    for (u32 repeat = 0; repeat < 2; repeat++)
        for (u32 order = 0; order < 2; order++)
            for (u32 p = 0; p < 2; p++) {
                u32 x = order ? b : a, y = order ? a : b;
                VRAM_WRITE(x, pattern[p]);
                VRAM_WRITE(y, pattern[1-p]);
                u32 got = VRAM_READ(x), again = VRAM_READ(x);
                u32 other = VRAM_READ(y);
                if (got != pattern[p] || again != pattern[p] || other != pattern[1-p]) {
                    if (!errors) {
                        first.first = x; first.second = y;
                        first.expected = pattern[p]; first.observed = got;
                        first.reread = again;
                        first.peer_expected = pattern[1-p];
                        first.peer_observed = other;
                    }
                    errors++;
                }
                if (got == pattern[1-p] && again == got && other == got) coupled++;
            }
    if (errors) {
        first.cpu_bits = a ^ b;
        first.alias = !local_bad && coupled == 8;
        if (report) report(&first);
    }
    return local_bad || errors;
}

u32 vram_address_diagnose(u32 base, u32 size, vram_address_report report)
{
    u32 failures = 0;
    /* Power-of-two size and aligned base keep XOR pairs within the window. */
    if (size < 8 || (size & (size-1)) || (base & (size-1))) return 0;
    for (u32 bit = 4; bit < size; bit <<= 1) {
        failures += check_pair(base, base ^ bit, report);
        u32 end = base + size - 4;
        failures += check_pair(end, end ^ bit, report);
        for (u32 other = bit << 1; other < size; other <<= 1)
            failures += check_pair(base ^ bit, base ^ other, report);
    }
    return failures;
}
