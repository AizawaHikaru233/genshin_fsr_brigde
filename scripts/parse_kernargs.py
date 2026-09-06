#!/usr/bin/env python3
"""Parse AMDGPU metadata (.note section, msgpack) from gfx1201 code object.
Extracts per-kernel kernarg layout: every arg's offset/size/value_kind,
kernarg_segment_size, workgroup sizes, register counts.
This is the authoritative parameter-struct dictionary for DlssNrCore network.cpp."""
import struct, json, sys
import msgpack

D = open(r'D:\FSR\DlssNrCore\data\co_gfx1201.bin', 'rb').read()

# .note at off 0x238, size 0x89c4 (from section dump)
note = D[0x238:0x238 + 0x89c4]
namesz, descsz, ntype = struct.unpack_from('<III', note, 0)
print('note: namesz=%d descsz=%d type=0x%x' % (namesz, descsz, ntype))
name = note[12:12 + namesz].rstrip(b'\x00')
desc_off = 12 + ((namesz + 3) & ~3)
desc = note[desc_off:desc_off + descsz]
print('note name:', name)

meta = msgpack.unpackb(desc, raw=False)
kernels = meta['amdhsa.kernels']
print('target:', meta.get('amdhsa.target'))
print('version:', meta.get('amdhsa.version'))
print('kernels:', len(kernels))

VALUE_KIND = {0:'by_value', 1:'global_buffer', 2:'dynamic_shared_pointer',
              3:'sampler', 4:'image', 5:'pipe', 6:'queue', 7:'hidden_global_offset_x',
              8:'hidden_global_offset_y', 9:'hidden_global_offset_z', 10:'hidden_none',
              11:'hidden_printf_buffer', 12:'hidden_default_stream', 13:'hidden_hostcall_buffer',
              14:'hidden_heap', 15:'hidden_multigrid_sync_arg', 16:'hidden_private_base',
              17:'hidden_shared_base', 18:'hidden_queue_ptr', 19:'hidden_global_worklist',
              20:'hidden_completion_action', 21:'hidden_workgroup_id_x', 22:'hidden_workgroup_id_y',
              23:'hidden_workgroup_id_z', 24:'hidden_workgroup_size_x', 25:'hidden_workgroup_size_y',
              26:'hidden_workgroup_size_z', 27:'hidden_block_count_x', 28:'hidden_block_count_y',
              29:'hidden_block_count_z', 30:'hidden_group_size_x', 31:'hidden_group_size_y',
              32:'hidden_group_size_z', 33:'hidden_remainder_x', 34:'hidden_remainder_y',
              35:'hidden_remainder_z', 36:'hidden_grid_dims', 37:'hidden_wavefront_size'}

out = []
for k in kernels:
    name_s = k.get('.name') or k.get('name', '?')
    args = k.get('.args', []) or []
    arglist = []
    for a in args:
        arglist.append({
            'offset': a.get('.offset'),
            'size': a.get('.size'),
            'kind': VALUE_KIND.get(a.get('.value_kind'), a.get('.value_kind')),
            'addrspace': a.get('.address_space', None),
            'pointee_align': a.get('.pointee_align', None),
        })
    entry = {
        'name': name_s,
        'kernarg_segment_size': k.get('.kernarg_segment_size'),
        'kernarg_segment_align': k.get('.kernarg_segment_align'),
        'group_segment_fixed_size': k.get('.group_segment_fixed_size'),
        'private_segment_fixed_size': k.get('.private_segment_fixed_size'),
        'max_flat_workgroup_size': k.get('.max_flat_workgroup_size'),
        'sgpr_count': k.get('.sgpr_count'),
        'vgpr_count': k.get('.vgpr_count'),
        'wavefront_size': k.get('.wavefront_size'),
        'args': arglist,
    }
    out.append(entry)
    # print summary
    print('\n=== %s ===' % name_s)
    print('  kernarg %d bytes (align %d), group %d, private %d, max_wg %d, sgpr %d, vgpr %d, wave %d' % (
        entry['kernarg_segment_size'], entry['kernarg_segment_align'],
        entry['group_segment_fixed_size'], entry['private_segment_fixed_size'],
        entry['max_flat_workgroup_size'], entry['sgpr_count'], entry['vgpr_count'],
        entry['wavefront_size']))
    for a in arglist:
        print('    +0x%-4x size=%-4d %s' % (a['offset'], a['size'], a['kind']))

with open(r'D:\FSR\DlssNrCore\docs\kernel_arg_layout.json', 'w', encoding='utf-8') as f:
    json.dump({'target': meta.get('amdhsa.target'), 'kernels': out}, f, indent=1, ensure_ascii=False)
print('\nsaved docs/kernel_arg_layout.json (%d kernels)' % len(out))
