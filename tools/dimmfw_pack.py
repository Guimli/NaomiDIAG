#!/usr/bin/env python3
"""dimmfw_pack.py SEGADIMM_ZIP FW403_BIN OUT.bin -- LOCAL USE ONLY.

Packs SEGA NetDIMM firmware slots, taken from the owner's own dumps, into the
blob that a local NaomiDIAG build embeds (make LANG=.. DIMMFW=1 ...). The blob
holds SEGA code: it is never committed nor published (local/ is git-ignored,
and so is every *.bin).

The seven distinct slot images are compressed together as ONE raw LZMA1
stream ("solid"): the versions share most of their code and LZMA's 8 MB
window finds it from one image to the next -- 477 KB for the seven, where
DEFLATE image by image needs 1.4 MB for four. The Naomi decodes the stream
from its start up to the image wanted.

  stream order        source                      slot
  4.01                401_203.bin  upper half       1
  2.03                401_203.bin  lower half       0
  3.17                317_312.bin  upper half       1
  3.12                317_312.bin  lower half       0
  2.17                217_203.bin  upper half       1
  2.13                213_203.bin  upper half       1
  2.06                fpr23718.ic36 upper half      1  (version word 0x02060000, built Jul 11 2001)
  extra entries, no data:
  4.03 CD-R           FW_Netdimm_403.bin upper half = 4.01 + byte patch, slot 1
  2.03                203_203.bin upper half == its lower half: slot 1 alias
FW_Netdimm_403.bin's LOWER half is the Naomi updater program, not a DIMM
slot 0: it is never packed.

Every slot is checked: whole-slot CRC-32 (0xEDB88320, init FFFFFFFF, no
final XOR) == 0, as the DIMM boot loader requires; slot 0 images must start
with the DIMM boot code. Each entry keeps the slot's check word (its last 4
bytes, different for every version): the Naomi names the versions already in
the flash from it and compares it after unpacking.

Format (little-endian):
  header 32 bytes: "DFW2", u32 count, u32 stream_off, u32 stream_len,
                   u32 total_out, u32 lc, u32 lp, u32 pb
  count x 48-byte entries:
    u32 slot, char label[20], u32 out_off (in the decoded stream), u32 0,
    u32 base (entry patched, or 0xFFFFFFFF), u32 npatch, u32 patch_off,
    u32 check word
  then the patch words, u32 (offset << 8 | byte), then the LZMA stream.
"""
import lzma, struct, sys, zipfile, zlib

SLOT = 0x100000
LC, LP, PB = 1, 1, 1          # SH-4 code: 16-bit instructions
DICT = 1 << 23


def boot_crc(b):
    return zlib.crc32(b) ^ 0xFFFFFFFF


def main():
    zpath, f403, out = sys.argv[1:4]
    z = zipfile.ZipFile(zpath)
    rd = {n: z.read(n) for n in ('401_203.bin', '317_312.bin', '217_203.bin',
                                 '213_203.bin', '203_203.bin', 'fpr23718.ic36')}
    rd['FW_Netdimm_403.bin'] = open(f403, 'rb').read()
    for n, d in rd.items():
        if len(d) != 2 * SLOT:
            sys.exit(f'{n}: {len(d)} octets, 2 Mo attendus')
    lo = lambda n: rd[n][:SLOT]
    hi = lambda n: rd[n][SLOT:]
    stream = [  # (slot, label, data)
        (1, '4.01', hi('401_203.bin')),
        (0, '2.03', lo('401_203.bin')),
        (1, '3.17', hi('317_312.bin')),
        (0, '3.12', lo('317_312.bin')),
        (1, '2.17', hi('217_203.bin')),
        (1, '2.13', hi('213_203.bin')),
        (1, '2.06', hi('fpr23718.ic36')),       # fpr23718.ic36: version word 0x02060000
    ]
    for slot, lab, d in stream:
        if boot_crc(d) != 0:
            sys.exit(f'slot {slot} {lab}: CRC-32 du slot != 0, image refusee')
        if slot == 0 and d[:2] != b'\x0e\xd0':
            sys.exit(f'slot 0 {lab}: ne commence pas par le code d\'amorcage DIMM')
    if hi('203_203.bin') != lo('401_203.bin'):
        sys.exit('203_203.bin : slot 1 different du slot 0 2.03, alias refuse')
    d403 = hi('FW_Netdimm_403.bin')
    if boot_crc(d403) != 0:
        sys.exit('4.03 : CRC-32 du slot != 0')

    word = lambda d: struct.unpack('<I', d[SLOT - 4:])[0]
    ents = []           # slot, label, out_off, base, patch list, check
    for i, (slot, lab, d) in enumerate(stream):
        ents.append([slot, lab, i * SLOT, 0xFFFFFFFF, [], word(d)])
    ref = stream[0][2]
    pat = [(k << 8) | d403[k] for k in range(SLOT) if d403[k] != ref[k]]
    ents.append([1, '4.03 CD-R', 0, 0, pat, word(d403)])
    ents.append([1, '2.03', 1 * SLOT, 0xFFFFFFFF, [], word(lo('401_203.bin'))])

    raw = b''.join(d for _, _, d in stream)
    filt = [{'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME,
             'dict_size': DICT, 'lc': LC, 'lp': LP, 'pb': PB}]
    comp = lzma.compress(raw, format=lzma.FORMAT_RAW, filters=filt)
    if lzma.decompress(comp, format=lzma.FORMAT_RAW, filters=filt)[:len(raw)] != raw:
        sys.exit('relecture LZMA differente')

    hdr = 32 + 48 * len(ents)
    patches = bytearray()
    poffs = []
    for e in ents:
        poffs.append(hdr + len(patches) if e[4] else 0)
        for p in e[4]:
            patches += struct.pack('<I', p)
    soff = hdr + len(patches)
    blob = bytearray(b'DFW2' + struct.pack('<7I', len(ents), soff, len(comp),
                                           len(raw), LC, LP, PB))
    for e, po in zip(ents, poffs):
        slot, lab, ooff, base, pl, chk = e
        blob += struct.pack('<I20sIIIIII', slot, lab.encode(), ooff, 0, base,
                            len(pl), po, chk)
    blob += patches + comp
    open(out, 'wb').write(blob)
    for slot, lab, ooff, base, pl, chk in ents:
        what = (f'flux +0x{ooff:06X}' if base == 0xFFFFFFFF
                else f'correctif de {len(pl)} octets sur l\'entree {base}')
        print(f'  slot {slot}  {lab:10s} mot de controle {chk:08X}  {what}')
    print(f'{out} : {len(blob)} octets ({len(blob) // 1024} Ko), '
          f'flux LZMA {len(comp) // 1024} Ko pour {len(raw) // SLOT} Mo')


main()
