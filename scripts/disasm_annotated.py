#!/usr/bin/env python3
"""Annotated disassembly: resolve lea rip-relative targets to strings."""
import sys, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path, rva_hex, size_hex, outpath = sys.argv[1:5]
pe = pefile.PE(path)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(path, 'rb').read()
rva = int(rva_hex, 16)
size = int(size_hex, 16)

off = None
for s in pe.sections:
    if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
        off = s.PointerToRawData + (rva - s.VirtualAddress)
        break
if off is None:
    print('cannot map rva'); sys.exit(1)

def readstr(va):
    r = va - imgbase
    o = None
    for s in pe.sections:
        if s.VirtualAddress <= r < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            o = s.PointerToRawData + (r - s.VirtualAddress)
            break
    if o is None: return None
    raw = data[o:o+90].split(b'\x00')[0]
    if all(32 <= b < 127 for b in raw) and len(raw) > 2:
        return raw.decode('ascii')
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
code = data[off:off+size]
lines = []
for insn in md.disasm(code, imgbase + rva):
    line = '0x%08x: %-38s' % (insn.address, insn.mnemonic + ' ' + insn.op_str)
    ann = ''
    m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
    if m:
        tgt = (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
        s = readstr(tgt)
        if s:
            ann = '  ; "%s"' % s[:70]
    lines.append(line + ann)

open(outpath, 'w').write('\n'.join(lines))
print('WROTE', outpath, len(lines), 'lines')
