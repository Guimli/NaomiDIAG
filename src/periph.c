/* Peripheral tests: battery-backed SRAM (non-destructive) and AICA RTC. */
#include "periph.h"
#include "aica.h"
#include "timer.h"

/* ---- backup SRAM ---- */

static void sram_note(ram_result *r, u32 addr, u32 exp, u32 got)
{
    r->errors++;
    u32 diff = (exp ^ got) & 0xFF;
    r->badbits |= diff;
    if (addr & 1)
        r->badbits_o |= diff;           /* odd byte lane -> chip B */
    else
        r->badbits_e |= diff;           /* even byte lane -> chip A */
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp [r->nfails] = exp;
        r->fail_got [r->nfails] = got;
        r->nfails++;
    }
}

static inline u32 xorshift32(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

u32 sram_test(ram_result *r, u32 passes)
{
    volatile u8 *p = (volatile u8 *)SRAM_P2_BASE;
    ram_result_clear(r);

    for (u32 pass = 0; pass < passes; pass++) {
        u32 x = 0x5EED5EED ^ (0x9E3779B9u * (pass + 1));
        for (u32 i = 0; i < SRAM_SIZE; i++) {
            u8 orig = p[i];
            x = xorshift32(x);
            u8 rnd = (u8)x;

            p[i] = 0x55;
            if (p[i] != 0x55)
                sram_note(r, SRAM_P2_BASE + i, 0x55, p[i]);
            p[i] = 0xAA;
            if (p[i] != 0xAA)
                sram_note(r, SRAM_P2_BASE + i, 0xAA, p[i]);
            p[i] = rnd;
            if (p[i] != rnd)
                sram_note(r, SRAM_P2_BASE + i, rnd, p[i]);

            p[i] = orig;                /* restore, then verify restore */
            if (p[i] != orig)
                sram_note(r, SRAM_P2_BASE + i, orig, p[i]);
        }
    }

    u32 mask = 0;
    if (r->badbits_e)
        mask |= 1u << 0;
    if (r->badbits_o)
        mask |= 1u << 1;
    return mask;
}

/* ---- settings EEPROM CRC (SEGA algorithm, netboot project) ---- */

u16 sega_eeprom_crc(const u8 *data, u32 len)
{
    u32 crc = 0xDEBDEB00;
    for (u32 i = 0; i <= len; i++) {        /* data bytes + one 0x00 */
        u32 byte = (i < len) ? data[i] : 0;
        crc = (crc & 0xFFFFFF00) + byte;
        for (int k = 0; k < 8; k++) {
            if (crc < 0x80000000u)
                crc <<= 1;
            else
                crc = (crc << 1) + 0x10210000;
        }
    }
    return (u16)(crc >> 16);
}

/* ---- serial-number 93C46, bit-banged on SH4 GPIO (PDTRA) ---- */

#define PCTRA   REG32(0xFF80002C)
#define PDTRA   REG16(0xFF800030)

static void ee_pins(u32 cs, u32 clk, u32 di)
{
    PDTRA = (u16)((cs << 5) | (di << 3) | (clk << 2));
    for (volatile int i = 0; i < 200; i++)
        ;
}

static u16 ee_read_word(u32 addr)
{
    ee_pins(0, 0, 0);
    ee_pins(1, 0, 0);                       /* CS up */
    u32 bits = (1u << 8) | (2u << 6) | (addr & 0x3F); /* SB,1,0,A5..A0 */
    for (int i = 8; i >= 0; i--) {
        u32 b = (bits >> i) & 1;
        ee_pins(1, 0, b);
        ee_pins(1, 1, b);                   /* clock rising latches DI */
    }
    u16 w = 0;
    for (int i = 0; i < 16; i++) {          /* MSB first after dummy 0 */
        ee_pins(1, 0, 0);
        ee_pins(1, 1, 0);
        w = (u16)((w << 1) | ((PDTRA >> 4) & 1));
    }
    ee_pins(0, 0, 0);
    return w;
}

void serial_eeprom_read(u8 *out128)
{
    BCR2 |= 1;                              /* PORTEN: pins act as GPIO */
    PCTRA = 0x00000450;                     /* CLK(2)/DI(3)/CS(5) out, DO(4) in */
    for (u32 i = 0; i < 64; i++) {
        u16 w = ee_read_word(i);
        out128[i * 2]     = (u8)(w >> 8);   /* match ROM byte order */
        out128[i * 2 + 1] = (u8)w;
    }
}

/* ---- X76F100 (cart security), bit-banged via ROM board BOARDID ---- */

#define BOARDID_W   REG16(0xA05F7078)
#define BOARDID_R   REG16(0xA05F707C)

static void bid_w(u32 rst, u32 cs, u32 scl, u32 sda)
{
    BOARDID_W = (u16)((rst << 3) | (cs << 2) | (scl << 1) | sda);
    for (volatile int i = 0; i < 200; i++)  /* ~us settle */
        ;
}

u32 x76f100_rtr(void)
{
    u32 v = 0;
    bid_w(0, 1, 0, 0);                      /* idle, deselected */
    bid_w(0, 0, 0, 0);                      /* select */
    bid_w(1, 0, 0, 0);                      /* RST high -> response-to-reset */
    for (int byte = 0; byte < 4; byte++) {
        u32 b = 0;
        for (int bit = 0; bit < 8; bit++) { /* LSB first per byte */
            bid_w(1, 0, 1, 0);
            b |= ((BOARDID_R >> 15) & 1u) << bit;
            bid_w(1, 0, 0, 0);
        }
        v |= b << (byte * 8);
    }
    bid_w(0, 1, 0, 0);                      /* release */
    return v;
}

/* ---- AICA RTC ---- */

/* the RTC sits behind G2: drain the bus before touching it */
#define RTC_HI      REG32(0xA0710000u)
#define RTC_LO      REG32(0xA0710004u)

static u32 rtc_read(void)
{
    aica_g2_wait();
    /* consistent 32-bit read of the split counter */
    for (int tries = 0; tries < 4; tries++) {
        u32 hi1 = RTC_HI & 0xFFFF;
        u32 lo  = RTC_LO & 0xFFFF;
        u32 hi2 = RTC_HI & 0xFFFF;
        if (hi1 == hi2)
            return (hi1 << 16) | lo;
    }
    return (RTC_HI << 16) | (RTC_LO & 0xFFFF);
}

u32 rtc_test(u32 *value)
{
    u32 t0 = rtc_read();
    delay_ms(2200);                     /* > 2 RTC ticks */
    u32 t1 = rtc_read();
    *value = t1;
    /* the counter must move, forward, by a plausible amount */
    if (t1 == t0)
        return 1;
    u32 d = t1 - t0;
    if (d < 1 || d > 10)
        return 1;
    return 0;
}
