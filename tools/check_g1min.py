#!/usr/bin/env python3
"""check_g1min.py image.bin : verifies the G1 unlock block at ROM 0x000-0x017.

crt0.S starts the ROM with a block that writes key 0x15 to Holly (0x5F74E4)
at reset; Holly then checks ROM 0x000-0x015 as it crosses the bus. That block
was validated byte for byte on a real Naomi 2. This tool refuses an image
whose block differs, or whose checksum over 0x000-0x015 is not 0.

Checksum (Holly G1, "v5"): each byte spread 8 -> 16 bits (bit m to bit 2m),
A += spread(byte) XOR fold; on a carry out of 16 bits, A &= 0xFFFF and fold
toggles between 0 and 0x1104. Over ROM 0x000-0x015 A must end at 0."""
import sys

EXPECTED = bytes.fromhex("5fe0184074cb1840e4cb15e5522000e802a009e84b6b0900")


def spread(b):
    b = (b | b << 4) & 0x0F0F
    b = (b | b << 2) & 0x3333
    return (b | b << 1) & 0x5555


def v5(data):
    a = fold = 0
    for byte in data:
        a += spread(byte) ^ fold
        if a >= 0x10000:
            a &= 0xFFFF
            fold ^= 0x1104
    return a, fold


rom = open(sys.argv[1], "rb").read()
if rom[:len(EXPECTED)] != EXPECTED:
    sys.exit(f"ERROR: {sys.argv[1]}: G1 unlock block at ROM 0x000-0x017 differs from the "
             f"validated one\n  got      {rom[:len(EXPECTED)].hex()}\n  expected {EXPECTED.hex()}")
a, fold = v5(rom[:0x16])
if a != 0:
    sys.exit(f"ERROR: {sys.argv[1]}: G1 checksum over ROM 0x000-0x015 = 0x{a:04X}, must be 0")
print(f"G1 unlock block OK: key 0x15, checksum 0x000-0x015 = 0x0000 (fold 0x{fold:04X})")
