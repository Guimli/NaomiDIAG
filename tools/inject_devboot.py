#!/usr/bin/env python3
"""Put the development BIOS's G1-unlock bytes into a built NaomiDIAG image.

    inject_devboot.py NaomiDIAG_xx.bin develop.ic27

Holly keeps the bus to the cartridge or DIMM closed until a key is written
to 0x5F74E4 and a known stretch of the boot ROM is read through it (the
Dreamcast's GD-ROM lock). The retail BIOS checks its whole 2 MB, which a
different ROM cannot pass; the Naomi development BIOS ("develop.ic27" or
"develop110.ic27" in MAME's naomi set) keys 0x3FF and then reads its bytes
0x100-0x1FFF. How many of them the key covers is not known, so all of them
are copied: NaomiDIAG keeps that range free (0xFF) and reads it the same
way.

Those 7936 bytes are SEGA's: they are copied here from the dump you own,
into your local image, and are never part of the NaomiDIAG sources or
releases. The image's CRC32 (last 4 bytes) is recomputed afterwards."""
import hashlib, sys, zlib

if len(sys.argv) != 3:
    sys.exit(__doc__)
rom_path, dev_path = sys.argv[1], sys.argv[2]
rom = bytearray(open(rom_path, "rb").read())
dev = open(dev_path, "rb").read()
if len(rom) != 0x200000:
    sys.exit(f"{rom_path}: not a 2 MB NaomiDIAG image")
if len(dev) != 0x200000:
    sys.exit(f"{dev_path}: not a 2 MB BIOS dump")
LO, HI = 0x100, 0x2000
blk = dev[LO:HI]
# The two MAME development BIOS dumps (develop.ic27, develop110.ic27):
# the same first kilobyte, different bytes after it. Anything else is
# refused rather than burnt on a hunch.
KNOWN = {"aca11b23b2e7f973af0eacbda985a3163ad36ae4",
         "6da7ba02e923e3d59191a6f89fec7aa303e81122"}
h = hashlib.sha1(blk).hexdigest()
if h not in KNOWN:
    sys.exit(f"{dev_path}: bytes 0x100-0x1FFF (sha1 {h}) are not the known "
             "development BIOS bootstrap")
if any(b != 0xFF for b in rom[LO:HI]):
    if rom[LO:HI] == blk:
        print("already injected")
    else:
        sys.exit(f"{rom_path}: 0x100-0x1FFF is not free (not a NaomiDIAG image "
                 "with the unlock hole?)")
rom[LO:HI] = blk
crc = zlib.crc32(bytes(rom[:0x1FFFFC])) & 0xFFFFFFFF
rom[0x1FFFFC:] = crc.to_bytes(4, "little")
open(rom_path, "wb").write(rom)
print(f"{rom_path}: dev BIOS unlock bytes injected, ROM CRC32 = {crc:08X}")
