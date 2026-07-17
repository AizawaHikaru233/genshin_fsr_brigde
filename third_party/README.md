# Third-party build dependencies

The production FSR2 translation build carries the stable public FSR2 C ABI headers and Microsoft Detours build artifacts locally. This prevents normal Bridge builds from depending on an OptiScaler source checkout or its directory layout.

- `fsr2/`: the AMD FidelityFX FSR2 core public ABI headers referenced by the translation layer. Backend-specific DX11, DX12, and Vulkan headers are intentionally omitted because this Bridge does not compile an AMD backend.
- `detours/`: Microsoft Detours headers and x64 import library used for the narrow `GetProcAddress` interception.

The FSR2 headers come from the AMD FidelityFX public API snapshot used by the validated OptiScaler 0.9.3 baseline. The Detours files come from Microsoft Detours. They retain their original upstream notices.
