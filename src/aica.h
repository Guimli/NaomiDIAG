#ifndef AICA_H
#define AICA_H
#include "hw.h"
#include "ramtest.h"

/* Sound RAM as seen from the SH4, P2 (uncached), G2 bus behind it. */
#define ARAM_P2_BASE    0xA0800000u
#define ARAM_SIZE       0x00800000u     /* 8 MB on Naomi */

/* Hold the ARM7 in reset and set master volume; call before touching ARAM. */
void aica_init(void);

/* Sound-RAM tests (G2-safe: FIFO wait every 8 words on the write side). */
u32  aram_test_databus(void);
void aram_test_pattern(u32 off, u32 len, u32 pattern, ram_result *r);
void aram_test_prng(u32 off, u32 len, u32 seed, ram_result *r);

/* Blocking speech: copy the clip into sound RAM, play it on slot 0,
 * wait for the end of playback, then wait `gap_ms` more.
 * Rate is fixed at 22050 Hz signed 8-bit mono. */
void aica_say(const signed char *pcm, u32 len, u32 gap_ms);

#endif
