"""Read-only evidence extraction for the user-supplied EPR-23608C BIOS."""
import hashlib
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
print('SHA1', hashlib.sha1(data).hexdigest())
if hashlib.sha1(data).hexdigest() != '25ef957ec1c58fdaff5e89102002bca6c38832c5':
    raise SystemExit('Unsupported BIOS: offsets below apply only to EPR-23608C.')
# Both payloads contain the same byte-error accumulator and half selector.
assert data[0x28420:0x28508] == data[0x1b01bc:0x1b02a4]
assert data[0x25454:0x2548a] == data[0x1ad1c8:0x1ad1fe]
for nums in ([4,16,18,20,22], [4,17,19,21,23], [4,111,113,115,117], [4,112,114,116,118]):
    needle = struct.pack('<'+'I'*len(nums), *nums)
    start = 0
    while (pos := data.find(needle, start)) >= 0:
        print('IC table', nums, hex(pos))
        start = pos+1
print('Naomi 2 descriptors: name, start, end-exclusive, block size, ICs')
def rom_offset(ptr):
    return ptr - 0x0c000000 + 0x188000
for off in range(0x1f4790, 0x1f4890, 32):
    name, start, end, _, block, _, _, table = struct.unpack_from('<8I', data, off)
    n = rom_offset(name)
    name = data[n:data.index(0,n)].decode('ascii')
    t = rom_offset(table)
    count = struct.unpack_from('<I',data,t)[0]
    chips = struct.unpack_from('<'+'I'*count,data,t+4)
    print(f'{off:06x} {name}: {start:08x}..{end:08x} block={block:x} ICs={chips}')
