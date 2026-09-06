#!/usr/bin/env python3
"""Trigger the proxy's Present hook in-process: create a real DXGI swapchain via
D3D11CreateDeviceAndSwapChain, then Present a few times. Observe _dlssnr_on_amd.log.
"""
import ctypes, ctypes.wintypes as wt, os, sys, time
from ctypes import wintypes

u32 = ctypes.c_uint32

# load the proxy DLL first so its hooks are installed in this process
if len(sys.argv) > 1:
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.LoadLibraryW.argtypes = [wt.LPCWSTR]
    k.LoadLibraryW.restype = wt.HMODULE
    h = k.LoadLibraryW(os.path.abspath(sys.argv[1]))
    print('proxy LoadLibraryW -> 0x%x (err %d)' % (h or 0, ctypes.get_last_error()))

class RECT(ctypes.Structure):
    _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                ('right', ctypes.c_long), ('bottom', ctypes.c_long)]

class DXGI_RATIONAL(ctypes.Structure):
    _fields_ = [('Numerator', u32), ('Denominator', u32)]

class DXGI_MODE_DESC(ctypes.Structure):
    _fields_ = [('Width', u32), ('Height', u32), ('RefreshRate', DXGI_RATIONAL),
                ('Format', ctypes.c_int), ('ScanlineOrdering', u32), ('Scaling', u32)]

class DXGI_SWAP_CHAIN_DESC(ctypes.Structure):
    _fields_ = [('BufferDesc', DXGI_MODE_DESC), ('SampleDesc', DXGI_RATIONAL),
                ('BufferUsage', u32), ('BufferCount', u32), ('OutputWindow', wt.HWND),
                ('Windowed', ctypes.c_int), ('SwapEffect', u32), ('Flags', u32)]

user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.CreateWindowExA.restype = wt.HWND
user32.CreateWindowExA.argtypes = [u32, wt.LPCSTR, wt.LPCSTR, u32, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMODULE, wt.HINSTANCE, wt.LPVOID]
user32.DestroyWindow.argtypes = [wt.HWND]

d3d11 = ctypes.WinDLL('d3d11', use_last_error=True)
d3d11.D3D11CreateDeviceAndSwapChain.restype = ctypes.c_long
d3d11.D3D11CreateDeviceAndSwapChain.argtypes = [
    wt.LPVOID,                      # pAdapter
    ctypes.c_int,                   # DriverType
    wt.HMODULE,                     # Software
    u32,                            # Flags
    wt.LPVOID,                      # pFeatureLevels
    u32,                            # FeatureLevels
    u32,                            # SDKVersion
    ctypes.POINTER(DXGI_SWAP_CHAIN_DESC),  # pSwapChainDesc
    ctypes.POINTER(wt.LPVOID),      # ppSwapChain
    ctypes.POINTER(wt.LPVOID),      # ppDevice
    ctypes.POINTER(u32),            # pFeatureLevel
    ctypes.POINTER(wt.LPVOID),      # ppImmediateContext
]

hwnd = user32.CreateWindowExA(0, b'STATIC', b'dlssnr_test', 0, 0, 0, 320, 180,
                              None, None, None, None)
print('hwnd =', hex(hwnd or 0))
if not hwnd:
    print('CreateWindowExA failed:', ctypes.get_last_error()); sys.exit(2)

desc = DXGI_SWAP_CHAIN_DESC()
desc.BufferDesc.Width = 320
desc.BufferDesc.Height = 180
desc.BufferDesc.RefreshRate.Numerator = 60
desc.BufferDesc.RefreshRate.Denominator = 1
desc.BufferDesc.Format = 28          # DXGI_FORMAT_R8G8B8A8_UNORM
desc.SampleDesc.Numerator = 1
desc.SampleDesc.Denominator = 1
desc.BufferUsage = 0x20              # DXGI_USAGE_RENDER_TARGET_OUTPUT
desc.BufferCount = 1
desc.OutputWindow = hwnd
desc.Windowed = 1
desc.SwapEffect = 0                  # DXGI_SWAP_EFFECT_DISCARD

print('sizeof(DXGI_SWAP_CHAIN_DESC) =', ctypes.sizeof(desc))

swapchain = wt.LPVOID()
device = wt.LPVOID()
ctx = wt.LPVOID()
feature_level = u32()
# explicit feature level array: D3D_FEATURE_LEVEL_11_0 = 0xb000
levels = (u32 * 1)(0xb000)
hr = d3d11.D3D11CreateDeviceAndSwapChain(
    None, 1, None, 0, levels, 1, 7,
    ctypes.byref(desc), ctypes.byref(swapchain),
    ctypes.byref(device), ctypes.byref(feature_level), ctypes.byref(ctx))
print('D3D11CreateDeviceAndSwapChain hr = 0x%08x' % (hr & 0xFFFFFFFF))
if hr != 0:
    print('FAILED'); user32.DestroyWindow(hwnd); sys.exit(3)
print('swapchain =', hex(swapchain.value or 0))

# IDXGISwapChain vtable: [0]=QueryInterface [1]=AddRef [2]=Release [3]=Present
vtbl = ctypes.cast(swapchain, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
present = ctypes.cast(vtbl[3], ctypes.WINFUNCTYPE(ctypes.c_long, wt.LPVOID, u32, u32))
print('Present fn =', hex(vtbl[3] or 0))
for i in range(3):
    hr = present(swapchain, 0, 0)
    print('Present #%d hr=0x%08x' % (i, hr & 0xFFFFFFFF))
    time.sleep(1.0)

# release swapchain (vtable[2]=Release)
release = ctypes.cast(vtbl[2], ctypes.WINFUNCTYPE(ctypes.c_ulong, wt.LPVOID))
print('Release swapchain ->', release(swapchain))
user32.DestroyWindow(hwnd)
print('done')
