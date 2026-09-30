#ifndef ETA_H
#define ETA_H
#include "hw.h"

/* -------------------------------------------------------------------------
 * Time left before the end of the boot suite, top right of the screen.
 *
 * Each step has an estimated duration measured on a real Naomi 2 (the
 * serial log attached to PR #2), extrapolated where no measurement exists:
 * the PVR-B and Elan RAM, and every memory step when no CPU RAM block could
 * take the relocated loops, so they run from the boot EPROM. The table and
 * its derivation are in eta.c.
 *
 * The estimate corrects itself as it goes: inside a memory step it follows
 * the progress of the passes, and the measured speed of the quick VRAM
 * check (which always runs from ROM) and of the CPU RAM test rescale the
 * steps that run the same loops.
 * ---------------------------------------------------------------------- */

enum {
    ETA_VIDEO,          /* quick VRAM check, screen on (runs from ROM)   */
    ETA_AUDIO,          /* quick sound RAM check, audio on, replay        */
    ETA_BOARD,          /* board identification                           */
    ETA_RELOC,          /* test-loop relocation                           */
    ETA_BIOS,           /* boot EPROM CRC (relocated loop when possible)  */
    ETA_MIE,            /* Maple / MIE, settings EEPROM, JVS              */
    ETA_SDRAM,          /* CPU RAM                                        */
    ETA_VRAM,           /* TEX0 + TEX1                                    */
    ETA_ARAM,           /* sound RAM                                      */
    ETA_N2,             /* PVR-B + Elan RAM (Naomi 2)                     */
    ETA_PERIPH,         /* NVRAM + RTC (+ late MIE stage)                 */
    ETA_SEEPROM,        /* serial EEPROM                                  */
    ETA_END,            /* relocated-code check, summary                  */
    ETA_STEPS
};

void eta_start(void);                   /* beginning of the suite */
void eta_step(u32 step);                /* a step begins */
void eta_phase(void);                   /* a progress phase begins (progress.c) */
void eta_tick(u32 screen_ok, u32 pct);  /* from the heartbeat: redraw if due */
void eta_finish(void);                  /* suite over or interrupted */

/* What the plan depends on, as soon as it is known. */
void eta_set_board_n2(u32 n2);
void eta_set_reloc(u32 cached);         /* loops relocated into CPU RAM */
void eta_set_mie_early(u32 early);      /* MIE stage ran before the RAM tests */
void eta_set_audio(u32 on);             /* results are spoken */
void eta_video_retried(void);           /* quick VRAM check ran twice */

u32  eta_remaining_ms(void);
u32  eta_format(char *buf, u32 ms);     /* "m:ss" / "h:mm:ss", buf >= 9 */

#endif
