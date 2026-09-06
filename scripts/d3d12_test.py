#!/usr/bin/env python3
"""D3D12 swapchain test — the exact path the DLSS-NR proxy targets:
D3D12CreateDevice -> ID3D12CommandQueue -> IDXGIFactory2::CreateSwapChainForHwnd -> Present.
Observe _dlssnr_on_amd.log for HIP/swapchain init lines.
"""
import ctypes, ctypes.wintypes as wt, os, sys, time, uuid

u32 = ctypes.c_uint32

def hr_str(hr):
    hr &= 0xFFFFFFFF
    return {0:'S_OK', 0x80004002:'E_NOINTERFACE', 0x80070057:'E_INVALIDARG',
            0x80004005:'E_FAIL', 0x887A0001:'DXGI_ERROR_INVALID_CALL',
            0x887A0004:'DXGI_ERROR_UNSUPPORTED', 0x887A002D:'DXGI_ERROR_NOT_CURRENTLY_AVAILABLE',
            0x887A0027:'DXGI_ERROR_NOT_FOUND'}.get(hr, '0x%08X' % hr)

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

d3d12 = ctypes.WinDLL('d3d12', use_last_error=True)
d3d12.D3D12CreateDevice.restype = ctypes.c_long
d3d12.D3D12CreateDevice.argtypes = [wt.LPVOID, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)]

def guid(s):
    return (ctypes.c_char*16).from_buffer_copy(uuid.UUID(s).bytes_le)

IID_ID3D12Device = guid('189819f1-1ec6-4bdc-ab4e-829f5d5f8ce4')
IID_ID3D12CommandQueue = guid('0ec870a6-7443-41ff-8cfc-11f9f08f2b4f')

device = wt.LPVOID()
hr = d3d12.D3D12CreateDevice(None, 0xc000, IID_ID3D12Device, ctypes.byref(device))
print('D3D12CreateDevice hr =', hr_str(hr), 'device =', hex(device.value or 0))
if hr != 0: sys.exit(3)

# ID3D12Device vtable: 0-2 IUnknown, 3 GetNodeCount, 4 CreateCommandQueue, 5 CreateCommandAllocator
dvt = ctypes.cast(device, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

class D3D12_COMMAND_QUEUE_DESC(ctypes.Structure):
    _fields_ = [('Type', u32), ('Priority', ctypes.c_int), ('Flags', u32), ('NodeMask', u32)]

create_cq = ctypes.cast(dvt[4], ctypes.WINFUNCTYPE(
    ctypes.c_long, wt.LPVOID, ctypes.POINTER(D3D12_COMMAND_QUEUE_DESC),
    ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)))
cqdesc = D3D12_COMMAND_QUEUE_DESC(0, 0, 0, 0)
queue = wt.LPVOID()
hr = create_cq(device, ctypes.byref(cqdesc), IID_ID3D12CommandQueue, ctypes.byref(queue))
print('CreateCommandQueue hr =', hr_str(hr), 'queue =', hex(queue.value or 0))
if hr != 0: sys.exit(4)

# DXGI factory2
dxgi = ctypes.WinDLL('dxgi', use_last_error=True)
dxgi.CreateDXGIFactory2.restype = ctypes.c_long
dxgi.CreateDXGIFactory2.argtypes = [u32, ctypes.POINTER(ctypes.c_char*16), ctypes.POINTER(wt.LPVOID)]
IID_IDXGIFactory2 = guid('50c83a1c-e072-4c48-87b0-3630fa36a6d0')
factory = wt.LPVOID()
hr = dxgi.CreateDXGIFactory2(0, IID_IDXGIFactory2, ctypes.byref(factory))
print('CreateDXGIFactory2 hr =', hr_str(hr), 'factory =', hex(factory.value or 0))
if hr != 0: sys.exit(5)

fvt = ctypes.cast(factory, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
# IDXGIFactory2 vtable: 0-2 IUnknown; 3 EnumAdapters; 4 MakeWindowAssociation; 5 GetWindowAssociation;
# 6 CreateSwapChain; 7 CreateSoftwareAdapter; 8 EnumAdapters1; 9 IsCurrent;
# 10 IsWindowedStereoEnabled; 11 CreateSwapChainForHwnd
create_sc_hwnd = ctypes.cast(fvt[11], ctypes.WINFUNCTYPE(
    ctypes.c_long, wt.LPVOID, wt.LPVOID, wt.HWND,
    wt.LPVOID, wt.LPVOID, wt.LPVOID, ctypes.POINTER(wt.LPVOID)))

class DXGI_RATIONAL(ctypes.Structure):
    _fields_ = [('Numerator', u32), ('Denominator', u32)]
class DXGI_SAMPLE_DESC(ctypes.Structure):
    _fields_ = [('Count', u32), ('Quality', u32)]
class DXGI_SWAP_CHAIN_DESC1(ctypes.Structure):
    _fields_ = [('Width', u32), ('Height', u32), ('Format', ctypes.c_int), ('Stereo', ctypes.c_int),
                ('SampleDesc', DXGI_SAMPLE_DESC), ('BufferUsage', u32), ('BufferCount', u32),
                ('Scaling', u32), ('SwapEffect', u32), ('AlphaMode', u32), ('Flags', u32)]

scdesc = DXGI_SWAP_CHAIN_DESC1()
scdesc.Width = 320; scdesc.Height = 180; scdesc.Format = 28  # R8G8B8A8_UNORM
scdesc.SampleDesc.Count = 1
scdesc.BufferUsage = 0x20
scdesc.BufferCount = 2
scdesc.SwapEffect = 4   # FLIP_DISCARD
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
