#!/usr/bin/env python3
"""Disassemble a full function range with capstone (x64)."""
import sys, os
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

def main(path, rva_hex, size_hex, outpath):
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
    code = data[off:off+size]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    lines = []
    for insn in md.disasm(code, imgbase + rva):
        lines.append('0x%016x: %-36s %s' % (insn.address, insn.mnemonic + ' ' + insn.op_str, ''))
        if len(lines) > 50000: break
    open(outpath, 'w').write('\n'.join(lines))
    print('WROTE', outpath, len(lines), 'lines')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
