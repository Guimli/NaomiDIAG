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
#endif

#endif
