#!/usr/bin/env python3
"""Enumerate functions via .pdata, find functions that reference interesting strings."""
import sys, os, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

def main(path):
    pe = pefile.PE(path)
    imgbase = pe.OPTIONAL_HEADER.ImageBase
    data = open(path, 'rb').read()
    md = Cs(CS_ARCH_X86, CS_MODE_64)

    # .pdata entries
    pdata = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress,
                        pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size)
    funcs = []
    for i in range(0, len(pdata), 12):
        begin, end, unwind = struct.unpack_from('<III', pdata, i)
        funcs.append((begin, end))
    print('total functions from .pdata:', len(funcs))

    # interesting strings -> rva
    interesting = [
        'IDXGISwapChain::Present', 'ID3D12CommandQueue::ExecuteCommandLists',
        'IDXGIFactory::CreateSwapChain', 'IDXGIFactory2::CreateSwapChainForHwnd',
        'IDXGISwapChain1::Present1', 'D3D12CreateDevice', 'CreateDXGIFactory2',
        'ffxFsr3UpscalerContextDispatch', 'ffxFsr3ContextDispatchUpscale',
        'ffxFsr3ContextCreate', 'ffxCreateContext',
        'weights build from nvngx_dlssnr.dll', 'dlssnr_on_amd.ini',
        'dlssnr_on_amd_weights.bin', 'hooked %s!%s', 'detour of %s!%s failed',
        'interop: hipImportExternalMemory', 'GetFileVersionInfoByHandle',
        'hipLaunchKernel', 'env: HIP device', 'DLSSNRW1',
        'first ffxFsr3UpscalerContextDispatch', 'first ffxFsr3ContextDispatchUpscale',
        'setup failed; idle', 'dlssnr_amd loaded',
    ]
    str_rvas = {}
    for s in interesting:
        idx = data.find(s.encode())
        if idx >= 0:
            rva = pe.get_rva_from_offset(idx)
            str_rvas[rva] = s
    print('found %d/%d strings in file' % (len(str_rvas), len(interesting)))

    # map string rva -> section VA for comparison (strings are in .rdata VA 0x55000..0x71xxx)
    # scan each function's code for lea reg,[rip+disp] pointing at those rvas
    def scan_func(begin, end):
        off = pe.get_offset_from_rva(begin)
        size = end - begin
        if off is None or size <= 0: return []
        code = data[off:off+size]
        hits = []
        for insn in md.disasm(code, imgbase + begin):
            if insn.mnemonic == 'lea' and insn.op_str.find('[rip +') >= 0:
                try:
                    # capstone gives op_str like 'rcx, [rip + 0x12345]'
                    disp = int(insn.op_str.split('0x')[1].rstrip(']'), 16)
                except Exception:
                    continue
                target = (insn.address + insn.size + disp) & 0xFFFFFFFFFFFFFFFF
                trva = target - imgbase
                if trva in str_rvas:
                    hits.append(str_rvas[trva])
        return hits

    results = []
    for begin, end in funcs:
        hits = scan_func(begin, end)
        if hits:
            results.append((begin, end, hits))
    results.sort(key=lambda r: r[0])
    print('\n=== functions referencing interesting strings ===')
    for begin, end, hits in results:
        print('func 0x%06x-0x%06x (size 0x%x): %s' % (begin, end, end-begin, '; '.join(sorted(set(hits)))))
    return results

if __name__ == '__main__':
    main(sys.argv[1])
