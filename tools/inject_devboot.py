#!/usr/bin/env python3
"""Put the G1-unlock kilobyte into a built NaomiDIAG image.

    inject_devboot.py NaomiDIAG_xx.bin develop.ic27

Holly keeps the bus to the cartridge or DIMM closed until a key is written
to 0x5F74E4 and a known stretch of the boot ROM is read through it (the
Dreamcast's GD-ROM lock). The retail BIOS checks its whole 2 MB, which a
different ROM cannot pass; the Naomi development BIOS ("develop.ic27" or
"develop110.ic27" in MAME's naomi set) keys 0x3FF, checking only its bytes
at 0x100-0x4FF. NaomiDIAG keeps that range free (0xFF) and reads it the
same way.

Those 1024 bytes are SEGA's: they are copied here from the dump you own,
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
blk = dev[0x100:0x500]
# The two MAME development BIOS dumps share this kilobyte; anything else
# is refused rather than burnt on a hunch.
KNOWN = {"339920f56d0b80ef96444fffbc384374077a0e9b"}
h = hashlib.sha1(blk).hexdigest()
if h not in KNOWN:
    sys.exit(f"{dev_path}: bytes 0x100-0x4FF (sha1 {h}) are not the known "
             "development BIOS bootstrap")
if any(b != 0xFF for b in rom[0x100:0x500]):
    if rom[0x100:0x500] == blk:
        print("already injected")
    else:
        sys.exit(f"{rom_path}: 0x100-0x4FF is not free (not a NaomiDIAG image "
                 "with the unlock hole?)")
rom[0x100:0x500] = blk
crc = zlib.crc32(bytes(rom[:0x1FFFFC])) & 0xFFFFFFFF
rom[0x1FFFFC:] = crc.to_bytes(4, "little")
open(rom_path, "wb").write(rom)
print(f"{rom_path}: unlock kilobyte injected, ROM CRC32 = {crc:08X}")
