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

#endif
