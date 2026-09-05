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
mods_dir =                      ; Mods 根目录；留空 = DLL 同目录 Mods（自动创建空的）
observe_only = 0                ; 1=只记录哈希匹配不替换；0=真正替换（默认）
log_level = 1                   ; 0=仅关键 1=常规（默认） 2=详细（每次运行覆盖上次日志）
vram_threshold = 15%            ; 显存压力阈值：15% / 1024M / 2G，非法回退 15%
max_texture_side = 0            ; 替换纹理最大边长；0=不限制（默认）
gdds_enabled = 1                ; 1=启用 GDDS（DirectStorage GPU 解压，默认）；0=禁用
async_load = 0                  ; 0=同步加载（默认，渲染线程建纹理）；1=异步（后台线程）
```

> **加载方式说明**：默认**同步加载**（渲染线程创建纹理，3DMigoto 同模型）——
> 兼容性最好；部分 NVIDIA 驱动（实测 RTX 4060 Laptop + 566.64）对"后台线程
> 创建 D3D11 纹理"的跨线程模式有缺陷（驱动工作线程崩溃/卡死），异步模式
> 下会触发。AMD 卡无此问题，可设 `async_load = 1` 换取切角色更流畅。

## 架构

- **统一加载器注册表**（`dds_loader.h`）：`TextureLoadResult` 统一结果类型 +
  `TextureLoaderEntry{extension, load}` 注册表；入队时按扩展名选定加载器
  （`.gdds` → DirectStorage GPU 解压；`.dds`/默认 → CPU 加载），替代运行时 if/else。
- **双队列双线程（异步模式）**：GDDS 与 DDS 各自独立消费队列——大 GDDS 任务
  （8-128MB GPU 解压）不阻塞 DDS 小纹理；登记/淘汰共享 `g_lock`。默认同步模式
  （`async_load=0`）绕过队列，渲染线程直接执行加载。
- **GDDS 失败终态隔离**：DirectStorage 初始化失败后 `Failed()` 置位，GDDS 任务不再
  入队（避免队列堆积/日志刷屏）；DDS 路径完全不受影响。
- **热路径两级无锁判定**：绑定热路径先比替换 SRV 指针（命中=无需处理，省 GetResource
  COM 调用），未命中才查原纹理活跃数组——替换视图反复绑定场景接近零开销。
- **渲染线程延迟释放**：非渲染线程不直接 Release D3D11 对象，规避 AMD 驱动 UAF。
- **替换缓存全局持久化**（3DMigoto 同策略）：原纹理销毁只减引用计数，不释放
  替换纹理/SRV——同 hash 再创建直接复用缓存（免重载、免释放，规避 NVIDIA 驱动
  释放路径缺陷）；显存压力时 VramMonitor 淘汰（释放资源保留条目，命中重载），
  进程退出时统一释放。
- **两级显存回收**：**常态持续回收**——每次巡检按"上次使用时间"排序淘汰闲置
  缓存（refcount==0，绝不碰使用中；加载即视为活动防抖动）；空闲阈值随剩余
  显存收紧（充足 60 秒 → 逼近阈值 15 秒，强度 4→16 条/轮）。剩余 < 阈值
  （默认 15%）时**紧急兜底**按缺口自适应回收（物理显存基准，大纹理优先）。

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
