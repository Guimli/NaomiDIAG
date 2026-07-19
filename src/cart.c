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
