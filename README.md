# NaomiDiag

A replacement diagnostic BIOS ROM for the **SEGA Naomi** and **Naomi 2**
arcade boards. It replaces the stock BIOS in the IC27 socket and tests the
board's components one by one, reporting results on up to three channels —
**serial (SCIF)**, **on-screen (VGA)** and **spoken audio** — without ever
relying on memory it has not yet proven good.

Available in **English** and **French**, text and voice both localized.
Pre-built ROMs ready to burn are on the
[**Releases**](https://github.com/Guimli/NaomiDIAG/releases/latest) page —
direct downloads:
[NaomiDIAG_EN.bin](https://github.com/Guimli/NaomiDIAG/releases/latest/download/NaomiDIAG_EN.bin)
·
[NaomiDIAG_FR.bin](https://github.com/Guimli/NaomiDIAG/releases/latest/download/NaomiDIAG_FR.bin).

> Français : voir [README.fr.md](README.fr.md).

## Three simultaneous output channels

Every test result is reported **at the same time on every channel that is
currently available** — serial, then serial + audio, then serial + audio +
video — so the operator can watch, listen, or capture the log, whichever is
convenient. As each result is produced it is printed on the SCIF, spoken
aloud, and drawn on the VGA report screen together.

The order in which the channels come up follows what each one needs:

- **SCIF serial** is the only output fully internal to the SH-4 CPU: it
  works from reset with **no external RAM at all**, so it is the primary and
  always-available channel.
- **Audio** needs the sound RAM (behind the AICA) and **video** needs the
  VRAM (behind the PowerVR); those memories are tested first, and each
  channel switches on only once its own memory has passed. When audio comes
  up it first replays every result acquired so far, then reports live.

Because sound and video are brought up before the long main-RAM test, the
machine never looks frozen during that ~1-minute test — the screen already
shows the earlier results and the SCIF prints per-pass progress.

Spoken reports are blocking, with at least one second of silence between two
messages so clips never overlap.

## What it tests

In order:

1. **SH-4 core** — the operand cache is configured as on-chip RAM and
   self-tested; this is also the stack/`.bss` for the whole ROM (no external
   RAM used until proven).
2. **Board identification** — Naomi 1 vs Naomi 2 (Elan T&L signature + VRAM
   aliasing).
3. **BIOS EPROM (IC27)** — CRC32 self-check.
4. **Sound RAM** (AICA, 8 MB, G2 bus) — then audio becomes a channel.
5. **VRAM** (PowerVR TEX0 = IC9-12, TEX1 = IC35) — then the VGA report
   screen comes up.
6. **Main CPU RAM** (SDRAM, 16/32 MB, IC16/18/20/22) — first a data-bus
   walking-ones test and an address-bus test, then **N passes** (build
   parameter `PASSES`, specified as 10; shipped as 1 for now — see
   "Pass count" below), each pass running three patterns in this order:
   - write `0x55555555` (0101…) over the whole region, then read it all back
     and compare;
   - write `0xAAAAAAAA` (1010…) over the whole region, then read back and
     compare;
   - write a pseudo-random stream (a different seed each pass) while
     accumulating a **CRC32 kept in a CPU register**, then read the region
     back recomputing the CRC and compare it against the write-side CRC as
     well as word by word.

   Any single bad cell marks the whole chip (and thus the entire interleaved
   RAM) defective; that RAM is never used for the rest of the program. If
   **all** CPU RAM is bad, the program keeps running from the SH-4 cache
   (OC-RAM) and completes every test that does not need main RAM — only the
   Maple/MIE test, which needs RAM for its DMA descriptors, is skipped.
7. **Naomi 2 only** — slave PVR VRAM (16 MB) and Elan RAM (32 MB).
8. **Backup SRAM** — non-destructive save/restore test.
9. **RTC** (AICA) — non-destructive tick check.
10. **DIMM board** (G1 mailbox) — presence and mailbox sanity.
11. **Maple bus / MIE** (315-6146 Z80) — version request + factory
    self-test.
12. **Settings EEPROM** (93C46 via MIE) — groundwork (needs a Z80 code
    upload; documented).
13. **Serial-number EEPROM** (93C46 on SH-4 GPIO) — read + content check.
14. **Cartridge security** (X76F100) — presence via response-to-reset.
15. **Cartridge content** — identifies the game against an embedded
    database of every known Naomi/Naomi 2 cartridge (192 games, 2298 ICs)
    and verifies each ROM chip by SHA-1, reporting the failing IC by its
    silkscreen name.
16. **Cartridge ROM set completeness** — once the game is identified, the
    **whole set of chips that game needs** is checked: every mask ROM in the
    database entry is probed and the result is stated affirmatively
    (`ROM set: 13 / 13 chips present`). A chip answering a constant
    0xFFFF/0x0000 everywhere is reported as *not responding* (missing,
    unseated, dead) rather than as bad content, and a chip returning the
    same bytes as another one is flagged as an address-aliasing mirror —
    an empty socket that a neighbouring chip answers for. Presence is
    checked on every chip even on QUICK builds; only hashing is trimmed.
17. **Cartridge data lines** — per-pin statistics over the 16-bit cartridge
    bus: the share of 1s each line reads (a line that never toggles is
    stuck), plus a comparison of two reads of the same addresses — any bit
    that differs is an unstable line, the signature of a tired bus
    transceiver or a dirty edge connector. This runs even when the game
    cannot be identified, since a dead line is precisely what prevents
    identification.

RAM faults are reported per component: a bit mask, the affected data lanes,
and the silkscreen IC designator (e.g. `CPU RAM 1 (IC16) DEFECTIVE`).


### Where the test loops execute

The SH-4 boots in P2, an address window the hardware never caches, so every
instruction is a bus cycle to the boot EPROM. Linking the whole ROM for P1
(the cached alias of the boot area) was tried on real hardware and the board
refuses it outright -- black screen before the first visible instruction --
which matches the original BIOS, that never executes cached from ROM either.

Cached execution from SDRAM is a different matter: it is where every Naomi
game runs. So the four memory-test loops -- 356 bytes, where essentially all
the time goes -- are copied into an 8 KB block of CPU RAM at boot and run
from there, cached, while everything else stays in ROM.

The four CPU RAM chips are interleaved by data lane rather than by address
range -- IC16/IC18 carry the even words, IC20/IC22 the odd ones -- so every
block spans all four. Blocks are scanned downward from the top and the first
sound one is taken, which gives immunity to a localized fault (a bad row or
column inside one chip) but not to a chip dead across its range: in that case
no block passes and the ROM copies keep being used. A block that fails the
scan is reported as the broken memory it is, not quietly skipped.

The window is tested with the full pattern and pseudo-random suite before
anything is copied into it, and the memory under test is still addressed
through P2, so the data path stays uncached and the test keeps its coverage.
If no usable RAM is found the pointers keep addressing the ROM copies and the
diagnostic runs slowly rather than not at all -- which is exactly the board
that needs diagnosing. Build with `RELOC=0` to disable it entirely.

### Pass count

Every RAM cell test runs `PASSES` passes; the specification calls for 10.
The images currently ship with `PASSES=1`, because the ROM executes uncached
straight from the boot EPROM: every instruction fetch is an EPROM access, so
a single pass over 32 MB already takes far too long on a real board to be
usable. Once execution speed is addressed, rebuild with the specified depth:

```
make LANG=EN PASSES=10
```

## Building

Toolchain: Debian `gcc-sh-elf` / `binutils-sh-elf`.

```sh
make LANG=EN            # -> NaomiDIAG_EN.bin (English text + voice)
make LANG=FR            # -> NaomiDIAG_FR.bin (French text + voice)
make LANG=EN QUICK=1    # 1 MB per RAM pass, for fast emulator bring-up
```

A 2 MB image is produced, ready to burn on a 27C160 EPROM (IC27). The build
refuses to produce an image larger than 2 MB (no silent truncation).

The same 27C160 image works on both Naomi 1 and Naomi 2 (both use a 2 MB
BIOS); the board is detected at runtime.

Regenerating generated sources (rarely needed, committed in the repo):

```sh
make audio     # re-render the spoken clips (needs the Piper venv + sox)
make cartdb    # rebuild the cartridge SHA-1 DB from `mame -listxml`
```

## Testing under MAME

```sh
make LANG=EN QUICK=1 mame-rom
mame naomi -rompath ../roms_diag -autoboot_script scif_tap.lua
```

`scif_tap.lua` captures the SH-4 SCIF transmit FIFO and prints the serial
console (MAME does not wire the SCIF to anything). The `*_fault*.lua`
scripts inject RAM faults for negative testing. Note: main RAM is fastram
under the DRC, so fault injection into it needs `-nodrc`.

## Real hardware notes

- Burn `NaomiDIAG_xx.bin` on a 27C160 (IC27). Serial output is on the SCIF
  pins at 3.3 V logic — use a 3.3 V USB-serial adapter, never RS-232 levels.
- A failed cache test means the SH-4 itself is dead: it is reported on SCIF
  and the ROM halts.
- A CPU exception restarts the ROM (the banner reprints) — a repeating
  banner is itself a diagnostic signal.
- The DIMM-board fan is monitored only by the DIMM firmware.

## Status and limitations

- Silkscreen IC designators for the RAM chips come from the original BIOS
  RAM TEST tables; the exact **lane→IC order** is a hypothesis pending
  confirmation by a forced fault on real hardware.
- IC designators for the SH-4, HOLLY, AICA and the two 62256 are not yet
  known (the BIOS never prints them); to be read off a real board.
- The settings EEPROM read and full JVS I/O-board testing need a Z80 code
  upload into the MIE (future work).

## Credits

Bootstrapped from the [JinGasa](https://github.com/Tchan0/JinGasa) minimal
Naomi BIOS project. Register-level details cross-checked against MAME and
libnaomi. Spoken clips generated with [Piper](https://github.com/rhasspy/piper).
