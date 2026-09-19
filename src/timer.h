#ifndef TIMER_H
#define TIMER_H
#include "hw.h"

/* TMU0-based blocking delays. Peripheral clock 50 MHz, prescaler /4. */
void timer_init(void);

/* Milliseconds since reset, immune to the 343-second wrap of TCNT1.
 * Must be called at least once per wrap; the heartbeat sees to that. */
u32  timer_ms(void);
void delay_ms(u32 ms);

/* Free-running counter, 12.5 MHz, wraps every ~343 s. Differences between
 * two readings are correct across the wrap. */
#define TIMER_HZ 12500000u
u32  timer_ticks(void);

#endif
