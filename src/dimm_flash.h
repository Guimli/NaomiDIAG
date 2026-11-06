#ifndef DIMM_FLASH_H
#define DIMM_FLASH_H
#ifdef HOST_TEST
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define REG16(a) (*(volatile u16 *)(a))
#else
#include "hw.h"
#endif

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
 * Identify and read are non-destructive. Erase/program are implemented per
 * SLOT with sector erase (the DIMM firmware's own method), never the
 * updater's chip erase, which wipes the recovery slot 0 as well. They are
 * reached only from the local firmware-update build (DIMMFW=...), never
 * from a published image, and are NOT yet validated on hardware.
 * ------------------------------------------------------------------------- */

/* G1 ROM-board PIO (shared with cart.c; here used in flash-access mode). */
#define G1_OFFSETH  REG16(0xA05F7000)   /* addr[16..] | mode flags          */
#define G1_OFFSETL  REG16(0xA05F7004)   /* addr[0..15]                      */
#define G1_DATA16   REG16(0xA05F7008)   /* 16-bit data window               */

/* Flash-mode control words reversed from the Sega updater (setup1/setup2).
 * 0x5F7000 = 0x10002000 enters flash mode; each access ORs 0x3000 (the low
 * 16 bits of the two armed flags 0x10002000 | 0x00901000) into the high
 * address word. */
#define DIMM_FLASH_MODE     0x10002000u
#define DIMM_FLASH_ADDRFLAG 0x00003000u

#define DIMM_SLOT_SIZE      0x100000u   /* 1 MB per slot, 2 slots          */

typedef struct {
    u32 entered;        /* 1 if flash mode was entered                     */
    u16 mfr;            /* manufacturer ID (AMD read-ID at word 0)         */
    u16 dev;            /* device ID (AMD read-ID at word 1)               */
    u32 id_ok;          /* 1 if read-ID returned a plausible (non-FF) pair */
    u32 layout_ok;      /* 1 if it is the top-boot 16 Mbit part (0x22C4)   */
} dimm_flash_id;

/* Switch the DIMM G1 window to flash and read the AMD manufacturer/device
 * ID, then return the window to read (reset) state. Non-destructive.
 * Requires a present, booted DIMM whose G1 slave honours the mode switch;
 * on a cartridge system or a DIMM that refuses it, id_ok stays 0. */
void dimm_flash_identify(dimm_flash_id *fi);

/* Read one 16-bit word from the DIMM flash at byte offset `addr`. Must be
 * bracketed by flash mode (the slot functions below do it). */
u16 dimm_flash_read16(u32 addr);

/* ---- erase / program (DESTRUCTIVE) --------------------------------------
 * Slot 0 = 0x000000-0x0FFFFF: reset vector, boot loader, recovery firmware.
 * Slot 1 = 0x100000-0x1FFFFF: main firmware. At boot the loader copies slot
 * 1 and checks its CRC-32; a bad slot 1 falls back to slot 0. A bad slot 0
 * leaves a board that no longer starts at all (external programmer).
 * Each slot is erased sector by sector, so writing one never touches the
 * other. */
#define DF_OK               0
#define DF_ERR_ARG          1
#define DF_ERR_SLOT0_LOCKED 2   /* slot 0 asked without DF_ALLOW_SLOT0      */
#define DF_ERR_IMAGE_CRC    3   /* image is not a valid slot (CRC != 0)     */
#define DF_ERR_IMAGE_SLOT0  4   /* image does not start like a boot loader  */
#define DF_ERR_CHIP         5   /* unknown flash: sector map not known      */
#define DF_ERR_TIMEOUT      6   /* erase/program never finished             */
#define DF_ERR_DQ5          7   /* the part reported a failed operation     */
#define DF_ERR_BLANK        8   /* a word is not FFFF after the erase       */
#define DF_ERR_VERIFY       9   /* a word reads back different              */
#define DF_ERR_FLASH_CRC    10  /* slot CRC read from the flash != 0        */

#define DF_PH_CHECK     0
#define DF_PH_ERASE     1
#define DF_PH_BLANK     2
#define DF_PH_PROGRAM   3
#define DF_PH_VERIFY    4
#define DF_PH_READ      5
#define DF_PH_DONE      6

#define DF_ALLOW_SLOT0  0x1u

#define DF_ERASE_TIMEOUT_MS 30000u  /* data sheet max 15 s per sector       */
#define DF_PROG_TIMEOUT_MS  50u     /* data sheet max 360 us per word       */

typedef void (*df_progress)(u32 phase, u32 done, u32 total);

typedef struct {
    int err;            /* DF_* */
    u32 stage;          /* DF_PH_* reached */
    u32 addr;           /* flash byte address of the failure */
    u32 got, want;
} df_report;

/* Erase, blank-check, program and verify one slot from a 1 MB image in RAM.
 * The image must have CRC-32 == 0 (a valid slot); slot 0 needs
 * DF_ALLOW_SLOT0 and an image starting with the DIMM boot code. */
int dimm_flash_write_slot(u32 slot, const u8 *img, u32 flags, df_progress p,
                          df_report *rep);

/* The check words (last 4 bytes) of slot 0 and slot 1, read from the flash:
 * they name the versions in place (dfw_name). Non-destructive. */
void dimm_flash_check_words(u32 *w0, u32 *w1);

/* CRC-32 of a slot as read from the flash (0 = valid slot). */
u32 dimm_flash_slot_crc(u32 slot, df_progress p);

/* Sector list of a slot (top-boot part); returns the count. */
u32 dimm_flash_sectors(u32 slot, u32 *start, u32 *size, u32 max);

/* CRC-32, 0xEDB88320, no final XOR (start with 0xFFFFFFFF). */
u32 dimm_crc32_update(u32 crc, const u8 *p, u32 n);

#endif
