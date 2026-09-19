/* TMU0 delays: fully internal to the SH4, no RAM needed. */
#include "timer.h"

#define TSTR    REG8 (0xFFD80004)
#define TCOR0   REG32(0xFFD80008)
#define TCNT0   REG32(0xFFD8000C)
#define TCR0    REG16(0xFFD80010)
#define TCOR1   REG32(0xFFD80014)
#define TCNT1   REG32(0xFFD80018)
#define TCR1    REG16(0xFFD8001C)

#define TCR_UNF     0x0100
#define TMU_HZ      12500000u       /* 50 MHz / 4 */

void timer_init(void)
{
    TSTR &= (u8)~1;                 /* stop channel 0 */
    TCR0 = 0;                       /* Pck/4, no interrupt */

    /* Channel 1 free-runs for the whole session as a clock nobody stops.
     * Channel 0 cannot serve: delay_ms starts and stops it on every call.
     * TCNT counts down and reloads from TCOR, so with TCOR at maximum it
     * simply wraps, and unsigned subtraction of two readings gives the
     * elapsed count correctly across the wrap. */
    TSTR &= (u8)~2;
    TCR1  = 0;                      /* Pck/4 */
    TCOR1 = 0xFFFFFFFF;
    TCNT1 = 0xFFFFFFFF;
    TSTR |= 2;
}

u32 timer_ticks(void)
{
    return ~TCNT1;                  /* down-counter read as an up-counter */
}

/* Milliseconds since reset, carried past the counter's own range.
 *
 * TCNT1 is 32 bits at 12.5 MHz, so it wraps every 343 seconds -- longer
 * than the boot suite but nothing at all next to a RAM test left looping
 * for an afternoon. The wrap is absorbed here by accumulating deltas
 * instead of reading an absolute count: unsigned subtraction is correct
 * across the wrap, so the only requirement is being called more often than
 * once every 343 seconds. The progress heartbeat guarantees that, and the
 * serial stamp calls it on every line besides.
 *
 * No division: this ROM links without libgcc, so there is no __udivsi3 to
 * call. Subtracting 1000 ms worth of ticks first bounds the fine loop to
 * 999 turns and the coarse one to 343, whatever the gap since last call. */
static u32 g_ms_prev;
static u32 g_ms_acc;
static u32 g_ms_rem;

u32 timer_ms(void)
{
    u32 now = ~TCNT1;
    g_ms_rem += now - g_ms_prev;    /* wrap-safe */
    g_ms_prev = now;
    while (g_ms_rem >= TMU_HZ) {
        g_ms_rem -= TMU_HZ;
        g_ms_acc += 1000u;
    }
    while (g_ms_rem >= TMU_HZ / 1000u) {
        g_ms_rem -= TMU_HZ / 1000u;
        g_ms_acc++;
    }
    return g_ms_acc;
}

void delay_ms(u32 ms)
{
    /* count down in <=1s slices to stay well inside 32 bits */
    while (ms) {
        u32 slice = ms > 1000 ? 1000 : ms;
        ms -= slice;
        TSTR &= (u8)~1;
        TCR0 = 0;
        TCNT0 = (TMU_HZ / 1000) * slice;
        TCOR0 = 0xFFFFFFFF;
        TSTR |= 1;
        /* BOUNDED: never hang on a dead timer -- the guard is far longer
         * than the slice itself, so a healthy TMU always wins the race. */
        for (u32 guard = slice * 50000u; guard; guard--)
            if (TCR0 & TCR_UNF)
                break;
        TSTR &= (u8)~1;
        TCR0 = 0;
    }
}
