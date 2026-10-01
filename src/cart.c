#include "cart.h"
#include "progress.h"
#include "dimm.h"           /* SB_GD* */
#include "timer.h"

/* G1 bus timing: the values the original BIOS (epr-21576h) leaves before
 * it touches the ROM board, cartridge or DIMM alike, logged from it
 * running under MAME. The ROM board lives where the Dreamcast has its
 * GD-ROM drive, so its cycles follow G1GDRC/G1GDWC (0x5F74A0/A4).
 *
 * This used to write G1GDRC = 5 and leave G1GDWC, the system ROM and the
 * flash timings at their reset values -- a misreading of the BIOS. MAME
 * ignores G1 timings, so nothing showed it there; on a real board the
 * DIMM's mailbox latches would not hold a write. */
void g1_bus_init(void)
{
    REG32(0xA05F7480) = 0x00000600;     /* G1RRC  system ROM read  */
    REG32(0xA05F7484) = 0x00000600;     /* G1RWC  system ROM write */
    REG32(0xA05F7488) = 0x00000200;     /* G1FRC  flash read       */
    REG32(0xA05F748C) = 0x00000200;     /* G1FWC  flash write      */
    REG32(0xA05F7490) = 0x00000511;     /* G1CRC                   */
    REG32(0xA05F7494) = 0x00000511;     /* G1CWC                   */
    REG32(0xA05F74A0) = 0x00001006;     /* G1GDRC ROM board read   */
    REG32(0xA05F74A4) = 0x00001006;     /* G1GDWC ROM board write  */
    /* G1 DMA protection: the range of system RAM the GD-DMA may write. The
     * BIOS sets 0x8843007F (key 0x8843, the whole of it allowed) before any
     * cartridge or DIMM transfer; left at its reset value, no DMA lands. */
    REG32(0xA05F74B8) = 0x8843007Fu;
}

/* The G1 DMA from the ROM board, as the BIOS programs it to load a game:
 * the board's DMA offset takes the same flags as the PIO one (auto-advance,
 * linear M2 addressing), its count is in 32-byte units, and SB_GDDIR = 1
 * brings the data into system RAM. MAME's M2 board reads the linear flag
 * from the PIO offset, so that one is set too. */
u32 cart_dma_busy(void)
{
    return SB_GDST & 1;
}

u32 cart_dma_wait(void)
{
    u32 t0 = timer_ticks();
    while (SB_GDST & 1)
        if (timer_ticks() - t0 > TIMER_HZ / 10)
            return 1;                   /* 100 ms for 8 KB: it is not coming */
    return 0;
}

u32 cart_dma_start(u32 offset, u32 phys, u32 len)
{
    if (cart_dma_wait())
        return 1;
    u16 hi = (u16)(((offset >> 16) & 0x1FFF) | 0xA000);
    CART_ROM_OFFSETH = hi;
    CART_ROM_OFFSETL = (u16)offset;
    REG32(0xA05F7010) = offset & 0xFFFF;
    REG32(0xA05F700C) = hi;
    REG32(0xA05F7014) = len >> 5;
    SB_GDSTAR = phys;
    SB_GDLEN  = len;
    SB_GDDIR  = 1;
    SB_GDEN   = 1;
    SB_GDST   = 1;
    return 0;
}

void cart_seek(u32 offset)
{
    /* bit31 = PIO auto-advance; bit29 = linear raw addressing on M2 carts
     * (otherwise the address is remapped in 4MB windows). Encryption
     * (bit30) stays 0 so we hash the raw mask-ROM content = MAME dump. */
    CART_ROM_OFFSETH = (u16)(((offset >> 16) & 0x1FFF) | 0xA000);
    CART_ROM_OFFSETL = (u16)offset;
}

void cart_read(u32 offset, u8 *buf, u32 len)
{
    cart_seek(offset);
    u32 i = 0;
    while (i + 1 < len) {
        u16 w = CART_ROM_DATA;
        buf[i++] = (u8)w;
        buf[i++] = (u8)(w >> 8);
    }
    if (i < len)
        buf[i] = (u8)CART_ROM_DATA;
}

u32 cart_ic_responds(u32 offset, u32 size)
{
    const u32 nsamples = 16;
    u32 step = size / nsamples;
    if (!step)
        step = 2;
    u16 first = 0;
    u32 varying = 0;
    for (u32 i = 0; i < nsamples; i++) {
        cart_seek(offset + i * step);
        u16 w = CART_ROM_DATA;
        if (i == 0)
            first = w;
        else if (w != first)
            varying = 1;
    }
    /* constant 0xFFFF / 0x0000 across the whole chip = nothing driving */
    if (!varying && (first == 0xFFFF || first == 0x0000))
        return 0;
    return 1;
}

void cart_pin_scan(u32 offset, u32 len, cart_pin_stats *st)
{
    u16 buf[256];                       /* 512 B, fits the OC-RAM stack */
    st->words = 0;
    for (u32 b = 0; b < 16; b++) {
        st->ones[b] = 0;
        st->flaky[b] = 0;
    }

    for (u32 done = 0; done + sizeof buf <= len; done += sizeof buf) {
        u32 addr = offset + done;
        progress_tick(done);

        cart_seek(addr);                /* first read pass */
        for (u32 i = 0; i < 256; i++)
            buf[i] = CART_ROM_DATA;

        cart_seek(addr);                /* second read, same addresses */
        for (u32 i = 0; i < 256; i++) {
            u16 w = CART_ROM_DATA;
            u16 diff = (u16)(w ^ buf[i]);
            for (u32 b = 0; b < 16; b++) {
                if (buf[i] & (1u << b))
                    st->ones[b]++;
                if (diff & (1u << b))
                    st->flaky[b]++;
            }
            st->words++;
        }
    }
}

static u32 cart_answers_at(u32 offset)
{
    u8 hdr[64];
    cart_read(offset, hdr, sizeof hdr);
    u32 zeros = 0, ones = 0;
    for (u32 i = 0; i < sizeof hdr; i++) {
        if (hdr[i] == 0x00)
            zeros++;
        if (hdr[i] == 0xFF)
            ones++;
    }
    return (zeros != sizeof hdr && ones != sizeof hdr);
}

/* Where the cartridge's first chip, and its header, sits. Sega's boards
 * put it at 0 (IC22). The Namco-built M2 boards have their program flash
 * in sockets 2F, 2D, 2C, 2B at 8 MB each in linear mode, and several games
 * (Mazan, Ninja Assault, World Kicks) leave 2F empty: 0 reads 0xFF and the
 * "NAOMI" header is at 0x800000, in 2D -- MAME's sets load it there. */
u32 cart_base(void)
{
    if (cart_answers_at(0))
        return 0;
    if (cart_answers_at(0x00800000))
        return 0x00800000;
    return CART_NONE;
}

u32 cart_present(void)
{
    return cart_base() != CART_NONE;
}
