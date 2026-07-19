#!/usr/bin/env python3
"""Store the CRC32 (IEEE) of the first 2MB-4 bytes of the ROM into its
last 4 bytes (little-endian), for the in-ROM BIOS self-test."""
import sys, zlib

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
assert len(data) == 0x200000, f"ROM must be 2MB, got {len(data)}"
crc = zlib.crc32(bytes(data[:0x1FFFFC])) & 0xFFFFFFFF
data[0x1FFFFC:] = crc.to_bytes(4, "little")
open(path, "wb").write(data)
print(f"patched ROM CRC32 = {crc:08X}")
