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
/* The DIMM's signature: the BIOS takes the board as present when the high
 * byte reads 0x55 (epr-21576h, 0xA0032F74 and 0xA0034080). */
#define DIMM_SIGNATURE  REG16(0xA05F7034)

typedef struct {
    u16 command, offsetl, paraml, paramh, status, signature;
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
#define SB_GDDIR    REG32(0xA05F740C)   /* direction: see dimm_g1_probe     */
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
/* dir_write / dir_read: the SB_GDDIR values dimm_g1_probe found */
void dimm_mem_test(u32 span, u32 pattern, dimm_mem_result *r,
                   u32 dir_write, u32 dir_read);

/* What the G1 DMA does with the DIMM, found on the board itself rather
 * than taken from the reversing, which disagrees with the BIOS on the
 * direction bit (the DIMM notes: 0 reads; the BIOS cartridge loader: 1
 * reads) and on the count's unit (8 or 32 bytes).
 *
 * On 1 KB of the game image, 1 MB in: the reference read twice through
 * the PIO port, then each SB_GDDIR value tried on a buffer holding a
 * pattern. The buffer becoming the reference is a read; the DIMM becoming
 * the pattern is a write, undone at once with the reference and checked.
 * Then both count readings on a read, and the time of 32 KB. The DIMM
 * flash is never touched; its SDRAM is left as it was. */
#define DP_NOT_RUN  0
#define DP_READ     1       /* DIMM -> system RAM                    */
#define DP_WRITE    2       /* system RAM -> DIMM                    */
#define DP_NONE     3       /* completed, nothing moved              */
#define DP_TIMEOUT  4       /* never completed                       */
#define DP_ODD      5       /* something moved, not the expected way */
#define DP_UNKNOWN  0xFFu
typedef struct {
    u32 pio_flags;          /* PIO offset flags that read it, 0 = none */
    u32 outcome[2];         /* DP_* per SB_GDDIR value                 */
    u32 dir_read, dir_write;/* SB_GDDIR values, or DP_UNKNOWN          */
    u32 restored;           /* a write was undone and verified         */
    u32 restore_failed;     /* ... or could not be                     */
    u32 cnt_ok[2];          /* count in 32-byte / 8-byte units worked  */
    u32 ticks_32k;          /* one 32 KB read, timer ticks             */
} dimm_g1_probe_result;
void dimm_g1_probe(dimm_g1_probe_result *r);

/* Quick CPU-side sanity check of the system-RAM scratch the DMA test needs.
 * Returns 1 if the scratch holds data, 0 if main RAM there is unusable. */
u32 dimm_scratch_ok(void);

#endif
