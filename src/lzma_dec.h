#ifndef LZMA_DEC_H
#define LZMA_DEC_H
#ifdef HOST_TEST
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#else
#include "hw.h"
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

#endif
