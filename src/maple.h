#ifndef MAPLE_H
#define MAPLE_H
#include "hw.h"

/* Maple bus (HOLLY DMA engine). On Naomi the MIE (315-6146 Z80) — the
 * JVS/inputs controller — is a Maple device: a version request transaction
 * exercises the DMA engine, the link and the MIE's Z80 in one go.
 * Requires working main RAM (descriptors + rx buffer), so this runs only
 * after the SDRAM has been validated. */

typedef struct {
    u32 found_port;         /* 0-3, or 0xFFFFFFFF if nothing answered */
    u32 response_cmd;       /* maple response command (5 = device status) */
    u32 func_codes;         /* first function-code word of the ID block */
    char id[64];            /* product name string (from the ID block)  */
} maple_result;

void maple_scan(maple_result *mr);

/* Read the 128-byte settings EEPROM (93C46 behind the MIE) through the
 * uploaded program: command 0xE0 with a slice index, 28 bytes per 0xE1
 * reply. Returns 0 on success. */
u32 maple_eeprom_read(u32 port, u8 *out128);

/* Upload and start the MIE program (src/mie_prog.z80). Everything below
 * needs it: the stock MIE firmware can do neither of these. */
u32 maple_mie_upload(u32 port);
u32 maple_mie_inputs(u32 port, u8 *state);

/* MIE self-test (stock kernel command 0x84 -> 0x85): the Z80 runs its
 * own ROM/RAM test; status word 0x00000000 = pass. Returns 0 on pass,
 * with the raw status in *status. */
u32 maple_mie_selftest(u32 port, u32 *status);

/* One 28-byte slice (idx 0..6) of the JVS info block kept by the JVS
 * master in the uploaded MIE program: command 0xE4 -> 0xE5. The layout is
 * documented in src/mie_prog.z80 (JVS INFO BLOCK) and mirrored by the
 * JVSI_* offsets below. Returns 0 on success. */
u32 maple_jvs_info(u32 port, u32 idx, u8 *out28);

#define JVSI_LEN        196
#define JVSI_STATE      0       /* 0 idle, 1 starting, 2 ready, 3 none, 4 error */
#define JVSI_SENSE      1
#define JVSI_ERR        2       /* 1 timeout, 2 sum, 3 status, 4 report */
#define JVSI_ERRCNT     3
#define JVSI_SEQ        4       /* 16-bit little-endian poll count */
#define JVSI_CMDREV     6
#define JVSI_JVSREV     7
#define JVSI_COMMVER    8
#define JVSI_PLAYERS    9
#define JVSI_SWBYTES    10
#define JVSI_COINS      11
#define JVSI_ANACH      12
#define JVSI_ANABITS    13
#define JVSI_FSTEP      14
#define JVSI_SW         16      /* system byte, then players' bytes (16) */
#define JVSI_COIN       32      /* 4 x 2 bytes, big-endian */
#define JVSI_ANA        40      /* 8 x 2 bytes, big-endian */
#define JVSI_FEAT       56      /* 32 bytes, raw feature list */
#define JVSI_ID         88      /* 64 bytes, 0-terminated */

#define JVS_READY       2
#define JVS_NONE        3
#define JVS_ERROR       4

/* Put the DMA descriptors and receive buffer at a 32-byte-aligned P2 address
 * in CPU SDRAM (needs 0x200 reserved bytes). MDAPRO follows these buffers.
 * They must NOT sit in memory a running test is writing patterns over. */
void maple_set_buffers(u32 p2_base);

#endif
