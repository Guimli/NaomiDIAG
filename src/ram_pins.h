#ifndef RAM_PINS_H
#define RAM_PINS_H
#include "hw.h"

/* Package pin numbers of the RAM chips, for the line reports: a cut data or
 * address line is named by its signal (DQ13, A5) and by the pin to probe on
 * the chip (pin 50). The two memories use different parts and packages, so
 * one table each.
 *
 * CPU RAM, IC9 / IC10 / IC11S / IC12S: Hitachi HM5264165, 64 Mbit SDRAM,
 * 4 banks x 1M x 16, TSOP-II 54 pins (JEDEC x16 SDRAM pinout, the one every
 * 64 Mbit x16 SDRAM of that time shares):
 *   DQ0-DQ7   2  4  5  7  8 10 11 13      DQ8-DQ15  42 44 45 47 48 50 51 53
 *   A0-A9    23 24 25 26 29 30 31 32 33 34  A10 22  A11 35   BA0 20  BA1 21
 *
 * VRAM, IC16-IC23 (and IC111-IC118 on a Naomi 2): Hynix HY57V161610D,
 * 16 Mbit SDRAM, 2 banks x 512K x 16, TSOP-II 50 pins (docs/ADDRESS_MAP.md):
 *   DQ0-DQ7   2  3  5  6  8  9 11 12      DQ8-DQ15  39 40 42 43 45 46 48 49
 *   A0-A9    21 22 23 24 27 28 29 30 31 32  A10 20  A11 (bank select) 19
 *
 * Elan RAM (Naomi 2), IC106 / IC107 / IC108S / IC109S: 32-bit wide SDRAM
 * (each chip carries a whole 32-bit word), TSOP-II 86 pins, JEDEC x32 SDRAM
 * pinout. Confirmed on a real board: "D27 (pin 50)" -- an open via on that
 * line, found from NaomiDIAG's report (arcade-projet forum):
 *   DQ0-DQ7   2  4  5  7  8 10 11 13     DQ8-DQ15  74 76 77 79 80 82 83 85
 *   DQ16-DQ23 31 33 34 36 37 39 40 42   DQ24-DQ31 45 47 48 50 51 53 54 56
 * Here the CPU data bit IS the DQ number.
 *
 * Which CPU data bit reaches which DQ: bit & 15 on the chip that carries
 * the bit's 16-bit half -- the rule the chip designation itself relies on
 * (validated physically on IC10 for the CPU RAM). The VRAM's address lines
 * go through HOLLY's own multiplexing, not documented: no pin is named for
 * them, only the CPU address bit.
 *
 * A data line is shared by every chip of its lane: the pin named is where to
 * probe, and the fault may sit on the trace or a via to another chip. */
static const u8 pin54_dq[16] = { 2, 4, 5, 7, 8, 10, 11, 13,
                                 42, 44, 45, 47, 48, 50, 51, 53 };
/* A0..A11, then BA0, BA1 (the order sdram_pin() numbers them) */
static const u8 pin54_addr[14] = { 23, 24, 25, 26, 29, 30, 31, 32, 33, 34,
                                   22, 35, 20, 21 };
static const u8 pin86_dq[32] = { 2, 4, 5, 7, 8, 10, 11, 13,
                                 74, 76, 77, 79, 80, 82, 83, 85,
                                 31, 33, 34, 36, 37, 39, 40, 42,
                                 45, 47, 48, 50, 51, 53, 54, 56 };
static const u8 pin50_dq[16] = { 2, 3, 5, 6, 8, 9, 11, 12,
                                 39, 40, 42, 43, 45, 46, 48, 49 };

#endif
