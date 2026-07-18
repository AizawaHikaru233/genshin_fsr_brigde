# Third-party build dependencies

The production FSR2 translation build carries the stable public FSR2 C ABI headers and Microsoft Detours build artifacts locally. This prevents normal Bridge builds from depending on an OptiScaler source checkout or its directory layout.

- `fsr2/`: AMD FidelityFX FSR2 public API headers used to define the exported compatibility ABI.
- `detours/`: Microsoft Detours headers and x64 import library used for the narrow `GetProcAddress` interception.

These files were copied from the OptiScaler 0.9.x source dependency snapshot already present in this workspace. They retain their original upstream notices.
