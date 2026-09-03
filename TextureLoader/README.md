# TextureLoader

独立轻量 **D3D11 纹理替换 DLL**（GPL-3.0-or-later）。用 3DMigoto 兼容哈希匹配
`[TextureOverride] hash=` + `[Resource] filename=` 映射，把游戏运行时创建的纹理
替换为 Mod 提供的 **DDS / GDDS** 贴图。设计为与已 hook 渲染链的插件共存
（ReShade、Dx11FsrBridge/FSR 等）。

## 功能

- **3DMigoto 兼容哈希**：`texture_hash.cpp` 逐字移植 bo3b/3Dmigoto
  `DirectX11/ResourceHash.cpp`（GPL-3.0），与 mod 作者预生成的 `hash=` 完全一致。
- **DDS 加载**（CPU 路径）：DDS + DX10 头解析，BC1-BC7 / unorm / mip / 数组。
- **GDDS 加载**（GPU 路径）：DirectStorage GPU GDeflate 解压直写共享纹理
  （D3D12 同适配器别名 + 共享 fence 同步），多子流逐请求，支持 8192/16384 大纹理。
- **异步加载**：后台线程消费加载队列，渲染线程不阻塞于磁盘 IO。
- **显存容量驱动淘汰**：可用显存低于阈值（默认 15%）时按大小优先 + LRU 淘汰
  已销毁原纹理的缓存；未达阈值不清理。
- **渲染线程延迟释放**：非渲染线程不直接 Release D3D11 对象，规避驱动 UAF。

## 构建

依赖：Visual Studio（C++ 桌面工作负载）、CMake、Ninja。
- **Detours**（MIT）：取自同仓库 `Dx11FsrBridge/third_party/detours`（构建时按
  `../Dx11FsrBridge/third_party/detours` 相对路径引用）。
- **DirectStorage SDK**：仓库内 `third_party/dstorage/` 已含头文件（MIT + MS 条款）。

```powershell
# 方式一：自动定位 VS（vswhere）+ 配置 + 构建
powershell -ExecutionPolicy Bypass -File .\build.ps1

# 方式二：手动
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

产物：`build/TextureLoader.dll`（约 180 KB）。

> 说明：CMake 不链接 `dstorage.lib`（GDDS 运行时经 `LoadLibraryW` 动态加载）；
> SDK 二进制（`dstorage.dll`/`dstoragecore.dll`）不进入 git，构建时若存在则自动
> 复制到输出目录，缺失仅告警——GDDS 功能需运行时自行提供这两个 DLL。

## 配置（TextureLoader.ini，放 DLL 同目录）

```ini
mods_dir = C:\path\to\your\Mods   ; 递归扫描 mod ini 的根目录
observe_only = 0                  ; 1=只记录哈希匹配不替换；0=真正替换
log_level = 1                     ; 0=仅关键日志 1=常规 2=详细
vram_threshold = 15%              ; 显存压力阈值：15% / 1024M / 2G，非法回退 15%
max_texture_side = 0              ; 替换纹理最大边长；0=不限制
```

## 目录结构

```
TextureLoader.cpp   — 主逻辑：hook、替换链路、异步加载、显存淘汰
dds_loader.cpp/.h   — DDS + DX10 头解析（自研）
gdds_interop.cpp/.h — GDDS DirectStorage GPU 解压互操作
texture_hash.cpp/.h — 3DMigoto 兼容哈希（GPL-3.0 移植）
mod_ini.cpp/.h      — [TextureOverride]/[Resource] ini 解析（自研）
crc32c/             — 硬件 CRC-32C（Mark Adler / Robert Vazan，zlib 许可）
log.cpp/.h          — 日志
third_party/dstorage/ — DirectStorage SDK 头文件与许可证（MIT + MS 条款）
```

## 许可证

- 本项目：**GPL-3.0-or-later**（见 `LICENSE.GPL.txt`）。
- 纹理哈希算法：移植自 bo3b/3Dmigoto `ResourceHash.cpp`（GPL-3.0），
  [来源仓库](https://github.com/bo3b/3Dmigoto)。3Dmigoto 作者：Chiri、Bo3b Johnson、
  Ian Munsie（AKA DarkStarSword）等（详见上游仓库 AUTHORS）。
- CRC-32C：Mark Adler / Robert Vazan 的 `crc32c-hw-1.0.5`（zlib 风格，见文件头）。
- DirectStorage SDK：Microsoft（MIT + MS 软件许可条款，见 `third_party/dstorage/`）。
