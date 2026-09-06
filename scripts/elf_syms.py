#!/usr/bin/env python3
"""Disassemble k_flag_set / k_flag_wait from the gfx1201 ELF code object."""
import struct
import pefile

d = open(r'D:\FSR\DlssNrCore\data\co_gfx1201.bin', 'rb').read()
assert d[:4] == b'\x7fELF'

# ELF64 little endian
e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
e_shentsize = struct.unpack_from('<H', d, 0x3A)[0]
e_shnum = struct.unpack_from('<H', d, 0x3C)[0]
e_shstrndx = struct.unpack_from('<H', d, 0x3E)[0]

def get_section(i):
    off = e_shoff + i * e_shentsize
    name_off, stype, flags, addr, offset, size = struct.unpack_from('<IIQQQQ', d, off)[:6]
    return name_off, stype, flags, addr, offset, size

# shstrtab
_, _, _, _, shstr_off, shstr_size = get_section(e_shstrndx)
shstr = d[shstr_off:shstr_off+shstr_size]

secs = []
for i in range(e_shnum):
    name_off, stype, flags, addr, offset, size = get_section(i)
    name = shstr[name_off:shstr.find(b'\x00', name_off)].decode()
    secs.append((name, stype, addr, offset, size))

symtab = None; strtab = None
for name, stype, addr, offset, size in secs:
    if name == '.symtab': symtab = (offset, size)
    if name == '.strtab': strtab = (offset, size)

# find k_flag_set symbol
syms = []
so, ss = symtab
st_off, st_size = strtab
for i in range(0, ss, 24):
    st_name, st_info, st_shndx, st_value, st_size2 = struct.unpack_from('<IBBHQ', d, so + i)[:5]
    name = d[st_off+st_name:st_off+st_name+80].split(b'\x00')[0].decode('latin1')
    syms.append((name, st_value, st_size2, st_shndx))

for name, val, size, shndx in syms:
    if 'flag' in name or 'k_flag' in name:
        print('SYM %-32s addr=0x%x size=%d shndx=%d' % (name, val, size, shndx))
