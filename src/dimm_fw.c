/* NetDIMM firmware images of a local build: table, unpack, checks.
 * See dimm_fw.h and tools/dimmfw_pack.py for the blob format. */
#include "dimm_fw.h"
#include "dimm_flash.h"
#include "inflate.h"

extern const u8 dimmfw_blob[];
extern const u8 dimmfw_blob_end[];

static const u8 *blob_hdr(void)
{
    const u8 *b = dimmfw_blob;
    if (dimmfw_blob_end - dimmfw_blob < 8 ||
        b[0] != 'D' || b[1] != 'F' || b[2] != 'W' || b[3] != '1')
        return 0;
    return b;
}

u32 dfw_count(void)
{
    const u8 *b = blob_hdr();
    return b ? *(const u32 *)(b + 4) : 0;
}

const dfw_entry *dfw_get(u32 i)
{
    if (i >= dfw_count())
        return 0;
    return (const dfw_entry *)(dimmfw_blob + 8 + 48 * i);
}

const char *dfw_name(u32 slot, u32 word)
{
    for (u32 i = 0; i < dfw_count(); i++) {
        const dfw_entry *e = dfw_get(i);
        if (e->slot == slot && e->check == word)
            return e->label;
    }
    return 0;
}

int dfw_unpack(u32 i, u8 *out)
{
    const dfw_entry *e = dfw_get(i);
    if (!e)
        return DFW_ERR_INDEX;
    const dfw_entry *src = e;
    if (e->base != 0xFFFFFFFFu) {
        src = dfw_get(e->base);
        if (!src || src->base != 0xFFFFFFFFu)
            return DFW_ERR_INDEX;
    }
    u32 got = 0;
    /* Huffman tables right after the 1 MB slot buffer, in main RAM */
    if (inflate_raw(dimmfw_blob + src->off, src->len, out, DIMM_SLOT_SIZE, &got,
                    out + DIMM_SLOT_SIZE))
        return DFW_ERR_INFLATE;
    if (got != DIMM_SLOT_SIZE)
        return DFW_ERR_SIZE;
    const u32 *pw = (const u32 *)(dimmfw_blob + e->poff);
    for (u32 k = 0; k < e->npatch; k++)
        out[pw[k] >> 8] = (u8)pw[k];
    if (dimm_crc32_update(0xFFFFFFFFu, out, DIMM_SLOT_SIZE) != 0)
        return DFW_ERR_CRC;
    u32 w = out[DIMM_SLOT_SIZE - 4] | (u32)out[DIMM_SLOT_SIZE - 3] << 8 |
            (u32)out[DIMM_SLOT_SIZE - 2] << 16 | (u32)out[DIMM_SLOT_SIZE - 1] << 24;
    if (w != e->check)
        return DFW_ERR_CHECK;
    return DFW_OK;
}
