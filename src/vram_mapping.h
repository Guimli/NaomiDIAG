#ifndef VRAM_MAPPING_H
#define VRAM_MAPPING_H
#include "hw.h"

/* EPR-23608C RAM TEST: 4 MiB half of an 8 MiB region, then data half.
 * See docs/ADDRESS_MAP.md. These are BIOS IC assignments,
 * not proof that the RAM die rather than its signal path is defective. */
static inline u32 vram_is_mapped(u32 addr)
{
    return (addr >= 0xA5000000u && addr < 0xA6000000u) ||
           (addr >= 0xA7000000u && addr < 0xA8000000u);
}

static inline u32 vram_chip_mask(u32 addr, u32 diff)
{
    if (!vram_is_mapped(addr)) return 0;
    u32 slot = (addr & 0x00400000u) ? 2 : 0;
    /* B is tested as one 16 MiB region; A uses two separate 8 MiB tests. */
    if (addr >= 0xA7800000u) slot += 4;
    return ((diff & 0xFFFFu ? 1u : 0) | (diff >> 16 ? 2u : 0)) << slot;
}

static inline u32 vram_ic(u32 addr, u32 data_bit)
{
    if (!vram_is_mapped(addr) || data_bit >= 32) return 0;
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
        mask |= vram_chip_mask(base, diff);
        u32 chunk = 0x00400000u - (base & 0x003FFFFFu);
        if (chunk > len) chunk = len;
        base += chunk;
        len -= chunk;
    }
    return mask;
}
#endif
