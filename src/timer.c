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
