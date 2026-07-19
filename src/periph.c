/* Peripheral tests: battery-backed SRAM (non-destructive) and AICA RTC. */
#include "periph.h"
#include "timer.h"

/* ---- backup SRAM ---- */

static void sram_note(ram_result *r, u32 addr, u32 exp, u32 got)
{
    r->errors++;
    u32 diff = (exp ^ got) & 0xFF;
    r->badbits |= diff;
    if (addr & 1)
        r->badbits_o |= diff;           /* odd byte lane -> chip B */
    else
        r->badbits_e |= diff;           /* even byte lane -> chip A */
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp [r->nfails] = exp;
        r->fail_got [r->nfails] = got;
        r->nfails++;
    }
}

static inline u32 xorshift32(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

u32 sram_test(ram_result *r, u32 passes)
{
    volatile u8 *p = (volatile u8 *)SRAM_P2_BASE;
    ram_result_clear(r);

    for (u32 pass = 0; pass < passes; pass++) {
        u32 x = 0x5EED5EED ^ (0x9E3779B9u * (pass + 1));
        for (u32 i = 0; i < SRAM_SIZE; i++) {
            u8 orig = p[i];
            x = xorshift32(x);
            u8 rnd = (u8)x;

            p[i] = 0x55;
            if (p[i] != 0x55)
                sram_note(r, SRAM_P2_BASE + i, 0x55, p[i]);
            p[i] = 0xAA;
            if (p[i] != 0xAA)
                sram_note(r, SRAM_P2_BASE + i, 0xAA, p[i]);
            p[i] = rnd;
            if (p[i] != rnd)
                sram_note(r, SRAM_P2_BASE + i, rnd, p[i]);

            p[i] = orig;                /* restore, then verify restore */
            if (p[i] != orig)
                sram_note(r, SRAM_P2_BASE + i, orig, p[i]);
        }
    }

    u32 mask = 0;
    if (r->badbits_e)
        mask |= 1u << 0;
    if (r->badbits_o)
        mask |= 1u << 1;
    return mask;
}

/* ---- AICA RTC ---- */

#define RTC_HI      REG32(0xA0710000u)
#define RTC_LO      REG32(0xA0710004u)

static u32 rtc_read(void)
{
    /* consistent 32-bit read of the split counter */
    for (int tries = 0; tries < 4; tries++) {
        u32 hi1 = RTC_HI & 0xFFFF;
        u32 lo  = RTC_LO & 0xFFFF;
        u32 hi2 = RTC_HI & 0xFFFF;
        if (hi1 == hi2)
            return (hi1 << 16) | lo;
    }
    return (RTC_HI << 16) | (RTC_LO & 0xFFFF);
}

u32 rtc_test(u32 *value)
{
    u32 t0 = rtc_read();
    delay_ms(2200);                     /* > 2 RTC ticks */
    u32 t1 = rtc_read();
    *value = t1;
    /* the counter must move, forward, by a plausible amount */
    if (t1 == t0)
        return 1;
    u32 d = t1 - t0;
    if (d < 1 || d > 10)
        return 1;
    return 0;
}
