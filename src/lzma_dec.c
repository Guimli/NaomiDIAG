/* Raw LZMA1 decoder for the DIMM firmware blob of a local build.
 *
 * Written for NaomiDIAG from the LZMA format description: range coder,
 * literal / match / rep coding, 12-state machine, length and distance trees.
 * The whole output stays in one linear buffer (the blob is decoded from its
 * start up to the image wanted), so the dictionary is that buffer and no
 * circular window is needed. Probabilities live in a caller work area (main
 * RAM). Every distance and every write is bounds-checked: a damaged stream
 * fails, it never writes outside the buffer.
 * Host-tested against Python's lzma (tools/test_lzma.sh). */
#include "lzma_dec.h"

#define PROB_INIT       1024u
#define N_STATES        12
#define POS_STATES_MAX  16

typedef u16 prob;

typedef struct {
    prob choice, choice2;
    prob low[POS_STATES_MAX][8];
    prob mid[POS_STATES_MAX][8];
    prob high[256];
} len_probs;

typedef struct lzma_work_s {
    prob is_match[N_STATES][POS_STATES_MAX];
    prob is_rep[N_STATES], is_rep_g0[N_STATES], is_rep_g1[N_STATES], is_rep_g2[N_STATES];
    prob is_rep0_long[N_STATES][POS_STATES_MAX];
    prob pos_slot[4][64];
    prob pos_spec[1 + 114];
    prob align[16];
    len_probs match_len, rep_len;
    prob literal[0x300 << 4];   /* up to lc + lp = 4 */
} lzma_work;

u32 lzma_work_size(void) { return sizeof(lzma_work); }

typedef struct {
    const u8 *in;
    u32 in_len, in_pos;
    u32 range, code;
    int err;
} rc_t;

static u8 rc_byte(rc_t *rc)
{
    if (rc->in_pos >= rc->in_len) {
        rc->err = LZMA_ERR_INPUT;
        return 0;
    }
    return rc->in[rc->in_pos++];
}

static void rc_norm(rc_t *rc)
{
    if (rc->range < (1u << 24)) {
        rc->range <<= 8;
        rc->code = (rc->code << 8) | rc_byte(rc);
    }
}

static u32 rc_bit(rc_t *rc, prob *p)
{
    u32 bound = (rc->range >> 11) * *p;
    u32 b;
    if (rc->code < bound) {
        rc->range = bound;
        *p = (prob)(*p + ((2048u - *p) >> 5));
        b = 0;
    } else {
        rc->range -= bound;
        rc->code -= bound;
        *p = (prob)(*p - (*p >> 5));
        b = 1;
    }
    rc_norm(rc);
    return b;
}

static u32 rc_direct(rc_t *rc, u32 n)
{
    u32 r = 0;
    while (n--) {
        rc->range >>= 1;
        u32 b = rc->code >= rc->range;
        if (b)
            rc->code -= rc->range;
        r = (r << 1) | b;
        rc_norm(rc);
    }
    return r;
}

static u32 tree(rc_t *rc, prob *p, u32 nbits)
{
    u32 m = 1;
    for (u32 i = 0; i < nbits; i++)
        m = (m << 1) | rc_bit(rc, &p[m]);
    return m - (1u << nbits);
}

static u32 tree_rev(rc_t *rc, prob *p, u32 nbits)
{
    u32 m = 1, sym = 0;
    for (u32 i = 0; i < nbits; i++) {
        u32 b = rc_bit(rc, &p[m]);
        m = (m << 1) | b;
        sym |= b << i;
    }
    return sym;
}

static u32 dec_len(rc_t *rc, len_probs *l, u32 ps)
{
    if (!rc_bit(rc, &l->choice))
        return tree(rc, l->low[ps], 3);
    if (!rc_bit(rc, &l->choice2))
        return 8 + tree(rc, l->mid[ps], 3);
    return 16 + tree(rc, l->high, 8);
}

static void (*g_progress)(u32 done);
void lzma_set_progress(void (*cb)(u32 done)) { g_progress = cb; }

int lzma_decode(const u8 *in, u32 in_len, u32 lc, u32 lp, u32 pb,
                u8 *out, u32 out_len, void *work)
{
    lzma_work *w = (lzma_work *)work;
    if (lc > 8 || lp > 4 || pb > 4 || lc + lp > 4)
        return LZMA_ERR_PROPS;

    /* every probability to one half */
    prob *pp = (prob *)w;
    u32 np = (u32)(((u8 *)&w->literal[0x300u << (lc + lp)]) - (u8 *)w) / sizeof(prob);
    for (u32 i = 0; i < np; i++)
        pp[i] = PROB_INIT;

    rc_t rc = { in, in_len, 0, 0xFFFFFFFFu, 0, 0 };
    if (rc_byte(&rc) != 0)
        return LZMA_ERR_DATA;
    for (int i = 0; i < 4; i++)
        rc.code = (rc.code << 8) | rc_byte(&rc);

    u32 pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
    u32 state = 0, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    u32 pos = 0;

    u32 next_cb = 0x4000;
    while (pos < out_len) {
        if (rc.err)
            return rc.err;
        if (pos >= next_cb) {
            next_cb = pos + 0x4000;
            if (g_progress)
                g_progress(pos);
        }
        u32 ps = pos & pb_mask;
        if (!rc_bit(&rc, &w->is_match[state][ps])) {
            /* literal */
            u32 prev = pos ? out[pos - 1] : 0;
            prob *lit = &w->literal[0x300u * (((pos & lp_mask) << lc) + (prev >> (8 - lc)))];
            u32 sym = 1;
            if (state >= 7) {
                if (rep0 >= pos)
                    return LZMA_ERR_DATA;
                u32 mb = out[pos - rep0 - 1];
                while (sym < 0x100) {
                    u32 mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    u32 b = rc_bit(&rc, &lit[0x100 + (mbit << 8) + sym]);
                    sym = (sym << 1) | b;
                    if (b != mbit)
                        break;
                }
            }
            while (sym < 0x100)
                sym = (sym << 1) | rc_bit(&rc, &lit[sym]);
            out[pos++] = (u8)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }

        u32 len;
        if (!rc_bit(&rc, &w->is_rep[state])) {
            /* new match */
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = dec_len(&rc, &w->match_len, ps);
            state = state < 7 ? 7 : 10;
            u32 ls = len < 4 ? len : 3;     /* len here is length - 2 */
            u32 slot = tree(&rc, w->pos_slot[ls], 6);
            if (slot < 4) {
                rep0 = slot;
            } else {
                u32 nd = (slot >> 1) - 1;
                rep0 = (2u | (slot & 1)) << nd;
                if (slot < 14) {
                    rep0 += tree_rev(&rc, &w->pos_spec[rep0 - slot], nd);
                } else {
                    rep0 += rc_direct(&rc, nd - 4) << 4;
                    rep0 += tree_rev(&rc, w->align, 4);
                    if (rep0 == 0xFFFFFFFFu)
                        break;              /* end marker */
                }
            }
        } else {
            if (!rc_bit(&rc, &w->is_rep_g0[state])) {
                if (!rc_bit(&rc, &w->is_rep0_long[state][ps])) {
                    /* short rep: one byte at rep0 */
                    if (rep0 >= pos)
                        return LZMA_ERR_DATA;
                    state = state < 7 ? 9 : 11;
                    out[pos] = out[pos - rep0 - 1];
                    pos++;
                    continue;
                }
            } else {
                u32 d;
                if (!rc_bit(&rc, &w->is_rep_g1[state])) {
                    d = rep1;
                } else {
                    if (!rc_bit(&rc, &w->is_rep_g2[state])) {
                        d = rep2;
                    } else {
                        d = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = d;
            }
            len = dec_len(&rc, &w->rep_len, ps);
            state = state < 7 ? 8 : 11;
        }
        len += 2;
        if (rep0 >= pos)
            return LZMA_ERR_DATA;
        if (len > out_len - pos)
            len = out_len - pos;            /* stop exactly at out_len */
        for (u32 i = 0; i < len; i++, pos++)
            out[pos] = out[pos - rep0 - 1];
    }
    if (rc.err)
        return rc.err;
    return pos == out_len ? LZMA_OK : LZMA_ERR_INPUT;
}
