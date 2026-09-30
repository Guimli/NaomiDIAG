#ifndef RAMTEST_H
#define RAMTEST_H

#include "hw.h"

/* CRC-32 accumulator shared by every memory test, one table step per byte.
 * Inline rather than a call: at one invocation per tested word the call
 * overhead alone would be a measurable share of the loop. */
extern const u32 crc32_tab8[256];

static inline u32 crc32_word(u32 crc, u32 w)
{
    crc ^= w;
    crc = (crc >> 8) ^ crc32_tab8[(u8)crc];
    crc = (crc >> 8) ^ crc32_tab8[(u8)crc];
    crc = (crc >> 8) ^ crc32_tab8[(u8)crc];
    crc = (crc >> 8) ^ crc32_tab8[(u8)crc];
    return crc;
}

/* hand-written inner loops, see ramtest_fast.S */

/* Shared with the assembly: the field ORDER IS PART OF THE ABI --
 * ramtest_fast.S addresses these by offset 0/4/8/12. */
typedef struct {
    u32 x;              /* PRNG state, in and out                        */
    u32 crc_w;          /* CRC of the stream we meant to write           */
    u32 crc_r;          /* CRC of the stream actually read back          */
    u32 diff_e;         /* differences on even-address words: chips 1,2  */
    u32 diff_o;         /* differences on odd-address words:  chips 3,4  */
} prng_ctx;

void ram_prng_fill_fast(u32 *base, u32 nwords, prng_ctx *c);
/* npairs counts PAIRS of words: the loop is unrolled two ways so that each
 * word's address parity, and therefore its chip, is known at assembly time. */
void ram_prng_verify_fast(const u32 *base, u32 npairs, prng_ctx *c);
void ram_fill_fast(u32 *base, u32 nblocks16, u32 pattern);
u32  ram_verify_fast(u32 *base, u32 nblocks8, u32 pattern, u32 *diff_odd);

/* Locating failing words after a verify pass saw a difference: see
 * ramtest_fast.S. Offsets MUST match the assembly. */
typedef struct {
    u32 x;                       /* pattern, or PRNG state before the word */
    u32 ign_e, ign_o;            /* bits to pass over, per word parity     */
    u32 skipped;                 /* mismatches passed over (counted)       */
    u32 exp, got;                /* the word reported                      */
} find_ctx;
u32 ram_find_pat_fast(const u32 *p, u32 n, find_ctx *c);
u32 ram_find_prng_fast(const u32 *p, u32 n, find_ctx *c);
/* SHA-1 of nblocks x 64 bytes read from the cartridge port (sha1.c) */
void sha1_pio_blocks(u32 h[5], u32 nblocks, u32 w[80]);
void sha1_mem_blocks(u32 h[5], u32 nblocks, u32 w[80], const u32 *src);

#define RAM_MAX_FAILS 8

typedef struct {
    u32 errors;                  /* total mismatching words             */
    u32 badbits;                 /* OR of all (expected ^ read) bits    */
    u32 badbits_e;               /* same, even 32-bit words (A2=0)      */
    u32 badbits_o;               /* same, odd 32-bit words (A2=1)       */
    u32 nfails;                  /* recorded failure details            */
    u32 fail_addr[RAM_MAX_FAILS];
    u32 fail_exp [RAM_MAX_FAILS];
    u32 fail_got [RAM_MAX_FAILS];
    u32 crc_w, crc_r;
                                 /* PRNG pass: write-side / read-side CRC32 */
    u32 unpinned;                /* seen while verifying, gone on re-scan    */
    u32 vram_chips;              /* BIOS slots, independent of word parity */
} ram_result;

void ram_result_clear(ram_result *r);

/* Locate the failing words of [base, base + 4n) after a verify pass saw a
 * difference: value is the pattern, or the PRNG seed when random is set.
 * The first RAM_MAX_FAILS words are recorded in full; after that only a
 * word bringing a data bit not yet seen in its 4 MiB half and on its word
 * parity (possibly another chip) is, the rest being counted. Runs the
 * relocated scan when there is one. label: progress label and serial
 * notice. Shared by CPU RAM and VRAM (vram_scan.c). */
void ram_locate(u32 base, u32 n, u32 value, u32 random, ram_result *r,
                const char *label, const char *bar);

/* Walking-ones data bus test at a single address. Returns bitmask of
 * faulty data lines (0 = OK). */
u32 ram_test_databus(u32 addr);

/* Power-of-two address line test over [base, base+size). Returns bitmask
 * of failing address-test steps (0 = OK). Data faults can set this mask;
 * it is not proof of defective address lines. */
u32 ram_test_addrbus(u32 base, u32 size);

/* Address lines, chip by chip. The same walk as ram_test_addrbus, but a
 * step only counts when a whole 16-bit lane of some cell reads back the
 * value written at the other address -- the signature of two addresses
 * landing on one cell. A data fault flips a bit or two and is not taken
 * for an address line. For each byte-address bit b (from lo_bit, 1 << b
 * below size), out[b] gets the chips it aliased on, as chip(addr, xor)
 * names them from the cell and the lanes that flipped. passes = 2 repeats
 * the walk on the odd 32-bit word (base + 4), for memories where word
 * parity picks the chip. g2 paces every access for the sound RAM. */
typedef u32 (*addr_chip_fn)(u32 addr, u32 lanes_xor);
void ram_addr_alias(u32 base, u32 size, u32 lo_bit, u32 passes, u32 g2,
                    addr_chip_fn chip, u32 out[32]);

/* Fixed-pattern pass: fill [base, base+len) with pattern, verify. */
void ram_test_pattern(u32 base, u32 len, u32 pattern, ram_result *r);

/* PRNG pass (xorshift32, given seed): write stream while accumulating a
 * CRC32 in a register variable, re-read regenerating the stream, compare
 * word-by-word and compare read-side CRC32 with write-side CRC32. */
void ram_test_prng(u32 base, u32 len, u32 seed, ram_result *r);

/* Numbered RAM components, distinguishable electrically from the SH4:
 * the SDRAM bus is 64-bit wide, a 32-bit access hits the low or high half
 * depending on address bit 2. Component numbers (bit n-1 set in the mask):
 *   #1 = D0-D15  even word   #2 = D16-D31 even word
 *   #3 = D0-D15  odd word    #4 = D16-D31 odd word
 * The number -> silkscreen IC mapping table is built later on real HW. */
static inline u32 ram_comp_mask(const ram_result *r)
{
    u32 m = 0;
    if (r->badbits_e & 0x0000FFFF) m |= 1u << 0;
    if (r->badbits_e & 0xFFFF0000) m |= 1u << 1;
    if (r->badbits_o & 0x0000FFFF) m |= 1u << 2;
    if (r->badbits_o & 0xFFFF0000) m |= 1u << 3;
    return m;
}

#endif
