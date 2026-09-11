# TextureLoader

独立轻量 **D3D11 纹理替换 DLL**（GPL-3.0-or-later）。用 3DMigoto 兼容哈希匹配
`[TextureOverride] hash=` + `[Resource] filename=` 映射，把游戏运行时创建的纹理
替换为 Mod 提供的 **DDS / GDDS** 贴图。设计为与已 hook 渲染链的插件共存
（ReShade、Dx11FsrBridge/FSR 等）。

---

## ⚠️ NVIDIA 显卡不支持（重要）

**本组件在 NVIDIA 显卡上会出现无法修复的纹理加载严重错误。因此它仅推荐 A 卡用户使用，
且默认仅对 A 卡开放启用。**

作者没有 NVIDIA 显卡，只能依靠 QQ 群群友协助反复测试，始终无法定位根因。已排除的因素：

| 排查项 | 结论 |
| --- | --- |
| mod 贴图文件本身 | 3139 个 DDS 全量校验：均为合法 `BC3_UNORM_SRGB`、`mipMapCount=1`，无损坏 |
| 格式 / SRV 视图处理 | N 卡日志的每个字段与 A 卡逐项一致，且 `hr=0`、零兜底回退 |
| 哈希匹配 / ini 覆盖 | 3139 条 `hash=` 全部唯一，无同名文件、无重复定义 |
| 线程路径 | 两机线程拓扑同构（创建钩子与绑定钩子同线程） |
| 跨机文件差异 | 同源文件，FNV 指纹在两机日志中逐位一致 |
| alpha 通道内容 | 全量画像与"哪张贴图异常"无关联 |
| 初始数据缓冲被后继加载复用 | 缺陷真实存在并已修复，但 N 卡画面**仍异常** ⇒ 非充分原因 |

唯一无法在本地复现的环节是 **N 卡驱动的纹理创建 / 上载时机**。

### 发布渠道的处理方式

- **芙芙启动器插件包**与 **GitHub 发布包**在检测到 NVIDIA 显卡时，会**直接隐藏并停用本组件的
  配置项与安装项**：不显示开关、不询问、不允许启用；即使 ini 中残留 `EnableTextureLoader=1`
  也不会加载本 DLL（插件侧有强制停用兜底）。
- **想使用的 N 卡用户**：欢迎自行拉取本仓库源码修复后提交合并请求（PR）。
- **赞助作者一张 NVIDIA 显卡**，作者会尝试定位并修复。

> A 卡用户不受影响，可正常启用本组件。

---

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
vram_threshold =                ; 显存危急阈值；留空=按显存容量自适应（4GB→20%...24GB→13%）
max_texture_side = 0            ; 替换纹理最大边长；0=不限制（默认）
gdds_enabled = 1                ; 1=启用 GDDS（DirectStorage GPU 解压，默认）；0=禁用
async_load =                    ; 留空=自动（NVIDIA→同步，AMD/Intel→异步）；0=强制同步；1=强制异步
```

> **加载方式说明**：`async_load` 留空时按 GPU 厂商自动选择——NVIDIA 用**同步**
> （渲染线程建纹理，规避驱动对后台线程建纹理的缺陷，实测 RTX 4060 Laptop +
> 566.64 异步会崩溃/卡死）；AMD/Intel 用**异步**（后台线程加载，避免渲染线程
> 阻塞磁盘 IO/纹理创建导致 GPU 负载不满）。可显式设置 `0`/`1` 覆盖。

## 架构

- **统一加载器注册表**（`dds_loader.h`）：`TextureLoadResult` 统一结果类型 +
  `TextureLoaderEntry{extension, load}` 注册表；入队时按扩展名选定加载器
  （`.gdds` → DirectStorage GPU 解压；`.dds`/默认 → CPU 加载），替代运行时 if/else。
- **双队列双线程（异步模式）**：GDDS 与 DDS 各自独立消费队列——大 GDDS 任务
  （8-128MB GPU 解压）不阻塞 DDS 小纹理；登记/淘汰共享 `g_lock`。同步模式
  （`async_load=0`，NVIDIA 默认）绕过队列，渲染线程直接执行加载。
- **GDDS 失败终态隔离**：DirectStorage 初始化失败后 `Failed()` 置位，GDDS 任务不再
  入队（避免队列堆积/日志刷屏）；DDS 路径完全不受影响。
- **热路径两级无锁判定**：绑定热路径先比替换 SRV 指针（命中=无需处理，省 GetResource
  COM 调用），未命中才查原纹理活跃数组——替换视图反复绑定场景接近零开销。
- **渲染线程延迟释放**：非渲染线程不直接 Release D3D11 对象，规避 AMD 驱动 UAF。
- **替换缓存全局持久化**（3DMigoto 同策略）：原纹理销毁只减引用计数，不释放
  替换纹理/SRV——同 hash 再创建直接复用缓存（免重载、免释放，规避 NVIDIA 驱动
  释放路径缺陷）；显存压力时 VramMonitor 淘汰（释放资源保留条目，命中重载），
  进程退出时统一释放。
- **显存回收（v2 低开销）**：四分区状态机（舒适/温和/压力/危急）+ 压力系数 P
  连续驱动回收参数（闲置阈值 10min→30s、每轮 2→32 条、LRU↔大纹理优先渐进
  切换）。临界百分比随显存容量自适应（4GB→20%、16GB→15.8%、24GB→13%），
  `vram_threshold` 可显式覆盖。**闲置候选索引**（`std::set<{last_used,hash}>`
  增量维护，登记/归零/淘汰/豁免到期时 O(log n) 更新，绑定热路径零开销，
  巡检惰性修正）——每轮 O(K log n) 而非全表扫描；缓存总量原子维护 O(1) 读。
  动态探针频率按 zone 定频（5s→0.8s，变化立即响应）；热缓存豁免（淘汰后
  30s 内重载 → 5 分钟豁免）防抖动；缓存总量预算（总显存 20%）防膨胀；
  危急区缺口自适应兜底；使用中（refcount>0）永不回收。

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
