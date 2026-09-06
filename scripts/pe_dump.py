#!/usr/bin/env python3
"""PE triage dump: headers, sections+entropy, imports, exports, resources, TLS, strings."""
import sys, os, math, struct
import pefile

def entropy(data):
    if not data: return 0.0
    freq = [0]*256
    for b in data: freq[b] += 1
    n = len(data)
    e = 0.0
    for c in freq:
        if c:
            p = c/n
            e -= p*math.log2(p)
    return e

def main(path, outdir):
    os.makedirs(outdir, exist_ok=True)
    base = os.path.basename(path)
    pe = pefile.PE(path, fast_load=False)
    lines = []
    def P(*a): lines.append(' '.join(str(x) for x in a))

    P('== HEADER ==')
    P('Machine: 0x%04x (%s)' % (pe.FILE_HEADER.Machine, {0x14c:'x86',0x8664:'x64',0xaa64:'ARM64'}.get(pe.FILE_HEADER.Machine,'?')))
    P('Sections: %d' % pe.FILE_HEADER.NumberOfSections)
    ts = pe.FILE_HEADER.TimeDateStamp
    P('TimeDateStamp: 0x%08x' % ts)
    P('Characteristics: 0x%04x' % pe.FILE_HEADER.Characteristics)
    P('OptionalHeader.Magic: 0x%04x' % pe.OPTIONAL_HEADER.Magic)
    P('Subsystem: %d' % pe.OPTIONAL_HEADER.Subsystem)
    P('DllCharacteristics: 0x%04x' % pe.OPTIONAL_HEADER.DllCharacteristics)
    P('ImageBase: 0x%x' % pe.OPTIONAL_HEADER.ImageBase)
    P('SizeOfImage: 0x%x' % pe.OPTIONAL_HEADER.SizeOfImage)
    P('EntryPoint RVA: 0x%x' % pe.OPTIONAL_HEADER.AddressOfEntryPoint)
    P('Debug dir RVA: 0x%x size 0x%x' % (pe.OPTIONAL_HEADER.DATA_DIRECTORY[6].VirtualAddress, pe.OPTIONAL_HEADER.DATA_DIRECTORY[6].Size))
    P('CLR header dir (14) RVA: 0x%x size 0x%x' % (pe.OPTIONAL_HEADER.DATA_DIRECTORY[14].VirtualAddress, pe.OPTIONAL_HEADER.DATA_DIRECTORY[14].Size))
    P('TLS dir (9) RVA: 0x%x size 0x%x' % (pe.OPTIONAL_HEADER.DATA_DIRECTORY[9].VirtualAddress, pe.OPTIONAL_HEADER.DATA_DIRECTORY[9].Size))
    P('Resource dir (2) RVA: 0x%x size 0x%x' % (pe.OPTIONAL_HEADER.DATA_DIRECTORY[2].VirtualAddress, pe.OPTIONAL_HEADER.DATA_DIRECTORY[2].Size))

    P('')
    P('== SECTIONS ==')
    raw_end = 0
    for s in pe.sections:
        name = s.Name.rstrip(b'\x00').decode('latin1')
        e = entropy(s.get_data())
        raw_end = max(raw_end, s.PointerToRawData + s.SizeOfRawData)
        P('%-8s VA=0x%08x VSize=0x%08x RawPtr=0x%08x RawSize=0x%08x Entropy=%.3f Chars=0x%08x' % (
            name, s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, e, s.Characteristics))
    fsize = os.path.getsize(path)
    P('File size: 0x%x (%d)' % (fsize, fsize))
    if fsize > raw_end:
        P('OVERLAY: 0x%x bytes after last section raw end' % (fsize - raw_end))

    P('')
    P('== IMPORTS ==')
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for entry in pe.DIRECTORY_ENTRY_IMPORT:
            P('DLL: %s' % entry.dll.decode('latin1'))
            for imp in entry.imports:
                nm = imp.name.decode('latin1') if imp.name else 'ord#%d' % imp.ordinal
                P('    %s' % nm)
    else:
        P('(none)')
    P('')
    P('== DELAY IMPORTS ==')
    if hasattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT'):
        for entry in pe.DIRECTORY_ENTRY_DELAY_IMPORT:
            P('DLL: %s' % entry.dll.decode('latin1'))
            for imp in entry.imports:
                nm = imp.name.decode('latin1') if imp.name else 'ord#%d' % imp.ordinal
                P('    %s' % nm)
    else:
        P('(none)')

    P('')
    P('== EXPORTS ==')
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        exp = pe.DIRECTORY_ENTRY_EXPORT
        P('DLL name: %s' % exp.name.decode('latin1'))
        for e in exp.symbols:
            nm = e.name.decode('latin1') if e.name else 'ord#%d' % e.ordinal
            fwd = ''
            if e.forwarder:
                fwd = ' -> %s' % e.forwarder.decode('latin1')
            P('  ord=%-5d rva=0x%08x %s%s' % (e.ordinal, e.address, nm, fwd))
    else:
        P('(none)')

    P('')
    P('== RESOURCES (top-level types) ==')
    if hasattr(pe, 'DIRECTORY_ENTRY_RESOURCE'):
        def walk(entry, depth):
            if depth == 0:
                for e in entry.entries:
                    tid = e.id if e.id is not None else e.name
                    P('  type=%s' % tid)
                    if e.directory:
                        walk(e.directory, 1)
            elif depth == 1:
                for e in entry.entries:
                    P('    name=%s lang_dir' % (e.name if e.name else e.id))
    else:
        P('(none)')

    P('')
    P('== TLS CALLBACKS ==')
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[9].VirtualAddress:
        tls = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[9].VirtualAddress, pe.OPTIONAL_HEADER.DATA_DIRECTORY[9].Size)
        if len(tls) >= 40:
            if pe.FILE_HEADER.Machine == 0x8664:
                cb_va = struct.unpack_from('<Q', tls, 24)[0]
            else:
                cb_va = struct.unpack_from('<I', tls, 16)[0]
            P('TLS callback array VA: 0x%x' % cb_va)
            # walk array
            if cb_va:
                imgbase = pe.OPTIONAL_HEADER.ImageBase
                arr_rva = cb_va - imgbase
                for i in range(8):
                    size = 8 if pe.FILE_HEADER.Machine == 0x8664 else 4
                    val = pe.get_data(arr_rva + i*size, size)
                    cbfn = struct.unpack('<Q' if size==8 else '<I', val)[0]
                    if cbfn == 0 or cbfn < imgbase: break
                    P('  callback[%d] = 0x%x (rva 0x%x)' % (i, cbfn, cbfn - imgbase))
    else:
        P('(none)')

    # debug info
    P('')
    P('== DEBUG ==')
    if hasattr(pe, 'DIRECTORY_ENTRY_DEBUG'):
        for d in pe.DIRECTORY_ENTRY_DEBUG:
            P('  type=%d size=0x%x' % (d.struct.Type, d.struct.SizeOfData))
            if d.struct.Type == 2:
                try:
                    P('  PDB: %s' % d.entry.PdbFileName.decode('latin1'))
                except Exception:
                    pass

    rep = '\n'.join(lines)
    out = os.path.join(outdir, base + '.pe.txt')
    with open(out, 'w', encoding='utf-8') as f:
        f.write(rep)
    print(rep)
    print('WROTE', out)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
