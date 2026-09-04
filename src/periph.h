#ifndef PERIPH_H
#define PERIPH_H
#include "hw.h"
#include "ramtest.h"

/* Battery-backed save SRAM (2x 62256), operator settings + bookkeeping:
 * the test is NON-destructive (save / test / restore every byte). */
#define SRAM_P2_BASE    0xA0200000u
#define SRAM_SIZE       0x00008000u

/* Returns component mask: bit0 = even byte lane (chip A),
 * bit1 = odd byte lane (chip B); 0 = all good. Fills *r with details. */
u32 sram_test(ram_result *r);

/* AICA RTC (32.768kHz crystal + battery), seconds since 1950.
 * Non-destructive: reads the counter twice around a TMU delay and checks
 * that it ticks. Returns 0 = OK, 1 = stuck/dead. *value out = counter. */
u32 rtc_test(u32 *value);

/* SEGA CRC over a settings-EEPROM block (algorithm from the netboot
 * project, validated against a BIOS-written EEPROM image). */
u16 sega_eeprom_crc(const u8 *data, u32 len);

/* Serial-number 93C46 EEPROM, bit-banged on the SH4 GPIO port (PDTRA:
 * DI=bit3, DO=bit4, CS=bit5, CLK=bit2). Reads all 128 bytes. */
void serial_eeprom_read(u8 *out128);

/* X76F100 cart security chip: clock out the 32-bit response-to-reset
 * through the ROM-board BOARDID register (0x5F7078/7C). Returns the
 * raw RTR value; 0x00000000 / 0xFFFFFFFF = no chip responding. */
u32 x76f100_rtr(void);

#endif
