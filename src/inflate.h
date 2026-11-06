#ifndef INFLATE_H
#define INFLATE_H
#ifdef HOST_TEST
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#else
#include "hw.h"
#endif

#define INF_OK          0
#define INF_ERR_INPUT   1   /* compressed stream ends early   */
#define INF_ERR_DATA    2   /* invalid stream                 */
#define INF_ERR_OUTPUT  3   /* would write past the out buffer */

/* Decode a raw DEFLATE stream (no zlib/gzip header). Returns INF_OK or an
 * INF_ERR_* code; *produced (if not NULL) gets the number of bytes written.
 * work: a 4-byte aligned area of inflate_work_size() bytes (Huffman tables). */
u32 inflate_work_size(void);
int inflate_raw(const u8 *in, u32 in_len, u8 *out, u32 out_len, u32 *produced,
                void *work);

#endif
