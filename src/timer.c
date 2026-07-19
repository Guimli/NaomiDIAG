/* TMU0 delays: fully internal to the SH4, no RAM needed. */
#include "timer.h"

#define TSTR    REG8 (0xFFD80004)
#define TCOR0   REG32(0xFFD80008)
#define TCNT0   REG32(0xFFD8000C)
#define TCR0    REG16(0xFFD80010)

#define TCR_UNF     0x0100
#define TMU_HZ      12500000u       /* 50 MHz / 4 */

void timer_init(void)
{
    TSTR &= (u8)~1;                 /* stop channel 0 */
    TCR0 = 0;                       /* Pck/4, no interrupt */
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
        while (!(TCR0 & TCR_UNF))
            ;
        TSTR &= (u8)~1;
        TCR0 = 0;
    }
}
