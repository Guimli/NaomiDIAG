#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HW_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u32 memory[64], mode, aliases, reads, bits, local_errors;
static u32 physical(u32 a)
{
    if (mode == 1) return a & ~16u;
    if (mode == 4 && (a & 48u)) return a | 48u;
    return a;
}
static u32 read_word(u32 a)
{
    assert(a < 256 && !(a & 3));
    reads++;
    u32 value = memory[physical(a)/4];
    if (mode == 2 && a == 252) value |= 0x300;
    if (mode == 3 && reads % 13 == 0) value ^= 1;
    if (mode == 5 && (value & 0x500u)) value |= 0x500u;
    return value;
}
static void write_word(u32 a, u32 v)
{
    assert(a < 256 && !(a & 3));
    memory[physical(a)/4] = v;
}
#define VRAM_READ(a) read_word(a)
#define VRAM_WRITE(a,v) write_word(a,v)
#include "../src/vram_diag.c"
static void report(const vram_address_event *e)
{
    if (e->alias) { aliases++; bits |= e->cpu_bits; }
    else local_errors++;
}
int main(void)
{
    for (mode = 0; mode < 6; mode++) {
        memset(memory, 0, sizeof(memory));
        aliases = reads = bits = local_errors = 0;
        u32 failures = vram_address_diagnose(0, 256, report);
        assert((failures != 0) == (mode != 0));
        if (mode == 1) { assert(aliases == 2); assert(bits == 16); }
        else if (mode == 4) { assert(aliases); assert(bits == 48); }
        else assert(aliases == 0);
        if (mode == 2 || mode == 3 || mode == 5) assert(local_errors);
    }
    puts("VRAM diagnosis: healthy, A4 alias, endpoint fault, intermittent reads, A4/A5 coupling, D8/D10 coupling passed");
}
