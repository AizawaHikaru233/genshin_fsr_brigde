#!/usr/bin/env python3
"""Extract grid/block computation for every push site (-> __hipPushCallConfiguration)
in the network body 0x1ddf0 and other network functions.
Pattern per push site:
  lea rcx, [rbp+OFF_G]   ; &grid dim3 (12B: x,y,z)
  lea rdx, [rbp+OFF_B]   ; &block dim3
  xor r8d, r8d           ; sharedMem = 0
  xor r9d, r9d           ; stream = 0
  call __hipPushCallConfiguration
Grid/block values are computed just before: usually
  block = {0x100, 0x1, 0x1} (256,1,1) via movabs 0x100000100
  grid.x = (w + bs - 1) / bs via lea ecx,[rax+bs-1]; sar ecx, log2(bs)
We dump the 40 instructions before each push to recover formulas."""
import pefile, struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

# push thunk = 0x53fc0
sites = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == imgbase + 0x53fc0:
            sites.append(src - 5)

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# 需要读内存值: helper 解析 [rbp+off] 和 movabs 常量
def extract(insns, tag):
    """insns: list of (addr, mnem, op) before the push call."""
    out = []
    for addr, mn, op in insns:
        if mn == 'movabs' and 'rax' in op:
            m = re.search(r'0x([0-9a-fA-F]+)', op)
            if m:
                out.append(('CONST rax', int(m.group(1), 16)))
        elif mn == 'mov' and 'qword ptr [rbp +' in op:
            m = re.search(r'\[rbp \+ 0x([0-9a-fA-F]+)\]', op)
            m2 = re.search(r'0x([0-9a-fA-F]+)$', op)
            if m and m2:
                out.append(('MOV64[rbp+%s]' % m.group(1), int(m2.group(1), 16)))
        elif mn == 'mov' and 'dword ptr [rbp +' in op:
            m = re.search(r'\[rbp \+ 0x([0-9a-fA-F]+)\]', op)
            m2 = re.search(r'0x([0-9a-fA-F]+)$', op)
            if m and m2:
                out.append(('MOV32[rbp+%s]' % m.group(1), int(m2.group(1), 16)))
        elif mn in ('sar', 'shl') and 'ecx' in op:
            out.append((mn.upper() + ' ecx', op.split(',')[-1].strip()))
        elif mn == 'lea' and 'ecx, [rax +' in op:
            m = re.search(r'\[rax \+ 0x([0-9a-fA-F]+)\]', op)
            if m:
                out.append(('LEA ecx,[rax+%s]' % m.group(1), None))
    return out

for site in sites:
    rva = site - imgbase
    # owner func
    pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                        pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
    owner = None
    for j in range(0, len(pdata), 12):
        b, e, u = struct.unpack_from('<III', pdata, j)
        if b <= rva < e:
            owner = (b, e); break
    if not owner or owner[0] not in (0xc7f0, 0x1c740, 0x1ddf0, 0x222f0, 0x22b60):
        continue
    lo = max(text_off, text_off + (rva - 0x1000) - 90)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + 4]
    insns = []
    for insn in md.disasm(code, text_va + (lo - text_off)):
        if insn.address > site: break
        insns.append((insn.address - imgbase, insn.mnemonic, insn.op_str))
    print('\n=== push @ 0x%05x (func 0x%05x) ===' % (rva, owner[0]))
    # 找 rcx/rdx 的 lea（grid/block 指针）与最近的常量
    g = b = None
    for addr, mn, op in insns:
        if mn == 'lea' and op.startswith('rcx,') and '[rbp' in op:
            m = re.search(r'\[rbp \+ 0x([0-9a-fA-F]+)\]', op)
            g = m.group(1) if m else None
        if mn == 'lea' and op.startswith('rdx,') and '[rbp' in op:
            m = re.search(r'\[rbp \+ 0x([0-9a-fA-F]+)\]', op)
            b = m.group(1) if m else None
    print('  grid@[rbp+0x%s]  block@[rbp+0x%s]' % (g or '?', b or '?'))
    for addr, mn, op in insns[-28:]:
        print('    0x%05x: %-9s %s' % (addr, mn, op))
