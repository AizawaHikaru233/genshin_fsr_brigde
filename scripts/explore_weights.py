#!/usr/bin/env python3
"""Explore WEIGHTS_HT blob format: try layout hypotheses, validate by
printable names + total size plausibility."""
import struct, sys

dll = sys.argv[1]
data = open(dll, 'rb').read()
magic = 0x08cda732
i = data.find(struct.pack('<I', magic))
res = data[i:]
print('resource len:', len(res))

def printable(b):
    return all(0x20 <= c < 0x7f for c in b)

# Hypothesis set; each is a function(entry_start) -> (name, next_pos) or None
def h_null_u64_u64(pos):
    # name: null-terminated ascii; then u64 size, u64 offset
    end = res.find(b'\x00', pos)
    if end < 0 or end - pos > 64: return None
    name = res[pos:end]
    if not printable(name) or not name: return None
    nxt = (end + 1 + 15) & ~7
    if nxt + 16 > len(res): return None
    return name, nxt, 'null+size+off'

def h_name20_3u64_u32(pos):
    name = res[pos:pos+20].rstrip(b'\x00')
    if not printable(name) or not name: return None
    nxt = pos + 20 + 28  # 3*u64 + u32
    if nxt > len(res): return None
    return name, nxt, 'name20+3u64+u32'

def h_u8len_name_u64_u64(pos):
    if pos + 1 > len(res): return None
    ln = res[pos]
    if ln > 64: return None
    name = res[pos+1:pos+1+ln]
    if not printable(name): return None
    nxt = (pos + 1 + ln + 7) & ~7
    if nxt + 16 > len(res): return None
    return name, nxt, 'u8len+name+size+off'

# start after first 16 bytes (magic+pad+count+pad)
for name, pos0 in [('null+u64+u64', 16), ('name20', 16), ('u8len', 16)]:
    print('=== hypothesis:', name)
    fn = {'null+u64+u64': h_null_u64_u64, 'name20': h_name20_3u64_u32, 'u8len': h_u8len_name_u64_u64}[name]
    pos = pos0
    blobs = []
    ok = True
    for k in range(24):
        r = fn(pos)
        if r is None:
            ok = False; print('  break at blob', k, 'pos', hex(pos)); break
        nm, nxt, tag = r
        a, b, c = struct.unpack_from('<QQQ', res, nxt - 28) if name=='name20' else (None,None,None)
        if name == 'name20':
            print('  %-30s size=%d off=%d c=%d' % (nm.decode(), a, b, c))
        else:
            sz, off = struct.unpack_from('<QQ', res, nxt)
            print('  %-30s size=%d off=%d' % (nm.decode(), sz, off))
        blobs.append(nm)
        pos = nxt
    print('  total parsed:', len(blobs))
    if ok: break
