#ifndef MAPLE_H
#define MAPLE_H
#include "hw.h"

/* Maple bus (HOLLY DMA engine). On Naomi the MIE (315-6146 Z80) — the
 * JVS/inputs controller — is a Maple device: a Device Request transaction
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

/* Read the 128-byte settings EEPROM (93C46 behind the MIE) using the
 * proprietary MIE protocol: 0x86/{0x01} starts the read, 0x86/{0x03}
 * fetches the result (0x87 response, 33 payload words: status + data).
 * Returns 0 on success. */
u32 maple_eeprom_read(u32 port, u8 *out128);

/* Upload and start the MIE program (src/mie_prog.z80). Everything below
 * needs it: the stock MIE firmware can do neither of these. */
u32 maple_mie_upload(u32 port);
u32 maple_mie_inputs(u32 port, u8 *state);

/* MIE self-test (stock kernel command 0x84 -> 0x85): the Z80 runs its
 * own ROM/RAM test; status word 0x00000000 = pass. Returns 0 on pass,
 * with the raw status in *status. */
u32 maple_mie_selftest(u32 port, u32 *status);

/* Raw JVS control words from the MIE (0x86/0x15 -> 0x87/0x16, 14 words).
 * The bit positions of TEST, SERVICE and START are not documented anywhere
 * and are established by watching these words while pressing the buttons --
 * see CFG_JVS_MAP in config.h. Returns 0 on success. */
u32 maple_jvs_read(u32 port, u32 out[14]);

/* Ask the JVS I/O at `addr` (1 = first board) for its switches. The answer
 * arrives in the NEXT maple_jvs_read, inside the packet its words carry. */
u32 maple_jvs_request(u32 port, u32 addr);

/* Put the DMA descriptors and receive buffer at p2_base (needs 0x200 bytes).
 * They must NOT sit in memory a running test is writing patterns over. */
void maple_set_buffers(u32 p2_base);

#endif
