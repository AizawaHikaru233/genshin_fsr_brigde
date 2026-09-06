#!/usr/bin/env python3
"""Find ALL kernel-launch call sites: calls (E8 rel32 or FF15) targeting either
the hipLaunchKernel IAT slot (0x18006a3f0) or its jmp thunk (0x1800540f0).
Maps each call site to its .pdata function."""
import struct
import pefile

pe = pefile.PE(r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll')
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll','rb').read()

pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                    pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
funcs = []
for i in range(0, len(pdata), 12):
    b, e, u = struct.unpack_from('<III', pdata, i)
    funcs.append((b, e))

def owner(rva):
    for b, e in funcs:
        if b <= rva < e:
            return (b, e)
    return None

# targets of interest: IAT slot (thunk target) and thunk address
iat_slot = 0x18006a3f0          # hipLaunchKernel
thunk = 0x1800540f0             # jmp [rip+x] -> iat_slot

text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400
hits = []
for i in range(text_off, text_off + text_size - 7):
    b0, b1 = data[i], data[i+1]
    if b0 == 0xE8:  # call rel32
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        dst = (src + disp) & 0xFFFFFFFFFFFFFFFF
        if dst == thunk:
            hits.append((src - 5, 'rel32->thunk'))
    elif b0 == 0xFF and b1 == 0x15:  # call [rip+disp]
        disp = struct.unpack_from('<i', data, i + 2)[0]
        src = text_va + (i - text_off) + 6
        dst = (src + disp) & 0xFFFFFFFFFFFFFFFF
        if dst == iat_slot:
            hits.append((src - 6, 'ff15->IAT'))
        elif dst == thunk:
            hits.append((src - 6, 'ff15->thunk'))

print('launch call sites:', len(hits))
for addr, kind in hits:
    f = owner(addr - imgbase)
    print('  0x%06x %-12s func 0x%06x-0x%06x (size 0x%x)' % (
        addr - imgbase, kind, f[0] if f else 0, f[1] if f else 0,
        (f[1] - f[0]) if f else 0))
