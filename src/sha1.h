#ifndef SHA1_H
#define SHA1_H
#include "hw.h"

typedef struct {
    u32 h[5];
    u8  buf[64];
    u32 buflen;
    u32 len_lo, len_hi;         /* byte count split (no 64-bit ops) */
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const u8 *data, u32 len);
void sha1_final(sha1_ctx *c, u8 out[20]);

/* Hash len bytes (a multiple of 64, whole blocks only so far) read straight
 * from the cartridge port, which must already be positioned (cart_seek).
 * Runs the assembly rounds, relocated into CPU RAM when possible. */
void sha1_update_cart(sha1_ctx *c, u32 len);

#endif
