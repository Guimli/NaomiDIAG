#ifndef DIMM_FW_H
#define DIMM_FW_H
#ifdef HOST_TEST
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#else
#include "hw.h"
#endif

/* NetDIMM firmware images embedded by a LOCAL build only (make ... DIMMFW=1,
 * blob from tools/dimmfw_pack.py): one solid raw-LZMA stream holding every
 * slot image back to back, and a table naming them. A normal or published
 * build has none: dfw_count() == 0 and nothing can be written. */
typedef struct {
    u32  slot;          /* 0 = recovery/boot slot, 1 = main slot            */
    char label[20];     /* "4.01", "4.03 CD-R", ...                         */
    u32  out_off;       /* offset of the image in the decoded stream        */
    u32  rsv;
    u32  base;          /* entry this one patches, or 0xFFFFFFFF            */
    u32  npatch, poff;  /* patch words (offset << 8 | byte)                 */
    u32  check;         /* slot check word (last 4 bytes), names a version  */
} dfw_entry;

#define DIMM_SLOT_SIZE  0x100000u   /* 1 MB per slot, 2 slots in the flash */
u32 dimm_crc32_update(u32 crc, const u8 *p, u32 n);

u32 dfw_count(void);
const dfw_entry *dfw_get(u32 i);

/* Name of the version whose check word is `word` in `slot`, or 0. */
const char *dfw_name(u32 slot, u32 word);

/* Bytes of decoded stream entry i needs (the stream is decoded from its
 * start): the size of the `dec` buffer dfw_unpack wants. */
u32 dfw_need(u32 i);

#define DFW_OK          0
#define DFW_ERR_INDEX   1
#define DFW_ERR_DECODE  2
#define DFW_ERR_CRC     4   /* unpacked slot CRC-32 != 0         */
#define DFW_ERR_CHECK   5   /* check word differs from the table */

/* Decode the stream into dec (dfw_need(i) bytes) using work (lzma_work_size()
 * bytes), copy entry i's 1 MB slot image to out, apply its patch, check its
 * CRC and check word. */
int dfw_unpack(u32 i, u8 *out, u8 *dec, void *work);

#endif
