#!/usr/bin/env python3
"""Final launch-site decoder: slot -> wrapper -> kernel registration order.
- registration order (33 kernels) known from register_func.asm
- slot table at 0x55170 + 8*N (N = registration index), values = wrapper fn ptrs
- launch sites: lea rcx,[rip+X] where X = slot address -> N -> kernel
Also dump the params setup region (rdx/r8 = two param struct pointers) for each site.
"""
import struct, re, json
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

# registration order from register_func.asm (33 kernels; index 0 = swin_1h)
KERNEL_ORDER = [
    'k_swin_1h_32_fp8', 'k_pre_block_1h_32_fp8', 'k_post_block_1h_32_fp8',
    'k_ffwd', 'k_conv_res', 'k_qkv_attn', 'k_ffwd2', 'k_conv_res2', 'k_qkv_attn2',
    'k_expand', 'k_conv_splitk', 'k_qkv', 'k_attention', 'k_expand2', 'k_contract2',
    'k_qkv2', 'k_attention2', 'k_ffwd_inpview', 'k_conv_res_views', 'k_final_head',
    'k_repack', 'k_dec_upsample', 'k_mean', 'k_import', 'k_export', 'k_reproject',
    'k_flag_wait', 'k_flag_set', 'k_swin_var32s', 'k_swin_var32', 'k_swin_var64',
    'k_swin_var128', 'k_swin_var256',
]

# slot base and stride: 0x55170 + 8*N
SLOT_BASE = 0x55170

def rva_of(va): return va - imgbase

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# ---- collect launch sites (calls to thunk 0x540f0) ----
launch_sites = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == imgbase + 0x540f0:
            launch_sites.append(src - 5)

# ---- decode each site ----
results = []
for site in launch_sites:
    rva = rva_of(site)
    lo = max(text_off, text_off + (rva - 0x1000) - 96)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + 8]
    slot = None
    # find the lea rcx,[rip+X] that targets the slot table
    for insn in md.disasm(code, text_va + (lo - text_off)):
        if insn.address > site: break
        if insn.mnemonic == 'lea' and insn.op_str.startswith('rcx,') and '[rip +' in insn.op_str:
            m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
            if m:
                tgt = (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
                if SLOT_BASE <= rva_of(tgt) <= SLOT_BASE + 8 * 40:
                    slot = rva_of(tgt)
    if slot is None:
        results.append((site, None, '?'))
        continue
    idx = (slot - SLOT_BASE) // 8
    kernel = KERNEL_ORDER[idx] if 0 <= idx < len(KERNEL_ORDER) else '?'
    results.append((site, idx, kernel))

print('decoded %d/%d launch sites' % (sum(1 for r in results if r[1] is not None), len(results)))
for site, idx, kernel in results:
    print('  site 0x%05x slot 0x%05x idx=%3s kernel=%-18s' % (
        rva_of(site), (SLOT_BASE + 8*idx) if idx is not None else 0, idx, kernel))

# ---- per-function launch sequence (network body functions) ----
print('\n--- launch sequence per function ---')
from collections import OrderedDict
seq = OrderedDict()
for site, idx, kernel in results:
    if idx is None: continue
    # find owner function via pdata
    pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                        pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
    owner = None
    for j in range(0, len(pdata), 12):
        b, e, u = struct.unpack_from('<III', pdata, j)
        if b <= rva_of(site) < e:
            owner = (b, e); break
    key = ('0x%05x' % owner[0]) if owner else ('0x%05x' % rva_of(site))
    seq.setdefault(key, []).append((rva_of(site), kernel))

for f, sites in seq.items():
    print('func %s (%d launches):' % (f, len(sites)))
    print('   ' + ' -> '.join(k for _, k in sites))
