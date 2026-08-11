/* PowerVR (HOLLY) support: VRAM enable + tests, then VGA display with an
 * 8x8 text renderer so the screen becomes the third report channel.
 * All register values were extracted from the original Naomi BIOS
 * (cross-checked with JinGasa HOLLY.s / libnaomi). */
#include "pvr.h"
#include "font8x8_basic.h"

#define PVR_REG(off)        REG32(0xA05F8000u + (off))

#define PVR_SOFTRESET       PVR_REG(0x008)
#define PVR_VO_BORDER_COL   PVR_REG(0x040)
#define PVR_FB_R_CTRL       PVR_REG(0x044)
#define PVR_FB_W_CTRL       PVR_REG(0x048)
#define PVR_FB_R_SOF1       PVR_REG(0x050)
#define PVR_FB_R_SOF2       PVR_REG(0x054)
#define PVR_FB_R_SIZE       PVR_REG(0x05C)
#define PVR_VRAM_REFRESH    PVR_REG(0x0A0)
#define PVR_VRAM_ARB_CFG    PVR_REG(0x0A4)
#define PVR_VRAM_CFG        PVR_REG(0x0A8)
#define PVR_SPG_HBLANK_INT  PVR_REG(0x0C8)
#define PVR_SPG_VBLANK_INT  PVR_REG(0x0CC)
#define PVR_SPG_CONTROL     PVR_REG(0x0D0)
#define PVR_SPG_HBLANK      PVR_REG(0x0D4)
#define PVR_SPG_LOAD        PVR_REG(0x0D8)
#define PVR_SPG_VBLANK      PVR_REG(0x0DC)
#define PVR_SPG_WIDTH       PVR_REG(0x0E0)
#define PVR_VO_CONTROL      PVR_REG(0x0E8)
#define PVR_VO_STARTX       PVR_REG(0x0EC)
#define PVR_VO_STARTY       PVR_REG(0x0F0)
#define PVR_FB_BURSTCTRL    PVR_REG(0x110)
#define PVR_FB_Y_COEFF      PVR_REG(0x118)

void pvr_vram_enable(void)
{
    PVR_SOFTRESET    = 0;                /* release sdram/pipeline/TA reset */
    PVR_VRAM_ARB_CFG = 0x0000001F;
    PVR_VRAM_CFG     = 0x15D1C951;       /* BIOS value */
    PVR_VRAM_REFRESH = 0x00000020;
}

/* Naomi 2 slave PVR: same controller at the +0x02000000 register window. */
void pvr2_vram_enable(void)
{
    REG32(0xA25F8008) = 0;
    REG32(0xA25F80A4) = 0x0000001F;
    REG32(0xA25F80A8) = 0x15D1C951;
    REG32(0xA25F80A0) = 0x00000020;
}

/* Elan T&L chip: control (bit 1..2 = enable slave/broadcast) and SDRAM
 * refresh, per the register defaults documented in the MAME driver.
 * Real-hardware init sequence still to be confirmed on a live 837-14009. */
void elan_init(void)
{
    REG32(0xA8800010) = 6;
    REG32(0xA8800014) = 0x2029;
}

/* ---- VRAM tests: identical suite to the SDRAM one ---- */

u32 vram_test_databus(u32 base)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 bad = 0;
    for (u32 bit = 1; bit != 0; bit <<= 1) {
        *p = bit;
        bad |= *p ^ bit;
        *p = ~bit;
        bad |= *p ^ ~bit;
    }
    return bad;
}

static void note_fail(ram_result *r, u32 addr, u32 exp, u32 got)
{
    r->errors++;
    r->badbits |= exp ^ got;
    if (addr & 4)
        r->badbits_o |= exp ^ got;
    else
        r->badbits_e |= exp ^ got;
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp [r->nfails] = exp;
        r->fail_got [r->nfails] = got;
        r->nfails++;
    }
}

void vram_test_pattern(u32 base, u32 len, u32 pattern, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 n = len >> 2;
    for (u32 i = 0; i < n; i++)
        p[i] = pattern;
    for (u32 i = 0; i < n; i++) {
        u32 got = p[i];
        if (got != pattern)
            note_fail(r, base + (i << 2), pattern, got);
    }
}

static const u32 crc4tab[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

static inline u32 crc32_word(u32 crc, u32 w)
{
    crc ^= w;
    for (int k = 0; k < 8; k++)
        crc = (crc >> 4) ^ crc4tab[crc & 0xF];
    return crc;
}

static inline u32 xorshift32(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void vram_test_prng(u32 base, u32 len, u32 seed, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 n = len >> 2;
    register u32 crc_w = 0xFFFFFFFF;
    register u32 crc_r = 0xFFFFFFFF;
    u32 x = seed ? seed : 1;

    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        p[i] = x;
        crc_w = crc32_word(crc_w, x);
    }
    x = seed ? seed : 1;
    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        u32 got = p[i];
        crc_r = crc32_word(crc_r, got);
        if (got != x)
            note_fail(r, base + (i << 2), x, got);
    }
    r->crc_w = ~crc_w;
    r->crc_r = ~crc_r;
    if (crc_w != crc_r && r->errors == 0)
        note_fail(r, base, crc_w, crc_r);
}

/* ---- display ---- */

void pvr_display_init(void)
{
    PVR_FB_R_CTRL     = 0x00800000;      /* VGA clock, reads off while cfg */
    PVR_VO_CONTROL    = 0x00160008;      /* blank screen during setup */
    PVR_SOFTRESET     = 0;
    PVR_FB_W_CTRL     = 0x00000009;      /* dither + RGB565 (unused, TA path) */
    PVR_FB_BURSTCTRL  = 0x00093F39;
    PVR_FB_Y_COEFF    = 0x00008040;
    PVR_FB_R_SOF1     = FB_VRAM_OFFSET;  /* both fields: no interlace */
    PVR_FB_R_SOF2     = FB_VRAM_OFFSET;
    /* width in 32-bit units - 1, height - 1, line modulus 1 */
    PVR_FB_R_SIZE     = (1u << 20) | ((FB_H - 1) << 10) | (FB_W * 2 / 4 - 1);
    PVR_SPG_LOAD      = 0x020C0359;      /* 858 x 525 total (VGA) */
    PVR_SPG_HBLANK    = 0x007E0345;
    PVR_SPG_VBLANK    = 0x00280208;
    PVR_SPG_WIDTH     = 0x03F1933F;
    PVR_SPG_CONTROL   = 0x00000100;      /* 31 kHz, non-interlaced */
    PVR_SPG_HBLANK_INT = 0x03450000;
    PVR_SPG_VBLANK_INT = 0x00150208;
    PVR_VO_STARTX     = 0x000000A8;
    PVR_VO_STARTY     = 0x00280028;
    PVR_VO_BORDER_COL = 0;
    PVR_FB_R_CTRL     = 0x00800005;      /* VGA clock, RGB565, display on */
    PVR_VO_CONTROL    = 0x00160000;      /* unblank */
}

static volatile u16 *fb(void)
{
    return (volatile u16 *)(0xA5000000u + FB_VRAM_OFFSET);
}

void fb_clear(u16 color)
{
    volatile u32 *p = (volatile u32 *)fb();
    u32 v = ((u32)color << 16) | color;
    for (u32 i = 0; i < FB_W * FB_H / 2; i++)
        p[i] = v;
}

/* 8x8 public-domain font, rendered x2 -> 16x16 cells, 40 cols x 30 rows */
void fb_text(u32 x, u32 y, const char *s, u16 color, u32 xmax)
{
    volatile u16 *base = fb();
    while (*s) {
        u8 c = (u8)*s++;
        if (c > 127)
            c = '?';
        const char *glyph = font8x8_basic[c];
        for (u32 gy = 0; gy < 8; gy++) {
            u8 row = (u8)glyph[gy];
            for (u32 gx = 0; gx < 8; gx++) {
                if (!(row & (1u << gx)))
                    continue;
                u32 px = x + gx * 2, py = y + gy * 2;
                volatile u16 *d = base + py * FB_W + px;
                d[0] = color;
                d[1] = color;
                d[FB_W] = color;
                d[FB_W + 1] = color;
            }
        }
        x += 16;
        if (x + 16 > xmax)
            return;                     /* clipped: keeps clear of the
                                           status column on the right */
    }
}
