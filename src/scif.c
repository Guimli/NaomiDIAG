/* SCIF console output. The SCIF was initialised in crt0.S before any RAM
 * existed; these helpers only need a few bytes of stack (OC-RAM). */
#include "scif.h"

void scif_putc(char c)
{
    while (!(SCFSR2 & SCFSR2_TDFE))
        ;
    SCFTDR2 = (u8)c;
    SCFSR2 &= (u16)~(SCFSR2_TDFE | SCFSR2_TEND);
}

void scif_flush(void)
{
    while (!(SCFSR2 & SCFSR2_TEND))
        ;
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
