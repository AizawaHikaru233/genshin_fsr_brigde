#!/usr/bin/env python3
"""Extract overlay from setup.exe and dump full PE info; extract version.dll sections of interest."""
import sys, os, struct
import pefile

def extract_overlay(exe_path, out_path):
    pe = pefile.PE(exe_path, fast_load=True)
    raw_end = 0
    for s in pe.sections:
        raw_end = max(raw_end, s.PointerToRawData + s.SizeOfRawData)
    data = open(exe_path, 'rb').read()
    ov = data[raw_end:]
    open(out_path, 'wb').write(ov)
    print('overlay size: 0x%x (%d)' % (len(ov), len(ov)))
    # magic sniff
    print('first 64 bytes:', ov[:64].hex())
    return ov

def dump_section(path, secname, outdir):
    pe = pefile.PE(path, fast_load=True)
    for s in pe.sections:
        nm = s.Name.rstrip(b'\x00').decode('latin1')
        if nm == secname:
            d = s.get_data()
            op = os.path.join(outdir, 'section_%s.bin' % secname)
            open(op, 'wb').write(d)
            print('%s -> %s (%d bytes)' % (nm, op, len(d)))
            return d
    print('section %s not found' % secname)

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'overlay':
        ov = extract_overlay(sys.argv[2], sys.argv[3])
    elif cmd == 'section':
        dump_section(sys.argv[2], sys.argv[3], sys.argv[4])
