# NaomiDiag

A replacement diagnostic BIOS ROM for the **SEGA Naomi** and **Naomi 2**
arcade boards. It replaces the stock BIOS in the IC27 socket and tests the
board's components one by one, reporting results on up to three channels —
**serial (SCIF)**, **on-screen (VGA)** and **spoken audio** — without ever
relying on memory it has not yet proven good.

Available in **English** and **French** (`NaomiDIAG_EN.bin` /
`NaomiDIAG_FR.bin`), text and voice both localized.

> Français : voir [README.fr.md](README.fr.md).

## Why three output channels, in this order

The SCIF serial port is the only output fully internal to the SH-4 CPU: it
works from reset with **no external RAM at all**, so it is the primary and
always-available channel. Sound and video need their own memories (sound
RAM behind the AICA, VRAM behind the PowerVR), so they are tested first and,
once proven good, become additional report channels. The slow main-RAM
test then runs with audio and screen already live, so the machine never
looks frozen during it.

## What it tests

In order:

1. **SH-4 core** — the operand cache is configured as on-chip RAM and
   self-tested; this is also the stack/`.bss` for the whole ROM (no external
   RAM used until proven).
2. **Board identification** — Naomi 1 vs Naomi 2 (Elan T&L signature + VRAM
   aliasing); the correct silkscreen IC table is selected, or numbered
   positions are used on an unknown board.
3. **BIOS EPROM (IC27)** — CRC32 self-check.
4. **Sound RAM** (AICA, 8 MB, G2 bus) — then audio becomes a channel.
5. **VRAM** (PowerVR TEX0 = IC9-12, TEX1 = IC35) — then the VGA report
   screen comes up.
6. **Main CPU RAM** (SDRAM, 16/32 MB, IC16/18/20/22) — data bus, address
   bus, then **10 passes of `0x55555555`, `0xAAAAAAAA` and a per-pass
   pseudo-random stream whose CRC32 is kept in a CPU register and compared
   read-vs-write**. Any bad cell marks the chip (and thus the whole
   interleaved RAM) unusable; it is never used afterwards.
7. **Naomi 2 only** — slave PVR VRAM (16 MB) and Elan RAM (32 MB).
8. **Backup SRAM** (2× 62256) — non-destructive save/restore test.
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

RAM faults are reported per component: a bit mask, the affected data lanes,
and the silkscreen IC designator (e.g. `CPU RAM 1 (IC16) DEFECTIVE`).

## Building

Toolchain: Debian `gcc-sh-elf` / `binutils-sh-elf`.

```sh
make LANG=EN            # -> NaomiDIAG_EN.bin (English text + voice)
make LANG=FR            # -> NaomiDIAG_FR.bin (French text + voice)
make LANG=EN QUICK=1    # 1 MB per RAM pass, for fast emulator bring-up
```

Both a 2 MB image is produced, ready to burn on a 27C160 EPROM (IC27). The
build refuses to produce an image larger than 2 MB (no silent truncation).

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
- The DIMM-board fan is monitored only by the DIMM firmware; the mainboard
  cannot read it directly. The motherboard fan has no tachometer.

## Status and limitations

- Silkscreen IC designators for the RAM chips come from the original BIOS
  RAM TEST tables; the exact **lane→IC order** is a hypothesis pending
  confirmation by a forced fault on real hardware.
- IC designators for the SH-4, HOLLY, AICA and the two 62256 are not yet
  known (the BIOS never prints them); to be read off a real board.
- The settings EEPROM read and full JVS I/O-board testing need a Z80 code
  upload into the MIE (future work).
- MAME faithfully models the digital buses but not drive/analog mechanics;
  real hardware is the final judge.

## Credits

Bootstrapped from the [JinGasa](https://github.com/Tchan0/JinGasa) minimal
Naomi BIOS project. Register-level details cross-checked against MAME and
libnaomi. Spoken clips generated with [Piper](https://github.com/rhasspy/piper).
