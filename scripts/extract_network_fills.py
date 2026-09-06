#!/usr/bin/env python3
"""Extract param struct fill sites for network-body kernels (func 0x1ddf0).
For each launch site, dump the code between the previous push (grid/block setup)
and the launch: that's the by_value param struct fill sequence."""
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

pushes = collect(0x53fc0)   # __hipPushCallConfiguration
launches = collect(0x540f0) # hipLaunchKernel

# 网络主体 0x1ddf0..0x208e2 内的 launch 与对应内核 (launch_sequence.json)
import json
seq = json.load(open(r'D:\FSR\DlssNrCore\docs\launch_sequence.json', encoding='utf-8'))
func_launches = []
for item in seq['functions'].get('0x1ddf0', []):
    func_launches.append((int(item['site'], 16), item['kernel']))

# 每个 launch 前最近的 push 点 (grid/block), 之间即填充现场
for site_rva, kname in func_launches:
    site = imgbase + site_rva   # JSON 存 RVA, 转绝对 VA
    prev_push = max((p for p in pushes if p < site and site - p < 0x400), default=None)
    if not prev_push:
        continue
    rva_from = prev_push - imgbase + 5  # push call 之后开始
    rva_to = site - imgbase
    lo = text_off + (rva_from - 0x1000)
    code = data[lo:lo + (rva_to - rva_from) + 8]
    print('\n========== %s (launch 0x%05x, push 0x%05x) ==========' % (kname, site - imgbase, prev_push - imgbase))
    for insn in md.disasm(code, imgbase + rva_from):
        if insn.address - imgbase > rva_to:
            break
        # 过滤无用 (call 返回后杂项)
        print('  0x%x: %-9s %s' % (insn.address - imgbase, insn.mnemonic, insn.op_str))
