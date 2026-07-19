/* DIMM board (G1 bus) mailbox probe. Read-only: safe with a real DIMM
 * mid-boot, and correctly reports absence on cartridge systems. */
#include "dimm.h"

void dimm_probe(dimm_info *di)
{
    di->command = DIMM_COMMAND;
    di->offsetl = DIMM_OFFSETL;
    di->paraml  = DIMM_PARAML;
    di->paramh  = DIMM_PARAMH;
    di->status  = DIMM_STATUS;
    /* all-ones on the command register = no board driving the bus */
    di->present = (di->command != 0xFFFF);
}
