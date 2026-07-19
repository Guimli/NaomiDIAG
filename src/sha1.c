/* Compact SHA-1 (FIPS 180-1). 32-bit arithmetic only. */
#include "sha1.h"

static inline u32 rol(u32 v, u32 n) { return (v << n) | (v >> (32 - n)); }

void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->buflen = 0;
    c->len_lo = 0;
    c->len_hi = 0;
}

static void sha1_block(sha1_ctx *c, const u8 *p)
{
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
               ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = c->h[0], b = c->h[1], d2 = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & d2) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ d2 ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & d2) | (b & d) | (d2 & d); k = 0x8F1BBCDC; }
        else             { f = b ^ d2 ^ d;                   k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = d2; d2 = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d2; c->h[3] += d; c->h[4] += e;
}

void sha1_update(sha1_ctx *c, const u8 *data, u32 len)
{
    u32 nl = c->len_lo + len;
    if (nl < c->len_lo)
        c->len_hi++;
    c->len_lo = nl;

    while (len) {
        u32 take = 64 - c->buflen;
        if (take > len)
            take = len;
        for (u32 i = 0; i < take; i++)
            c->buf[c->buflen + i] = data[i];
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == 64) {
            sha1_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

void sha1_final(sha1_ctx *c, u8 out[20])
{
    u32 bits_lo = c->len_lo << 3;
    u32 bits_hi = (c->len_hi << 3) | (c->len_lo >> 29);
    u8 pad = 0x80;
    sha1_update(c, &pad, 1);
    u8 z = 0;
    while (c->buflen != 56)
        sha1_update(c, &z, 1);
    u8 lenb[8] = {
        (u8)(bits_hi >> 24), (u8)(bits_hi >> 16),
        (u8)(bits_hi >> 8),  (u8)bits_hi,
        (u8)(bits_lo >> 24), (u8)(bits_lo >> 16),
        (u8)(bits_lo >> 8),  (u8)bits_lo
    };
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (u8)(c->h[i] >> 24);
        out[i * 4 + 1] = (u8)(c->h[i] >> 16);
        out[i * 4 + 2] = (u8)(c->h[i] >> 8);
        out[i * 4 + 3] = (u8)c->h[i];
    }
}
