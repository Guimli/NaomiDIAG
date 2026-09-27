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
Distinct patterns, their complements, and opposite write orders detect
mirrors, one-way broadcast, and failed readback at the sampled locations.
Scratch contents are restored, B first and A last. Display writes are
suppressed during the destructive B test; the report is redrawn afterwards.

An access failure produces a separate access failure and skips the
B/Elan memory tests, without announcing defective RAM chips. Serial codes:

| Code | Meaning |
| --- | --- |
| 1 | Elan ID unavailable |
| 2 | Elan interface-control readback incorrect |
| 3 | PVR-B ID unavailable after enabling its windows |
| 4 | Scratch readback failed: aliasing, broadcast, inaccessible or faulty RAM |
| 5 | A-only scratch failure; B remains inconclusive, including possible B-to-A broadcast |

Code 4 does not prove aliasing: genuinely faulty scratch cells can also
fail qualification. These samples do not prove the entire address map;
the normal data-bus, address-bus, and pattern tests still run after success.
A persistent hardware broadcast fault may prevent complete restoration.

On code 4 or 5, eight `PVR probe` lines capture both passes at the first failing
offset, including matching locations. Each line contains the address,
expected value, observed value, and XOR difference. Reads are captured
during the probe and printed after restoration; logging does not repeat
the memory accesses or change the interval between writes and reads.
Pass 0 writes A-low, A-high, B-low, B-high; pass 1 reverses that order and
complements the patterns. A matching ID alone does not prove independent
VRAM decoding. A physical Naomi 2 reported an A-only mismatch at A5FFFFFC,
with XOR 00000300. This is now classified as code 5 and triggers sparse
address-pair diagnosis of that A half. Code 4 triggers diagnosis of B.
An A-only mismatch can also result from B-to-A broadcast, so it does not
authorize the full B test. The probe alone cannot determine the physical cause.

## Known limitation: a local PVR-A data fault also blocks B

A real Naomi 2 run with DQ9 deliberately disconnected completed the TEX1
cell tests and reported CPU data mask `00000200`, starting at `A5C00000`,
in the BIOS-associated IC21 lane. During access qualification it recorded:

| Location | Expected | Observed | XOR |
| --- | --- | --- | --- |
| A: `A5FFFFFC`, pass 0 | `2468ACE0` | `2468AEE0` | `00000200` |
| B: `A77FFFFC`, pass 0 | `A5963CC3` | `A5963CC3` | `00000000` |
| B: `A7FFFFFC`, pass 0 | `5A69C33C` | `5A69C33C` | `00000000` |

Both B locations also matched on pass 1. Nevertheless, `pvr2_prepare()`
returns `PVR2_A_REFERENCE` (code 5), and `test_naomi2_ram`'s access-failure path
returns before either full memory test. Thus **both PVR-B VRAM and Elan RAM
are skipped**. This is a software qualification rule, not a PVR-B or Elan
RAM failure verdict. Passing the sampled B reads does not certify all B RAM.

The current probe compares A only after writes to both A and B. It cannot
separate an existing local A data fault from changes caused by writes to B.
The same guard intentionally rejects potential B-to-A broadcast, but also
rejects this known local A fault. Further sparse diagnosis of A does not
resume the skipped tests.

A proposed improvement, **not implemented**, is to characterize A before
writing B, then check for changes attributable to B writes. Qualification
would need to tolerate independently established A data faults while still
detecting mirrors and one-way broadcast, including intermittent readback.
Simply ignoring code 5 or masking every faulty A bit would not establish
independent access. The current conservative skip remains in effect.

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
failed control writes, missing IDs, and a bad scratch cell. It checks
enable-before-access ordering and restoration where the mapping permits it.

```sh
sh tools/test_pvr2.sh
```

Physical Naomi 1/2 validation remains required. MAME maps B independently
and stubs Elan control, so its results alone cannot validate hardware routing.

References:
- [MAME Naomi 2 map](https://github.com/mamedev/mame/blob/master/src/mame/sega/naomi.cpp)
- [KallistiOS Elan interface](https://kos-docs.dreamcast.wiki/elan_8h.html)
