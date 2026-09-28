/* Host test of the RTC check: the production periph.c, with the two RTC
 * registers replaced by a scripted clock and delay_ms() moving it.
 * Run: sh tools/test_rtc.sh */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define HW_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u32 now_ms, mode, base_secs, reads, dummy;
static u32 clock_secs(void)
{
    switch (mode) {
    case 0: return base_secs + now_ms / 1000;              /* healthy */
    case 1: return base_secs;                              /* stopped */
    case 2: return now_ms <= 2200 ? base_secs              /* stalls, then */
                                 : base_secs + now_ms / 1000; /* moves */
    case 3: return reads < 4 ? base_secs : base_secs - 100;   /* backwards */
    default: return base_secs + (now_ms / 1000) * 50;      /* races ahead */
    }
}
static volatile u32 *reg(u32 a)
{
    static u32 v;
    if (a == 0xA0710000u) { v = clock_secs() >> 16; return &v; }
    if (a == 0xA0710004u) { reads++; v = clock_secs() & 0xFFFF; return &v; }
    return &dummy;
}
#define REG32(a) (*reg(a))
#define REG16(a) (*(volatile u16 *)reg(a))
static u16 bcr2;
#define BCR2 bcr2
void aica_g2_wait(void) {}
void delay_ms(u32 ms) { now_ms += ms; }
#include "../src/periph.c"
void ram_result_clear(ram_result *r) { (void)r; }

static void run(u32 m, u32 expect, u32 n)
{
    rtc_result r;
    mode = m; now_ms = 0; reads = 0; base_secs = 2400000000u;
    rtc_check(&r);
    if (r.verdict != expect || r.n != n)
        printf("mode %u: verdict %u n %u\n", m, r.verdict, r.n), fflush(stdout);
    assert(r.verdict == expect && r.n == n);
}
int main(void)
{
    run(0, RTC_OK, 2);
    run(1, RTC_STUCK, 4);
    run(2, RTC_IRREGULAR, 4);
    run(3, RTC_JUMP, 4);
    run(4, RTC_JUMP, 4);
    /* year boundaries: the counter starts at 1950-01-01 */
    assert(rtc_year(0) == 1950);
    assert(rtc_year(17121856u) == 1950);            /* 1950-07-18, PR #3 log */
    assert(rtc_year(2366841599u) == 2024);          /* 2024-12-31 23:59:59 */
    assert(rtc_year(2429913600u) == 2027);          /* 2027-01-01 */
    u32 y2026 = 0;
    for (u32 y = 1950; y < 2026; y++)
        y2026 += ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366u : 365u) * 86400u;
    assert(rtc_year(y2026 - 1) == 2025 && rtc_year(y2026) == 2026);
    puts("RTC: healthy, stopped, irregular, backwards, runaway; year boundaries passed");
    return 0;
}
