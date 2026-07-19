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

#endif
