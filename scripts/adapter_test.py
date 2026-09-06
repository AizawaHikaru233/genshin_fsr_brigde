#!/usr/bin/env python3
"""Enumerate DXGI adapters, pick AMD (VendorId 0x1002), create D3D12 device.
Optional: load proxy DLL first (argv[1]). Prints HRESULTs and adapter info.
"""
import ctypes, ctypes.wintypes as wt, os, sys, uuid

u32 = ctypes.c_uint32

def hr_str(hr):
    hr &= 0xFFFFFFFF
    return {0:'S_OK', 0x80004002:'E_NOINTERFACE', 0x80070057:'E_INVALIDARG',
            0x80004005:'E_FAIL', 0x887A0004:'DXGI_ERROR_UNSUPPORTED',
            0x887A0027:'DXGI_ERROR_NOT_FOUND'}.get(hr, '0x%08X' % hr)

def guid(s):
    return (ctypes.c_char*16).from_buffer_copy(uuid.UUID(s).bytes_le)

if len(sys.argv) > 1:
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.LoadLibraryW.argtypes = [wt.LPCWSTR]; k.LoadLibraryW.restype = wt.HMODULE
    h = k.LoadLibraryW(os.path.abspath(sys.argv[1]))
    print('proxy LoadLibraryW -> 0x%x (err %d)' % (h or 0, ctypes.get_last_error()))

class LUID(ctypes.Structure):
    _fields_ = [('LowPart', u32), ('HighPart', ctypes.c_long)]

class DXGI_ADAPTER_DESC1(ctypes.Structure):
    _fields_ = [('Description', wt.WCHAR*128), ('VendorId', u32), ('DeviceId', u32),
                ('SubSysId', u32), ('Revision', u32), ('DedicatedVideoMemory', ctypes.c_size_t),
                ('DedicatedSystemMemory', ctypes.c_size_t), ('SharedSystemMemory', ctypes.c_size_t),
                ('AdapterLuid', LUID)]

dxgi = ctypes.WinDLL('dxgi', use_last_error=True)
dxgi.CreateDXGIFactory1.restype = ctypes.c_long
dxgi.CreateDXGIFactory1.argtypes = [ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)]
IID_IDXGIFactory1 = guid('770aae78-f26f-4dba-a829-253c83d1b387')
factory = wt.LPVOID()
hr = dxgi.CreateDXGIFactory1(IID_IDXGIFactory1, ctypes.byref(factory))
print('CreateDXGIFactory1 hr =', hr_str(hr))
if hr != 0: sys.exit(2)

fvt = ctypes.cast(factory, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
enum_adapters1 = ctypes.cast(fvt[8], ctypes.WINFUNCTYPE(
    ctypes.c_long, wt.LPVOID, u32, ctypes.POINTER(wt.LPVOID)))
release_f = ctypes.cast(fvt[2], ctypes.WINFUNCTYPE(ctypes.c_ulong, wt.LPVOID))

adapters = []
for i in range(8):
    a = wt.LPVOID()
    hr = enum_adapters1(factory, i, ctypes.byref(a))
    if hr != 0: break
    avt = ctypes.cast(a, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_desc = ctypes.cast(avt[4], ctypes.WINFUNCTYPE(
        ctypes.c_long, wt.LPVOID, ctypes.POINTER(DXGI_ADAPTER_DESC1)))
    d = DXGI_ADAPTER_DESC1()
    hr2 = get_desc(a, ctypes.byref(d))
    name = d.Description.value if hr2 == 0 else '?'
    print('adapter[%d] 0x%x: %s vendor=0x%04x device=0x%04x VRAM=%.0f MB' % (
        i, a.value or 0, name, d.VendorId, d.DeviceId, d.DedicatedVideoMemory/1048576.0))
    adapters.append((a.value, name, d.VendorId, a))
    release = ctypes.cast(avt[2], ctypes.WINFUNCTYPE(ctypes.c_ulong, wt.LPVOID))
    release(a)

release_f(factory)

# pick AMD adapter
amd = None
for val, name, vid, a in adapters:
    if vid == 0x1002:
        amd = (val, name, a)
if not amd:
    print('no AMD adapter found'); sys.exit(3)
print('using AMD adapter:', amd[1])

d3d12 = ctypes.WinDLL('d3d12', use_last_error=True)
d3d12.D3D12CreateDevice.restype = ctypes.c_long
d3d12.D3D12CreateDevice.argtypes = [wt.LPVOID, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)]
IID_ID3D12Device = guid('189819f1-1ec6-4bdc-ab4e-829f5d5f8ce4')
device = wt.LPVOID()
for fl, flname in ((0xc000, '12_0'), (0xb100, '12_1'), (0xb000, '11_0')):
    hr = d3d12.D3D12CreateDevice(amd[2], fl, IID_ID3D12Device, ctypes.byref(device))
    print('D3D12CreateDevice(%s) hr = %s device = 0x%x' % (flname, hr_str(hr), device.value or 0))
    if hr == 0: break
print('done')
