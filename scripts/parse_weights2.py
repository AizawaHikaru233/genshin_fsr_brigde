#!/usr/bin/env python3
"""Parse WEIGHTS_HT by the format confirmed from parser disassembly:
entry = u64 name_len, name[name_len], u64 size_a, u64 size_b, u64 size_c, u32 flag, data...
"""
import struct, sys, os

dll = sys.argv[1]
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)
data = open(dll, 'rb').read()
res_off = 0x1149cb8
res = data[res_off:res_off + 0x8cda732]

pos = 0
# header? first u64 may be name_len of first entry directly (pos 0)
# check first 8 bytes: 0x08cda732 as u64 -> resource size marker, so header is 8 bytes
u0 = struct.unpack_from('<Q', res, 0)[0]
print('first u64: 0x%x (resource size 0x%x)' % (u0, len(res)))
pos = 8
blobs = []
while pos + 8 <= len(res):
    name_len = struct.unpack_from('<Q', res, pos)[0]
    if name_len > 0x100:
        print('stop: bad name_len %d at 0x%x' % (name_len, pos))
        break
    name_b = res[pos+8:pos+8+name_len]
    if not all(0x20 <= b < 0x7f for b in name_b):
        print('stop: non-printable name at 0x%x: %s' % (pos, name_b[:32]))
        break
    name = name_b.decode('ascii')
    p = pos + 8 + name_len
    if p + 28 > len(res):
        print('stop: header truncated at', hex(pos))
        break
    size_a, size_b, size_c = struct.unpack_from('<QQQ', res, p)
    flag = struct.unpack_from('<I', res, p + 24)[0]
    ok = (size_b == size_a) and (size_c + 0x14 <= size_a)
    blobs.append((pos, name, size_a, size_c, flag, ok))
    # next entry position: header(8+len+24+4) + (size_a - 0x14)? try size_c+0x14 then verify
    data_start = p + 28
    nxt = data_start + (size_c + 0x14)  # hypothesis A
    # sanity: next entry name_len must be printable-range
    if nxt + 8 <= len(res):
        nl2 = struct.unpack_from('<Q', res, nxt)[0]
        if not (0 < nl2 <= 0x100):
            nxt = data_start + size_a  # hypothesis B
            if nxt + 8 <= len(res):
                nl2 = struct.unpack_from('<Q', res, nxt)[0]
                if not (0 < nl2 <= 0x100):
                    print('stop: cannot locate next entry after', name, 'at', hex(pos))
                    break
    pos = nxt

print('total entries parsed:', len(blobs))
with open(os.path.join(outdir, 'weights_blobs.txt'), 'w') as f:
    for off, name, sa, sc, fl, ok in blobs:
        f.write('%-48s off=0x%07x size_a=%d size_c=%d flag=%d ok=%s\n' % (name, off, sa, sc, fl, ok))
# summary stats
import collections
szs = collections.Counter()
for off, name, sa, sc, fl, ok in blobs:
    if name.startswith('block'):
        b = name.split('.')[0]
        szs[b] += 1
print('blob count per block prefix:')
for k in sorted(szs, key=lambda x: int(x[5:])):
    print('  %s: %d' % (k, szs[k]))
print('total payload approx: %.1f MB' % (sum(b[2] for b in blobs)/1048576.0))
