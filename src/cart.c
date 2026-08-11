#include "cart.h"

void cart_seek(u32 offset)
{
    /* bit31 = PIO auto-advance; bit29 = linear raw addressing on M2 carts
     * (otherwise the address is remapped in 4MB windows). Encryption
     * (bit30) stays 0 so we hash the raw mask-ROM content = MAME dump. */
    CART_ROM_OFFSETH = (u16)(((offset >> 16) & 0x1FFF) | 0xA000);
    CART_ROM_OFFSETL = (u16)offset;
}

void cart_read(u32 offset, u8 *buf, u32 len)
{
    cart_seek(offset);
    u32 i = 0;
    while (i + 1 < len) {
        u16 w = CART_ROM_DATA;
        buf[i++] = (u8)w;
        buf[i++] = (u8)(w >> 8);
    }
    if (i < len)
        buf[i] = (u8)CART_ROM_DATA;
}

u32 cart_ic_responds(u32 offset, u32 size)
{
    const u32 nsamples = 16;
    u32 step = size / nsamples;
    if (!step)
        step = 2;
    u16 first = 0;
    u32 varying = 0;
    for (u32 i = 0; i < nsamples; i++) {
        cart_seek(offset + i * step);
        u16 w = CART_ROM_DATA;
        if (i == 0)
            first = w;
        else if (w != first)
            varying = 1;
    }
    /* constant 0xFFFF / 0x0000 across the whole chip = nothing driving */
    if (!varying && (first == 0xFFFF || first == 0x0000))
        return 0;
    return 1;
}

void cart_pin_scan(u32 offset, u32 len, cart_pin_stats *st)
{
    u16 buf[256];                       /* 512 B, fits the OC-RAM stack */
    st->words = 0;
    for (u32 b = 0; b < 16; b++) {
        st->ones[b] = 0;
        st->flaky[b] = 0;
    }

    for (u32 done = 0; done + sizeof buf <= len; done += sizeof buf) {
        u32 addr = offset + done;

        cart_seek(addr);                /* first read pass */
        for (u32 i = 0; i < 256; i++)
            buf[i] = CART_ROM_DATA;

        cart_seek(addr);                /* second read, same addresses */
        for (u32 i = 0; i < 256; i++) {
            u16 w = CART_ROM_DATA;
            u16 diff = (u16)(w ^ buf[i]);
            for (u32 b = 0; b < 16; b++) {
                if (buf[i] & (1u << b))
                    st->ones[b]++;
                if (diff & (1u << b))
                    st->flaky[b]++;
            }
            st->words++;
        }
    }
}

u32 cart_present(void)
{
    u8 hdr[64];
    cart_read(0, hdr, sizeof hdr);
    u32 zeros = 0, ones = 0;
    for (u32 i = 0; i < sizeof hdr; i++) {
        if (hdr[i] == 0x00)
            zeros++;
        if (hdr[i] == 0xFF)
            ones++;
    }
    return (zeros != sizeof hdr && ones != sizeof hdr);
}
