#!/usr/bin/env python3
"""Verify the proxy's version.dll exports forward transparently to the real
system version.dll. Loads proxy by full path, then calls GetFileVersionInfoSizeW/
GetFileVersionInfoW/VerQueryValueW on notepad.exe and compares to direct system call.
"""
import ctypes, ctypes.wintypes as wt, os, sys

u32 = ctypes.c_uint32
LPVOID = ctypes.c_void_p

k = ctypes.WinDLL('kernel32', use_last_error=True)
k.LoadLibraryW.argtypes = [wt.LPCWSTR]; k.LoadLibraryW.restype = wt.HMODULE
k.FreeLibrary.argtypes = [wt.HMODULE]

def call_via(hmod, target):
    k.GetProcAddress.argtypes = [wt.HMODULE, wt.LPCSTR]; k.GetProcAddress.restype = LPVOID
    fn_size = k.GetProcAddress(hmod, b'GetFileVersionInfoSizeW')
    fn_info = k.GetProcAddress(hmod, b'GetFileVersionInfoW')
    fn_query = k.GetProcAddress(hmod, b'VerQueryValueW')
    if not fn_size or not fn_info or not fn_query:
        return None
    size_fn = ctypes.WINFUNCTYPE(u32, wt.LPCWSTR, LPVOID)(fn_size)
    info_fn = ctypes.WINFUNCTYPE(ctypes.c_long, wt.LPCWSTR, u32, LPVOID, LPVOID)(fn_info)
    query_fn = ctypes.WINFUNCTYPE(ctypes.c_long, LPVOID, wt.LPCWSTR, LPVOID, ctypes.POINTER(u32))(fn_query)

    path = 'C:\\Windows\\System32\\notepad.exe'
    sz = size_fn(path, None)
    if sz == 0:
        return 'GetFileVersionInfoSizeW=0 err=%d' % ctypes.get_last_error()
    buf = ctypes.create_string_buffer(sz)
    if info_fn(path, 0, sz, buf) == 0:
        return 'GetFileVersionInfoW failed err=%d' % ctypes.get_last_error()
    # VerQueryValueW("\\StringFileInfo\\%s\\FileVersion")
    out = LPVOID()
    outlen = u32()
    # find translation to build path
    q1 = ctypes.WINFUNCTYPE(ctypes.c_long, LPVOID, wt.LPCWSTR, LPVOID, ctypes.POINTER(u32))(query_fn)
    if q1(buf, '\\\\VarFileInfo\\\\Translation', ctypes.byref(out), ctypes.byref(outlen)) == 0:
        return 'no translation'
    lang = ctypes.cast(out, ctypes.POINTER(u32)).contents.value
    hexlang = '%04x%04x' % ((lang & 0xFFFF), (lang >> 16))
    sub = '\\\\StringFileInfo\\\\%s\\\\FileVersion' % hexlang
    if query_fn(buf, sub, ctypes.byref(out), ctypes.byref(outlen)) == 0:
        return 'no FileVersion'
    ver = ctypes.wstring_at(out.value, outlen.value // 2)
    return ver

proxy_path = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else None
system = k.LoadLibraryW('C:\\Windows\\System32\\version.dll')
print('system version.dll handle 0x%x' % (system or 0))
print('direct system call FileVersion:', call_via(system, 'C:\\Windows\\System32\\notepad.exe'))
k.FreeLibrary(system)

if proxy_path:
    ph = k.LoadLibraryW(proxy_path)
    print('proxy version.dll handle 0x%x' % (ph or 0))
    if ph:
        print('via proxy FileVersion:     ', call_via(ph, proxy_path))
        # also check the 17 exports resolve to real code
        for exp in (b'GetFileVersionInfoA', b'GetFileVersionInfoW', b'GetFileVersionInfoExW',
                    b'GetFileVersionInfoSizeW', b'VerQueryValueW', b'VerFindFileW', b'VerLanguageNameW'):
            p = k.GetProcAddress(ph, exp)
            print('  %-28s -> 0x%x' % (exp.decode(), p or 0))
        k.FreeLibrary(ph)
