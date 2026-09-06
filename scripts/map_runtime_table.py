#!/usr/bin/env python3
"""Resolve the mod's runtime-resolved HIP function table at 0x6b3d8+.
Find the init code that fills the table (GetProcAddress calls) and map slot->name."""
import pefile, struct, re

pe = pefile.PE(r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll')
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll','rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

# find all calls to GetProcAddress thunk (import from KERNEL32)
slots = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in entry.imports:
        if imp.address and imp.name and imp.name.decode() == 'GetProcAddress':
            iat_getproc = imp.address
        if imp.address:
            slots[imp.address] = imp.name.decode() if imp.name else ''

# find thunk for GetProcAddress
thunk_gpa = None
for i in range(text_off, text_off + text_size - 6):
    if data[i] == 0xFF and data[i+1] == 0x25:
        disp = struct.unpack_from('<i', data, i+2)[0]
        tgt = (text_va + (i-text_off) + 6 + disp) & 0xFFFFFFFFFFFFFFFF
        if tgt == iat_getproc:
            thunk_gpa = text_va + (i-text_off)
            break
print('GetProcAddress thunk: 0x%x' % (thunk_gpa or 0))

# find calls to it; each call: rcx = module handle, rdx = name string (lea rdx,[rip+X])
def read_cstr(va):
    r = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= r < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            off = s.PointerToRawData + (r - s.VirtualAddress)
            return data[off:off+64].split(b'\x00')[0].decode('latin1')
    return None

calls = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i+1)[0]
        src = text_va + (i-text_off) + 5
        if ((src+disp) & 0xFFFFFFFFFFFFFFFF) == thunk_gpa:
            calls.append(src-5)
print('GetProcAddress call sites:', len(calls))

# for each call site, find lea rdx,[rip+X] before it (function name)
resolved = []
for c in calls:
    rva = c - imgbase
    lo = max(text_off, text_off + (rva - 0x1000) - 64)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + 4]
    name = None
    lea_rdx = None
    for j in range(len(code)-6):
        if code[j] == 0x48 and code[j+1] == 0x8D and code[j+2] == 0x15:  # lea rdx,[rip+disp]
            disp = struct.unpack_from('<i', code, j+3)[0]
            tgt = (text_va + (lo-text_off) + j + 7 + disp) & 0xFFFFFFFFFFFFFFFF
            name = read_cstr(tgt)
            lea_rdx = text_va + (lo-text_off) + j
    resolved.append((c, name, lea_rdx))
for c, name, lr in resolved:
    print('  call 0x%05x lea 0x%05x %s' % (c-imgbase, (lr-imgbase) if lr else 0, name))
