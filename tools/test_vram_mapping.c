#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define HW_H
typedef uint32_t u32;
#include "../src/vram_mapping.h"

/* Independent reconstruction of the BIOS byte-error accumulator, followed
 * by its 0x33333333/0xCCCCCCCC reduction and half-region selection. */
static u32 bios_slot(u32 word_index, u32 bit, u32 upper)
{
    u32 packed = 1u << ((4 * (word_index % 8) + bit / 8) % 32);
    u32 mask = (packed & 0x33333333u ? 1u : 0) |
               (packed & 0xCCCCCCCCu ? 2u : 0);
    return mask << (upper ? 2 : 0);
}
int main(void)
{
    const u32 base[4] = {0xA5000000,0xA5800000,0xA7000000,0xA7800000};
    const u32 ic[4][4] = {{16,18,20,22},{17,19,21,23},
                          {111,113,115,117},{112,114,116,118}};
    for (u32 region=0; region<4; region++)
        for (u32 upper=0; upper<2; upper++)
            for (u32 word=0; word<8; word++)
                for (u32 bit=0; bit<32; bit++) {
                    u32 addr=base[region]+upper*0x400000+word*4;
                    u32 expected=bios_slot(word,bit,upper);
                    assert(vram_chip_mask(addr,1u<<bit)==(expected<<(region==3?4:0)));
                    assert(vram_ic(addr,bit)==ic[region][upper*2+bit/16]);
                }
    assert(vram_ic(0xA5FFFFFC,8)==21 && vram_ic(0xA5FFFFFC,9)==21);
    assert(vram_chip_mask(0xA5FFFFFC,0x300)==4);
    assert(vram_chip_mask(0xA53FFFFC,0xFFFF0000)==2);
    assert(vram_chip_mask(0xA5400000,0xFFFF0000)==8);
    assert(vram_range_chip_mask(0xA5000000,0x800000,0x300)==5);
    assert(vram_range_chip_mask(0xA7000000,0x1000000,0x300)==0x55);
    assert(vram_chip_mask(0xAC000000,~0u)==0);
    assert(vram_ic(0xAA000000,8)==0);
    puts("BIOS VRAM mapping: all 32 bits, word parities, four regions and boundaries passed");
}
