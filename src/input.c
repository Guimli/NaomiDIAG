#include "input.h"
#include "scif.h"
#include "maple.h"
#include "timer.h"

/* set by main.c once the MIE program is up; before that our commands
 * do not exist and every poll would be a wasted Maple round trip */
extern u32 g_mie_prog;

/* The board's two push buttons, read through the Z80 program uploaded into
 * the MIE (src/mie_prog.z80).
 *
 * This used to go through Maple command 0x86, which the MIE's factory
 * firmware does not implement -- its boot ROM answers 0x03, 0x80, 0x82 and
 * 0x84 and refuses everything else. Every poll failed, silently, so no
 * button press ever reached the diagnostic. Disassembling the MIE ROM said
 * so and a probe confirmed it: command 0x86 returns rc=1 on a stock board.
 *
 * The uploaded program answers 0xE2 with the MIE's input port instead:
 * DIP SW1:1-4 in bits 0-3, TEST in bit 4, SERVICE in bits 5-6, all active
 * LOW, which is why each is inverted before use. Wiring confirmed in MAME's
 * mie.cpp and in the blob the original BIOS uploads.
 *
 * The cabinet's TEST and START come from the JVS master in the same
 * program, which polls the I/O board by itself (see buttons_poll). */
#define MIE_TEST_BIT    4
#define MIE_SERVICE_BIT 5

#define BUTTON_PERIOD   (TIMER_HZ / 4u)     /* a Maple round trip, 4x/second */

static u32 g_mie_port1;
static u32 g_last_poll;
static u32 g_psw1_prev, g_psw2_prev;

void input_set_mie_port(u32 port_plus_1)
{
    g_mie_port1 = port_plus_1;
    g_last_poll = timer_ticks();
}

/* The board's buttons, and the cabinet's TEST and START from the JVS I/O
 * board when one answers. The JVS master in the MIE keeps the switches up
 * to date on its own; reading them is one more Maple round trip. JVS
 * switches are active HIGH: system byte bit 7 is TEST, player 1's first
 * byte bit 7 is START. Cabinet TEST acts as the board's TEST (next entry),
 * START as SERVICE (run it). */
static u32 buttons_poll(input_event *ev)
{
    u8 in5;
    if (maple_mie_inputs(g_mie_port1 - 1, &in5) != 0)
        return 0;

    u32 psw1 = (~(in5 >> MIE_TEST_BIT)) & 1u;
    u32 psw2 = (~(in5 >> MIE_SERVICE_BIT)) & 1u;

    u8 jvs[28];
    if (maple_jvs_info(g_mie_port1 - 1, 0, jvs) == 0 &&
        jvs[JVSI_STATE] == JVS_READY) {
        psw1 |= (jvs[JVSI_SW] >> 7) & 1u;
        if (jvs[JVSI_PLAYERS])
            psw2 |= (jvs[JVSI_SW + 1] >> 7) & 1u;
    }

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

    if (!g_mie_port1 || !g_mie_prog)
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
