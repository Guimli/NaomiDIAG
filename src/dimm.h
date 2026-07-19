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

#endif
