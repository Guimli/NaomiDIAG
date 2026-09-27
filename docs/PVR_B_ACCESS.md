# Naomi 2 PVR-B access qualification

The PVR-B test uses the uncached 32-bit window at `0xA7000000`, size
`0x01000000` (16 MiB). The 64-bit window at `0xA6000000` is another view
of the same VRAM, not additional RAM. Their address layouts differ.

Previously, the ROM tested B before enabling its windows through Elan.
Board detection also assumed that a write to an inaccessible B window
could be distinguished from independent RAM by reading A alone.

The revised sequence identifies the board independently of VRAM contents,
checks Elan's ID, sets IFCTL bits 1 and 2 and clears broadcast bit 0,
verifies the control readback, then checks and initializes PVR-B. It probes
four scratch offsets in both halves of A and B through the 32-bit windows.
Scratch contents are restored, B first and A last. Display writes are
suppressed during the destructive B test; the report is redrawn afterwards.

At each offset the probe runs in two steps:

1. **Each cell alone.** Every one of the four cells is written and read back
   immediately, with its marker and the complement, twice. The bits that fail
   there are that cell's own fault — a cut data line, a dead chip — and are
   recorded as *unreliable* for that cell.
2. **All four together.** Distinct patterns, their complements, and opposite
   write orders, every cell read only after all four hold distinct contents.
   A cell whose *reliable* bits changed was changed by a write to another
   window: a mirror, a one-way broadcast or crossed halves.

A cell's own fault therefore no longer blocks anything: it is left for the
RAM tests, which locate and name it. Only a write reaching another window
does, because then testing B would write into A.

An access failure produces a separate access failure and skips the PVR-B
memory test, without announcing defective RAM chips. The Elan RAM is the
Elan's own memory, not a window onto either GPU: testing it writes no VRAM,
so it is still tested unless the Elan itself does not answer (codes 1-2).
Serial codes:

| Code | Meaning | PVR-B test | Elan RAM test |
| --- | --- | --- | --- |
| 1 | Elan ID unavailable | skipped | skipped |
| 2 | Elan interface-control readback incorrect | skipped | skipped |
| 3 | PVR-B ID unavailable after enabling its windows | skipped | run |
| 4 | A write to one window changed another: mirror, broadcast, crossed halves | skipped | run |
| 5 | A PVR-A cell fails on more than 16 bits alone: too few left to judge | skipped | run |
| 6 | A PVR-B cell fails on more than 16 bits alone: B holds nothing written | skipped | run |

Codes 5 and 6 leave too few trustworthy bits to see another window's marker
through, so nothing is concluded. Code 6 means PVR-B's VRAM keeps nothing
written to it — its chips, its controller, or an initialisation this ROM
gets wrong on real hardware; no chip is named, and B is not written any
further, because B writes were never shown to leave A alone.

These samples do not prove the entire address map; the normal data-bus,
address-bus, and pattern tests still run after success. A persistent
hardware broadcast fault may prevent complete restoration.

On code 4, eight `PVR probe pass=` lines capture both passes at the first
failing offset, including matching locations; on codes 5 and 6, four
`PVR probe cell alone` lines capture the cells written alone. Each line
contains the address, expected value, observed value, XOR difference and,
when non-zero, `alone=`: the bits that failed with the cell written alone.
Reads are captured during the probe and printed after restoration; logging
does not repeat the memory accesses. Pass 0 writes A-low, A-high, B-low,
B-high; pass 1 reverses that order and complements the patterns. Code 4
triggers sparse address-pair diagnosis of B, code 5 of the failing A half.

## The case that motivated per-cell characterization

A real Naomi 2 run with DQ9 deliberately disconnected completed the TEX1
cell tests and reported CPU data mask `00000200`, starting at `A5C00000`,
in the BIOS-associated IC21 lane. During access qualification it recorded:

| Location | Expected | Observed | XOR |
| --- | --- | --- | --- |
| A: `A5FFFFFC`, pass 0 | `2468ACE0` | `2468AEE0` | `00000200` |
| B: `A77FFFFC`, pass 0 | `A5963CC3` | `A5963CC3` | `00000000` |
| B: `A7FFFFFC`, pass 0 | `5A69C33C` | `5A69C33C` | `00000000` |

Both B locations also matched on pass 1. The former probe compared A only
after writes to both A and B, could not tell this local fault from a B write
changing A, and returned code 5 — skipping both PVR-B VRAM and Elan RAM on a
board whose B side was never in question. With per-cell characterization,
`A5FFFFFC` fails bit 9 alone, bit 9 is left out of the comparison, the other
31 bits show no interference, and both memories are tested. The host harness
reproduces this case (`BAD_A`) and the same fault combined with a real
mirror (`BAD_A_MIRROR`, still code 4).

## Board selection

`CFG_BOARD_MODEL=0` (default) reads Elan's ID. On Naomi 1 this assumes an
open-bus response from an unpopulated window. It does not catch exceptions
or recover a stalled bus. A damaged Elan can also be mistaken for absence.
Use the known-board setting when testing identified hardware:

```sh
make LANG=FR CFLAGS_EXTRA=-DCFG_BOARD_MODEL=1  # Naomi 1: no Elan probe
make LANG=FR CFLAGS_EXTRA=-DCFG_BOARD_MODEL=2  # Naomi 2: qualify B access
```

Selecting Naomi 2 does not bypass access qualification. Do not select it
on Naomi 1. The fixed Naomi 1 setting avoids all B/Elan accesses.

## Validation

The host harness executes the production setup and board-detection code
with simulated MMIO. It covers healthy memory, whole-window and half-window
mirrors, crossed halves, discarded writes, both broadcast directions,
failed control writes, missing IDs, a bad scratch cell, a data line cut on
PVR-A alone and combined with a mirror, a dead PVR-A cell and a data line
cut across PVR-B. It checks enable-before-access ordering and restoration
where the mapping permits it.

```sh
sh tools/test_pvr2.sh
```

Physical Naomi 1/2 validation remains required. MAME 0.288 does not emulate
the Naomi 2's own hardware: one PowerVR only, `0x07000000` mirrors
`0x05000000`, the Elan ID reads 0 and nothing answers at the Elan RAM. A
Naomi 2 therefore runs as a Naomi 1 there. `mame/elan_id.lua` fakes the ID
and IFCTL so the Naomi 2 path can be exercised: it ends in code 4, the mirror
correctly refused, and in a failing Elan RAM.

References:
- [MAME Naomi 2 map](https://github.com/mamedev/mame/blob/master/src/mame/sega/naomi.cpp)
- [KallistiOS Elan interface](https://kos-docs.dreamcast.wiki/elan_8h.html)
