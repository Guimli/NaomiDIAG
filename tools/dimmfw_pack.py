#!/usr/bin/env python3
"""dimmfw_pack.py SEGADIMM_ZIP FW403_BIN OUT.bin -- LOCAL USE ONLY.

Packs SEGA NetDIMM firmware slots, taken from the owner's own dumps, into the
blob that a local NaomiDIAG build embeds (make LANG=.. AUDIO=0 DIMMFW=1 ...).
The blob holds SEGA code: it is never committed nor published (local/ is
git-ignored, and so is every *.bin).

Entries (1 MB slots, raw DEFLATE; 4.03 stored as a byte patch on 4.01):
  slot 1  4.01        401_203.bin  upper half
  slot 1  4.03 CD-R   FW_Netdimm_403.bin upper half (patch on 4.01)
  slot 1  3.17        317_312.bin  upper half
  slot 0  2.03        401_203.bin  lower half
  slot 0  3.12        317_312.bin  lower half
FW_Netdimm_403.bin's LOWER half is the Naomi updater program, not a DIMM
slot 0: it is never packed.

Every slot is checked: whole-slot CRC-32 (0xEDB88320, init FFFFFFFF, no
final XOR) == 0, as the DIMM boot loader requires. The table also stores the
slot's check word (its last 4 bytes, different for every version): the Naomi
reads it at 0x0FFFFC / 0x1FFFFC to name the versions already in the flash,
and compares it after unpacking.

Format (little-endian):
  "DFW1", u32 count, then count x 48-byte entries:
    u32 slot, char label[20], u32 data_off, u32 data_len,
    u32 base (index, or 0xFFFFFFFF), u32 npatch, u32 patch_off, u32 check word
  data: DEFLATE streams; patches: u32 (offset << 8 | byte)
"""
import struct, sys, zipfile, zlib

SLOT = 0x100000


def boot_crc(b):            # DIMM loader CRC: no final XOR
    return zlib.crc32(b) ^ 0xFFFFFFFF


def main():
    zpath, f403, out = sys.argv[1:4]
    z = zipfile.ZipFile(zpath)
    i401 = z.read('401_203.bin')
    i317 = z.read('317_312.bin')
    i403 = open(f403, 'rb').read()
    for n, d in (('401_203.bin', i401), ('317_312.bin', i317), ('FW_Netdimm_403.bin', i403)):
        if len(d) != 2 * SLOT:
            sys.exit(f'{n}: {len(d)} octets, 2 Mo attendus')
    ents = [  # (slot, label, data, base)
        (1, '4.01', i401[SLOT:], None),
        (1, '4.03 CD-R', i403[SLOT:], 0),
        (1, '3.17', i317[SLOT:], None),
        (0, '2.03', i401[:SLOT], None),
        (0, '3.12', i317[:SLOT], None),
    ]
    for slot, lab, d, _ in ents:
        if boot_crc(d) != 0:
            sys.exit(f'slot {slot} {lab}: CRC-32 du slot != 0, image refusee')
        if slot == 0 and d[:2] != b'\x0e\xd0':
            sys.exit(f'slot 0 {lab}: ne commence pas par le code d\'amorcage DIMM')
    hdr_len = 8 + 48 * len(ents)
    data = bytearray()
    table = []
    for slot, lab, d, base in ents:
        if base is None:
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = c.compress(d) + c.flush()
            doff = hdr_len + len(data)
            data += comp
            while len(data) % 4:
                data += b'\0'
            pat = []
            table.append([slot, lab, doff, len(comp), 0xFFFFFFFF, 0, 0, struct.unpack('<I', d[SLOT - 4:])[0]])
        else:
            ref = ents[base][2]
            pat = [(i << 8) | d[i] for i in range(SLOT) if d[i] != ref[i]]
            poff = hdr_len + len(data)
            for p in pat:
                data += struct.pack('<I', p)
            table.append([slot, lab, 0, 0, base, len(pat), poff, struct.unpack('<I', d[SLOT - 4:])[0]])
    blob = bytearray(b'DFW1' + struct.pack('<I', len(ents)))
    for slot, lab, doff, dlen, base, npat, poff, crc in table:
        blob += struct.pack('<I20sIIIIII', slot, lab.encode(), doff, dlen, base, npat, poff, crc)
    blob += data
    open(out, 'wb').write(blob)
    for slot, lab, doff, dlen, base, npat, poff, crc in table:
        what = f'{dlen // 1024} Ko compresses' if base == 0xFFFFFFFF else f'correctif de {npat} octets sur l\'entree {base}'
        print(f'  slot {slot}  {lab:10s} mot de controle {crc:08X}  {what}')
    print(f'{out} : {len(blob)} octets ({len(blob) // 1024} Ko)')


main()
