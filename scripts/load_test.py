#!/usr/bin/env python3
"""Controlled loader for dynamic testing of version.dll proxy (x64).

Usage: python load_test.py <dll_path> [hold_seconds]
Loads the DLL via LoadLibraryW, holds, then frees. Prints module path.
"""
import ctypes, ctypes.wintypes as wt, os, sys, time

k = ctypes.WinDLL('kernel32', use_last_error=True)
k.LoadLibraryW.argtypes = [wt.LPCWSTR]
k.LoadLibraryW.restype = wt.HMODULE
k.GetModuleFileNameW.argtypes = [wt.HMODULE, wt.LPWSTR, wt.DWORD]
k.GetModuleFileNameW.restype = wt.DWORD
k.FreeLibrary.argtypes = [wt.HMODULE]
k.FreeLibrary.restype = wt.BOOL

path = os.path.abspath(sys.argv[1])
hold = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

print('PID', os.getpid())
print('loading:', path)
h = k.LoadLibraryW(path)
err = ctypes.get_last_error()
print('LoadLibraryW -> 0x%x (err %d)' % (h or 0, err))
if h:
    buf = ctypes.create_unicode_buffer(1024)
    n = k.GetModuleFileNameW(h, buf, 1024)
    print('module path:', buf.value[:n])
    print('holding %.1fs...' % hold)
    time.sleep(hold)
    print('FreeLibrary ->', k.FreeLibrary(h))
else:
    sys.exit(1)
print('loader done')
