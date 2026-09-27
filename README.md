# NaomiDiag

> **Work in progress.** The Naomi 2 memory tests and the JVS master are
> coded but not yet validated on real hardware. And other functions have not
> been tested yet. But I am working on it as fast as I can :-)

A replacement diagnostic BIOS ROM for the **SEGA Naomi** and **Naomi 2**
arcade boards. It replaces the stock BIOS in the IC27 socket and tests the
board's components one by one, reporting results on up to three channels —
**serial (SCIF)**, **on-screen (VGA)** and **spoken audio** — without ever
relying on memory it has not yet proven good.

It has been used to repair a board: the ROM named **IC10**, replacing IC10
alone cleared the fault.

Available in **English** and **French**, text and voice both localized.
Pre-built ROMs ready to burn are on the
[**Releases**](https://github.com/Guimli/NaomiDIAG/releases/latest) page —
direct downloads:
[NaomiDIAG_EN.bin](https://github.com/Guimli/NaomiDIAG/releases/latest/download/NaomiDIAG_EN.bin)
·
[NaomiDIAG_FR.bin](https://github.com/Guimli/NaomiDIAG/releases/latest/download/NaomiDIAG_FR.bin).

> Français : voir [README.fr.md](README.fr.md).

### A run, with a fault

![Finding a dead data line on CPU RAM](docs/fault_ic9.gif)

Data line D5 held low across part of the CPU RAM, under MAME. The cell test
fails, and the ROM names the chip: D5 is in the low half of the 64-bit word,
so it belongs to the even-word pair — **IC9**, never the odd pair. The
flashing border is the heartbeat, which pulses for as long as the ROM is
alive.

The full run is on the release page:
[**NaomiDIAG_EN_IC9_fault.mp4**](https://github.com/Guimli/NaomiDIAG/releases/latest/download/NaomiDIAG_EN_IC9_fault.mp4)
— 125 seconds, every test, **with the sound**, so you can hear the fault
announced as well as read it. GitHub will not play a video that lives in a
repository (it strips the `<video>` tag), hence the silent GIF above and the
download for the real thing.

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
  VRAM (behind the PowerVR); the small area of each that its channel uses is
  checked first, and each channel switches on only once its own area has
  passed. The full tests of those memories come later. When audio comes
  up it first replays every result acquired so far, then reports live.

Because sound and video are brought up before the long main-RAM test, the
machine never looks frozen during that ~1-minute test — the screen already
shows the earlier results and the SCIF prints per-pass progress.

Spoken reports are blocking, with at least one second of silence between two
messages so clips never overlap.

The clips are stored as **4-bit Yamaha ADPCM** and handed to the AICA in that
form (`PCMS=2`), which decodes it in hardware. That is a straight 4:1 saving
on the EPROM with no decompressor, no scratch buffer and no CPU cost — the
bytes are copied into sound RAM exactly as they sit in the ROM. The full
French build went from 98 % of the EPROM to 29 %; with the clips added since
(Naomi 2 chips, JVS board, data-line reports) a full build sits at
about 50 %.

## What it tests

The **boot suite** runs on its own, in this order. The screen and the
speaker come up long before the memories they live in are fully tested:
each channel is brought up on the small region it actually uses, so results
are reported as they land.

1. **SH-4 core** — the operand cache is configured as on-chip RAM and
   self-tested; this is also the stack/`.bss` for the whole ROM (no external
   RAM used until proven).
2. **Screen and speaker bring-up** — a quick check of the VRAM area the
   framebuffer uses and of the sound RAM area the clips play from. Each
   channel switches on only if its own area passes; the speaker then replays
   every result acquired so far.
3. **Board identification** — Naomi 1 vs Naomi 2 by the Elan chip's ID
   (`E1AD0000` on a Naomi 2, confirmed on a real board). On a Naomi 1 that
   read lands in an unpopulated area and is assumed to return open bus;
   `CFG_BOARD_MODEL=1` avoids it, `=2` selects a known Naomi 2 — see
   [PVR_B_ACCESS.md](docs/PVR_B_ACCESS.md).
4. **BIOS EPROM (IC27)** — CRC32 self-check.
5. **Test-loop relocation** — the memory-test loops are copied into an 8 KB
   block of CPU RAM, tested first, and run from there cached (see
   [Where the test loops execute](#where-the-test-loops-execute)).
6. **Maple bus / MIE** (315-6146 Z80) — version request + factory
   self-test.
7. **Settings EEPROM** (93C46 via MIE) — read and both CRC-checked copies
   verified. Needs a Z80 program uploaded into the MIE first
   (`src/mie_prog.z80`), which then stays resident and also serves the
   board's buttons and DIP switches (both reported on the serial console).
8. **JVS I/O board** — the same program is a JVS master: it resets the
   bus, gives the board address 1, and reports its identification,
   revisions and inputs (players, switches, coin slots, analog channels).
   No board answering is reported as *not present*, not as a fault.

   Steps 6-8 run here, before the memory tests, on the block qualified at
   step 5, so the buttons can interrupt the long part of the suite. On a
   board where no block qualifies they wait for the CPU RAM test and are
   skipped if it fails.
9. **Main CPU RAM** (SDRAM, 16/32 MB, IC9/IC10/IC11S/IC12S) — first a data-bus
   walking-ones test and an address-bus test, then **three phases**, each
   reported as `pass n/3` and each driving the progress bar from 0 to 100%:
   - **1/3** — write `0x55555555` (0101…) over the whole region, then read it
     all back and compare;
   - **2/3** — write `0xAAAAAAAA` (1010…) over the whole region, then read
     back and compare;
   - **3/3** — write a pseudo-random stream, then read it back and compare
     word by word. With `CFG_RAM_CRC=1` a CRC32 kept in a CPU register is
     also accumulated on both sides and compared (see [Building](#building)).

   A fault is reported per chip, and a failed test means the summary calls
   main RAM unusable. If **all** CPU RAM is bad, the program keeps running
   from the SH-4 cache (OC-RAM) and completes every test that does not need
   main RAM.
10. **VRAM** — 16 MB in eight 16 Mbit chips around the graphics chip, tested
    through the 32-bit window as two 8 MB regions, TEX0 (IC16/18/20/22) and
    TEX1 (IC17S/19S/21S/23S), with the same bus tests and three phases.
    Inside a region a chip is a 4 MiB half and a 16-bit data half (see
    [TEX and PVR-A/B diagnostics](#tex-and-pvr-ab-diagnostics)).
11. **Sound RAM** (IC35, 8 MB, behind the AICA IC33 on the G2 bus), same
    tests, every access paced by the G2 FIFO.
12. **Naomi 2 only** — PVR-B VRAM (16 MB, IC111 to IC118S) once its windows
    are shown independent of PVR-A, and the Elan RAM (32 MB,
    IC106/107/108S/109S); see [below](#pvr-b-access-and-the-elan-ram).
13. **Backup NVRAM (IC29)** — non-destructive save/restore test.
14. **RTC** (inside the AICA, IC33) — non-destructive tick check; the date
    it holds is shown.
15. **Serial-number EEPROM** (IC31, 93C46 on SH-4 GPIO) — read + content
    check.
16. **Relocated code integrity** — the relocated loops ran from the very RAM
    under test, so they are read back and compared with the ROM copy.

The **operator actions** are on the operator menu (see
[Operator menu](#operator-menu)): they either write to something or
take long enough that they have no business delaying the report.

- **RAM loops** (`c`, `v`, `s`) — the CPU, video or sound RAM test, pass
  after pass, for intermittent faults.
- **DIMM board** (`d`) — register dump and read stability, then the
  destructive SDRAM test over the G1 DMA; `f` identifies the DIMM's firmware
  flash (see [DIMM board](#dimm-board)).
- **JVS input test** (`j`) — every input of the I/O board, live.
- **Video test pattern** (`m`) — colour bars, crosshatch, grey scale,
  per-component ramps, purity fields and a one-pixel checkerboard, for the
  monitor and the video output stage.
- **Cartridge** (`g`):
  - **Security chip** (X76F100) — presence via response-to-reset.
  - **Content** — identifies the game against an embedded
    database of every known Naomi/Naomi 2 cartridge (192 games, 2298 ICs)
    and verifies each ROM chip by SHA-1, reporting the failing IC by its
    silkscreen name. Identification streams the first chip and snapshots the
    SHA-1 at each known first-ROM size, so a cart is named without hashing
    all of it. Chips are hashed **raw**, decryption not applied, so the
    digests are the ones in the MAME dumps. A cart that matches nothing is
    reported as *unknown content* rather than as faulty — it may simply be a
    dump this database does not carry — and the data-line test below still
    runs, because a dead line is one reason a known cart fails to match.
  - **ROM set completeness** — once the game is identified, the
    **whole set of chips that game needs** is checked: every mask ROM in the
    database entry is probed and the result is stated affirmatively
    (`ROM set: 13 / 13 chips present`). A chip answering a constant
    0xFFFF/0x0000 everywhere is reported as *not responding* (missing,
    unseated, dead) rather than as bad content, and a chip returning the
    same bytes as another one is flagged as an address-aliasing mirror —
    an empty socket that a neighbouring chip answers for. Presence is
    checked on every chip even on QUICK builds; only hashing is trimmed.
  - **Data lines** — per-pin statistics over the 16-bit cartridge
    bus: the share of 1s each line reads (a line that never toggles is
    stuck), plus a comparison of two reads of the same addresses — any bit
    that differs is an unstable line, the signature of a tired bus
    transceiver or a dirty edge connector. This runs even when the game
    cannot be identified, since a dead line is precisely what prevents
    identification.

RAM faults are reported per component: a bit mask, the affected data lanes,
and the silkscreen IC designator (e.g. `CPU RAM 1 (IC9) DEFECTIVE`). VRAM and
Elan RAM chips are named as a *suspect lane (chip or connections)*.

A CPU RAM fault is also checked for a **cut data line**. Each failing bit is
probed on 64 addresses spread over the tested region, on the word parity it
failed on: all written, then a decoy of the opposite polarity so a floating
line cannot simply hold the last value driven, then all read back, with the
bit at 0 and at 1. Wrong on 60 or more is a line, not a cell, and it gets
its own line on screen and in speech — `Line D5 cut on IC9 DQ5`, "Line D,
five, cut on, I C nine, D Q, five" — with how it reads on serial (always 0,
always 1, or floating). The line is named twice: first as the SH-4's 64-bit
bus carries it (an even word is D0-D31, an odd word D32-D63), then as the
chip's own data pin, DQ0-DQ15, the one to probe on the package.

**Address lines** are walked chip by chip on every memory: a step counts
only when a whole 16-bit lane of some cell reads back the value written at
the other address — two addresses landing on one cell — so a data fault is
not taken for an address line. On the CPU RAM the walk also covers the odd
32-bit word, which the classic address test never touches, and the CPU bit
is named as the SDRAM pin it travels on, from the SH-4's multiplexing table
(Renesas SH7750 hardware manual, appendix F: table 9 for 32 MB, table 13
for 16 MB). One pin carries a column bit and a row bit:

| CPU byte-address bit | SDRAM pin (32 MB) | SH-4 pin |
|---|---|---|
| 3-10 | A0-A7, column | A3-A10 |
| 11-20 | A0-A9, row | A3-A12 |
| 21, 22 | A10, A11, row | A13, A14 |
| 23, 24 | BA0, BA1, bank | A15, A16 |

Address lines are common to the four chips: `Address A5 cut on IC10`
("Address line A, five, cut on, I C ten") points at that chip's pin,
`Address BA0 cut, all 4 chips` at the shared trace or the SH-4. The serial
console adds the SH-4 pin and the CPU bits behind the SDRAM pin. VRAM, Elan
RAM and sound RAM sit behind controllers whose multiplexing is not
documented, so for them the report names the CPU bit and the chips it
touched: `Address bit 12 faulty on IC21`. A chip that aliases on many bits
at once is left to the data and cell tests: that is a dead chip or lane,
not cut address lines.

The progress bar retires after the Naomi 2 memories: from there on nothing
is being measured — the NVRAM, the RTC and the serial EEPROM answer yes or
no — and the rows it occupied go to the report instead.

### Time left

The top right corner of the screen counts down to the end of the boot
suite, and the serial console prints the estimate once the plan is known.
Each step has a duration taken from the serial log of a real Naomi 2 (the
one attached to PR #2), spoken results included; what that log does not
cover is extrapolated:

| Step | Loops relocated | No CPU RAM for the loops |
|---|---|---|
| Screen + speaker bring-up | 0:28 | 0:28 |
| Board, BIOS CRC | 0:08 | 0:08 |
| Loop relocation | 0:35 | 0:31 (up to 128 blocks scanned from ROM) |
| MIE, settings EEPROM, JVS | 0:20 | skipped |
| CPU RAM | 0:44 | 12:39 |
| VRAM TEX0 + TEX1 | 0:34 | 6:25 |
| Sound RAM | 7:47 | 7:47 (never relocated) |
| Naomi 2: PVR-B + Elan RAM | 1:28 | 19:00 |
| NVRAM, RTC, serial EEPROM, end | 0:35 | 0:35 |
| **Naomi 1 total** | **11:11** | **28:34** |
| **Naomi 2 total** | **12:40** | **47:34** |

- The MIE stage (about 20 s under MAME), PVR-B (16 MB through the TEX
  window: twice TEX0) and the Elan RAM (32 MB, taken at the VRAM rate) are
  not in that log.
- Without a CPU RAM block the loops run from the boot EPROM. The quick VRAM
  check always does: 3 passes over 600 KB in 14.5 s, 7.9 s per
  megabyte-pass, about 14 times the cached rate. Instruction fetch then
  dominates, so that rate is applied to every memory the loops test.
- The count corrects itself as it goes: inside a memory step it follows the
  passes' progress, the measured duration of the quick VRAM check rescales
  the ROM case, and the CPU RAM test rescales the later memory steps.
  Without audio, the speech time is left out.


### Where the test loops execute

The SH-4 boots in P2, an address window the hardware never caches, so every
instruction is a bus cycle to the boot EPROM. Linking the whole ROM for P1
(the cached alias of the boot area) was tried on real hardware and the board
refuses it outright -- black screen before the first visible instruction --
which matches the original BIOS, that never executes cached from ROM either.

Cached execution from SDRAM is a different matter: it is where every Naomi
game runs. So the memory-test loops and the failure scan -- about 500 bytes, where
essentially all the time goes -- are copied into an 8 KB block of CPU RAM at boot and run
from there, cached, while everything else stays in ROM.

The four CPU RAM chips are interleaved by data lane rather than by address
range -- IC9/IC10 carry the even words, IC11S/IC12S the odd ones -- so every
block spans all four. Blocks are scanned downward from the top and the first
sound one is taken, which gives immunity to a localized fault (a bad row or
column inside one chip) but not to a chip dead across its range: in that case
no block passes and the ROM copies keep being used. A block that fails the
scan is reported as the broken memory it is, not quietly skipped.

A chip dead across its whole range is decided in thirty-two accesses by the
data bus test, before any block is scanned: all 128 would fail for the same
reason, and a megabyte of futile testing from the EPROM would delay the one
thing the operator needs to know. Whatever happens, the report names the
window the loops really execute from -- `8Cxxxxxx`/`8Dxxxxxx` for cached CPU
RAM, `A0xxxxxx` for the boot EPROM -- read back from the pointer that will
actually be called rather than from a flag.

The window is tested with the full pattern and pseudo-random suite before
anything is copied into it, and the memory under test is still addressed
through P2, so the data path stays uncached and the test keeps its coverage.
If no usable RAM is found the pointers keep addressing the ROM copies and the
diagnostic runs slowly rather than not at all -- which is exactly the board
that needs diagnosing. Build with `RELOC=0` to disable it entirely.

## TEX and PVR-A/B diagnostics

VRAM failures use the EPR-23608C BIOS lane mapping: the **4 MiB half** and
**D0–D15 / D16–D31**, rather than word parity. Tables, disassembly evidence,
limits and pinouts are consolidated in [ADDRESS_MAP.md](docs/ADDRESS_MAP.md)
(in French). A named IC identifies a **suspect lane: chip, connections or
controller**, not proof of an internal RAM failure.

### Interpreting failures

- The data mask is expected XOR observed. `00000200` identifies D9;
  `00000300` identifies D8 and D9. At `A5C00000`–`A5FFFFFC`, the BIOS assigns
  these bits to **IC21**. Real-board tests with DQ9 and DQ8/DQ9
  disconnected reproduced these errors. The supplied pinout assigns DQ8
  to pin 39 and DQ9 to pin 40; it does not itself prove CPU-to-DQ wiring.
- The address-test mask identifies **failed steps**, not defective address
  pins. Data faults can produce `007FFFFC`. A single-address data-bus test
  can pass while cells elsewhere in the region fail.
- After a data- or address-bus failure, additional diagnosis checks isolated
  cells (alternating patterns, walking ones and zeros, two reads), then
  address pairs in two write orders, two polarities and two repetitions.
  Samples cover the start, end and power-of-two offsets; they do not replace
  a full memory test.
- `VRAM readback failure` preserves reads and rereads of both cells. A zero
  initial XOR can therefore accompany a reread failure.
  `VRAM coupling candidate` requires isolated cells to pass and then reproduce
  the other cell's pattern in all eight trials. Reported address bits are
  **CPU byte-address bits**, not physical A0–A11 pins. A zero coupling mask
  means that these trials confirmed no coupling.

The first 32 diagnostic events are printed; counters and masks include later
ones. Full passes retain eight failure details but continue counting and
mapping subsequent failures. A mismatch that cannot be located again leaves
multiple IC candidates across the tested range. A clean sample does not
cancel an earlier intermittent failure.

### Rescanning after 90%

The fast verify passes only tell whether something differed. When it did, a
rescan of the region locates the failing words; it prints “Error detected,
locating VRAM failures” (or CPU RAM failures) with its own progress and
checks abort input every 1024 words.

The rescan is hand-written assembly and lives in the relocated block, so it
runs cached from CPU RAM when a block was qualified, from the EPROM
otherwise. It records the first eight failing words in full, for the detail
lines; after that it stops only on a word that brings a data bit not yet
seen in its 4 MiB half and on its word parity — possibly another chip — and
merely counts the others. The error total and the chips named stay exact.

This matters for a cut data line, which fails every word of its half. On a
real Naomi 2 with DQ9 cut on IC21, the former rescan — C from ROM, every
bad word recorded — took about 3 min 20 s per failing pass, and TEX1 took
7 minutes instead of 17 seconds. The rescan now costs about one more read
of the region: a few seconds cached, well under a minute from ROM.

### PVR-B access and the Elan RAM

Before PVR-B is written, access qualification checks that the A and B
windows are independent. Each sampled cell is first written and read back
alone; the bits that fail there are that cell's own fault and are left out
of the comparison that follows, where all four cells hold distinct values
and a change can only come from a write to another window. A cut data line
on one chip — the DQ9 case above — therefore no longer blocks the PVR-B and
Elan tests: it is reported by the RAM test of its own region. A mirror or a
broadcast still is (code 4), and so is a cell too broken to judge (code 5
on A, 6 on B). **Untested means neither healthy nor defective.**

The Elan RAM is the Elan's own memory, not a window onto either GPU, so it
is tested whatever PVR-B's qualification says, unless the Elan itself does
not answer. Its chips follow the BIOS POLY table: even and odd 32-bit words
of the lower 16 MiB are **IC106** and **IC107**, of the upper 16 MiB
**IC108S** and **IC109S** — see [ADDRESS_MAP.md](docs/ADDRESS_MAP.md).
The `v` loop covers PVR-B and the Elan RAM on a Naomi 2, under the same
conditions. See [PVR_B_ACCESS.md](docs/PVR_B_ACCESS.md) for the sequence
and all access codes.

### Maple / MIE communication on real hardware

Maple DMA buffers live in a validated CPU RAM block, usually near the top
of 32 MiB. `MDAPRO` protection is computed from both buffers using inclusive
1 MiB bounds. The old `0x6155404F` constant covered only the first 16 MiB,
leaving high-RAM buffers outside the allowed range and potentially causing
a false "MIE not responding" result. The
[libnaomi driver](https://github.com/DragonMinded/libnaomi/blob/main/libnaomi/maple.c)
uses the same bounds encoding. MAME ignores this protection register, so
emulator success does not validate it on hardware.

Failed detection prints one trace per port: `desc`, `rx`, `mdapro` (the
programmed value), `mdst`, the response header and `isterr_before` /
`isterr_after`. The latter are raw snapshots that may contain old or unrelated
errors; the diagnostic does not clear them. The `status` field distinguishes:

| Status | Meaning |
| --- | --- |
| `invalid-request` | Invalid buffer address, alignment or request size |
| `busy-timeout` | Previous transfer still active; buffers were not reused |
| `dma-timeout` | New transfer still active after 100 ms |
| `rx-unchanged` | DMA finished but the receive sentinel was untouched |
| `no-response` | Response header indicates no response |
| `invalid-version-reply` | Received a reply that is not a valid MIE version |

A communication failure does not by itself identify a defective MIE.
Host tests (`sh tools/test_maple.sh`) check DMA bounds, high RAM, deadlines,
timer wraparound and invalid replies.

Validated on a working Naomi 2 PCB using the **filter board** TEST/SERVICE
buttons: MIE communication, self-test, Z80 program upload and serial-menu
navigation all worked on real hardware. The observed identification before
uploading the Z80 program was (spacing preserved):

```text
MIE on maple port 0, resp cmd 0x00000083
MIE version: "315-6149    COPYRIGHT SEGA E"
```

The self-test status was `00000000`, followed by a successful program upload.
The input report showed raw port `000000FB`, DIP SW1 set to 31 kHz / OFF /
ON / OFF, and both TEST and SERVICE released.

`0x83` answers the `0x82` version request; `00000000` indicates a successful
self-test. This is the string displayed by our response reader, not a required
identity or proof of the chip's physical marking. The reader displays the
first response frame and does not assemble any continuation of the version
text. DIP and button states describe this particular run, not mandatory
values for a healthy board. This validates filter-board buttons, not the
controls of an external JVS I/O board.

## Operator menu

The menu lists all eight actions on both VGA and the serial console. Each
serial entry includes its direct key (`c`, `v`, `s`, `d`, `g`, `f`, `j`, `m`); `>` marks
the current selection. TEST prints the list again with the next selection,
and SERVICE runs it. Navigation needs neither a VGA monitor nor ANSI support.

The boot suite is not the end of it. `a` or a menu key on the serial port,
a board button (**TEST** or **SERVICE**), or the cabinet's **TEST** or player
1 **START** once the JVS board has answered, stops the suite: the test running
at the time stops at its next block and draws **no** verdict from the part it
did — its phases end in `interrupted`, not `ok` — and the suite does not
resume. `a` goes to the report, a button opens the **operator menu**, and a
menu key (`c`, `v`, `s`, `d`, `g`, `f`, `j`, `m`) runs that action straight
away. `h` prints the
help without interrupting anything; other keys are ignored.

The buttons need a small Z80 program uploaded into the MIE first — the
315-6146's factory firmware answers four Maple commands and reading the
buttons is not one of them. The upload happens right after the test loops
are relocated, before the memory tests, as soon as a block of CPU RAM has
been qualified to hold the Maple DMA buffers; the program then stays
resident. So the buttons can interrupt the long part of the suite. On a
board with no usable block the MIE stage falls back to its old place, after
the CPU RAM test.
The cabinet's **TEST** and player 1 **START**, read from the JVS I/O board,
act as the board's **TEST** and **SERVICE** once the board has answered.

The JVS master lives in the same Z80 program. It drives the MIE's
16550-style UART at 115200 baud (divisor 8, the original BIOS's own
set-up), switches the RS-485 driver around each frame as that program
does, and polls the I/O board by itself between Maple packets, one byte at
a time, so a Maple request is always answered at once. The SH-4 only reads
the result, 28 bytes per request.

Keys on the serial console:

| Key | Action |
|---|---|
| `h` | help — the only key that never interrupts |
| `a` | abort the current test and go to the report |
| `c` / `v` / `s` | loop the CPU / video / sound RAM test |
| `d` | full DIMM SDRAM test |
| `g` | game flash SHA-1 integrity |
| `f` | DIMM firmware flash — identify, and choose a version |
| `j` | JVS input test — every switch, coin count and analog channel, live |
| `m` | video test pattern |

On screen, the operator menu lists the same actions: **TEST** (or the
cabinet's TEST) steps through them and wraps; **SERVICE** (or player 1
**START**) runs the selection. The
three RAM loops run until a button is pressed and nothing else stops them —
that is the point, since an intermittent fault shows up on the tenth pass,
not the first. The one exception: when the buttons are unavailable (the MIE
never answered the uploaded program), `a` stops a loop too, or it could only
be stopped by a reset. The video loop covers TEX0 and TEX1, plus PVR-B and
the Elan RAM on a Naomi 2, and every loop names a failing chip the way the
boot suite does.

The DIMM, cartridge and flash actions each start a report of its own, print
it, and wait for **TEST** or **SERVICE** to bring the operator menu back.

The video test pattern shows ten full-screen images in turn: colour bars, a
crosshatch for geometry and convergence, a 16-step grey scale, red, green,
blue and white ramps (a stuck DAC bit shows as banding), white, red, green,
blue and black fields for purity, and a one-pixel checkerboard for
bandwidth. **TEST** or any serial key shows the next one; **SERVICE**,
**START**, `a` or `q` leaves. The border stops pulsing while they are up. The
output is the ROM's own 640x480 at 31 kHz, so a 15 kHz monitor shows
nothing.

The JVS input test shows each input the I/O board declared: the system
switches (TEST, TILT1-3), each player's START, SERVICE, four directions
and buttons, lit while pressed; coin counters; analog channels in hex; and
the raw switch bytes for a board whose layout differs. The serial console
prints a line whenever a switch or a coin count changes. The cabinet's own
TEST is one of the inputs under test, so the way out is a board button or
any serial key.

Two of these are operator-initiated precisely because they are not safe to
run unattended: the DIMM SDRAM test overwrites whatever game is loaded in the
DIMM (not the firmware, which runs from its own RAM), and the flash action
touches the DIMM's firmware flash — read-only for now, see below.

## Building

Toolchain: Debian `gcc-sh-elf` / `binutils-sh-elf`.

```sh
make LANG=EN            # -> NaomiDIAG_EN.bin (English text + voice)
make LANG=FR            # -> NaomiDIAG_FR.bin (French text + voice)
make LANG=EN QUICK=1    # 1 MB per RAM pass, for fast emulator bring-up
make LANG=EN BAUD=115200  # serial console at 115200 instead of 57600
```

[`src/config.h`](src/config.h) holds the options that are a decision about
the ROM rather than a per-build variation. The Makefile carries what changes
from one build to the next — `LANG`, `QUICK`, `RELOC`, `BAUD`, `ROM_BASE`; the header
carries what is edited once and stays. Anything in it can still be overridden
without touching the file:

```sh
make LANG=EN CFLAGS_EXTRA=-DCFG_LANE_BEACON=1
```

**`CFG_RAM_CRC`** (off by default) restores the CRC32 comparison in the RAM
cell tests. The specification asked for it, it is implemented and it works —
but it is provably redundant and expensive. Redundant, because the same loop
already compares every word against the value it should hold: if no word
differed, the two byte streams are identical and their CRCs cannot differ.
Expensive, because a CRC-32 costs 20 instructions per 32-bit word and there
are two of them — **40 of the 59 instructions** in the read-back loop, against
**3** for the word comparison that does the actual detecting. With it off the
loop is 17 instructions per word, about three times faster, and the test
loses no ability to find or locate a fault.

**`CFG_LANE_BEACON`** (off by default) enables the lane beacon: after the
report it cycles through the twelve 16-bit slices of the CPU RAM and VRAM
buses, naming one at a time and hammering it so a scope on the chips shows
which package carries which lane. It is a bench instrument for establishing
the lane-to-designator map, not part of diagnosing a board — it never ends,
so the report stays up but the machine never settles. Turn it on when you
have a probe in hand.

**`AUDIO=0`** builds a silent ROM: the spoken-clip table is replaced by a
stub and the image drops to 6 % of the EPROM. It was introduced when the
clips were 16-bit PCM and left no room for anything else; since they became
ADPCM a full build sits at about 50 %, so this is no longer a way of
making room — it is for a bench where the speech is in the way, and it boots
a little faster. `AUDIO=1` is the default.

```sh
make LANG=EN AUDIO=0
```

Note that the shell often exports `LANG=fr_FR.UTF-8`, which overrides the
Makefile's `LANG ?= EN`. Always pass `LANG=` explicitly on every `make`,
including `make mame-rom`, or a target will rebuild with different flags.


A 2 MB image is produced, ready to burn on a 27C160 EPROM (IC27). The build
refuses to produce an image larger than 2 MB (no silent truncation).

The same 27C160 image works on both Naomi 1 and Naomi 2 (both use a 2 MB
BIOS); the board is detected at runtime.

Regenerating generated sources (rarely needed, committed in the repo):

```sh
make audio     # re-render the spoken clips (needs the Piper venv + sox)
               # TTS -> 22050 Hz mono -> ADPCM (tools/adpcm.py)
make cartdb    # rebuild the cartridge SHA-1 DB from `mame -listxml`
make mieprog   # rebuild src/mie_prog.h from the Z80 source (z80asm)
```

## Testing under MAME

```sh
make LANG=EN QUICK=1 mame-rom
mame naomi -rompath ../roms_diag -autoboot_script mame/scif_tap.lua
```

The MAME scripts all live in [`mame/`](mame). `scif_tap.lua` captures the
SH-4 SCIF transmit FIFO and prints the serial console (MAME does not wire the
SCIF to anything). The `*_fault*.lua` scripts inject RAM faults for negative
testing. Note: main RAM is fastram
under the DRC, so fault injection into it needs `-nodrc`.

MAME (0.288) does not emulate the Naomi 2's own hardware: its `naomi2` has
one PowerVR, mirrors the PVR-B window onto PVR-A, reads 0 at the Elan ID and
has no Elan RAM, so the ROM sees a Naomi 1 there. `mame/elan_id.lua` fakes
the Elan ID so the Naomi 2 path can be exercised: it ends, correctly, in an
access refusal (code 4, the mirror) and a failing Elan RAM. The JVS master
can be tested: MAME's `naomi` carries an emulated 837-13551 I/O board.

## Real hardware notes

- Burn `NaomiDIAG_xx.bin` on a 27C160 (IC27). Serial output is on the SCIF
  pins at 3.3 V logic — use a 3.3 V USB-serial adapter, never RS-232 levels.
  For where to connect it on the board, refer to the
  [JinGasa](https://github.com/Tchan0/JinGasa) project: it exists solely to
  talk to a Naomi over the serial port and documents the wiring, which is
  more than can be said for the pinouts circulating elsewhere.
- **Set the terminal to 57600 baud, 8N1, no flow control.** The ROM says so
  itself in its first lines, once the console is up.

  With the SH-4's 50 MHz peripheral clock, the rate is
  `Pck/(32*(SCBRR+1))`. The ROM uses `SCBRR2 = 26`, giving approximately
  **57870 baud, 0.47 % above 57600**, confirmed on a Naomi 2 with a USB
  serial adapter. `make BAUD=115200` builds the former setting instead:
  `SCBRR2 = 13`, **111607 baud, 3.1 % below 115200** — inside UART tolerance,
  with less margin.
- A failed cache test means the SH-4 itself is dead: it is reported on SCIF
  and the ROM halts.
- A CPU exception is reported and the ROM halts, on every channel that is up
  and in this order: **serial** first (cause code `EXPEVT` and the faulting
  address — it needs no stack, so it comes out even when the stack is what
  broke), then a red line on the **screen**, then the words *"CPU
  exception"* on the **speaker**. The border turns solid red: the machine
  has stopped. Only an exception in the very first instructions, before the
  vector table is installed, restarts the ROM instead.
- The DIMM-board fan is monitored only by the DIMM firmware.

## Status and limitations

- The IC designators were **read off a board** and are listed in
  [`docs/ADDRESS_MAP.md`](docs/ADDRESS_MAP.md). An earlier version derived
  them from the order the original BIOS RAM TEST prints its numbers, which
  was wrong three times over — including having the CPU RAM and GPU RAM
  groups the wrong way round — so nothing rests on that inference any more.
- VRAM lanes now follow the EPR-23608C BIOS calculation documented in
  [ADDRESS_MAP.md](docs/ADDRESS_MAP.md). Physical wiring and address pins
  still need verification. The **S** suffix denotes the PCB underside.
- For WORK, IC10 on D16–D31 of the even word was validated by repair.
  IC9, IC11S and IC12S still follow the inferred lane order without
  individual measurements; the lane beacon can verify them.
- Every RAM chip the ROM can name has its own spoken clip, Naomi 2 chips
  included (TEX1, PVR-B, Elan RAM), and the **S** suffix is spoken: a fault
  on IC11S is heard as "I C eleven S". The voice is Piper text-to-speech, so
  the screen and the serial console remain authoritative.
- The Naomi 2 paths — board identification by the Elan ID, the Elan's
  initialisation, the PVR-B access check, the PVR-B and Elan RAM tests —
  are covered by host-side tests but have run on no real Naomi 2 yet, and
  MAME cannot run them. The Elan ID read on a Naomi 1 has not been tried on
  a real board either; `CFG_BOARD_MODEL=1` removes it.
- The JVS master has been checked against MAME's emulated 837-13551 I/O
  board. MAME does not model the UART's timing or the RS-485 direction, so
  those follow the original BIOS's program and still need a real cabinet.
  Only one I/O board is addressed (address 1); a daisy chain is not
  enumerated.

## DIMM board

The mailbox protocol between the Naomi and a DIMM board is not publicly
documented. What has been recovered from the board's own firmware is written
up in [`docs/DIMM_FIRMWARE.md`](docs/DIMM_FIRMWARE.md): the load base, the
mailbox window as the DIMM sees it, the response format, the command
dispatcher, and the two VxWorks message queues behind it.

The short version is that **the mailbox itself offers a diagnostic ROM
nothing** — no identity, no version, no memory test, no reflash. The only
three commands it accepts from the Naomi are a doorbell for a BSD socket
proxy, and one of them is a no-op.

What does work goes around it, on the G1 bus:

- **`d` — DIMM SDRAM test.** Holly's GD-DMA has a direction bit; with
  `SB_GDDIR = 1` the Naomi writes system RAM into the DIMM. The ROM uses it
  for a real memory test (`0x01010101`, `0x10101010`, CRC-32, one-second DMA
  timeout). It overwrites the loaded game, so it is on the operator menu only.
- **`f` — DIMM firmware flash.** The flash is reachable through the G1
  ROM-board PIO with AMD commands. The ROM performs a read-ID, which is
  non-destructive, and offers a 3.17 / 4.01 / 4.03 selection. **Writing is
  deliberately not armed.** Sega's own updater was decompiled and it does a
  single chip-erase followed by a full-image reprogram — so the board's
  two-slot recovery net survives a successful flash but not an interrupted
  one. That waits on hardware validation, not on more reading.

The supporting analysis of SEGA's firmware images stays out of this
repository on purpose.

## Credits

Bootstrapped from the [JinGasa](https://github.com/Tchan0/JinGasa) minimal
Naomi BIOS project. Register-level details cross-checked against MAME and
libnaomi. Spoken clips generated with [Piper](https://github.com/rhasspy/piper).
