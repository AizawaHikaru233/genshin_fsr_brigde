#!/usr/bin/env python3
"""Precise grid/block extraction: for each push site in the network-body function
0x1ddf0 (and its callees), dump the exact computation of the dim3 structs
pointed to by rcx (grid) and rdx (block). We reconstruct x,y,z u32 values from
register/stack writes before the lea."""
import pefile, struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

def collect(thunk_rva):
    tgt = imgbase + thunk_rva
    out = []
    for i in range(text_off, text_off + text_size - 5):
        if data[i] == 0xE8:
            disp = struct.unpack_from('<i', data, i + 1)[0]
            src = text_va + (i - text_off) + 5
            if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == tgt:
                out.append(src - 5)
    return out

pushes = collect(0x53fc0)
launches = collect(0x540f0)

def disasm_until(va, back, forward):
    rva = va - imgbase
    lo = max(text_off, text_off + (rva - 0x1000) - back)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + forward]
    return [(insn.address - imgbase, insn.mnemonic, insn.op_str)
            for insn in md.disasm(code, text_va + (lo - text_off))
            if insn.address <= va]

def analyze(push_va):
    insns = disasm_until(push_va, 96, 4)
    # 定位 lea rcx/lea rdx 目标
    grid_off = block_off = None
    for addr, mn, op in insns:
        m = re.search(r'\[(rbp|rsp) \+ 0x([0-9a-fA-F]+)\]', op)
        if not m: continue
        if mn == 'lea' and op.startswith('rcx,'):
            grid_off = (m.group(1), int(m.group(2), 16))
        if mn == 'lea' and op.startswith('rdx,'):
            block_off = (m.group(1), int(m.group(2), 16))
    # 收集目标区的写入
    def dim_read(base, off):
        # 扫描写入 [base+off..off+12] 的指令, 解析 x/y/z
        vals = {}
        for addr, mn, op in insns:
            m = re.search(r'\[%s \+ 0x([0-9a-fA-F]+)\]' % base, op)
            if not m: continue
            o = int(m.group(1), 16)
            if not (off <= o < off + 16): continue
            m2 = re.search(r'0x([0-9a-fA-F]+)$', op)
            if not m2: continue
            v = int(m2.group(1), 16)
            if mn == 'mov' and op.startswith('qword ptr'):
                vals[o] = (v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF)
            elif mn == 'mov' and op.startswith('dword ptr'):
                vals[o] = v
            elif mn == 'movq':
                vals[o] = (v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF)
        return vals
    g = dim_read(grid_off[0], grid_off[1]) if grid_off else {}
    b = dim_read(block_off[0], block_off[1]) if block_off else {}
    # 输出人类可读
    def fmt(vals, off):
        x = vals.get(off); y = vals.get(off + 4); z = vals.get(off + 8)
        return (x, y, z)
    return insns, grid_off, block_off, g, b

# 只分析网络体 0x1ddf0 及被调用的 0x21680 包装、0x22b60 层算子
targets = [p for p in pushes if 0x1ddf0 <= p - imgbase < 0x1ddf0 + 0x2af2]
print('network-body pushes:', len(targets))
for p in targets:
    insns, g_off, b_off, g, b = analyze(p)
    print('\n=== push @ 0x%05x ===' % (p - imgbase))
    if g_off: print('  grid @ [%s+0x%x]: %s' % (g_off[0], g_off[1], g))
    if b_off: print('  block@ [%s+0x%x]: %s' % (b_off[0], b_off[1], b))
    # 打印最后 20 条指令（公式来源）
    for addr, mn, op in insns[-22:]:
        print('    0x%05x: %-9s %s' % (addr, mn, op))
