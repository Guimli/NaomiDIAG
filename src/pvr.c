/* PowerVR (HOLLY) support: VRAM enable + tests, then VGA display with an
 * 8x8 text renderer so the screen becomes the third report channel.
 * All register values were extracted from the original Naomi BIOS
 * (cross-checked with JinGasa HOLLY.s / libnaomi). */
#include "pvr.h"
#include "ramtest.h"
#include "progress.h"
#include "reloc.h"
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

/* VRAM sits on the PowerVR bus, plain accesses with no FIFO discipline, so
 * it can use the same hand-written loops as the CPU RAM. Sound RAM
 * deliberately does NOT: it is behind G2, whose write FIFO must be drained
 * every eight words, and trading that for speed is what locked the bus up. */
static void vram_pattern_locate(u32 base, u32 n, u32 pattern, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    for (u32 i = 0; i < n; i++) {
        u32 got = p[i];
        if (got != pattern)
            note_fail(r, base + (i << 2), pattern, got);
    }
}

void vram_test_pattern(u32 base, u32 len, u32 pattern, ram_result *r)
{
    u32 n = len >> 2, done = 0, diff = 0;

    while (done + 16 <= n) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        chunk &= ~15u;
        progress_tick(done);
        p_ram_fill_fast((u32 *)(base + (done << 2)), chunk >> 4, pattern);
        done += chunk;
    }
    for (u32 i = done; i < n; i++)
        ((volatile u32 *)base)[i] = pattern;

    done = 0;
    while (done + 8 <= n) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        chunk &= ~7u;
        progress_tick(n + done);
        diff |= p_ram_verify_fast((u32 *)(base + (done << 2)), chunk >> 3,
                                pattern);
        done += chunk;
    }
    for (u32 i = done; i < n; i++)
        diff |= ((volatile u32 *)base)[i] ^ pattern;

    if (diff)
        vram_pattern_locate(base, n, pattern, r);
}



static inline u32 xorshift32(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void vram_prng_locate(u32 base, u32 n, u32 seed, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 x = seed ? seed : 1;
    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        u32 got = p[i];
        if (got != x)
            note_fail(r, base + (i << 2), x, got);
    }
}

void vram_test_prng(u32 base, u32 len, u32 seed, ram_result *r)
{
    u32 n = len >> 2;
    prng_ctx c;
    c.x = seed ? seed : 1;
    c.crc_w = 0xFFFFFFFF;
    c.crc_r = 0xFFFFFFFF;
    c.diff = 0;

    for (u32 done = 0; done < n; ) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        progress_tick(done);
        p_ram_prng_fill_fast((u32 *)(base + (done << 2)), chunk, &c);
        done += chunk;
    }
    c.x = seed ? seed : 1;
    for (u32 done = 0; done < n; ) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        progress_tick(n + done);
        p_ram_prng_verify_fast((const u32 *)(base + (done << 2)), chunk, &c);
        done += chunk;
    }

    r->crc_w = ~c.crc_w;
    r->crc_r = ~c.crc_r;
    if (c.diff)
        vram_prng_locate(base, n, seed, r);
    if (c.crc_w != c.crc_r && r->errors == 0)
        note_fail(r, base, c.crc_w, c.crc_r);
}

/* ---- display ---- */

static u32 g_fb_live;                    /* framebuffer proven and displayed */
u32 fb_progress_enabled(void) { return g_fb_live; }

/* Program the video timings WITHOUT enabling framebuffer reads.
 *
 * This is the earliest possible visual sign of life: it touches no VRAM, so
 * it can run before a single byte of memory has been proven. The screen
 * shows the border colour over its whole surface, which progress_phase()
 * then uses as a POST code. Called first thing on boot, so a board that
 * dies later still tells us how far it got.
 *
 * DIP switch 1 selects 31 kHz (VGA) or 15 kHz on the Naomi, so a monitor
 * that shows nothing here is a cabling or DIP question, not a board fault. */
void pvr_video_on(void)
{
    PVR_FB_R_CTRL     = 0x00800000;      /* VGA clock, framebuffer reads off */
    PVR_VO_CONTROL    = 0x00160008;      /* blank while we program timings */
    PVR_SOFTRESET     = 0;
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
    PVR_VO_CONTROL    = 0x00160000;      /* unblank: border fills the screen */
}

void pvr_border(u32 rgb)
{
    PVR_VO_BORDER_COL = rgb;
}

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
    g_fb_live = 1;                       /* framebuffer proven: bar allowed */
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

/* ---- progress display ----------------------------------------------
 * Drawn straight into the framebuffer, below the report area. Kept cheap:
 * only the newly filled slice of the bar is painted on each update, so
 * calling this once per percent inside a memory test costs nothing
 * measurable next to the test itself. */

#define BAR_X   16
#define BAR_Y   (FB_H - 40)
#define BAR_W   (FB_W - 32)
#define BAR_H   20

static u32 g_bar_filled;                 /* pixels currently painted green */

void fb_fill_rows(u32 y0, u32 y1, u16 color)
{
    volatile u32 *p = (volatile u32 *)fb();
    u32 v = ((u32)color << 16) | color;
    for (u32 y = y0; y < y1 && y < FB_H; y++)
        for (u32 i = 0; i < FB_W / 2; i++)
            p[y * (FB_W / 2) + i] = v;
}

static void fb_rect(u32 x, u32 y, u32 w, u32 h, u16 color)
{
    volatile u16 *p = fb();
    for (u32 dy = 0; dy < h && y + dy < FB_H; dy++)
        for (u32 dx = 0; dx < w && x + dx < FB_W; dx++)
            p[(y + dy) * FB_W + (x + dx)] = color;
}

void fb_progress(const char *label, u32 pct)
{
    if (!fb_progress_enabled())
        return;
    if (pct > 100)
        pct = 100;

    u32 inner = BAR_W - 4;
    u32 filled = (inner * pct) / 100;      /* pct <= 100: no overflow */

    /* A new test starts at 0%, i.e. filled goes backwards: wipe the whole
     * interior then. Otherwise paint only the slice that just appeared --
     * repainting the full bar on every percent would cost more VRAM writes
     * than the memory test it is reporting on. */
    if (filled < g_bar_filled) {
        fb_rect(BAR_X + 2, BAR_Y + 2, inner, BAR_H - 4, 0);
        fb_fill_rows(BAR_Y - 22, BAR_Y - 4, 0);     /* and the label line */
        fb_text(BAR_X, BAR_Y - 22, label, COL_WHITE, FB_W - 16 * 5);
    } else if (filled > g_bar_filled) {
        fb_rect(BAR_X + 2 + g_bar_filled, BAR_Y + 2,
                filled - g_bar_filled, BAR_H - 4, COL_GREEN);
    }
    g_bar_filled = filled;

    /* percentage, in its own fixed field so it needs no full-line clear */
    fb_rect(FB_W - 16 * 5, BAR_Y - 22, 16 * 5, 18, 0);
    char num[5];
    u32 n = pct, i = 0;
    char tmp[4];
    do { tmp[i++] = (char)('0' + n % 10); n /= 10; } while (n && i < 3);
    u32 j = 0;
    while (i)
        num[j++] = tmp[--i];
    num[j++] = '%';
    num[j] = 0;
    fb_text(FB_W - 16 * 5, BAR_Y - 22, num, COL_TITLE, FB_W);

    /* frame */
    fb_rect(BAR_X, BAR_Y, BAR_W, 1, COL_WHITE);
    fb_rect(BAR_X, BAR_Y + BAR_H - 1, BAR_W, 1, COL_WHITE);
    fb_rect(BAR_X, BAR_Y, 1, BAR_H, COL_WHITE);
    fb_rect(BAR_X + BAR_W - 1, BAR_Y, 1, BAR_H, COL_WHITE);
}

/* after a full-screen repaint nothing of the bar is left on screen: forget
 * how much was drawn so the next call paints it whole again */
void fb_progress_invalidate(void)
{
    g_bar_filled = 0;
}
