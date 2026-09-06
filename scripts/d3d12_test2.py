#!/usr/bin/env python3
"""Corrected D3D12 + DXGI test against the DLSS-NR proxy.
Correct vtable offsets (IDXGIObject has 7 methods):
  IDXGIFactory:   EnumAdapters=7, CreateSwapChain=10
  IDXGIFactory1:  EnumAdapters1=12, IsCurrent=13
  IDXGIFactory2:  IsWindowedStereoEnabled=14, CreateSwapChainForHwnd=15
Loads proxy (argv[1]), enumerates adapters, picks AMD (0x1002), creates D3D12 device,
command queue, swapchain, and Presents. Log lands in _dlssnr_on_amd.log.
"""
import ctypes, ctypes.wintypes as wt, os, sys, uuid, time

u32 = ctypes.c_uint32

def hr_str(hr):
    hr &= 0xFFFFFFFF
    return {0:'S_OK', 0x80004002:'E_NOINTERFACE', 0x80070057:'E_INVALIDARG',
            0x80004005:'E_FAIL', 0x887A0004:'DXGI_ERROR_UNSUPPORTED',
            0x887A0027:'DXGI_ERROR_NOT_FOUND', 0x887A002D:'DXGI_ERROR_NOT_CURRENTLY_AVAILABLE'
            }.get(hr, '0x%08X' % hr)

def guid(s):
    return (ctypes.c_char*16).from_buffer_copy(uuid.UUID(s).bytes_le)

class LUID(ctypes.Structure):
    _fields_ = [('LowPart', u32), ('HighPart', ctypes.c_long)]

class DXGI_ADAPTER_DESC1(ctypes.Structure):
    _fields_ = [('Description', wt.WCHAR*128), ('VendorId', u32), ('DeviceId', u32),
                ('SubSysId', u32), ('Revision', u32), ('DedicatedVideoMemory', ctypes.c_size_t),
                ('DedicatedSystemMemory', ctypes.c_size_t), ('SharedSystemMemory', ctypes.c_size_t),
                ('AdapterLuid', LUID)]

if len(sys.argv) > 1:
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.LoadLibraryW.argtypes = [wt.LPCWSTR]; k.LoadLibraryW.restype = wt.HMODULE
    h = k.LoadLibraryW(os.path.abspath(sys.argv[1]))
    print('proxy LoadLibraryW -> 0x%x (err %d)' % (h or 0, ctypes.get_last_error()))

user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.CreateWindowExA.restype = wt.HWND
user32.CreateWindowExA.argtypes = [u32, wt.LPCSTR, wt.LPCSTR, u32, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMODULE, wt.HINSTANCE, wt.LPVOID]
user32.DestroyWindow.argtypes = [wt.HWND]
hwnd = user32.CreateWindowExA(0, b'STATIC', b'dlssnr_d3d12', 0, 0, 0, 320, 180,
                              None, None, None, None)
print('hwnd =', hex(hwnd or 0))

# ---------- factory ----------
dxgi = ctypes.WinDLL('dxgi', use_last_error=True)
dxgi.CreateDXGIFactory1.restype = ctypes.c_long
dxgi.CreateDXGIFactory1.argtypes = [ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)]
factory = wt.LPVOID()
hr = dxgi.CreateDXGIFactory1(guid('770aae78-f26f-4dba-a829-253c83d1b387'), ctypes.byref(factory))
print('CreateDXGIFactory1 hr =', hr_str(hr))
if hr != 0: sys.exit(2)
fvt = ctypes.cast(factory, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

# ---------- enumerate adapters (slot 12) ----------
enum_adapters1 = ctypes.cast(fvt[12], ctypes.WINFUNCTYPE(
    ctypes.c_long, wt.LPVOID, u32, ctypes.POINTER(wt.LPVOID)))
amd_adapter = None
for i in range(8):
    a = wt.LPVOID()
    hr = enum_adapters1(factory, i, ctypes.byref(a))
    if hr != 0:
        print('adapter[%d] enum hr=%s' % (i, hr_str(hr)))
        break
    avt = ctypes.cast(a, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_desc = ctypes.cast(avt[8], ctypes.WINFUNCTYPE(  # IDXGIAdapter::GetDesc = slot 8
        ctypes.c_long, wt.LPVOID, ctypes.POINTER(DXGI_ADAPTER_DESC1)))
    d = DXGI_ADAPTER_DESC1()
    hr2 = get_desc(a, ctypes.byref(d))
    name = str(d.Description).rstrip('\x00') if hr2 == 0 else '?'
    print('adapter[%d]: %s vendor=0x%04x device=0x%04x VRAM=%.0f MB' % (
        i, name, d.VendorId, d.DeviceId, d.DedicatedVideoMemory/1048576.0))
    if d.VendorId == 0x1002 and amd_adapter is None:
        amd_adapter = (a.value, name, a)
if not amd_adapter:
    print('no AMD adapter'); sys.exit(3)
print('picked:', amd_adapter[1])

# ---------- D3D12 device on AMD adapter ----------
d3d12 = ctypes.WinDLL('d3d12', use_last_error=True)
d3d12.D3D12CreateDevice.restype = ctypes.c_long
d3d12.D3D12CreateDevice.argtypes = [wt.LPVOID, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)]
IID_ID3D12Device = guid('189819f1-1ec6-4bdc-ab4e-829f5d5f8ce4')
device = wt.LPVOID()
hr = d3d12.D3D12CreateDevice(amd_adapter[2], 0xc000, IID_ID3D12Device, ctypes.byref(device))
print('D3D12CreateDevice(12_0) hr =', hr_str(hr), 'device =', hex(device.value or 0))
if hr != 0: sys.exit(4)
dvt = ctypes.cast(device, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

# ---------- command queue (ID3D12Device slot 4) ----------
class D3D12_COMMAND_QUEUE_DESC(ctypes.Structure):
    _fields_ = [('Type', u32), ('Priority', ctypes.c_int), ('Flags', u32), ('NodeMask', u32)]
IID_ID3D12CommandQueue = guid('0ec870a6-7443-41ff-8cfc-11f9f08f2b4f')
create_cq = ctypes.cast(dvt[4], ctypes.WINFUNCTYPE(
    ctypes.c_long, wt.LPVOID, ctypes.POINTER(D3D12_COMMAND_QUEUE_DESC),
    ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)))
queue = wt.LPVOID()
hr = create_cq(device, ctypes.byref(D3D12_COMMAND_QUEUE_DESC(0, 0, 0, 0)),
               IID_ID3D12CommandQueue, ctypes.byref(queue))
print('CreateCommandQueue hr =', hr_str(hr), 'queue =', hex(queue.value or 0))
if hr != 0: sys.exit(5)

# ---------- swapchain (IDXGIFactory2 slot 15) ----------
class DXGI_SAMPLE_DESC(ctypes.Structure):
    _fields_ = [('Count', u32), ('Quality', u32)]
class DXGI_SWAP_CHAIN_DESC1(ctypes.Structure):
    _fields_ = [('Width', u32), ('Height', u32), ('Format', ctypes.c_int), ('Stereo', ctypes.c_int),
                ('SampleDesc', DXGI_SAMPLE_DESC), ('BufferUsage', u32), ('BufferCount', u32),
                ('Scaling', u32), ('SwapEffect', u32), ('AlphaMode', u32), ('Flags', u32)]
create_sc_hwnd = ctypes.cast(fvt[15], ctypes.WINFUNCTYPE(
    ctypes.c_long, wt.LPVOID, wt.LPVOID, wt.HWND,
    ctypes.POINTER(DXGI_SWAP_CHAIN_DESC1), wt.LPVOID, wt.LPVOID, ctypes.POINTER(wt.LPVOID)))
scdesc = DXGI_SWAP_CHAIN_DESC1()
scdesc.Width = 320; scdesc.Height = 180; scdesc.Format = 28
scdesc.SampleDesc.Count = 1
scdesc.BufferUsage = 0x20
scdesc.BufferCount = 2
scdesc.SwapEffect = 4  # FLIP_DISCARD
swapchain = wt.LPVOID()
hr = create_sc_hwnd(factory, queue, hwnd, ctypes.byref(scdesc), None, None, ctypes.byref(swapchain))
print('CreateSwapChainForHwnd hr =', hr_str(hr), 'swapchain =', hex(swapchain.value or 0))
if hr != 0: sys.exit(6)

svt = ctypes.cast(swapchain, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
present = ctypes.cast(svt[3], ctypes.WINFUNCTYPE(ctypes.c_long, wt.LPVOID, u32, u32))
for i in range(4):
    phr = present(swapchain, 0, 0)
    print('Present #%d hr=%s' % (i, hr_str(phr)))
    time.sleep(1.0)
release = ctypes.cast(svt[2], ctypes.WINFUNCTYPE(ctypes.c_ulong, wt.LPVOID))
release(swapchain)
user32.DestroyWindow(hwnd)
print('done')
