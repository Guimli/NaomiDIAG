/* Raw DEFLATE (RFC 1951) decoder, written for the DIMM firmware images of the
 * local firmware-update build. Small and table-light: canonical Huffman
 * decoding by code length counts, no lookup tables, no allocation. It needs
 * the whole compressed stream in memory and writes into a caller buffer of
 * known size; out-of-range input fails instead of overrunning.
 * Host-tested against zlib (tools/test_inflate.sh). */
#include "inflate.h"

typedef struct {
    const u8 *in;
    u32 in_len, in_pos;
    u32 bitbuf, bitcnt;
    u8 *out;
    u32 out_len, out_pos;
    int err;
    struct inf_work_s *w;
} inf_state;

typedef struct {
    u16 count[16];      /* number of codes of each length */
    u16 symbol[320];    /* symbols ordered by code        */
} huff;

/* the four tables live in the caller's work area (main RAM on the Naomi):
 * the boot-time .bss is only the 4 KB of OC-RAM */
typedef struct inf_work_s { huff flit, fdist, dlit, ddist; } inf_work;

static u32 bits(inf_state *s, u32 need)
{
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) {
            s->err = INF_ERR_INPUT;
            return 0;
        }
        s->bitbuf |= (u32)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    u32 v = s->bitbuf & ((1u << need) - 1u);
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

static int build(huff *h, const u8 *len, u32 n)
{
    u16 offs[16];
    for (u32 i = 0; i < 16; i++)
        h->count[i] = 0;
    for (u32 i = 0; i < n; i++)
        h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (u32 i = 1; i < 15; i++)
        offs[i + 1] = offs[i] + h->count[i];
    for (u32 i = 0; i < n; i++)
        if (len[i])
            h->symbol[offs[len[i]]++] = (u16)i;
    return 0;
}

static int decode(inf_state *s, const huff *h)
{
    int code = 0, first = 0, index = 0;
    for (u32 l = 1; l < 16; l++) {
        code |= (int)bits(s, 1);
        if (s->err)
            return -1;
        int count = h->count[l];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = INF_ERR_DATA;
    return -1;
}

static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23,
    27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65,
    97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(inf_state *s, const huff *lit, const huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (s->err)
            return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_len) {
                s->err = INF_ERR_OUTPUT;
                return -1;
            }
            s->out[s->out_pos++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) {
                s->err = INF_ERR_DATA;
                return -1;
            }
            u32 len = len_base[sym] + bits(s, len_extra[sym]);
            int ds = decode(s, dist);
            if (s->err || ds < 0 || ds >= 30) {
                s->err = s->err ? s->err : INF_ERR_DATA;
                return -1;
            }
            u32 d = dist_base[ds] + bits(s, dist_extra[ds]);
            if (s->err)
                return -1;
            if (d > s->out_pos || s->out_pos + len > s->out_len) {
                s->err = d > s->out_pos ? INF_ERR_DATA : INF_ERR_OUTPUT;
                return -1;
            }
            for (u32 i = 0; i < len; i++, s->out_pos++)
                s->out[s->out_pos] = s->out[s->out_pos - d];
        }
    }
}

static int fixed(inf_state *s)
{
    huff *lit_ = &s->w->flit, *dist_ = &s->w->fdist;
#define lit (*lit_)
#define dist (*dist_)
    u8 l[288];
    u32 i;
    for (i = 0; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build(&lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(&dist, l, 30);
    return codes(s, &lit, &dist);
#undef lit
#undef dist
}

static int dynamic(inf_state *s)
{
    huff *lit_ = &s->w->dlit, *dist_ = &s->w->ddist;
#define lit (*lit_)
#define dist (*dist_)
    static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3,
                                  13, 2, 14, 1, 15 };
    u8 l[320];
    u32 nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) {
        s->err = s->err ? s->err : INF_ERR_DATA;
        return -1;
    }
    u32 i;
    for (i = 0; i < 19; i++)
        l[order[i]] = 0;
    for (i = 0; i < ncode; i++)
        l[order[i]] = (u8)bits(s, 3);
    build(&lit, l, 19);
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lit);
        if (s->err)
            return -1;
        if (sym < 16) {
            l[i++] = (u8)sym;
        } else {
            u32 rep;
            u8 v = 0;
            if (sym == 16) {
                if (i == 0) { s->err = INF_ERR_DATA; return -1; }
                v = l[i - 1];
                rep = 3 + bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + bits(s, 3);
            } else {
                rep = 11 + bits(s, 7);
            }
            if (s->err || i + rep > nlen + ndist) {
                s->err = s->err ? s->err : INF_ERR_DATA;
                return -1;
            }
            while (rep--)
                l[i++] = v;
        }
    }
    build(&lit, l, nlen);
    build(&dist, l + nlen, ndist);
    return codes(s, &lit, &dist);
#undef lit
#undef dist
}

u32 inflate_work_size(void) { return sizeof(inf_work); }

int inflate_raw(const u8 *in, u32 in_len, u8 *out, u32 out_len, u32 *produced,
                void *work)
{
    inf_state s;
    s.w = (inf_work *)work;
    s.in = in; s.in_len = in_len; s.in_pos = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.out_len = out_len; s.out_pos = 0;
    s.err = 0;
    u32 last;
    do {
        last = bits(&s, 1);
        u32 type = bits(&s, 2);
        if (s.err)
            break;
        if (type == 0) {
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_len) { s.err = INF_ERR_INPUT; break; }
            u32 len = s.in[s.in_pos] | (u32)s.in[s.in_pos + 1] << 8;
            u32 nlen = s.in[s.in_pos + 2] | (u32)s.in[s.in_pos + 3] << 8;
            s.in_pos += 4;
            if (len != (~nlen & 0xFFFFu)) { s.err = INF_ERR_DATA; break; }
            if (s.in_pos + len > s.in_len) { s.err = INF_ERR_INPUT; break; }
            if (s.out_pos + len > s.out_len) { s.err = INF_ERR_OUTPUT; break; }
            while (len--)
                s.out[s.out_pos++] = s.in[s.in_pos++];
        } else if (type == 1) {
            fixed(&s);
        } else if (type == 2) {
            dynamic(&s);
        } else {
            s.err = INF_ERR_DATA;
        }
    } while (!last && !s.err);
    if (produced)
        *produced = s.out_pos;
    return s.err;
}
