#!/usr/bin/env python3
"""Map kernel function-pointer globals to kernel symbols via __hipRegisterFunction
call sites; then decode every launch site (lea rcx,[rip+X] -> kernel).
"""
import struct, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()

# ---- kernel symbols (from .hip_fat strtab / kernels.cpp table) ----
KERNELS = [
    '_Z21k_pre_block_1h_32_fp89PreParams', '_Z22k_post_block_1h_32_fp810PostParams',
    '_Z16k_swin_1h_32_fp810SwinParams', '_Z10k_swin_varILi32ELb0EEv9VarParams',
    '_Z10k_swin_varILi64ELb0EEv9VarParams', '_Z10k_swin_varILi128ELb0EEv9VarParams',
    '_Z10k_swin_varILi256ELb0EEv9VarParams', '_Z10k_swin_varILi32ELb1EEv9VarParams',
    '_Z10k_conv_res10ConvParams', '_Z11k_conv_res211Conv2Params',
    '_Z13k_conv_splitk12ConvParams1d', '_Z16k_conv_res_views12ConvPlParams',
    '_Z14k_ffwd_inpview12FfwdPlParams', '_Z10k_qkv_attn10AttnParams',
    '_Z11k_qkv_attn210AttnParams', '_Z11k_attention12AttnParams1d',
    '_Z12k_attention212AttnParams1d', '_Z11k_contract212ConvParams1d',
    '_Z11k_reproject12ReprojParams', '_Z14k_dec_upsample11DecUpParams',
    '_Z12k_final_head10HeadParams', '_Z10k_flag_setPjj', '_Z11k_flag_waitPjjj',
]

# ---- 1) find __hipRegisterFunction call sites (via its thunk) ----
# locate thunk: find all jmp qword ptr [rip+X] that target an IAT slot imported from amdhip64
slots = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    if entry.dll.decode() == 'amdhip64_7.dll':
        for imp in entry.imports:
            if imp.address:
                slots[imp.address] = imp.name.decode() if imp.name else ''

# find thunk for __hipRegisterFunction
thunk_reg = None
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400
for i in range(text_off, text_off + text_size - 6):
    if data[i] == 0xFF and data[i+1] == 0x25:
        disp = struct.unpack_from('<i', data, i + 2)[0]
        tgt = (text_va + (i - text_off) + 6 + disp) & 0xFFFFFFFFFFFFFFFF
        if slots.get(tgt) == '__hipRegisterFunction':
            thunk_reg = text_va + (i - text_off)
            break
print('__hipRegisterFunction thunk: 0x%x' % (thunk_reg or 0))

# ---- 2) find calls to that thunk; for each, resolve kernel symbol string & global slot ----
def read_cstr(va):
    r = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= r < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            off = s.PointerToRawData + (r - s.VirtualAddress)
            raw = data[off:off+200].split(b'\x00')[0]
            return raw.decode('latin1') if raw else None
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
# scan all E8 calls to thunk_reg; look back for lea rdx/rcx of kernel name and store target
reg_sites = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] != 0xE8: continue
    disp = struct.unpack_from('<i', data, i + 1)[0]
    src = text_va + (i - text_off) + 5
    if ((src + disp) & 0xFFFFFFFFFFFFFFFF) != thunk_reg: continue
    reg_sites.append(src - 5)
print('__hipRegisterFunction call sites:', len(reg_sites))

# for each site, disassemble backwards ~60 bytes to find: kernel name string (lea reg,[rip+X]),
# and after the call find store to a global slot (mov [rip+Y], rax)  -- the kernel handle slot.
handle_slots = {}
for site in reg_sites:
    rva = site - imgbase
    lo = max(text_off, text_off + (rva - 0x1000) - 96)
    hi = min(text_off + text_size, text_off + (rva - 0x1000) + 64)
    code = data[lo:hi]
    found_name = None
    store_slot = None
    for insn in md.disasm(code, text_va + (lo - text_off)):
        a = insn.address - imgbase
        if insn.mnemonic == 'lea' and '[rip +' in insn.op_str:
            m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
            if m:
                tgt = (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
                s = read_cstr(tgt)
                if s and ('k_' in s or 'kernel' in s or s.startswith('_Z')):
                    found_name = s
        if insn.mnemonic == 'mov' and '[rip +' in insn.op_str and 'rax' in insn.op_str:
            m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
            if m:
                tgt = (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
                store_slot = tgt - imgbase
        if insn.address > site + 16: break
    if found_name and store_slot:
        handle_slots[store_slot] = found_name

print('kernel handle slots resolved:', len(handle_slots))
for k, v in sorted(handle_slots.items()):
    print('  slot 0x%06x <- %s' % (k, v))

import json
with open(r'C:\Users\Celia\Desktop\dlssnr_re\03_static\kernel_slot_map.json', 'w') as f:
    json.dump({hex(k): v for k, v in handle_slots.items()}, f, indent=1)
print('saved kernel_slot_map.json')
