#!/usr/bin/env python3
"""Clean DXGI adapter enumeration probe (no proxy). Verify vtable slots."""
import ctypes, ctypes.wintypes as wt, sys, uuid

u32 = ctypes.c_uint32

def hr_str(hr):
    hr &= 0xFFFFFFFF
    return {0:'S_OK', 0x80004002:'E_NOINTERFACE', 0x80070057:'E_INVALIDARG',
            0x887A0027:'DXGI_ERROR_NOT_FOUND'}.get(hr, '0x%08X' % hr)

def guid(s):
    return (ctypes.c_char*16).from_buffer_copy(uuid.UUID(s).bytes_le)

class LUID(ctypes.Structure):
    _fields_ = [('LowPart', u32), ('HighPart', ctypes.c_long)]

class DXGI_ADAPTER_DESC1(ctypes.Structure):
    _fields_ = [('Description', wt.WCHAR*128), ('VendorId', u32), ('DeviceId', u32),
                ('SubSysId', u32), ('Revision', u32), ('DedicatedVideoMemory', ctypes.c_size_t),
                ('DedicatedSystemMemory', ctypes.c_size_t), ('SharedSystemMemory', ctypes.c_size_t),
                ('AdapterLuid', LUID)]

dxgi = ctypes.WinDLL('dxgi')
dxgi.CreateDXGIFactory1.restype = ctypes.c_long
dxgi.CreateDXGIFactory1.argtypes = [ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)]
factory = wt.LPVOID()
hr = dxgi.CreateDXGIFactory1(guid('770aae78-f26f-4dba-a829-253c83d1b387'), ctypes.byref(factory))
print('factory hr =', hr_str(hr), 'ptr =', hex(factory.value or 0))
fvt = ctypes.cast(factory, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

for slot in (7, 12):   # EnumAdapters (slot 7), EnumAdapters1 (slot 12)
    enum = ctypes.cast(fvt[slot], ctypes.WINFUNCTYPE(
        ctypes.c_long, wt.LPVOID, u32, ctypes.POINTER(wt.LPVOID)))
    print('\n--- slot %d ---' % slot)
    for i in range(8):
        a = wt.LPVOID()
        hr = enum(factory, i, ctypes.byref(a))
        if hr != 0:
            print('  adapter[%d] hr = %s' % (i, hr_str(hr)))
            break
        print('  adapter[%d] ptr = 0x%x' % (i, a.value or 0))
        avt = ctypes.cast(a, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        print('    vtable[7]=0x%x vtable[8]=0x%x vtable[9]=0x%x' % (avt[7] or 0, avt[8] or 0, avt[9] or 0))
        get_desc = ctypes.cast(avt[8], ctypes.WINFUNCTYPE(  # IDXGIAdapter::GetDesc = slot 8 (after IDXGIObject's 7)
            ctypes.c_long, wt.LPVOID, ctypes.POINTER(DXGI_ADAPTER_DESC1)))
        d = DXGI_ADAPTER_DESC1()
        hr2 = get_desc(a, ctypes.byref(d))
        if hr2 == 0:
            print('    desc: %s vendor=0x%04x dev=0x%04x VRAM=%.0fMB' % (
                str(d.Description).rstrip('\x00'), d.VendorId, d.DeviceId, d.DedicatedVideoMemory/1048576.0))
        else:
            print('    GetDesc hr =', hr_str(hr2))
print('\ndone')
