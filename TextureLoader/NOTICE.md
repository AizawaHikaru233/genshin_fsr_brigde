# TextureLoader — 第三方组件、许可证与参考来源声明 (NOTICE)

TextureLoader 是独立开发的轻量 D3D11 纹理替换 DLL，设计为与 ReShade、
Dx11FsrBridge 等已 hook 渲染链的插件共存（只 hook `CreateTexture2D` 与
`PSSetShaderResources` 两个点，不包装整个设备 vtable，也不作为 d3d11 proxy）。

本项目整体以 **GPL-3.0-or-later** 发布（见 `LICENSE.GPL.txt`）。

## 参考来源与代码溯源

| 组件 | 来源 | 许可证 | 说明 |
|---|---|---|---|
| 纹理哈希算法 | [bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) `DirectX11/ResourceHash.cpp` | GPL-3.0 | `texture_hash.cpp` 逐字移植（`CalcTexture2DDataHash`/`CalcTexture2DDescHash`/`hash_tex2d_data`/`Texture2DLength`/`GetSurfaceInfo`/`BitsPerPixel`），保证与 GIMI `texture_hash=0` 的哈希完全一致，从而匹配 mod 作者预生成的 `[TextureOverride] hash=` |
| CRC-32C (Castagnoli) | Mark Adler / Robert Vazan 的 `crc32c-hw-1.0.5`（3DMigoto 内嵌） | zlib (自定义 3 条款，见 `crc32c/crc32c.cpp` 头部) | 硬件加速 CRC-32C，`0x82f63b78` |
| DDS 格式布局 | DDS/DX10 公共格式规范 + DirectXTK 语义 | MIT (格式本身无版权) | `dds_loader.cpp` 从零编写，未链接 DirectXTK |
| ini 格式语义 | 3DMigoto `[TextureOverride]`/`[Resource]` 语法 | 格式兼容（自研解析） | `mod_ini.cpp` 从零编写 |
| 3DMigoto 原始源码快照 | [bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) | GPL-3.0 | 存放于 `3dmigoto-core/`，仅作参考与溯源，未参与构建 |

## 3DMigoto 作者 (AUTHORS.txt)

3Dmigoto 由 Chiri、Bo3b Johnson、Ian Munsie (AKA DarkStarSword) 等开发，
详见 `AUTHORS.txt` 与 `3dmigoto-core/AUTHORS.txt`。

## 构建依赖

- **Detours** (Microsoft Research)：`../Dx11FsrBridge/third_party/detours`，MIT。
- **DirectStorage SDK** (Microsoft)：已 vendor 至 `third_party/dstorage/`，MIT + MS 软件许可条款
  （见该目录 `LICENSE-CODE.txt` / `LICENSE.txt` / `NOTICES.txt`）；运行时
  `dstorage.dll`/`dstoragecore.dll` 动态加载（不静态链接）。
- Windows SDK 的 `d3d11.lib` / `dxgi.lib` / `shlwapi.lib`。

## 与 XXMI/GIMI 的关系

本项目**不包含、不链接、不复制** XXMI-Launcher 或 GIMI 的任何二进制或闭源代码。
它解析的 `[TextureOverride] hash=` 与 `[Resource] filename=` 是 3DMigoto 公开的
mod 格式，Texture++ 等 mod 作者按该格式发布映射表。哈希算法取自 GPL 的 3DMigoto，
已按 GPL-3.0 履行署名与许可证义务。
