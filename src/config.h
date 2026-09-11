#ifndef CONFIG_H
#define CONFIG_H

/* -------------------------------------------------------------------------
 * Build-time options that are a decision about the ROM rather than a
 * per-build variation.
 *
 * The Makefile carries what changes from one build to the next -- LANG,
 * QUICK, RELOC, ROM_BASE -- because you pass those on the command line. What
 * lives here is edited once and stays. Each option can still be overridden
 * with -D on the command line without touching this file.
 * ---------------------------------------------------------------------- */

/* Lane beacon (0 = off, 1 = on).
 *
 * After the report, cycles through the twelve 16-bit slices of the CPU RAM
 * and VRAM buses, naming one at a time and hammering it so a scope on the
 * chips shows which package carries which lane. It is a bench instrument for
 * establishing the lane-to-designator map, not part of diagnosing a board:
 * it never ends, so the report stays on screen but the machine never settles.
 *
 * Off by default for that reason. Turn it on when you have a probe in hand
 * and a designator to pin down -- see "Status and limitations" in the README
 * for what is still unmeasured. */
#ifndef CFG_LANE_BEACON
#define CFG_LANE_BEACON 0

/* Let the AICA's ARM7 run (0 = hold it in reset, 1 = let it spin).
 *
 * We drive the sound channels from the SH-4 and have no use for the ARM, so
 * this ROM held it in reset. A working Naomi does not: dumping the AICA under
 * the original BIOS shows 0x2C00 = 0, the ARM released, while ours sat at 1.
 * That is the one structural difference left between our configuration and a
 * machine that is known to make sound, and it is invisible to an emulator --
 * no game holds the ARM in reset, so nothing ever exercised that case.
 *
 * With this on, a four-byte ARM program that branches to itself is written at
 * sound RAM offset 0, the ARM's reset vector, and the reset is released. The
 * ARM spins in place, touching nothing, and the AICA sits in the same state
 * as on a booting Naomi. Set to 0 to go back to holding it in reset. */
#ifndef CFG_AICA_ARM_RUN
#define CFG_AICA_ARM_RUN 1
#endif

#endif

#endif
