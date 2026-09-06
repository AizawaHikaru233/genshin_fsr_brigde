#!/usr/bin/env python3
"""Disassemble functions in version.dll with capstone, map RVAs."""
import sys, os
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

def rva_to_off(pe, rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def disasm_range(pe, data, rva, size, out):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    off = rva_to_off(pe, rva)
    if off is None:
        out.append('; cannot map rva 0x%x' % rva); return
    code = data[off:off+size]
    out.append('; --- func @ rva 0x%x (file off 0x%x, %d bytes) ---' % (rva, off, size))
    for insn in md.disasm(code, pe.OPTIONAL_HEADER.ImageBase + rva):
        out.append('0x%016x: %-32s %s %s' % (insn.address, insn.mnemonic + ' ' + insn.op_str, '', ''))
        if len(out) > 20000: break

def main(path, rvas_hex, outpath):
    pe = pefile.PE(path)
    data = open(path, 'rb').read()
    out = []
    for h in rvas_hex.split(','):
        rva = int(h.strip(), 16)
        # find size: until next export or 0x200 cap
        disasm_range(pe, data, rva, 0x180, out)
    open(outpath, 'w').write('\n'.join(out))
    print('WROTE', outpath, len(out), 'lines')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3])
