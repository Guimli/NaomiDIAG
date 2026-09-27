#ifndef VRAM_MAPPING_H
#define VRAM_MAPPING_H
#include "hw.h"

/* EPR-23608C RAM TEST: 4 MiB half of an 8 MiB region, then data half.
 * See docs/ADDRESS_MAP.md. These are BIOS IC assignments,
 * not proof that the RAM die rather than its signal path is defective.
 *
 * The Elan RAM (POLY, IC106/107/108S/109S) follows another rule in the same
 * BIOS: its byte errors are reduced with 0F0F0F0F / F0F0F0F0, which select
 * the even and the odd 32-bit word whole, and the upper half of the region
 * shifts the result by two. So a chip there is a word parity and a 16 MiB
 * half, and the data bit does not matter: each chip is 32 bits wide. */
static inline u32 elan_is_mapped(u32 addr)
{
    return addr >= 0xAA000000u && addr < 0xAC000000u;
}

static inline u32 vram_is_mapped(u32 addr)
{
    return (addr >= 0xA5000000u && addr < 0xA6000000u) ||
           (addr >= 0xA7000000u && addr < 0xA8000000u) ||
           elan_is_mapped(addr);
}

static inline u32 vram_chip_mask(u32 addr, u32 diff)
{
    if (!vram_is_mapped(addr) || !diff) return 0;
    if (elan_is_mapped(addr))
        return 1u << (((addr & 0x01000000u) ? 2 : 0) + ((addr & 4u) ? 1 : 0));
    u32 slot = (addr & 0x00400000u) ? 2 : 0;
    /* B is tested as one 16 MiB region; A uses two separate 8 MiB tests. */
    if (addr >= 0xA7800000u) slot += 4;
    return ((diff & 0xFFFFu ? 1u : 0) | (diff >> 16 ? 2u : 0)) << slot;
}

static inline u32 vram_ic(u32 addr, u32 data_bit)
{
    if (!vram_is_mapped(addr) || data_bit >= 32) return 0;
    if (elan_is_mapped(addr))
        return 106u + ((addr & 0x01000000u) ? 2u : 0u) + ((addr & 4u) ? 1u : 0u);
    u32 first = addr < 0xA6000000u ? 16u : 111u;
    if (addr & 0x00800000u) first++;
    return first + ((addr & 0x00400000u) ? 4u : 0u) +
           (data_bit >= 16 ? 2u : 0u);
}

/* Conservative candidates when a fast-pass mismatch cannot be relocated. */
static inline u32 vram_range_chip_mask(u32 base, u32 len, u32 diff)
{
    u32 mask = 0;
    while (len) {
        /* both word parities: they are separate chips in the Elan RAM */
        mask |= vram_chip_mask(base, diff) | vram_chip_mask(base + 4, diff);
        u32 chunk = 0x00400000u - (base & 0x003FFFFFu);
        if (chunk > len) chunk = len;
        base += chunk;
        len -= chunk;
    }
    return mask;
}
#endif
