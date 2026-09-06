#!/usr/bin/env python3
"""Extract ASCII and UTF-16LE strings from a binary with RVA mapping."""
import sys, os, re

def extract(path, minlen=5):
    data = open(path, 'rb').read()
    ascii_re = re.compile(rb'[\x20-\x7e]{%d,}' % minlen)
    wide_re = re.compile(rb'(?:[\x20-\x7e]\x00){%d,}' % minlen)
    out = []
    for m in ascii_re.finditer(data):
        s = m.group().decode('ascii', 'ignore')
        if s.count('\x00') == 0:
            out.append(('A', m.start(), s))
    for m in wide_re.finditer(data):
        s = m.group().decode('utf-16le', 'ignore')
        if s.count('\x00') == 0 and any(c.isalpha() for c in s):
            out.append(('W', m.start(), s))
    return out

if __name__ == '__main__':
    path = sys.argv[1]
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    base = os.path.basename(path)
    items = extract(path)
    with open(os.path.join(outdir, base + '.strings.txt'), 'w', encoding='utf-8') as f:
        for kind, off, s in items:
            f.write('%s\t0x%08x\t%s\n' % (kind, off, s))
    print('total strings:', len(items))
    # print interesting ones
    keys = ('hip','HIP','kernel','Kernel','dlss','DLSS','nvngx','Nvngx','NGX','fsr','FSR','tone','Tone','temporal','Temporal','model','Model','mask','Mask','skin','Skin','structure','Structure','dxgi','DXGI','d3d12','D3D12','swapchain','SwapChain','nvapi','NVAPI','create','Create','hook','Hook','HipDevice','Interop','Inline','version','Version','AMD','amd','NVIDIA','nvidia','scale','Scale','http','www','github','patreon','discord','error','Error','failed','Failed','config','Config','.ini','exe','dll')
    seen = set()
    print('--- interesting ---')
    for kind, off, s in items:
        if any(k in s for k in keys):
            key = (kind, s)
            if key in seen: continue
            seen.add(key)
            print('%s 0x%08x  %s' % (kind, off, s[:200]))
