#!/usr/bin/env python3
"""Decode every kernel launch site in version.dll:
- resolve the lea rcx,[rip+X] target (kernel handle slot) for each launch
- resolve the registration sequence (index order) to map slot -> kernel symbol
- produce launch-site table: address, slot, kernel, register index
"""
import struct, re, json
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

def rva_of(va): return va - imgbase

def section_of(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.Name.rstrip(b'\x00').decode('latin1')
    return '?'

def read_u64(va):
    rva = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            off = s.PointerToRawData + (rva - s.VirtualAddress)
            if off + 8 <= len(data):
                return struct.unpack_from('<Q', data, off)[0]
    return None

# ---- 1) registration order: calls to __hipRegisterFunction thunk 0x53fe0 ----
reg_order = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == imgbase + 0x53fe0:
            reg_order.append(src - 5)
print('register call sites:', len(reg_order))

# for each, find lea r8,[rip+X] -> device name string (the '_Z...' symbol)
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
reg_sym = []
for site in reg_order:
    rva = rva_of(site)
    lo = max(text_off, text_off + (rva - 0x1000) - 80)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + 8]
    sym = None
    for insn in md.disasm(code, text_va + (lo - text_off)):
        if insn.address > site: break
        if insn.mnemonic == 'lea' and '[rip +' in insn.op_str:
            m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
            if m:
                tgt = (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
                # read string
                r = rva_of(tgt)
                for s in pe.sections:
                    if s.VirtualAddress <= r < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
                        off = s.PointerToRawData + (r - s.VirtualAddress)
                        raw = data[off:off+200].split(b'\x00')[0]
                        if raw.startswith(b'_Z'):
                            sym = raw.decode('latin1')
                        break
    reg_sym.append((site, sym))
print('registration symbol order:')
for idx, (site, sym) in enumerate(reg_sym):
    print('  #%02d site 0x%05x %s' % (idx, rva_of(site), sym))

# ---- 2) launch sites: calls to hipLaunch thunk 0x540f0, resolve lea rcx target ----
launch_sites = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == imgbase + 0x540f0:
            launch_sites.append(src - 5)
print('\nlaunch sites:', len(launch_sites))

decoded = []
for site in launch_sites:
    rva = rva_of(site)
    lo = max(text_off, text_off + (rva - 0x1000) - 64)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + 4]
    slot = None
    reg_ops = []
    for insn in md.disasm(code, text_va + (lo - text_off)):
        if insn.address > site: break
        if insn.mnemonic == 'lea' and 'rcx,' in insn.op_str and '[rip +' in insn.op_str:
            m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
            if m:
                tgt = (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
                slot = rva_of(tgt)
        elif insn.mnemonic == 'lea' and '[rip +' in insn.op_str:
            m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
            if m:
                tgt = (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
                reg_ops.append(hex(tgt))
    decoded.append((site, slot, reg_ops))

print('\n--- launch site -> kernel slot ---')
for site, slot, regs in decoded:
    print('  site 0x%05x slot %s (sec %s, val %s) regs=%s' % (
        rva_of(site), hex(slot) if slot else '?',
        section_of(slot) if slot else '?',
        hex(read_u64(imgbase+slot)) if slot else '?',
        ','.join(regs)))
