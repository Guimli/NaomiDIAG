#ifndef DIMM_FLASH_H
#define DIMM_FLASH_H
#include "hw.h"

/* -------------------------------------------------------------------------
 * DIMM firmware flash access from the Naomi, reversed from Sega's own
 * "DIMM FIRMWARE UPDATE" program (the Naomi executable found in slot 0 of
 * FW_Netdimm_403.bin, mathieulh/SEGA_DIMM_CD-R). Protocol and constants in
 * analysis/dimm_dis/DIMM_FLASH_PROTO.md.
 *
 * The DIMM's flash is driven directly through the G1 ROM-board PIO -- the
 * same 0x5F7000/7004/7008 registers cart.c already uses -- after a
 * flash-mode switch. Flash commands are the AMD set, written as duplicated
 * bytes (16-bit device / two chips in parallel).
 *
 * WHAT IS SAFE HERE: only the read side. Entering flash mode is a window
 * switch (non-destructive); read-ID (AMD 0x90) and read-reset (0xF0) do not
 * alter the flash. This module deliberately implements ONLY the
 * non-destructive identify. The erase/program path is understood (single
 * AMD chip-erase 0x10 then full-image reprogram) but NOT implemented here:
 * a chip-erase wipes the recovery slot 0 as well, so a wrong sequence or an
 * interrupted write bricks the board. It waits until validated on hardware.
 * ------------------------------------------------------------------------- */

/* G1 ROM-board PIO (shared with cart.c; here used in flash-access mode). */
#define G1_OFFSETH  REG16(0xA05F7000)   /* addr[16..] | mode flags          */
#define G1_OFFSETL  REG16(0xA05F7004)   /* addr[0..15]                      */
#define G1_DATA16   REG16(0xA05F7008)   /* 16-bit data window               */
#define G1_DATA32   REG32(0xA05F7008)   /* 32-bit data window               */
#define G1_CTRL32   REG32(0xA05F7000)   /* 32-bit control (flash-mode enter) */

/* Flash-mode control words reversed from the Sega updater (setup1/setup2).
 * 0x5F7000 = 0x10002000 enters flash mode; each access ORs 0x3000 (the low
 * 16 bits of the two armed flags 0x10002000 | 0x00901000) into the high
 * address word. */
#define DIMM_FLASH_MODE     0x10002000u
#define DIMM_FLASH_ADDRFLAG 0x00003000u

typedef struct {
    u32 entered;        /* 1 if flash mode was entered                     */
    u16 mfr;            /* manufacturer ID (AMD read-ID at word 0)         */
    u16 dev;            /* device ID (AMD read-ID at word 1)               */
    u32 id_ok;          /* 1 if read-ID returned a plausible (non-FF) pair */
} dimm_flash_id;

/* Switch the DIMM G1 window to flash and read the AMD manufacturer/device
 * ID, then return the window to read (reset) state. Non-destructive.
 * Requires a present, booted DIMM whose G1 slave honours the mode switch;
 * on a cartridge system or a DIMM that refuses it, id_ok stays 0. */
void dimm_flash_identify(dimm_flash_id *fi);

/* Read one 16-bit word from the DIMM flash at byte offset `addr`, in
 * flash-mode. Caller brackets a batch with the identify (which leaves the
 * window in flash read state). Non-destructive. */
u16 dimm_flash_read16(u32 addr);

#endif
