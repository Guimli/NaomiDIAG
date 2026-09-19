/* SCIF console output. The SCIF was initialised in crt0.S before any RAM
 * existed; these helpers only need a few bytes of stack (OC-RAM). */
#include "scif.h"
#include "timer.h"

/* Bounded waits: exit early when the flag rises (real hardware), or time
 * out and proceed — a diagnostic must never hang on its own console, and
 * some emulators model the SCIF status flags only partially. */
#define SCIF_SPIN 0x20000

static void scif_raw(char c)
{
    for (u32 spin = 0; spin < SCIF_SPIN; spin++)
        if (SCFSR2 & SCFSR2_TDFE)
            break;
    SCFTDR2 = (u8)c;
    SCFSR2 &= (u16)~(SCFSR2_TDFE | SCFSR2_TEND);
}

/* Every line is stamped with the time since reset, [MM:SS.mmm].
 *
 * It goes in here rather than at each call site because the report is
 * built from fragments -- a label, then a status, then a newline -- and a
 * stamp per fragment would be nonsense. Tracking the start of a line is
 * the only place that knows where a message really begins.
 *
 * Blank lines are left bare: they separate sections, and a timestamp on
 * nothing is just noise. The stamp costs 13 characters, about a
 * millisecond of serial time at 115200 baud, a tenth of a second over a
 * whole run.
 *
 * TMU and not the RTC: the RTC counts whole seconds, so a dozen
 * consecutive lines would carry the same stamp and the one thing the log
 * is wanted for -- how long each test took -- would be invisible. The RTC
 * dates the run instead, once, where the clock's coarseness does not
 * matter. */
/* Zero-initialised on purpose: this ROM links with no writable .data, so
 * the flag has to mean "mid-line" rather than "at a line start". */
static u32 g_mid_line;
static u32 g_stamping;

static void scif_stamp(void)
{
    u32 ms = timer_ms();
    u32 min = 0, sec = 0;
    while (ms >= 60000u) { ms -= 60000u; min++; }
    while (ms >= 1000u)  { ms -= 1000u;  sec++; }

    g_stamping = 1;
    scif_raw('[');
    scif_raw((char)('0' + (min / 10u) % 10u));
    scif_raw((char)('0' + min % 10u));
    scif_raw(':');
    scif_raw((char)('0' + sec / 10u));
    scif_raw((char)('0' + sec % 10u));
    scif_raw('.');
    scif_raw((char)('0' + ms / 100u));
    scif_raw((char)('0' + (ms / 10u) % 10u));
    scif_raw((char)('0' + ms % 10u));
    scif_raw(']');
    scif_raw(' ');
    g_stamping = 0;
}

void scif_putc(char c)
{
    if (!g_stamping) {
        if (!g_mid_line && c != '\n' && c != '\r') {
            scif_stamp();
            g_mid_line = 1;
        }
        if (c == '\n')
            g_mid_line = 0;
    }
    scif_raw(c);
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
