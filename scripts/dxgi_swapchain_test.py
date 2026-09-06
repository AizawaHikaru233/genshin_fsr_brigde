#!/usr/bin/env python3
"""Isolated DXGI swapchain creation test against the proxy's vtable detours.
Loads version.dll (hooks), creates a D3D11 device, then IDXGIFactory::CreateSwapChain
directly, and Present()s a few times. Prints HRESULTs; logs go to _dlssnr_on_amd.log.
"""
import ctypes, ctypes.wintypes as wt, os, sys, time

u32 = ctypes.c_uint32

def hr_str(hr):
    hr = hr & 0xFFFFFFFF
    if hr == 0: return 'S_OK'
    if hr == 0x887A0001: return 'DXGI_ERROR_INVALID_CALL'
    if hr == 0x887A002D: return 'DXGI_ERROR_NOT_CURRENTLY_AVAILABLE'
    if hr == 0x80070057: return 'E_INVALIDARG'
    if hr == 0x80004005: return 'E_FAIL'
    if hr == 0x887A0004: return 'DXGI_ERROR_UNSUPPORTED'
    if hr == 0x887A0005: return 'DXGI_ERROR_NOT_FOUND'
    if hr == 0x8007000E: return 'E_OUTOFMEMORY'
    return '0x%08X' % hr

if len(sys.argv) > 1:
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.LoadLibraryW.argtypes = [wt.LPCWSTR]
    k.LoadLibraryW.restype = wt.HMODULE
    h = k.LoadLibraryW(os.path.abspath(sys.argv[1]))
    print('proxy LoadLibraryW -> 0x%x (err %d)' % (h or 0, ctypes.get_last_error()))

user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.CreateWindowExA.restype = wt.HWND
user32.CreateWindowExA.argtypes = [u32, wt.LPCSTR, wt.LPCSTR, u32, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMODULE, wt.HINSTANCE, wt.LPVOID]
user32.DestroyWindow.argtypes = [wt.HWND]
user32.GetMessageA.argtypes = [wt.LPVOID, wt.HWND, u32, u32]

hwnd = user32.CreateWindowExA(0, b'STATIC', b'dlssnr_test', 0, 0, 0, 320, 180,
                              None, None, None, None)
print('hwnd =', hex(hwnd or 0))

# --- 1) D3D11CreateDevice alone ---
d3d11 = ctypes.WinDLL('d3d11', use_last_error=True)
d3d11.D3D11CreateDevice.restype = ctypes.c_long
d3d11.D3D11CreateDevice.argtypes = [wt.LPVOID, ctypes.c_int, wt.HMODULE, u32,
                                    wt.LPVOID, u32, u32,
                                    ctypes.POINTER(wt.LPVOID), ctypes.POINTER(u32),
                                    ctypes.POINTER(wt.LPVOID)]
device = wt.LPVOID()
ctx = wt.LPVOID()
fl = u32()
levels = (u32 * 1)(0xb000)
hr = d3d11.D3D11CreateDevice(None, 1, None, 0, levels, 1, 7,
                             ctypes.byref(device), ctypes.byref(fl), ctypes.byref(ctx))
print('D3D11CreateDevice hr =', hr_str(hr), 'device =', hex(device.value or 0))
if hr != 0:
    user32.DestroyWindow(hwnd); sys.exit(3)

# --- 2) CreateDXGIFactory1 -> IDXGIFactory vtable ---
dxgi = ctypes.WinDLL('dxgi', use_last_error=True)
dxgi.CreateDXGIFactory1.restype = ctypes.c_long
dxgi.CreateDXGIFactory1.argtypes = [ctypes.POINTER(ctypes.c_char * 16), ctypes.POINTER(wt.LPVOID)]
import uuid
IID_IDXGIFactory1 = uuid.UUID('770aae78-f26f-4dba-a829-253c83d1b387')
iid = (ctypes.c_char * 16).from_buffer_copy(IID_IDXGIFactory1.bytes_le)
factory = wt.LPVOID()
hr = dxgi.CreateDXGIFactory1(iid, ctypes.byref(factory))
print('CreateDXGIFactory1 hr =', hr_str(hr), 'factory =', hex(factory.value or 0))
if hr != 0:
    user32.DestroyWindow(hwnd); sys.exit(4)

fvtbl = ctypes.cast(factory, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
# IDXGIFactory vtable: 0-2 IUnknown, 3 EnumAdapters, 4 MakeWindowAssociation,
# 5 GetWindowAssociation, 6 CreateSwapChain, 7 CreateSoftwareAdapter
create_swapchain = ctypes.cast(fvtbl[10], ctypes.WINFUNCTYPE(
    ctypes.c_long, wt.LPVOID, wt.LPVOID, wt.LPVOID, ctypes.POINTER(wt.LPVOID)))

class DXGI_RATIONAL(ctypes.Structure):
    _fields_ = [('Numerator', u32), ('Denominator', u32)]

class DXGI_MODE_DESC(ctypes.Structure):
    _fields_ = [('Width', u32), ('Height', u32), ('RefreshRate', DXGI_RATIONAL),
                ('Format', ctypes.c_int), ('ScanlineOrdering', u32), ('Scaling', u32)]

class DXGI_SAMPLE_DESC(ctypes.Structure):
    _fields_ = [('Count', u32), ('Quality', u32)]

class DXGI_SWAP_CHAIN_DESC(ctypes.Structure):
    _fields_ = [('BufferDesc', DXGI_MODE_DESC), ('SampleDesc', DXGI_SAMPLE_DESC),
                ('BufferUsage', u32), ('BufferCount', u32), ('OutputWindow', wt.HWND),
                ('Windowed', ctypes.c_int), ('SwapEffect', u32), ('Flags', u32)]

for effect, name in ((0, 'DISCARD'), (1, 'SEQUENTIAL'), (2, 'FLIP_SEQUENTIAL'), (4, 'FLIP_DISCARD')):
    for bcount in (1, 2):
        desc = DXGI_SWAP_CHAIN_DESC()
        desc.BufferDesc.Width = 320
        desc.BufferDesc.Height = 180
        desc.BufferDesc.RefreshRate.Numerator = 60
        desc.BufferDesc.RefreshRate.Denominator = 1
        desc.BufferDesc.Format = 28
        desc.SampleDesc.Count = 1
        desc.BufferUsage = 0x20
        desc.BufferCount = bcount
        desc.OutputWindow = hwnd
        desc.Windowed = 1
        desc.SwapEffect = effect
        sc = wt.LPVOID()
        hr = create_swapchain(factory, device, ctypes.byref(desc), ctypes.byref(sc))
        print('CreateSwapChain(%s, buf=%d) hr = %s swapchain = %s' % (name, bcount, hr_str(hr), hex(sc.value or 0)))
        if hr == 0:
            svtbl = ctypes.cast(sc, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            present = ctypes.cast(svtbl[7], ctypes.WINFUNCTYPE(ctypes.c_long, wt.LPVOID, u32, u32))
            for i in range(3):
                phr = present(sc, 0, 0)
                print('  Present #%d hr=%s' % (i, hr_str(phr)))
                time.sleep(0.5)
            release = ctypes.cast(svtbl[2], ctypes.WINFUNCTYPE(ctypes.c_ulong, wt.LPVOID))
            release(sc)
            break  # first success is enough
    if hr == 0:
        break

user32.DestroyWindow(hwnd)
print('done')
