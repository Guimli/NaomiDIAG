/* SCIF console output. The SCIF was initialised in crt0.S before any RAM
 * existed; these helpers only need a few bytes of stack (OC-RAM). */
#include "scif.h"

/* Bounded waits: exit early when the flag rises (real hardware), or time
 * out and proceed — a diagnostic must never hang on its own console, and
 * some emulators model the SCIF status flags only partially. */
#define SCIF_SPIN 0x20000

void scif_putc(char c)
{
    for (u32 spin = 0; spin < SCIF_SPIN; spin++)
        if (SCFSR2 & SCFSR2_TDFE)
            break;
    SCFTDR2 = (u8)c;
    SCFSR2 &= (u16)~(SCFSR2_TDFE | SCFSR2_TEND);
}

void scif_flush(void)
{
    for (u32 spin = 0; spin < SCIF_SPIN; spin++)
        if (SCFSR2 & SCFSR2_TEND)
            break;
}

void scif_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            scif_putc('\r');
        scif_putc(*s++);
    }
}

void scif_puthex(u32 v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        scif_putc(hex[(v >> i) & 0xF]);
}

/* Decimal output without division (no libgcc in this ROM). */
void scif_putdec(u32 v)
{
    static const u32 pow10[] = { 1000000000u, 100000000u, 10000000u, 1000000u,
                                 100000u, 10000u, 1000u, 100u, 10u, 1u };
    int started = 0;
    for (int i = 0; i < 10; i++) {
        u32 d = 0;
        while (v >= pow10[i]) {
            v -= pow10[i];
            d++;
        }
        if (d || started || i == 9) {
            scif_putc((char)('0' + d));
            started = 1;
        }
    }
}

/* Non-blocking read of one received byte; returns -1 when nothing waits.
 *
 * Reception has been on since crt0 set SCSCR2 = TE|RE, so bytes have always
 * been landing in the 16-byte receive FIFO -- this ROM simply never looked.
 * The FIFO is why polling is enough: a keystroke waits there until read, so
 * checking once per test block cannot miss one.
 *
 * Both DR and RDF are tested. RDF rises only at the FIFO trigger level; DR
 * covers the single byte that a human typing will actually produce, which
 * RDF alone would leave sitting there indefinitely.
 *
 * Receive errors and breaks are cleared rather than reported: an unplugged
 * or half-connected adapter generates them continuously, and a diagnostic
 * must not turn a loose cable into a stream of complaints. */
int scif_getc(void)
{
    u16 st = SCFSR2;

    if (st & (SCFSR2_ER | SCFSR2_BRK)) {
        SCFSR2 = (u16)~(SCFSR2_ER | SCFSR2_BRK);
        SCLSR2 = 0;
        return -1;
    }
    if (!(st & (SCFSR2_RDF | SCFSR2_DR)))
        return -1;

    int c = (int)(u8)SCFRDR2;
    SCFSR2 = (u16)~(SCFSR2_RDF | SCFSR2_DR);
    return c;
}
