#ifndef CART_H
#define CART_H
#include "hw.h"

/* Naomi ROM board PIO interface (G1): set the byte offset (bit 31 =
 * auto-advance enable), then each 16-bit read of ROM_DATA returns the
 * next word. Raw content is read as-is (encrypted games hash their
 * ciphertext — matching the MAME dumps). */
#define CART_ROM_OFFSETH    REG16(0xA05F7000)
#define CART_ROM_OFFSETL    REG16(0xA05F7004)
#define CART_ROM_DATA       REG16(0xA05F7008)

void cart_seek(u32 offset);
void cart_read(u32 offset, u8 *buf, u32 len);

/* 1 if something answers with plausible data (not stuck all-0/all-1) */
u32 cart_present(void);

/* Presence probe for one mask ROM: samples words spread over the chip's
 * address range. A chip that is missing, unseated or not driving the bus
 * answers a constant 0xFFFF (pull-ups) or 0x0000 (floating low) at every
 * sample. Returns 1 if the chip answers with varying data, 0 otherwise. */
u32 cart_ic_responds(u32 offset, u32 size);

/* Per-data-line statistics over the cartridge bus (16-bit PIO path).
 * Two independent reads of the same addresses are compared: any bit that
 * differs between them is an unstable line -- the signature of a tired
 * bus transceiver or a dirty edge connector, which a content checksum
 * alone cannot distinguish from bad ROM data. */
typedef struct {
    u32 words;          /* words sampled per read pass */
    u32 ones[16];       /* per line: number of 1s seen (stuck-line check) */
    u32 flaky[16];      /* per line: mismatches between the two reads */
} cart_pin_stats;

void cart_pin_scan(u32 offset, u32 len, cart_pin_stats *st);

#endif
