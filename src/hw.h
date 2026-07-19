/* =============================================================================
 * hw.h — SH7091 (SH4) + Naomi hardware registers used by the diagnostic ROM
 * ============================================================================= */
#ifndef HW_H
#define HW_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define REG8(a)   (*(volatile u8  *)(a))
#define REG16(a)  (*(volatile u16 *)(a))
#define REG32(a)  (*(volatile u32 *)(a))

/* ---- CCN ---- */
#define MMUCR       REG32(0xFF000010)
#define CCR         REG32(0xFF00001C)

/* ---- BSC (bus state controller / SDRAM) ---- */
#define BCR1        REG32(0xFF800000)
#define BCR2        REG16(0xFF800004)
#define WCR1        REG32(0xFF800008)
#define WCR2        REG32(0xFF80000C)
#define WCR3        REG32(0xFF800010)
#define MCR         REG32(0xFF800014)
#define RTCSR       REG16(0xFF80001C)
#define RTCNT       REG16(0xFF800020)
#define RTCOR       REG16(0xFF800024)
#define RFCR        REG16(0xFF800028)
#define PCTRA       REG32(0xFF80002C)
#define SDMR3_ADDR  0xFF940190      /* byte write: address encodes SDRAM mode */

/* Values extracted from the original Naomi BIOS (epr-21576h @0xA0000440,
 * literal pool @ROM offset 0x550) and cross-checked against JinGasa SH4.s */
#define BCR1_VAL        0xA3020008
#define BCR2_VAL        0x0000
#define WCR1_VAL        0x01110111
#define WCR2_VAL        0x018060D8
#define WCR3_VAL        0x07777777
#define RTCSR_VAL       0xA510
#define RTCOR_VAL       0xA55E
#define RTCNT_VAL       0xA500
#define RFCR_VAL        0xA400
#define MCR_32MB        0xC0121214
#define MCR_16MB        0xC00A0E24

/* ---- DMAC ---- */
#define DMAOR       REG32(0xFFA00040)

/* ---- SCIF (serial, fully internal to the SH4) ---- */
#define SCSMR2      REG16(0xFFE80000)
#define SCBRR2      REG8 (0xFFE80004)
#define SCSCR2      REG16(0xFFE80008)
#define SCFTDR2     REG8 (0xFFE8000C)
#define SCFSR2      REG16(0xFFE80010)
#define SCFRDR2     REG8 (0xFFE80014)
#define SCFCR2      REG16(0xFFE80018)
#define SCFDR2      REG16(0xFFE8001C)
#define SCLSR2      REG16(0xFFE80024)

#define SCFSR2_TEND 0x0040
#define SCFSR2_TDFE 0x0020

/* ---- Memory map (P2 = uncached mirrors; mandatory while testing RAM) ---- */
#define SDRAM_P2_BASE   0xAC000000u
#define SDRAM_16MB      0x01000000u
#define SDRAM_32MB      0x02000000u

/* OC-RAM (SH4 operand cache as RAM, CCR.ORA=1 + OIX=1) */
#define OCRAM_BANK0     0x7C000000u     /* 4KB: stack */
#define OCRAM_BANK1     0x7E000000u     /* 4KB: .bss  */

#endif
