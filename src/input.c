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
#define JVS_PKT_WORD    5       /* length in bits 8-15, packet bytes after */

#define BUTTON_PERIOD   (TIMER_HZ / 4u)     /* a Maple round trip, 4x/second */

static u32 g_mie_port1;
static u32 g_last_poll;
static u32 g_psw1_prev, g_psw2_prev;

void input_set_mie_port(u32 port_plus_1)
{
    g_mie_port1 = port_plus_1;
    g_last_poll = timer_ticks();
}

/* The cabinet's TEST and START, from the JVS packet the MIE relays. Unlike
 * the board's own buttons these are active HIGH, and they only exist if a
 * JVS I/O board is attached and answered the request sent on the previous
 * poll. Absent one, both stay 0 and the board buttons carry the menu. */
static void jvs_buttons(const u32 *words, u32 *test, u32 *start)
{
    *test = 0;
    *start = 0;

    u32 len = (words[JVS_PKT_WORD] >> 8) & 0xFFu;
    if (len < 8)
        return;
    const volatile u8 *pkt = (const volatile u8 *)&words[JVS_PKT_WORD] + 2;
    if (pkt[0] != 0xE0)                 /* JVS start of message */
        return;
    if (pkt[3] != 0x01 || pkt[4] != 0x01)  /* response code, report code */
        return;

    *test  = (pkt[5] >> 7) & 1u;
    *start = (pkt[6] >> 7) & 1u;
}

static u32 buttons_poll(input_event *ev)
{
    u32 words[14];
    if (maple_jvs_read(g_mie_port1 - 1, words) != 0)
        return 0;

    u32 psw1 = (~(words[PSW_WORD] >> PSW1_BIT)) & 1u;
    u32 psw2 = (~(words[PSW_WORD] >> PSW2_BIT)) & 1u;

    /* Both routes drive the same two actions, so a board on a bench and a
     * board in a cabinet behave identically without the operator choosing. */
    u32 jtest, jstart;
    jvs_buttons(words, &jtest, &jstart);
    psw1 |= jtest;
    psw2 |= jstart;

    /* queue the request whose answer the next poll will read */
    maple_jvs_request(g_mie_port1 - 1, 1);

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
