#ifndef RAMTEST_H
#define RAMTEST_H
#include "hw.h"

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
    u32 crc_w, crc_r;            /* PRNG pass: write-side / read-side CRC32 */
} ram_result;

void ram_result_clear(ram_result *r);

/* Walking-ones data bus test at a single address. Returns bitmask of
 * faulty data lines (0 = OK). */
u32 ram_test_databus(u32 addr);

/* Power-of-two address line test over [base, base+size). Returns bitmask
 * of faulty address lines (0 = OK). */
u32 ram_test_addrbus(u32 base, u32 size);

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
