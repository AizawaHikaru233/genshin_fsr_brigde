#!/usr/bin/env python3
"""Parse .note section of gfx1201 code object for AMDGPU metadata (kernarg layout)."""
import struct

d = open(r'D:\FSR\DlssNrCore\data\co_gfx1201.bin', 'rb').read()
# .note at off 0x238 size 0x89c4
note = d[0x238:0x238+0x89c4]
pos = 0
print('=== .note notes ===')
while pos + 12 <= len(note):
    namesz, descsz, ntype = struct.unpack_from('<III', note, pos)
    name = note[pos+12:pos+12+namesz].rstrip(b'\x00')
    desc_off = pos + 12 + ((namesz + 3) & ~3)
    desc = note[desc_off:desc_off+descsz]
    print('note @0x%x name=%s type=0x%x descsz=0x%x' % (pos, name, ntype, descsz))
    if name == b'AMDGPU' and ntype == 0xA0000000:  # NT_AMD_AMDGPU_METADATA
        print('--- METADATA (first 4000 chars) ---')
        print(desc[:4000].decode('utf-8', 'replace'))
        break
    pos = desc_off + ((descsz + 3) & ~3)
