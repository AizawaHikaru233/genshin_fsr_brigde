#!/usr/bin/env python3
"""Final v2: launch site -> kernel via slot static value (wrapper addr) -> wrapper->kernel table.
Wrapper order == registration order (33 kernels).
Also dumps, for each launch site, the two param-struct stack pointers (rdx/r8 lea targets).
"""
import struct, re, json
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

# wrapper address -> kernel name (registration order == wrapper order)
WRAPPERS = [
    (0x1000, 'k_swin_1h_32_fp8'), (0x1060, 'k_pre_block_1h_32_fp8'), (0x10c0, 'k_post_block_1h_32_fp8'),
    (0x1120, 'k_ffwd'), (0x1180, 'k_conv_res'), (0x11e0, 'k_qkv_attn'),
    (0x1240, 'k_ffwd2'), (0x12a0, 'k_conv_res2'), (0x1300, 'k_qkv_attn2'),
    (0x1360, 'k_expand'), (0x13c0, 'k_conv_splitk'), (0x1420, 'k_qkv'),
    (0x1480, 'k_attention'), (0x14e0, 'k_expand2'), (0x1540, 'k_contract2'),
    (0x15a0, 'k_qkv2'), (0x1600, 'k_attention2'), (0x1660, 'k_ffwd_inpview'),
    (0x16c0, 'k_conv_res_views'), (0x1720, 'k_final_head'), (0x1780, 'k_repack'),
    (0x17e0, 'k_dec_upsample'), (0x1840, 'k_mean'), (0x18a0, 'k_import'),
    (0x1900, 'k_export'), (0x1960, 'k_reproject'), (0x1ae0, 'k_flag_wait'),
    (0x1b70, 'k_flag_set'), (0x21680, 'k_swin_var32s'), (0x23c00, 'k_swin_var32'),
    (0x23c60, 'k_swin_var64'), (0x23cc0, 'k_swin_var128'), (0x23d20, 'k_swin_var256'),
]
WRAP_TO_KERNEL = {imgbase + a: n for a, n in WRAPPERS}
assert len(WRAP_TO_KERNEL) == 33

def read_u64(va):
    rva = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            off = s.PointerToRawData + (rva - s.VirtualAddress)
            if off + 8 <= len(data):
                return struct.unpack_from('<Q', data, off)[0]
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# launch sites
launch_sites = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == imgbase + 0x540f0:
            launch_sites.append(src - 5)

def rip_target(insn):
    m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', insn.op_str)
    if m:
        return (insn.address + insn.size + int(m.group(1), 16)) & 0xFFFFFFFFFFFFFFFF
    return None

rows = []
for site in launch_sites:
    rva = rva_of = site - imgbase
    lo = max(text_off, text_off + (rva - 0x1000) - 0x200)
    code = data[lo:lo + (rva - (0x1000 + (lo - text_off))) + 8]
    kernel = None
    params = []
    for insn in md.disasm(code, text_va + (lo - text_off)):
        if insn.address > site: break
        if insn.mnemonic != 'lea': continue
        tgt = rip_target(insn)
        if not tgt: continue
        val = read_u64(tgt)
        if val in WRAP_TO_KERNEL:
            kernel = WRAP_TO_KERNEL[val]
        else:
            params.append(tgt - imgbase)
    rows.append((site, kernel, params))

print('launch sites decoded: %d/%d\n' % (sum(1 for r in rows if r[1]), len(rows)))
from collections import OrderedDict
# group by owner function
pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                    pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
def owner(rva):
    for j in range(0, len(pdata), 12):
        b, e, u = struct.unpack_from('<III', pdata, j)
        if b <= rva < e: return (b, e)
    return None

groups = OrderedDict()
for site, kernel, params in rows:
    f = owner(site - imgbase)
    key = ('0x%05x' % f[0]) if f else ('0x%05x' % (site - imgbase))
    groups.setdefault(key, []).append((site - imgbase, kernel, params))

for f, sites in groups.items():
    print('func %s:' % f)
    for s, k, p in sites:
        ps = ' '.join('0x%05x' % x for x in p[:4])
        print('   0x%05x  %-20s params:[%s]' % (s, k or '?', ps))
