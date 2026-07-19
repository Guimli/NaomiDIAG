#ifndef SCIF_H
#define SCIF_H
#include "hw.h"

void scif_putc(char c);
void scif_puts(const char *s);
void scif_puthex(u32 v);        /* 8 hex digits */
void scif_putdec(u32 v);
void scif_flush(void);

#endif
