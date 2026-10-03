#ifndef DIMM_HOST_H
#define DIMM_HOST_H
#include "hw.h"

/* -------------------------------------------------------------------------
 * Standing in for the BIOS on the DIMM's PEEK/POKE requests.
 *
 * The DIMM firmware does not wait to be asked: it reads and writes the
 * BIOS's work area in Naomi RAM itself, one request at a time through the
 * mailbox, and blocks until the BIOS answers (protocol in
 * analysis/dimm_dis/DIMM_AUTOTEST.md section 8, from the BIOS's own service
 * routine). What it writes there is what this diagnostic wants -- the size
 * of its memory, its firmware version -- and the status byte of every
 * answer tells it whether the BIOS is in test mode, which is what keeps it
 * idle instead of loading a game while its SDRAM is under test.
 *
 * Two of its habits shape this module:
 *   - its loader starts by RESETTING THE NAOMI and then waits, without a
 *     time limit, for a first message from the BIOS (dimm_host_start sends
 *     it). With no valid game in a network-mode DIMM it does that reset a
 *     second time, after this first message: main.c survives it with a
 *     resume block.
 *   - a DIMM left without answers for long enough in its network loop takes
 *     the silence for a missing BIOS and resets the Naomi again. So once
 *     started, the session answers from progress_heartbeat(), which every
 *     loop of this ROM already calls, until the next reset.
 *
 * Nothing real is read or written for the DIMM: its addresses land in a
 * 512-byte shadow of the BIOS work area 0x0C01FC00-0x0C01FDFF, in OC-RAM;
 * anything outside it reads 0 and is counted as refused.
 * ------------------------------------------------------------------------- */

/* what the DIMM has done so far in this session (dimm_host_got) */
#define DH_GOT_MEM      0x01u   /* posted its memory word (0x0C01FC04) */
#define DH_GOT_VER      0x02u   /* posted its version word (0x0C01FC0C) */
#define DH_GOT_IDLE     0x04u   /* read the test request word: its loader is
                                 * done and it sits in its request loop */
#define DH_GOT_ANY      0x08u   /* answered at least one request */

void dimm_host_start(void);     /* open the session, send the first message */
void dimm_host_stop(void);
u32  dimm_host_active(void);
void dimm_host_service(void);   /* answer one pending request, if any */
u32  dimm_host_got(void);
u32  dimm_host_requests(void);  /* requests answered */
u32  dimm_host_refused(void);   /* outside the shadow, or unknown */

/* a 32-bit word of the shadow, by its Naomi address (0x0C01Fxxx) */
u32  dimm_host_word(u32 addr);

/* The memory word, decoded. Bits 0-11: memory past the DIMM's own 16 MB,
 * in MB; bits 16-19 / 20-23: size code of socket 0 / 1 (64 MB << code,
 * 64 MB meaning empty); bit 28: Ethernet fitted; bit 29: the newer
 * acknowledge. Returns 0 if the DIMM has not posted it. */
u32  dimm_host_size(void);      /* bytes */

#endif
