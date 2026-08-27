#include "progress.h"
#include "pvr.h"
#include "scif.h"

/* border colour per phase, 0x00RRGGBB as VO_BORDER_COL wants it */
static const u32 phase_col[] = {
    0x000000FF,     /* PH_ROM_ALIVE  blue    */
    0x0000FFFF,     /* PH_BUS_READY  cyan    */
    0x0000FF00,     /* PH_VIDEO_ON   green   */
    0x00FFFF00,     /* PH_AUDIO_ON   yellow  */
    0x00FF00FF,     /* PH_BOARD_ID   magenta */
    0x00FFFFFF,     /* PH_TESTING    white   */
    0x00808080,     /* PH_DONE       grey    */
    0x00FF0000      /* PH_FAILED     red     */
};

static boot_phase_t g_phase;

void progress_phase(boot_phase_t ph)
{
    g_phase = ph;
    pvr_border(phase_col[ph]);
}

boot_phase_t progress_current_phase(void) { return g_phase; }

/* ---- long test progress ---- */

static const char *g_label;
static u32 g_pct, g_next, g_step, g_active, g_serial_mark;
static u32 g_screen_off;   /* set while the bar would corrupt the test */

void progress_screen_enable(u32 on) { g_screen_off = !on; }

/* binary long division: no libgcc in a freestanding build, and this runs
 * once per test, never in the tick path */
static u32 udiv32(u32 a, u32 b)
{
    if (!b)
        return 0;
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1u);
        if (r >= b) {
            r -= b;
            q |= 1u << i;
        }
    }
    return q;
}

void progress_begin(const char *label, u32 total)
{
    g_label = label;
    g_pct = 0;
    g_active = 1;
    g_serial_mark = 0;
    g_step = udiv32(total, 100);
    if (!g_step)
        g_step = 1;
    g_next = g_step;
    if (!g_screen_off)
        fb_progress(label, 0);
}

void progress_tick(u32 done)
{
    if (!g_active || done < g_next)
        return;
    /* a tick can jump several percent if the caller samples coarsely */
    while (done >= g_next && g_pct < 100) {
        g_pct++;
        g_next += g_step;
    }
    if (!g_screen_off)
        fb_progress(g_label, g_pct);
    /* the serial log gets a mark every 10%, so a headless run still shows
     * that the test is advancing rather than wedged */
    if (g_pct - g_serial_mark >= 10) {
        g_serial_mark = g_pct;
        scif_putdec(g_pct);
        scif_puts("% ");
    }
}

void progress_end(void)
{
    if (!g_active)
        return;
    g_pct = 100;
    if (!g_screen_off)
        fb_progress(g_label, 100);
    g_active = 0;
}

const char *progress_label(void) { return g_label ? g_label : ""; }
u32 progress_pct(void)           { return g_pct; }
u32 progress_active(void)        { return g_active; }
