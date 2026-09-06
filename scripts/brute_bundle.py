#!/usr/bin/env python3
"""Brute-force the __CLANG_OFFLOAD_BUNDLE__ descriptor layout.
Candidates vary: field order (size/off/kind/archlen), field widths (4/8),
alignment after arch string. Valid = 5 entries, 4 with ELF magic at offset,
sizes consistent with file bounds, readable arch strings."""
import struct, sys

d = open(r'D:\FSR\DlssNrCore\data\dlssnr_kernels.fatbin', 'rb').read()
n = struct.unpack_from('<Q', d, 24)[0]
print('entries:', n)

def try_layout(fmt_desc, align_arch, label):
    """fmt_desc: list of (name, size) for the 24-byte-ish descriptor;
    align_arch: pad each entry to this boundary after arch string."""
    total_w = sum(sz for _, sz in fmt_desc)
    p = 32
    entries = []
    for i in range(n):
        if p + total_w > len(d):
            return None
        vals = {}
        q = p
        for name, sz in fmt_desc:
            vals[name] = struct.unpack_from('<Q' if sz == 8 else '<I', d, q)[0]
            q += sz
        arch = d[q:q + vals.get('archlen', 0)]
        # arch string: strip at NUL for display
        arch_s = arch.split(b'\x00')[0].decode('ascii', 'replace')
        entries.append((vals, arch_s, q))
        # advance: descriptor + archlen, then align
        nextp = q + vals.get('archlen', 0)
        if align_arch:
            nextp = (nextp + align_arch - 1) // align_arch * align_arch
        p = nextp
        if p > len(d):
            return None
    return entries

layouts = {
    'A [size8 off8 kind4 archlen4]': ([('size',8),('off',8),('kind',4),('archlen',4)], 0),
    'B [size8 off8 kind4 archlen4]+8align': ([('size',8),('off',8),('kind',4),('archlen',4)], 8),
    'C [size8 off8 archlen4 kind4]': ([('size',8),('off',8),('archlen',4),('kind',4)], 0),
    'D [size8 off8 archlen8]': ([('size',8),('off',8),('archlen',8)], 0),
    'E [size8 off8 archlen8]+8align': ([('size',8),('off',8),('archlen',8)], 8),
    'F [size8 off8 kind8 archlen8]': ([('size',8),('off',8),('kind',8),('archlen',8)], 0),
    'G [size8 off8 kind8 archlen8]+8align': ([('size',8),('off',8),('kind',8),('archlen',8)], 8),
    'H [off8 size8 kind4 archlen4]': ([('off',8),('size',8),('kind',4),('archlen',4)], 0),
    'I [off8 size8 kind4 archlen4]+8align': ([('off',8),('size',8),('kind',4),('archlen',4)], 8),
    'J [size4 off4 kind4 archlen4]': ([('size',4),('off',4),('kind',4),('archlen',4)], 0),
}

for label, (fmt, align) in layouts.items():
    try:
        entries = try_layout(fmt, align, label)
    except Exception:
        continue
    if not entries:
        continue
    # validation: every entry's 'off' points to ELF magic OR (i==0 host)
    ok = True
    for i, (vals, arch_s, q) in enumerate(entries):
        off = vals.get('off', -1)
        sz = vals.get('size', -1)
        if not (0 <= off < len(d)):
            ok = False; break
        is_elf = d[off:off+4] == b'\x7fELF'
        if i == 0 and not is_elf:
            # host object: not ELF necessarily
            pass
        elif i > 0 and not is_elf:
            ok = False; break
        if not (0 < sz <= len(d)):
            ok = False; break
    if ok:
        print('=== VALID: %s ===' % label)
        for i, (vals, arch_s, q) in enumerate(entries):
            off = vals.get('off'); sz = vals.get('size')
            print('  [%d] size=0x%x off=0x%x kind=%s archlen=%s arch=[%s] elf=%s' % (
                i, sz, off, vals.get('kind'), vals.get('archlen'), arch_s,
                d[off:off+4] == b'\x7fELF'))
