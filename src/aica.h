#ifndef AICA_H
#define AICA_H
#include "hw.h"
#include "ramtest.h"

/* Sound RAM as seen from the SH4, P2 (uncached), G2 bus behind it. */
#define ARAM_P2_BASE    0xA0800000u
#define ARAM_SIZE       0x00800000u     /* 8 MB on Naomi */

/* Block until the G2 bus is idle; mandatory around any access to sound RAM,
 * AICA registers or the RTC (all sit behind G2). */
void aica_g2_wait(void);

/* Number of times the G2 bus failed to go idle within the bound: non-zero
 * means the bus itself is faulty, and any sound RAM result is void. */
u32 aica_g2_stalled(void);

/* The AICA's ARM7: parked on a branch-to-self and running, or held in reset.
 * It must be halted before anything overwrites sound RAM offset 0. */
void aica_arm_park(void);
void aica_arm_halt(void);

/* Running a program on the ARM7 (see arm/aica_arm_test.S). The words go to
 * sound RAM 0 and are read back: 0 if they stuck. Release lets the ARM7
 * start at its reset vector; aica_arm_halt() stops it again. */
u32  aica_arm_load(const u32 *prog, u32 nwords);
void aica_arm_release(void);
u32  aram_rd(u32 off);                  /* one G2-safe word read / write */
void aram_wr(u32 off, u32 v);

/* The ARM7 program's mailbox, in sound RAM (offsets from MB) */
#define ARMB            0x3F00u
#define ARMB_SIG        0x00u
#define ARMB_STEP       0x04u
#define ARMB_STATUS     0x08u
#define ARMB_PROG       0x0Cu
#define ARMB_ERRS       0x10u
#define ARMB_BAD        0x14u
#define ARMB_NFAIL      0x18u
#define ARMB_FAILS      0x1Cu           /* 8 x {exp, got, addr} */
#define ARMB_START      0x80u
#define ARMB_END        0x84u
#define ARMB_SEED       0x88u
#define ARMB_DETAIL     0x8Cu
#define ARMB_TICKS      0x90u
#define ARM_WINDOW      0x4000u         /* code + scratch + mailbox */
#define ARM_SIG         0x41524D37u     /* 'ARM7' */
#define ARM_DONE        0x600D600Du

/* Hold the ARM7 in reset and set master volume; call before touching ARAM. */
void aica_init(void);

/* Sound-RAM tests (G2-safe: FIFO wait every 8 words on the write side). */
u32  aram_test_databus(void);
u32  aram_test_addrbus(void);
void aram_test_pattern(u32 off, u32 len, u32 pattern, ram_result *r);
void aram_test_prng(u32 off, u32 len, u32 seed, ram_result *r);

/* Blocking speech: copy the clip into sound RAM, play it on slot 0,
 * wait for the end of playback, then wait `gap_ms` more.
 * Clips are 4-bit Yamaha ADPCM at 22050 Hz mono, decoded by the AICA
 * itself (PCMS=2); `samples` is a sample count, not a byte count. */
void aica_say(const signed char *pcm, u32 samples, u32 gap_ms);

#endif
