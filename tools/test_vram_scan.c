#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LANG_EN
#include "../src/hw.h"
static u32 base, words, reads, ticks, last, abort_at, value, fault, random_mode;
static u32 stream;
static u32 read_word(u32 addr)
{
    assert(addr == base + reads * 4);
    assert(reads - last < 1024);
    u32 got = value;
    if (random_mode) {
        stream ^= stream << 13;
        stream ^= stream >> 17;
        stream ^= stream << 5;
        got = stream;
    }
    reads++;
    /* Fault only in the upper 4 MiB: exercise mapping after detail saturation. */
    if (addr >= base + 0x400000) got ^= fault;
    return got;
}
#define VRAM_READ(a) read_word(a)
#include "../src/vram_scan.c"
void scif_puts(const char *s) { assert(s); }
void progress_begin(const char *s, u32 n) { assert(s && n == words); }
void progress_tick(u32 n) { assert(n == reads); last = n; ticks++; }
u32 progress_aborted(void) { return abort_at && reads >= abort_at; }

static void run(u32 random, u32 mask, u32 stop)
{
    ram_result r = {0};
    base = 0xA5800000; words = 0x800000 / 4;
    reads = ticks = last = 0;
    random_mode = random; fault = mask; abort_at = stop;
    value = random ? 0 : 0x55555555;
    stream = 1;
    vram_locate(base, words, value, random, &r);
    assert(reads == (stop ? stop : words));
    assert(ticks == reads / 1024 + 1);
    assert(r.errors == (mask && !stop ? words / 2 : 0));
    assert(r.badbits == (stop ? 0 : mask));
    assert(r.vram_chips == (mask && !stop ? 4u : 0));
    if (mask && !stop) {
        assert(r.nfails == RAM_MAX_FAILS);
        assert(r.fail_addr[0] == 0xA5C00000);
    }
}
int main(void)
{
    run(0, 0, 0);
    run(0, 0x200, 0);
    run(0, 0x300, 0);
    run(1, 0, 0);
    run(1, 0x200, 0);
    run(1, 0x300, 0);
    run(0, 0x200, 2048);
    run(1, 0x200, 2048);
    puts("VRAM rescan: full-range D8/D9 faults, progress and abort OK");
    return 0;
}
