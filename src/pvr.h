#ifndef PVR_H
#define PVR_H
#include "hw.h"
#include "ramtest.h"

/* VRAM regions as tested by the original BIOS RAM TEST (32-bit path, P2):
 * TEX0 = IC9-IC12 (4x 16Mbit), TEX1 = IC35 (1x 64Mbit). */
#define VRAM_TEX0_BASE  0xA5000000u
#define VRAM_TEX0_SIZE  0x00800000u
#define VRAM_TEX1_BASE  0xA5800000u
#define VRAM_TEX1_SIZE  0x00800000u

/* Framebuffer: 640x480 RGB565 at VRAM offset 0x00200000 (same place the
 * original BIOS puts it), inside TEX0 -> tested before use. */
#define FB_VRAM_OFFSET  0x00200000u
#define FB_W            640
#define FB_H            480

/* Enable the PVR VRAM controller (SDRAM cfg/refresh) — needed before any
 * VRAM access; values lifted from the original BIOS. */
void pvr_vram_enable(void);

/* Naomi 2 only: slave PVR VRAM (32-bit path) and Elan T&L RAM. */
#define VRAM_PVRB_BASE  0xA7000000u
#define VRAM_PVRB_SIZE  0x01000000u     /* 16 MB */
#define ELAN_RAM_BASE   0xAA000000u
#define ELAN_RAM_SIZE   0x02000000u     /* 32 MB */

void pvr2_vram_enable(void);            /* slave PVR SDRAM controller */
void elan_init(void);                   /* Elan control + SDRAM refresh */

/* VRAM tests (plain bus accesses, same suite as SDRAM). */
u32  vram_test_databus(u32 base);
void vram_test_pattern(u32 base, u32 len, u32 pattern, ram_result *r);
void vram_test_prng(u32 base, u32 len, u32 seed, ram_result *r);

/* Display: VGA 640x480@31kHz, RGB565, timings from the original BIOS. */
void pvr_display_init(void);
void fb_clear(u16 color);
void fb_text(u32 x, u32 y, const char *s, u16 color);   /* 8x8 font, x2 scale */

#define RGB565(r, g, b) (u16)(((r) & 0x1F) << 11 | ((g) & 0x3F) << 5 | ((b) & 0x1F))
#define COL_WHITE   RGB565(31, 63, 31)
#define COL_GREEN   RGB565(6, 55, 6)
#define COL_RED     RGB565(31, 12, 6)
#define COL_TITLE   RGB565(31, 55, 0)

#endif
