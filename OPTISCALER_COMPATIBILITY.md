# OptiScaler compatibility contract

The production Bridge does not import, link, locate, or call `OptiScaler.dll` directly. Runtime integration uses only the public FSR2 executable-export discovery contract:

- `ffxFsr2ContextCreate`
- `ffxFsr2ContextDispatch`
- `ffxFsr2ContextDestroy`
- `ffxFsr2GetUpscaleRatioFromQualityMode`
- `ffxFsr2GetRenderResolutionFromQualityMode`
- `ffxFsr2GetJitterPhaseCount`

The Bridge is loaded as a normal DLL by an external loader. It must finish initialization before `OptiScaler.dll` is loaded, because OptiScaler performs its DX11 FSR2 executable scan only during startup. The Bridge redirects only these six main-executable `GetProcAddress` requests. Backend selection, FSR3/FSR4 provider loading, FFX runtime DLL versions, and OptiScaler configuration remain owned by OptiScaler.

Diagnostic builds can confirm the discovery contract with:

```text
fsr2_get_proc_address_shim_queries mask=0x3F
fsr2_translation_context_created ... detoured=1
```

Release builds intentionally filter these success messages and may leave the Bridge log empty when no error occurs. This contract was checked against the official OptiScaler 0.9.3 source and binary release, which is the automatic-download baseline for the non-frame-generation package. The external loader remains responsible for the order: `Dx11FsrBridge.dll` first, `OptiScaler.dll` second.
