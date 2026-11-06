#ifndef DIMM_FW_H
#define DIMM_FW_H
#ifdef HOST_TEST
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#else
#include "hw.h"
#endif

/* NetDIMM firmware images embedded by a LOCAL build only (make ... AUDIO=0
 * DIMMFW=1, blob from tools/dimmfw_pack.py). A normal or published build has
 * none: dfw_count() == 0 and nothing can be written. */
typedef struct {
    u32  slot;          /* 0 = recovery/boot slot, 1 = main slot          */
    char label[20];     /* "4.01", "4.03 CD-R", ...                       */
    u32  off, len;      /* DEFLATE stream, offset from the blob start     */
    u32  base;          /* entry this one patches, or 0xFFFFFFFF          */
    u32  npatch, poff;  /* patch words (offset << 8 | byte)               */
    u32  check;         /* slot check word (last 4 bytes), names a version */
} dfw_entry;

u32 dfw_count(void);
const dfw_entry *dfw_get(u32 i);

/* Name of the version whose check word is `word` in `slot`, or 0. */
const char *dfw_name(u32 slot, u32 word);

#define DFW_OK          0
#define DFW_ERR_INDEX   1
#define DFW_ERR_INFLATE 2
#define DFW_ERR_SIZE    3
#define DFW_ERR_CRC     4   /* unpacked slot CRC-32 != 0       */
#define DFW_ERR_CHECK   5   /* check word differs from the table */

/* Unpack entry i into out (1 MB, followed by inflate_work_size() bytes of
 * scratch), apply its patch, check CRC and check word. */
int dfw_unpack(u32 i, u8 *out);

#endif
