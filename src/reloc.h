#ifndef RELOC_H
#define RELOC_H
#include "hw.h"
#include "ramtest.h"

/* -------------------------------------------------------------------------
 * Running the hot loops from CPU RAM instead of the boot EPROM.
 *
 * The SH-4 boots in P2, which the hardware never caches, so every
 * instruction is an EPROM bus cycle. Linking the whole ROM for P1 (the
 * cached alias of the boot area) was tried and the board refuses it outright
 * -- black screen before the first visible instruction -- which matches the
 * original BIOS, that never executes cached from ROM either.
 *
 * Cached execution from SDRAM is a different proposition entirely: it is
 * where every Naomi game runs. And the whole diagnostic does not need to
 * move, only the four memory-test loops, which is where essentially all the
 * time goes. They are 356 bytes, they are position independent as written,
 * and the memory they test is still addressed through P2 -- so the data path
 * stays uncached and the test keeps exactly the coverage it has today.
 *
 * The rule of the project is unchanged: this RAM is used only after it has
 * been proven. If no region passes, the pointers keep addressing the ROM
 * copies and the diagnostic runs slowly rather than not at all -- which is
 * precisely the case of a board with bad CPU RAM, the one that most needs
 * to be diagnosed.
 * ---------------------------------------------------------------------- */

#define RELOC_WINDOW    0x2000u     /* 8 KB block the loops are copied into */
#define RELOC_TRIES     128u        /* blocks scanned downward from the top */
#define RELOC_SCRATCH   0x1000u     /* offset of the self-test area in it  */

/* Called through, never directly, so a failed or disabled relocation simply
 * keeps executing the ROM copies. */
extern void (*p_ram_fill_fast)(u32 *base, u32 nblocks16, u32 pattern);
extern u32  (*p_ram_verify_fast)(u32 *base, u32 nblocks8, u32 pattern);
extern void (*p_ram_prng_fill_fast)(u32 *base, u32 nwords, prng_ctx *c);
extern void (*p_ram_prng_verify_fast)(const u32 *base, u32 nwords,
                                      prng_ctx *c);

void reloc_init(void);              /* point everything at the ROM copies */

/* Copy the loops to p2_dest, prove they execute and compute correctly there,
 * then switch the pointers to the cached alias. Returns 1 on success; on any
 * doubt it leaves the ROM pointers in place and returns 0. */
u32  reloc_install(u32 p2_dest);

u32  reloc_active(void);

#endif
