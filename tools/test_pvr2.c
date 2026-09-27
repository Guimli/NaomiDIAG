/* Host regression harness for the production MMIO sequence.
 * Run: cc -std=c11 -Wall -Wextra -Werror tools/test_pvr2.c -o /tmp/test_pvr2
 *      /tmp/test_pvr2
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Substitute only hardware access; execute the production C unchanged. */
#define HW_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u32 read_mmio(u32 addr);
static void write_mmio(u32 addr, u32 value);
static u32 *board_reg(u32 addr);
#define REG32(a) (*board_reg(a))
#define PVR2_READ(a) read_mmio(a)
#define PVR2_WRITE(a, v) write_mmio(a, v)
#include "../src/pvr2.c"
#include "../src/board.c"

enum { GOOD, MIRROR_A, MIRROR_HALF, OPEN_B, BROADCAST_AB, BROADCAST_BA,
       CONTROL_STUCK, NO_ELAN, NO_PVR, BAD_CELL, CROSS_HALF, BAD_A };
static int mode;
static u32 mem[4][4], original[4][4], ctl, writes, b_accesses;
static u32 board_regs[5];
static u32 id_reads;

static u32 *board_reg(u32 addr)
{
    switch (addr) {
    case 0xFF000030: return &board_regs[0];
    case 0xA05F8000: return &board_regs[1];
    case 0xA05F8004: return &board_regs[2];
    case 0xA8800000: id_reads++; return &board_regs[3];
    case 0xA8800004: return &board_regs[4];
    default: assert(0); return 0;
    }
}

static int decode(u32 addr, u32 *slot)
{
    u32 base[] = { VRAM_TEX0_BASE, VRAM_TEX1_BASE,
                   VRAM_PVRB_BASE, VRAM_PVRB_BASE + 0x800000 };
    u32 offsets[] = { 0, 4, 0x1000, 0x7ffffc };
    for (int i = 0; i < 4; i++)
        for (u32 j = 0; j < 4; j++)
            if (addr == base[i] + offsets[j]) {
                *slot = j;
                return i;
            }
    return -1;
}

static int physical(int bank)
{
    if (mode == MIRROR_A && bank >= 2) return bank - 2;
    if (mode == MIRROR_HALF && bank == 3) return 2;
    if (mode == CROSS_HALF && bank >= 2) return 3 - bank;
    return bank;
}

static u32 read_mmio(u32 addr)
{
    if (addr == 0xA8800000) return mode == NO_ELAN ? 0 : 0xE1AD0000;
    if (addr == 0xA8800010) return ctl;
    if (addr == 0xA25F8000) {
        assert((ctl & 7) == 6); /* ID read must follow route enable. */
        b_accesses++;
        return mode == NO_PVR ? 0 : 0x17FD11DB;
    }
    u32 slot;
    int bank = decode(addr, &slot);
    assert(bank >= 0);
    if (bank >= 2) {
        b_accesses++;
        assert((ctl & 7) == 6);
        if (mode == OPEN_B) return 0xFFFFFFFF;
    }
    u32 value = mem[physical(bank)][slot];
    if (mode == BAD_A && bank == 1 && slot == 3) value |= 0x300u;
    return value;
}

static void write_mmio(u32 addr, u32 value)
{
    writes++;
    if (addr == 0xA8800010) {
        if (mode != CONTROL_STUCK) ctl = value;
        return;
    }
    if (addr == 0xA8800014) { assert((ctl & 7) == 6); return; }
    if (addr >= 0xA25F8000 && addr < 0xA25F8100) {
        assert((ctl & 7) == 6);
        b_accesses++;
        return;
    }
    u32 slot;
    int bank = decode(addr, &slot);
    assert(bank >= 0);
    if (bank >= 2) {
        assert((ctl & 7) == 6);
        if (mode == OPEN_B) return;
    }
    if (mode == BAD_CELL && bank == 2 && slot == 2) value &= ~1u;
    mem[physical(bank)][slot] = value;
    if (mode == BROADCAST_AB && bank < 2) mem[bank + 2][slot] = value;
    if (mode == BROADCAST_BA && bank >= 2) mem[bank - 2][slot] = value;
}

int main(void)
{
    for (mode = GOOD; mode <= BAD_A; mode++) {
        ctl = 0x80 | 1; /* Disabled B plus broadcast, preserve unrelated bits. */
        writes = b_accesses = 0;
        for (u32 i = 0; i < 4; i++)
            for (u32 j = 0; j < 4; j++) mem[i][j] = (i * 100 + j) * 2;
        memcpy(original, mem, sizeof(mem));
        pvr2_access result = pvr2_prepare();
        pvr2_access expected = mode == GOOD ? PVR2_READY :
            mode == CONTROL_STUCK ? PVR2_CONTROL :
            mode == NO_ELAN ? PVR2_NO_ELAN :
            mode == NO_PVR ? PVR2_NO_PVR :
            (mode == BAD_A || mode == BROADCAST_BA) ? PVR2_A_REFERENCE : PVR2_MAPPING;
        assert(result == expected);
        u32 count, mismatches = 0;
        const pvr2_probe_sample *samples = pvr2_probe_samples(&count);
        assert(count == ((result == PVR2_READY || result == PVR2_MAPPING ||
                          result == PVR2_A_REFERENCE) ? 8u : 0u));
        for (u32 i = 0; i < count; i++) {
            if (samples[i].expected != samples[i].observed) mismatches++;
            if (mode == OPEN_B && i % 4 >= 2)
                assert(samples[i].observed == 0xFFFFFFFFu);
            if (mode == BAD_CELL)
                assert((samples[i].addr & 0x7FFFFFu) == 0x1000u);
        }
        assert((mismatches != 0) == (result == PVR2_MAPPING || result == PVR2_A_REFERENCE));
        if (mode == NO_ELAN) assert(writes == 0);
        if (mode == NO_ELAN || mode == CONTROL_STUCK) assert(b_accesses == 0);
        if (mode != NO_ELAN && mode != CONTROL_STUCK) assert(ctl == 0x86);
        /* Healthy RAM and aliases must preserve all physical cells. A
         * forced one-way broadcast may make full restoration impossible. */
        if (mode != BROADCAST_AB && mode != BROADCAST_BA && mode != BAD_A)
            assert(memcmp(original, mem, sizeof(mem)) == 0);
    }
    for (u32 present = 0; present < 2; present++) {
        board_regs[1] = 0x17FD11DB;
        board_regs[3] = present ? 0xE1AD0000 : 0;
        id_reads = 0;
        board_info b;
        board_detect(&b);
        assert(b.type == (CFG_BOARD_MODEL == 2 ? BOARD_NAOMI2 :
            CFG_BOARD_MODEL == 1 ? BOARD_NAOMI1 :
            present ? BOARD_NAOMI2 : BOARD_NAOMI1));
        assert(id_reads == (CFG_BOARD_MODEL == 1 ? 0u : 1u));
    }
    puts("PVR-B: 12 access scenarios and board identification passed");
    return 0;
}
