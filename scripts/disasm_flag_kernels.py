#!/usr/bin/env python3
"""Disassemble k_flag_set / k_flag_wait ISA from gfx1201 ELF using capstone."""
import struct
from capstone import Cs, CS_ARCH_AMDGPU, CS_MODE_M7

d = open(r'D:\FSR\DlssNrCore\data\co_gfx1201.bin', 'rb').read()
assert d[:4] == b'\x7fELF'

e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
e_shentsize = struct.unpack_from('<H', d, 0x3A)[0]
e_shnum = struct.unpack_from('<H', d, 0x3C)[0]
e_shstrndx = struct.unpack_from('<H', d, 0x3E)[0]

def get_section(i):
    off = e_shoff + i * e_shentsize
    name_off, stype, flags, addr, offset, size = struct.unpack_from('<IIQQQQ', d, off)[:6]
    return name_off, stype, flags, addr, offset, size

_, _, _, _, shstr_off, shstr_size = get_section(e_shstrndx)
shstr = d[shstr_off:shstr_off+shstr_size]

secs = []
for i in range(e_shnum):
    name_off, stype, flags, addr, offset, size = get_section(i)
    name = shstr[name_off:shstr.find(b'\x00', name_off)].decode()
    secs.append((name, stype, addr, offset, size))

symtab = strtab = None
text = None
for name, stype, addr, offset, size in secs:
    if name == '.symtab': symtab = (offset, size)
    if name == '.strtab': strtab = (offset, size)
    if name == '.text': text = (addr, offset, size)
    if name == '.AMDGPU.config' or name == '.note': pass

so, ss = symtab
st_off, st_size = strtab
want = {}
for i in range(0, ss, 24):
    st_name, st_info, st_shndx, st_value, st_size2 = struct.unpack_from('<IBBHQ', d, so + i)[:5]
    name = d[st_off+st_name:st_off+st_name+96].split(b'\x00')[0].decode('latin1')
    if name in ('_Z10k_flag_setPjj', '_Z11k_flag_waitPjjj', '_Z10k_conv_res10ConvParams',
                '_Z21k_pre_block_1h_32_fp89PreParams'):
        want[name] = (st_value, st_size2)

text_addr, text_off, text_size = text
md = Cs(CS_ARCH_AMDGPU, CS_MODE_M7)
md.skipdata = True

for name, (val, size) in want.items():
    print('=== %s (addr 0x%x, %d bytes) ===' % (name, val, size))
    rva = val - text_addr
    code = d[text_off + rva : text_off + rva + size]
    n = 0
    for insn in md.disasm(code, val):
        print('  0x%08x: %-28s %s' % (insn.address, insn.mnemonic, insn.op_str))
        n += 1
        if n > 60: break
    print()
