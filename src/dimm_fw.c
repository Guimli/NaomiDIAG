/* NetDIMM firmware images of a local build: table, unpack, checks.
 * See dimm_fw.h and tools/dimmfw_pack.py for the blob format ("DFW2"). */
#include "dimm_fw.h"
#include "lzma_dec.h"
#include "reloc.h"

extern const u8 dimmfw_blob[];
extern const u8 dimmfw_blob_end[];

typedef struct {
    char magic[4];
    u32 count, stream_off, stream_len, total_out, lc, lp, pb;
} dfw_hdr;

static const dfw_hdr *hdr(void)
{
    const dfw_hdr *h = (const dfw_hdr *)dimmfw_blob;
    if (dimmfw_blob_end - dimmfw_blob < (int)sizeof(dfw_hdr) ||
        h->magic[0] != 'D' || h->magic[1] != 'F' || h->magic[2] != 'W' ||
        h->magic[3] != '2')
        return 0;
    return h;
}

u32 dfw_count(void)
{
    const dfw_hdr *h = hdr();
    return h ? h->count : 0;
}

const dfw_entry *dfw_get(u32 i)
{
    if (i >= dfw_count())
        return 0;
    return (const dfw_entry *)(dimmfw_blob + sizeof(dfw_hdr) + 48 * i);
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

static const dfw_entry *data_entry(u32 i)
{
    const dfw_entry *e = dfw_get(i);
    if (e && e->base != 0xFFFFFFFFu)
        e = dfw_get(e->base);
    return (e && e->base == 0xFFFFFFFFu) ? e : 0;
}

u32 dfw_need(u32 i)
{
    const dfw_entry *d = data_entry(i);
    return d ? d->out_off + DIMM_SLOT_SIZE : 0;
}

static void (*g_dfw_cb)(u32 done);
void dfw_set_progress(void (*cb)(u32 done)) { g_dfw_cb = cb; }
u32 dfw_unpack_total(u32 i) { return dfw_need(i) + DIMM_SLOT_SIZE; }

/* 1 MB, word by word, from CPU RAM (an EPROM loop would cost seconds) */
static RAMTEXT __attribute__((noinline)) void copy_slot(u8 *out, const u8 *src)
{
    if ((((u32)out | (u32)src) & 3) == 0) {
        u32 *o = (u32 *)out;
        const u32 *s = (const u32 *)src;
        for (u32 k = 0; k < DIMM_SLOT_SIZE / 4; k++)
            o[k] = s[k];
    } else {
        for (u32 k = 0; k < DIMM_SLOT_SIZE; k++)
            out[k] = src[k];
    }
}

int dfw_unpack(u32 i, u8 *out, u8 *dec, void *work)
{
    const dfw_hdr *h = hdr();
    const dfw_entry *e = dfw_get(i), *d = data_entry(i);
    if (!h || !e || !d || dfw_need(i) > h->total_out)
        return DFW_ERR_INDEX;
    if (!ramtext_install())             /* the decoder and the CRC run there */
        return DFW_ERR_RAM;
    lzma_set_progress(g_dfw_cb);
    int le = lzma_decode(dimmfw_blob + h->stream_off, h->stream_len, h->lc,
                         h->lp, h->pb, dec, dfw_need(i), work);
    lzma_set_progress(0);
    if (le != LZMA_OK)
        return DFW_ERR_DECODE;
    copy_slot(out, dec + d->out_off);
    const u32 *pw = (const u32 *)(dimmfw_blob + e->poff);
    for (u32 k = 0; k < e->npatch; k++)
        out[pw[k] >> 8] = (u8)pw[k];
    u32 crc = 0xFFFFFFFFu;
    for (u32 k = 0; k < DIMM_SLOT_SIZE; k += 0x10000) {
        crc = dimm_crc32_update(crc, out + k, 0x10000);
        if (g_dfw_cb)
            g_dfw_cb(dfw_need(i) + k);
    }
    if (crc != 0)
        return DFW_ERR_CRC;
    u32 w = out[DIMM_SLOT_SIZE - 4] | (u32)out[DIMM_SLOT_SIZE - 3] << 8 |
            (u32)out[DIMM_SLOT_SIZE - 2] << 16 | (u32)out[DIMM_SLOT_SIZE - 1] << 24;
    if (w != e->check)
        return DFW_ERR_CHECK;
    return DFW_OK;
}

/* CRC-32 (0xEDB88320, no final XOR): the DIMM boot loader accepts a slot
 * whose CRC over the whole slot, this way, is 0 -- and so does its firmware
 * update, which refuses anything else. */
RAMTEXT u32 dimm_crc32_update(u32 crc, const u8 *p, u32 n)
{
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}
