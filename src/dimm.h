#ifndef DIMM_H
#define DIMM_H
#include "hw.h"

/* DIMM board mailbox on the G1 bus (protocol documented in MAME
 * naomigd.cpp). Reading all-ones on the command register means no DIMM
 * board is attached (normal for cartridge setups).
 *
 * Fan note: the DIMM fan tach is wired to the DIMM's own firmware only;
 * the mainboard never sees it directly. A fan failure surfaces as an
 * abnormal DIMM status / boot error code, which this probe reports. */
#define DIMM_COMMAND    REG16(0xA05F703C)
#define DIMM_OFFSETL    REG16(0xA05F7040)
#define DIMM_PARAML     REG16(0xA05F7044)
#define DIMM_PARAMH     REG16(0xA05F7048)
#define DIMM_STATUS     REG16(0xA05F704C)

typedef struct {
    u16 command, offsetl, paraml, paramh, status;
    u32 present;
} dimm_info;

void dimm_probe(dimm_info *di);

/* -------------------------------------------------------------------------
 * DIMM SDRAM cell test over the G1-DMA path.
 *
 * The DIMM's SDRAM is reachable from the Naomi in BOTH directions through
 * the same GD-DMA engine the BIOS uses to load a game: SB_GDDIR selects the
 * direction (0 = DIMM->system RAM, 1 = system RAM->DIMM). The registers and
 * the address encoding were reversed from the DIMM-aware BIOS epr-23605c
 * (transfer routines ~0xA00335F0 write / 0xA0040440 read); see
 * analysis/dimm_dis/DIMM_G1_ACCES.md.
 *
 * Unlike the mailbox probe and the double-read pin scan, this test WRITES
 * the DIMM SDRAM: it destroys any game image resident on the board. That is
 * why it is only run from the operator-initiated DIMM test, never silently.
 * It cannot touch the DIMM's own firmware, which runs from the board's flash
 * and internal RAM, not this SDRAM.
 * ------------------------------------------------------------------------- */

/* GD-DMA engine (system RAM <-> DIMM SDRAM). Distinct from the cart PIO
 * path in cart.h even though both drive the G1 ROM-board interface. */
#define SB_GDSTAR   REG32(0xA05F7404)   /* system RAM address (physical)   */
#define SB_GDLEN    REG32(0xA05F7408)   /* byte length (multiple of 32)    */
#define SB_GDDIR    REG32(0xA05F740C)   /* 0 = DIMM->RAM, 1 = RAM->DIMM     */
#define SB_GDEN     REG32(0xA05F7414)   /* enable                          */
#define SB_GDST     REG32(0xA05F7418)   /* start; reads !=0 while running   */

typedef struct {
    u32 blocks;         /* 32 KB blocks written and read back            */
    u32 errors;         /* mismatching 32-bit words                      */
    u32 badbits;        /* OR of every (written ^ read)                  */
    u32 crc_w, crc_r;   /* CRC32 of the stream written / read back       */
    u32 timeout;        /* 1 if a DMA never completed (DIMM not booted?) */
    u32 first_addr;     /* DIMM offset of the first mismatch             */
    u32 first_exp;      /* value written there                           */
    u32 first_got;      /* value read back                               */
} dimm_mem_result;

/* Write `pattern` across the DIMM SDRAM [0, span), read it back over G1-DMA,
 * and compare word-by-word while accumulating a CRC32 of both streams.
 * DESTRUCTIVE. Requires a present, booted DIMM and a validated system-RAM
 * scratch (checked internally; on scratch failure r->timeout is left 0 and
 * r->blocks is 0). Abortable at each block via the progress layer. */
void dimm_mem_test(u32 span, u32 pattern, dimm_mem_result *r);

/* Quick CPU-side sanity check of the system-RAM scratch the DMA test needs.
 * Returns 1 if the scratch holds data, 0 if main RAM there is unusable. */
u32 dimm_scratch_ok(void);

#endif
