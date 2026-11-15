#ifndef LZMA_DEC_H
#define LZMA_DEC_H
#ifdef HOST_TEST
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#else
#include "hw.h"
#endif

/* The decode path runs from CPU RAM: the ROM is linked for P2, never cached,
 * and every instruction fetched from the EPROM costs a slow G1 bus cycle --
 * the decode of one 1 MB image took 3 minutes from there. linker.ld keeps
 * .ramtext in the EPROM and links it for CPU RAM; ramtext_install()
 * (reloc.c) copies it there before use. */
#ifdef HOST_TEST
#define RAMTEXT
#else
#define RAMTEXT __attribute__((section(".ramtext")))
#endif

#define LZMA_OK         0
#define LZMA_ERR_INPUT  1   /* stream ends before out_len bytes          */
#define LZMA_ERR_DATA   2   /* invalid stream (distance before the start) */
#define LZMA_ERR_PROPS  3   /* lc/lp/pb out of range (lc + lp <= 4 here)  */

/* Decode a raw LZMA1 stream (no header) until out_len bytes are written;
 * the stream may go on (a prefix is enough). work: 4-byte aligned area of
 * lzma_work_size() bytes. The output buffer is the dictionary. */
u32 lzma_work_size(void);
int lzma_decode(const u8 *in, u32 in_len, u32 lc, u32 lp, u32 pb,
                u8 *out, u32 out_len, void *work);
/* Called with the bytes written so far every 16 KB of output (0 = none):
 * a decode of several MB takes a while, and the screen must show it is
 * moving. */
void lzma_set_progress(void (*cb)(u32 done));

#endif
