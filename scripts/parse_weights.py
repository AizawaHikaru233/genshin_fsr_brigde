#!/usr/bin/env python3
"""Parse the WEIGHTS_HT resource blob list from nvngx_dlssnr.dll.
Blob list = network structure map (every tensor name + size).
"""
import struct, sys, os, re

dll = sys.argv[1]
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)

data = open(dll, 'rb').read()
magic = 0x08cda732
i = data.find(struct.pack('<I', magic))
print('magic at 0x%x' % i)
# header: magic(4) + pad(4) + count(8)?  -- from hexdump: 32a7cd08 00000000 13000000 00000000
if i < 0: sys.exit(1)
count = struct.unpack_from('<Q', data, i + 8)[0]
print('blob count =', count)
pos = i + 16
blobs = []
for n in range(count):
    # name: length-prefixed? try: u8 len + name + pad + u64 size + u64 offset
    namelen = data[pos]
    name = data[pos+1:pos+1+namelen].decode('ascii', 'replace')
    pos += 1 + namelen
    pos = (pos + 7) & ~7  # align 8
    size, off = struct.unpack_from('<QQ', data, pos)
    blobs.append((name, size, off))
    pos += 16
    if pos > 0x200000:
        print('stopped early at', n); break

with open(os.path.join(outdir, 'weights_blobs.txt'), 'w') as f:
    total = 0
    for name, size, off in blobs:
        f.write('%-48s size=%-10d off=0x%08x\n' % (name, size, off))
        total += size
print('parsed %d blobs, total payload %.1f MB' % (len(blobs), total/1048576.0))
# summary: name patterns
from collections import Counter
pat = Counter()
for name, _, _ in blobs:
    m = re.match(r'(block\d+|enc|dec|head|stem|pre|post|fuse|upsample|reproj|input|output)(.*)', name)
    pat[m.group(1) if m else name] += 1
print('--- prefix histogram ---')
for k, v in sorted(pat.items()):
    print('%-24s %d' % (k, v))
print('--- first 60 ---')
for name, size, off in blobs[:60]:
    print('%-48s %d' % (name, size))
