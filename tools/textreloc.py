#!/usr/bin/env python3
"""textreloc.py <elf> <bin> -- fills the table that lets the ROM run its code
from CPU RAM (text_install, src/reloc.c).

The program is linked for the EPROM. A copy of .text placed elsewhere runs
as it is except for the absolute addresses its literal pools hold: the
branches are PC-relative, but a call through `jsr @rn` or a function pointer
loads a 32-bit link-time address. The link keeps its relocations
(--emit-relocs), and this lists every R_SH_DIR32 in .text whose target is
.text: the word offsets the copy must shift. Nothing else is touched --
pointers to .rodata stay EPROM addresses on purpose (the program compares
some of them, e.g. e->comps == pvrb_comps, across the switch).

Left out: the block of loops reloc_install copies to its own window
(reloc_blk_start..end) -- it must stay identical to the EPROM image, which
reloc_verify compares it against.

Refused (the build fails): a relocation in .text of any other type that
reaches outside .text, a target outside .text that is not .rodata/.bss/
OC-RAM/.ramtext/hardware, or more entries than the table holds.

The table (textreloc_tab, src/reloc.c): u32 magic 'TRL1', u32 count, u32
text size, then count u16 word offsets from the start of .text."""
import os, re, struct, subprocess, sys

elf, binf = sys.argv[1], sys.argv[2]
cross = os.environ.get("CROSS", "sh-elf-")
env = dict(os.environ, LC_ALL="C")

syms = {}
for l in subprocess.run([cross + "nm", elf], capture_output=True, text=True, env=env, check=True).stdout.splitlines():
    f = l.split()
    if len(f) == 3:
        syms[f[2]] = int(f[0], 16)
ts, te = syms["_start"], syms["__text_end"]
rb0, rb1 = syms["_reloc_blk_start"], syms["_reloc_blk_end"]
tab = syms["_textreloc_tab"]
rom = ts & ~0x1FFFFF                       # the 2 MB window the image is linked in

data = bytearray(open(binf, "rb").read())
def word(a):
    return struct.unpack_from("<I", data, a - rom)[0]

offs, sec, bad = [], None, []
for l in subprocess.run([cross + "readelf", "-rW", elf], capture_output=True, text=True, env=env, check=True).stdout.splitlines():
    m = re.match(r"Relocation section '(\S+)'", l)
    if m:
        sec = m.group(1)
        continue
    f = l.split()
    if sec != ".rela.text" or len(f) < 3 or not f[2].startswith("R_SH_"):
        continue
    at = int(f[0], 16)
    if f[2] != "R_SH_DIR32":
        # PC-relative: fine as long as its target moves with the code
        tgt = int(f[3], 16) if len(f) > 3 else 0
        if not ts <= tgt < te:
            bad.append(f"{f[2]} at {at:08x} reaches {tgt:08x}, outside .text")
        continue
    v = word(at)
    if ts <= v < te:
        if rb0 <= at < rb1:
            continue
        offs.append((at - ts) // 4)
        if (at - ts) & 3:
            bad.append(f"unaligned DIR32 at {at:08x}")
    elif not (te <= v < rom + 0x200000 or 0x7C000000 <= v < 0x80000000 or
              (v & 0x1FFFFFFF) >= 0x0C000000 or v >= 0xE0000000 or
              (v & 0x1FFFFFFF) < 0x01000000):
        bad.append(f"DIR32 at {at:08x} -> {v:08x}: unexpected target")

magic, cap = struct.unpack_from("<II", data, tab - rom)
if magic != 0x4C525450:                     # 'PTRL': the placeholder
    bad.append(f"textreloc_tab placeholder not found (magic {magic:08x})")
cap_entries = cap                            # the placeholder's second word: capacity
if len(offs) > cap_entries:
    bad.append(f"{len(offs)} relocations > table capacity {cap_entries}")
if max(offs, default=0) > 0xFFFF:
    bad.append(".text larger than 256 KB: u16 word offsets overflow")
if bad:
    for b in bad:
        print("textreloc:", b)
    sys.exit(1)
struct.pack_into("<III", data, tab - rom, 0x314C5254, len(offs), te - ts)   # 'TRL1'
struct.pack_into(f"<{len(offs)}H", data, tab - rom + 12, *offs)
open(binf, "wb").write(data)
print(f"textreloc: {len(offs)} code addresses to shift, .text {te - ts} bytes")
