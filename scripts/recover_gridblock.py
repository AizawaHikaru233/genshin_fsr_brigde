#!/usr/bin/env python3
"""Recover grid/block dims for each launch site from version.dll disassembly.
Strategy: at each launch site, the mod builds a ParamStruct and calls
0x53fb0 (thunk->__hipPopCallConfiguration) with (grid*, block*, shared*, stream*)
out-pointers on the stack, then launches. The grid/block values are computed
from job context fields earlier in the function. Here we extract the launch
site + the immediate preceding computation of grid/block (the rsi/rdi targets)
and the hidden-args block that hipLaunchKernel would fill (we must fill them
ourselves since we use hipModuleLaunchKernel, whose hidden args are auto-filled).
Key insight: hipModuleLaunchKernel auto-fills hidden args from grid/block dims.
So we only need grid/block numbers. For k_flag_set/k_flag_wait grid=(1,1,1).
For compute kernels the grid = (H+bs-1)/bs etc. computed at each site.
We'll dump per-site context to identify the formulas.
"""
import struct, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# 启动点 (E8 -> 0x540f0)
sites = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == imgbase + 0x540f0:
            sites.append(src - 5)

print('launch sites:', len(sites))

# 对每个站点，反汇编其宿主函数开头 ~0x80 字节前的寄存器初始化，
# 提取 grid/block 计算涉及的字段 (movsxd/mov dword 等)
for site in sites[:10]:  # 先看前10个网络体站点
    rva = site - imgbase
    # owner function
    pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                        pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
    owner = None
    for j in range(0, len(pdata), 12):
        b, e, u = struct.unpack_from('<III', pdata, j)
        if b <= rva < e:
            owner = (b, e); break
    if not owner:
        continue
    # 反汇编调用点前 96 字节
    lo = max(text_off, text_off + (rva - 0x1000) - 96)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + 8]
    print('\n--- site 0x%05x (func 0x%05x) ---' % (rva, owner[0]))
    for insn in md.disasm(code, text_va + (lo - text_off)):
        if insn.address > site:
            break
        # 只显示与 grid/block/launch 相关的指令
        s = insn.op_str
        if any(k in insn.mnemonic for k in ('mov', 'lea', 'imul', 'sar', 'add', 'call', 'xor', 'shl')):
            print('  0x%x: %-8s %s' % (insn.address - imgbase, insn.mnemonic, s))
