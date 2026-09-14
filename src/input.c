#include "input.h"
#include "scif.h"
#include "maple.h"
#include "timer.h"

/* The board's two push buttons live in the MIE's own status word, not in a
 * JVS packet: no I/O board has to be attached for them to work. Their bits
 * are active LOW, which is why each is inverted before use.
 *
 * Positions confirmed against libnaomi's maple driver, which reads the same
 * 0x86/0x15 response:  dip switches bits 16-19, PSW1 bit 20, PSW2 bit 21 of
 * the second payload word. Our maple_jvs_read hands back the payload starting
 * at word 0, so that word is out[1]. */
#define PSW_WORD        1
#define PSW1_BIT        20
#define PSW2_BIT        21

#define BUTTON_PERIOD   (TIMER_HZ / 4u)     /* a Maple round trip, 4x/second */

static u32 g_mie_port1;
static u32 g_last_poll;
static u32 g_psw1_prev, g_psw2_prev;

void input_set_mie_port(u32 port_plus_1)
{
    g_mie_port1 = port_plus_1;
    g_last_poll = timer_ticks();
}

static u32 buttons_poll(input_event *ev)
{
    u32 words[14];
    if (maple_jvs_read(g_mie_port1 - 1, words) != 0)
        return 0;

    u32 psw1 = (~(words[PSW_WORD] >> PSW1_BIT)) & 1u;
    u32 psw2 = (~(words[PSW_WORD] >> PSW2_BIT)) & 1u;

    /* Report the press, not the hold: a finger rests on a button for far
     * longer than the poll interval, and a held button must not scroll the
     * menu by one entry every 250 ms. */
    u32 fired = 0;
    if (psw1 && !g_psw1_prev) {
        ev->kind = INPUT_SELECT;
        fired = 1;
    } else if (psw2 && !g_psw2_prev) {
        ev->kind = INPUT_CONFIRM;
        fired = 1;
    }
    g_psw1_prev = psw1;
    g_psw2_prev = psw2;
    return fired;
}

u32 input_poll(input_event *ev)
{
    int c = scif_getc();
    if (c >= 0) {
        ev->kind = INPUT_KEY;
        ev->key = c;
        return 1;
    }

    if (!g_mie_port1)
        return 0;

    /* The serial check above is a register read; this one is a Maple round
     * trip costing milliseconds, so it is gated by the free-running timer
     * rather than run on every block. */
    u32 now = timer_ticks();
    if ((u32)(now - g_last_poll) < BUTTON_PERIOD)
        return 0;
    g_last_poll = now;

    ev->kind = INPUT_NONE;
    return buttons_poll(ev);
}
