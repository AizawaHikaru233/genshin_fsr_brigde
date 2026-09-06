#!/usr/bin/env python3
"""Final v4: decode launch sites with robust byte-pattern backward scan for
`lea rcx,[rip+disp32]` (48 8D 0D ...). Emits launch sequence JSON."""
import struct, re, json
import pefile

PE_PATH = r'C:\Users\Celia\Desktop\dlssnr_re\01_files\version.dll'
pe = pefile.PE(PE_PATH)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = open(PE_PATH, 'rb').read()
text_off, text_va, text_size = 0x400, imgbase + 0x1000, 0x53400

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

def read_u64(va):
    rva = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            off = s.PointerToRawData + (rva - s.VirtualAddress)
            if off + 8 <= len(data):
                return struct.unpack_from('<Q', data, off)[0]
    return None

# launch sites: E8 -> thunk 0x540f0
launch_sites = []
for i in range(text_off, text_off + text_size - 5):
    if data[i] == 0xE8:
        disp = struct.unpack_from('<i', data, i + 1)[0]
        src = text_va + (i - text_off) + 5
        if ((src + disp) & 0xFFFFFFFFFFFFFFFF) == imgbase + 0x540f0:
            launch_sites.append(src - 5)

def file_of_va(va):
    rva = va - imgbase
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def find_last_lea_rcx_rip(site_va):
    """Backward byte scan for 48 8D 0D disp32 before the call site."""
    fo = file_of_va(site_va)
    if fo is None: return None
    start = max(text_off, fo - 0x300)
    best = None
    for i in range(start, fo - 6):
        if data[i] == 0x48 and data[i+1] == 0x8D and data[i+2] == 0x0D:
            disp = struct.unpack_from('<i', data, i + 3)[0]
            tgt = (text_va + (i - text_off) + 7 + disp) & 0xFFFFFFFFFFFFFFFF
            best = (text_va + (i - text_off), tgt)
    return best

def owner(rva):
    pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                        pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
    for j in range(0, len(pdata), 12):
        b, e, u = struct.unpack_from('<III', pdata, j)
        if b <= rva < e: return (b, e)
    return None

rows = []
for site in launch_sites:
    hit = find_last_lea_rcx_rip(site)
    kernel = None
    if hit:
        lea_va, tgt = hit
        val = read_u64(tgt)
        kernel = WRAP_TO_KERNEL.get(val)
    f = owner(site - imgbase)
    rows.append({
        'site': site - imgbase,
        'func': ('0x%05x' % f[0]) if f else ('0x%05x' % (site - imgbase)),
        'kernel': kernel,
        'slot': (tgt - imgbase) if hit else None,
    })

ok = sum(1 for r in rows if r['kernel'])
print('decoded %d/%d' % (ok, len(rows)))
from collections import OrderedDict
groups = OrderedDict()
for r in rows:
    groups.setdefault(r['func'], []).append(r)
out = []
for f, rs in groups.items():
    line = '%s: %s' % (f, ' -> '.join((r['kernel'] or '?') for r in rs))
    print(line)
    out.append(line)

with open(r'C:\Users\Celia\Desktop\dlssnr_re\03_static\launch_sequence.json', 'w') as fp:
    json.dump({'kernel_registration_order': [n for _, n in WRAPPERS],
               'functions': {f: [{'site': '0x%05x' % r['site'], 'kernel': r['kernel']} for r in rs]
                             for f, rs in groups.items()}}, fp, indent=1)
print('saved launch_sequence.json')
