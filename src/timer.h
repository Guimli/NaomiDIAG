#ifndef TIMER_H
#define TIMER_H
#include "hw.h"

/* TMU0-based blocking delays. Peripheral clock 50 MHz, prescaler /4. */
void timer_init(void);
void delay_ms(u32 ms);

#endif
