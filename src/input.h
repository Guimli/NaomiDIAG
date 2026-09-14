#ifndef INPUT_H
#define INPUT_H
#include "hw.h"

/* -------------------------------------------------------------------------
 * Operator input while the tests run: a key on the serial console, or the
 * two push buttons on the Naomi board itself.
 *
 * Polled, not interrupt driven. The buttons cannot be anything else -- they
 * are read through a Maple transaction to the MIE, so nothing arrives
 * unbidden -- and the serial side gains nothing from an interrupt because
 * the SCIF holds a received byte in a 16-byte FIFO until it is read. A check
 * once per test block therefore cannot miss a keystroke.
 *
 * The cost is kept off the assembly test loops entirely: input_poll() is
 * called from progress_tick, which already runs once per 1024-word block and
 * already reads a hardware register. The Maple transaction costs milliseconds
 * and so is gated to roughly four per second by the free-running timer.
 * ---------------------------------------------------------------------- */

#define INPUT_NONE      0
#define INPUT_KEY       1       /* a serial byte is waiting in ev->key      */
#define INPUT_SELECT    2       /* PSW1 on the board: next menu entry       */
#define INPUT_CONFIRM   3       /* PSW2 on the board: run the selection     */

typedef struct {
    u32 kind;
    int key;                    /* valid when kind == INPUT_KEY */
} input_event;

/* Tell the input layer which Maple port the MIE answered on (port + 1, as
 * test_maple_mie stores it; 0 means no MIE, buttons are then unavailable). */
void input_set_mie_port(u32 port_plus_1);

/* Returns 1 and fills ev when something happened. Never blocks. */
u32  input_poll(input_event *ev);

#endif
