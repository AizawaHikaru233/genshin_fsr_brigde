#!/usr/bin/env python3
"""Final WEIGHTS_HT walker. Format (confirmed from parser disasm + hexdump):
  [u64 total_size]
  per entry: u64 name_len, name[name_len], u64 A, u64 B(==A), u64 C(==A-0x28), u32 flag,
             data[A-0x14 bytes]; next entry at data end.
Outputs: weights_blobs.txt (manifest) and extracted per-tensor .bin files.
"""
import struct, sys, os, re

dll = sys.argv[1]
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)
tensordir = os.path.join(outdir, 'tensors')
os.makedirs(tensordir, exist_ok=True)

data = open(dll, 'rb').read()
res_off = 0x1149cb8
res = data[res_off:res_off + 0x8cda732]
total = struct.unpack_from('<Q', res, 0)[0]
print('header total_size: 0x%x (%d), resource len: 0x%x (%d), match: %s' % (
    total, total, len(res), len(res), total == len(res)))

pos = 8
entries = []
while pos + 8 <= len(res):
    name_len = struct.unpack_from('<Q', res, pos)[0]
    if not (0 < name_len <= 0x100):
        print('stop: bad name_len %d @0x%x' % (name_len, pos)); break
    name_b = res[pos+8:pos+8+name_len]
    if not all(0x20 <= b < 0x7f for b in name_b):
        print('stop: non-printable name @0x%x: %r' % (pos, name_b[:40])); break
    name = name_b.decode('ascii')
    p = pos + 8 + name_len
    if p + 28 > len(res):
        print('stop: header truncated @', hex(pos)); break
    A, B, C = struct.unpack_from('<QQQ', res, p)
    flag = struct.unpack_from('<I', res, p + 24)[0]
    ok = (B == A) and (C == A - 0x28) and (A >= 0x28)
    if not ok:
        print('stop: bad size triple @0x%x name=%s A=%d B=%d C=%d' % (pos, name, A, B, C)); break
    data_len = A - 0x14
    dstart = p + 28
    if dstart + data_len > len(res):
        print('stop: data overrun @', hex(pos)); break
    entries.append((pos, name, A, C, flag, dstart, data_len))
    pos = dstart + data_len

print('parsed entries:', len(entries), ' end pos: 0x%x (res len 0x%x, gap %d)' % (
    pos, len(res), len(res) - pos))

# manifest
with open(os.path.join(outdir, 'weights_blobs.txt'), 'w') as f:
    for off, name, A, C, fl, dstart, dlen in entries:
        f.write('%-52s off=0x%07x data=0x%07x len=%-9d A=%d C=%d flag=%d\n' % (
            name, off, dstart, dlen, A, C, fl))
        with open(os.path.join(tensordir, name.replace('/', '_') + '.bin'), 'wb') as tf:
            tf.write(res[dstart:dstart+dlen])

# stats
import collections
pref = collections.Counter()
for off, name, *_ in entries:
    m = re.match(r'(block\d+|enc|dec|head|stem|pre|post|fuse|upsample|reproj|input|output|tone|struct|skin|mask)', name)
    pref[m.group(1) if m else name] += 1
print('--- prefix histogram ---')
for k, v in sorted(pref.items(), key=lambda kv: (kv[0][:5], int(re.search(r'\d+', kv[0]).group()) if re.search(r'\d+', kv[0]) else 0)):
    print('%-14s %d' % (k, v))
tot = sum(e[6] for e in entries)
print('total tensor bytes: %.2f MB' % (tot/1048576.0))
