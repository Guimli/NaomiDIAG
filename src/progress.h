#ifndef PROGRESS_H
#define PROGRESS_H
#include "hw.h"

/* ---------------------------------------------------------------------
 * Boot phase indicator, and progress reporting for the long tests.
 *
 * Why the border colour: it is the ONLY visual output that needs no RAM
 * whatsoever -- no framebuffer, no VRAM, not even a working VRAM
 * controller. The PowerVR paints it straight from a register, so it is
 * available within a fraction of a second of reset, long before anything
 * has been proven. That makes it a POST code: if the board stops, the
 * colour on screen names the last phase reached.
 *
 * Colours, in the order they appear on a healthy board:
 *   BLUE     ROM is executing, video timings programmed
 *   CYAN     bus controller and SDRAM controller configured
 *   GREEN    framebuffer VRAM proven, picture on
 *   YELLOW   sound RAM clip zone proven, audio on
 *   MAGENTA  board model identified
 *   ORANGE   first execution of the loops relocated into CPU RAM
 *   WHITE    running the exhaustive test suite
 *   RED      the phase in progress failed
 * ------------------------------------------------------------------- */
typedef enum {
    PH_ROM_ALIVE = 0,
    PH_BUS_READY,
    PH_VIDEO_ON,
    PH_AUDIO_ON,
    PH_BOARD_ID,
    PH_RELOC,
    PH_TESTING,
    PH_DONE,
    PH_FAILED
} boot_phase_t;

void progress_phase(boot_phase_t ph);

/* Pulse the border so a long phase cannot be mistaken for a crash.
 *
 * The first phases can run for a while -- the SDRAM controller comes up and
 * the framebuffer is proven while the code is still fetching every
 * instruction from the EPROM -- and a screen holding one flat colour reads
 * as a hung machine. This alternates the phase colour between full and
 * quarter brightness roughly every 0.7 s.
 *
 * Same hue, two brightnesses, deliberately: the colour is a POST code that
 * names the phase, so blinking between two different hues would destroy the
 * one thing it is there to say. Cheap enough to call inside a test loop --
 * it reads a timer and returns unless the interval has elapsed. */
void progress_heartbeat(void);

/* delay_ms, but pulsing: for the seconds spent waiting on audio. */
void progress_wait_ms(u32 ms);
boot_phase_t progress_current_phase(void);

/* Long test progress. begin() takes the total unit count; tick() is cheap
 * enough to sit inside a memory test loop -- it divides nothing, it only
 * compares against the precomputed count at which the next percent lands. */
void progress_begin(const char *label, u32 total);
void progress_tick(u32 done);
void progress_end(void);

/* Suppress the on-screen bar while a test writes over the framebuffer's own
 * VRAM: drawing the bar there would corrupt the very data under test and
 * manufacture failures. Serial marks and the border colour keep working. */
void progress_screen_enable(u32 on);

/* drawn by the screen renderer as the bottom line of the report */
const char *progress_label(void);
u32 progress_pct(void);
u32 progress_active(void);

#endif
