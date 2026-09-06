#!/usr/bin/env python3
"""Pair each __hipPushCallConfiguration (grid/block setup) with the next
kernel launch in the same function, and summarize grid/block formulas.
Also decode the fixed block constant (0x100000100 -> {256,1,1}) and
grid computations ((w+bs-1)/bs patterns, psrad/sar divisions)."""
import pefile, struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def collect_sites(thunk_rva):
    """E8 calls to thunk at thunk_rva -> list of call-site VAs."""
    tgt = imgbase + thunk_rva
    out = []
    for i in range(text_off, text_off + text_size - 5):
        if data[i] == 0xE8:
            disp = struct.unpack_from('<i', data, i + 1)[0]
            src = text_va + (i - text_off) + 5
            if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == tgt:
                out.append(src - 5)
    return out

push_sites = collect_sites(0x53fc0)   # __hipPushCallConfiguration
launch_sites = collect_sites(0x540f0) # hipLaunchKernel

def owner(rva):
    pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                        pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
    for j in range(0, len(pdata), 12):
        b, e, u = struct.unpack_from('<III', pdata, j)
        if b <= rva < e:
            return (b, e)
    return None

def disasm_around(va, before, after):
    rva = va - imgbase
    lo = max(text_off, text_off + (rva - 0x1000) - before)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + after]
    return [(insn.address - imgbase, insn.mnemonic, insn.op_str)
            for insn in md.disasm(code, text_va + (lo - text_off))
            if insn.address <= va]

def decode_dim(insns, reg):
    """Find the dim3 for 'reg' from preceding insns: 12 bytes (x,y,z u32).
    Returns dict or None."""
    # look for movabs reg, imm (x,y packed) and mov dword [base+8], 1 (z)
    # simpler: find last 'lea reg, [rbp+OFF]' and read stack writes
    dim = {}
    for addr, mn, op in reversed(insns):
        if mn == 'lea' and op.startswith(reg + ','):
            break  # reached the lea; stop
        m = re.search(r'\[(rbp|rsp) \+ 0x([0-9a-fA-F]+)\]', op)
        if not m:
            continue
        base, off = m.group(1), int(m.group(2), 16)
        # writes: mov qword [base+off], imm64  (x,y packed) / mov dword [base+off], imm
        m2 = re.search(r'0x([0-9a-fA-F]+)$', op)
        if not m2:
            continue
        val = int(m2.group(1), 16)
        if mn == 'mov' and op.startswith('qword ptr'):
            dim[off] = val          # 64-bit write -> x=lo32,y=hi32
        elif mn == 'mov' and op.startswith('dword ptr'):
            dim[off] = val          # 32-bit write
        elif mn == 'movq':
            dim[off] = val
    return dim if dim else None

def fmt_dim(insns, reg):
    dim = decode_dim(insns, reg)
    if not dim:
        return '?'
    # 组合成 x,y,z
    parts = {}
    for off, val in dim.items():
        if val > 0xFFFFFFFF:
            parts[off] = (val & 0xFFFFFFFF, (val >> 32) & 0xFFFFFFFF)
        else:
            parts[off] = val
    return str(parts)

# 对每个网络函数: 找 push -> launch 配对
NET_FUNCS = {0x1c740: 'frame_io', 0x1ddf0: 'network_body', 0x222f0: 'stage_a', 0x22b60: 'layer_ops', 0x0c7f0: 'sync'}

for func, fname in NET_FUNCS.items():
    fend = func + 0x10000
    pushes = sorted(p for p in push_sites if func <= p - imgbase < func + 0x10000)
    launches = sorted(l for l in launch_sites if func <= l - imgbase < func + 0x10000 and l - imgbase < fend)
    print('\n### %s (0x%05x): %d pushes, %d launches' % (fname, func, len(pushes), len(launches)))
    # 每个 push 匹配其后的第一个 launch（按地址顺序）
    li = 0
    for p in pushes:
        # 找 p 之后最近的 launch
        nxt = [l for l in launches if l > p]
        if not nxt:
            continue
        l = nxt[0]
        insns = disasm_around(p, 96, 8)
        grid = fmt_dim(insns, 'rcx')
        block = fmt_dim(insns, 'rdx')
        # 提取关键公式: sar/shl/psrad 除法
        ops = [op for _, mn, op in insns if mn in ('sar', 'psrad', 'shl', 'lea', 'movabs')]
        print('  push 0x%05x -> launch 0x%05x  grid=%s block=%s' % (p - imgbase, l - imgbase, grid, block))
