#!/usr/bin/env python3
"""Locate the kernel launcher function and all its callers in version.dll.
The launcher contains the single hipLaunchKernel call (IAT 0x18006a3f0).
Output: launcher bounds + callers (function start, call site).
"""
import struct, sys
import pefile

pe = pefile.PE(r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll')
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll','rb').read()

# .pdata functions
pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                    pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
funcs = []
for i in range(0, len(pdata), 12):
    b, e, u = struct.unpack_from('<III', pdata, i)
    funcs.append((b, e))
print('total .pdata funcs:', len(funcs))

def owner(rva):
    for b, e in funcs:
        if b <= rva < e:
            return (b, e)
    return None

# 1) find function containing hipLaunchKernel call site 0x540f0
o = owner(0x540f0)
print('hipLaunchKernel call 0x540f0 inside func:', hex(o[0]) if o else None, '-', hex(o[1]) if o else None)
launcher = o

# 2) find all direct callers: call rel32 (E8) to launcher start in .text
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400
callers = []
if launcher:
    target = imgbase + launcher[0]
    for i in range(text_off, text_off + text_size - 5):
        if data[i] == 0xE8:
            disp = struct.unpack_from('<i', data, i + 1)[0]
            src = text_va + (i - text_off) + 5
            if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == target:
                callers.append(src - 5)
print('direct callers of launcher:', len(callers))
for c in callers:
    f = owner(c - imgbase)
    print('  call site 0x%06x in func 0x%06x-0x%06x (size 0x%x)' % (
        c - imgbase, f[0] if f else 0, f[1] if f else 0, (f[1]-f[0]) if f else 0))
