#ifndef SDRAM_H
#define SDRAM_H
#include "hw.h"

/* Initialise the BSC + SDRAM exactly like the original BIOS, detect
 * 16MB vs 32MB by mirror test, return the size in bytes. */
u32 sdram_init(void);

#endif
