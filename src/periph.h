#ifndef PERIPH_H
#define PERIPH_H
#include "hw.h"
#include "ramtest.h"

/* Battery-backed save SRAM (2x 62256), operator settings + bookkeeping:
 * the test is NON-destructive (save / test / restore every byte). */
#define SRAM_P2_BASE    0xA0200000u
#define SRAM_SIZE       0x00008000u

/* Returns component mask: bit0 = even byte lane (chip A),
 * bit1 = odd byte lane (chip B); 0 = all good. Fills *r with details. */
u32 sram_test(ram_result *r, u32 passes);

/* AICA RTC (32.768kHz crystal + battery), seconds since 1950.
 * Non-destructive: reads the counter twice around a TMU delay and checks
 * that it ticks. Returns 0 = OK, 1 = stuck/dead. *value out = counter. */
u32 rtc_test(u32 *value);

#endif
