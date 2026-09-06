# TextureLoader — 功能与架构文档

> 独立轻量 D3D11 纹理替换 DLL。用 3DMigoto 兼容哈希匹配
> `[TextureOverride] hash=` + `[Resource] filename=` 映射，把运行时创建的纹理
> 替换为 Mod 提供的 DDS / GDDS 贴图。设计为与已 hook 渲染链的插件共存
> （ReShade、Dx11FsrBridge/FSR 等）。

| 项 | 值 |
|---|---|
| 产物 | `TextureLoader.dll`（~180 KB，随宿主插件加载器 DllList 注入） |
| 许可证 | GPL-3.0-or-later（`LICENSE.GPL.txt` + `NOTICE.md` 完整溯源） |
| 构建 | CMake + Ninja（依赖系统 Windows SDK 与仓库内 `third_party/`） |
| 配置 | `TextureLoader.ini`（`mods_dir`、`observe_only`、`log_level`、`vram_threshold`、`max_texture_side`） |

### 核心链路

```
游戏创建纹理 → HookCreateTexture2D 算 3DMigoto 兼容哈希 → 命中 [TextureOverride]
→ 登记替换条目 + 异步后台加载 DDS/GDDS → 绑定时刻 SetShaderResources 热路径替换 SRV
→ 原纹理销毁时 ReplacementTracker 引用计数释放缓存
```

---

## 1. Hook 面（Detours；镜像 vtable 结构体按成员名访问，零硬编码槽位号）

| Hook | 作用 |
|---|---|
| `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` | 设备就绪时 `AttachToDevice`：挂 device/context vtable、启动后台线程 |
| `ID3D11Device::CreateTexture2D` | 对带初始数据的纹理计算 3DMigoto 兼容哈希；命中则登记替换（refcount++、挂 ReplacementTracker、登记活跃数组、丢异步加载队列） |
| `ID3D11DeviceContext::PSSetShaderResources` / `VS` / `GS` / `HS` / `DS` / `CS` | 六个阶段统一替换（飘带/布料在 VS/CS 采样，只换 PS 会导致阶段间数据不一致） |
| `ID3D11DeviceContext::UpdateSubresource` | 被替换后的纹理若再被更新 → 标记动态，绑定慢路径跳过（避免闪烁） |

## 2. 哈希算法（3DMigoto 兼容，逐字移植）

`texture_hash.cpp` 移植自 bo3b/3Dmigoto `DirectX11/ResourceHash.cpp`（GPL-3.0）：
- 数据哈希：`crc32c_hw`（Castagnoli `0x82f63b78`，硬件加速，zlib 许可）
- 描述哈希：`CalcTexture2DDescHash = crc32c_hw(data_hash, desc, sizeof(D3D11_TEXTURE2D_DESC))`
- `texture_hash=0`（GIMI 默认）→ v1.2.1 兼容路径：`length_v12 = W*H*ArraySize`，
  ≤ length 走 v1.2.1 全缓冲 CRC，否则 v1.2.11+ 跳行 padding

与 mod 作者预生成的 `hash=` 逐字一致，无需运行时学习。

## 3. 替换缓存与生命周期

- `g_replacements`（`unordered_map<hash, ReplacementEntry{texture, srv, refcount, last_used, mem_bytes}>`）
- **ReplacementTracker**（IUnknown，经 `SetPrivateDataInterface` 挂到原纹理）：原纹理销毁时
  D3D 回调 Release → refcount-- → 归零释放替换纹理/SRV。**AB-BA 死锁规避**：锁内只操作
  map 收集指针、锁外 Release D3D 资源。
- **活跃资源数组**（`g_activeArr[4096]` 紧凑数组，写互斥/读无锁）：替换命中时登记；
  原纹理销毁时移除（swap-remove）——绑定热路径线性扫存活条目（几十~几百项 ~20ns），
  替代 GetPrivateData COM 调用。

## 4. 性能与并发

| 优化 | 效果 |
|---|---|
| 极速通道：`g_activeCount==0` 时单条 volatile 读直接透传 | 未启用替换时零开销 |
| 紧凑活跃数组替代 `unordered_set` + 共享锁 | 消除 93 万次/秒绑定 × 锁的原子操作开销 |
| 异步 DDS/GDDS 加载（`AsyncLoadThread` 消费 `g_loadQueue`） | 切换角色掉帧大幅下降（渲染线程不阻塞于磁盘 IO + 建纹理） |
| 渲染线程延迟释放（`QueueRelease`/`FlushPendingRelease`） | 非渲染线程不直接 Release D3D11 对象——修复 AMD 驱动内 UAF（快速切换角色崩溃） |
| 显存容量驱动淘汰（`VramMonitorThread` 每 2s 查 `IDXGIAdapter3::QueryVideoMemoryInfo`） | 可用显存低于阈值（`vram_threshold`，默认 15%）时按**大小优先 + LRU** 淘汰 `refcount==0` 的缓存；未达阈值不清理 |

并发安全：`g_hasReplacement` 读写统一 `g_hitLock`（曾因 `unordered_set` 并发读写在高频切角色
时崩溃，已修复）；`g_replacements` 全量 `g_lock`；淘汰/加载/绑定三线程经引用计数安全互操作。

## 5. GDDS 加载（DirectStorage GPU 解压）

`.gdds` = **GDDS 魔数 + 标准 DDS/DX10 头 + GDDS 元数据 + GDeflate 压缩块**。
全链路 GPU 处理：DirectStorage 直接把压缩纹理送进显卡并完成 GDeflate 解压，不做 CPU 回读。

### 5.1 GDDS 多子流格式（作者加载器反汇编破解）

```
offset   0:  "GDDS" magic (4)
offset   4:  DDS_HEADER (124B)
offset 128:  DDS_HEADER_DXT10 (20B)
offset 148:  GDDS 元数据头
offset 180:  GDeflate TileStream 头 + tile 偏移表 + 压缩 tiles
```

- **关键差异**：文件含 **N 条独立 GDeflate TileStream**（未压缩每流 ≤8MB），
  流记录表在 `@124`（数量）与 `@148`（N×16B：{流偏移, 压缩大小, 未压缩大小}）。
  旧实现只加载第一条流导致 4096² 纹理只有一半数据（模糊/颜色异常）；
  现按**每条流独立 TEXTURE_REGION 请求**（单请求未压缩 ≤8MB，天然规避 DS 32MB
  单请求上限）→ 8192/16384 大纹理完整加载。

### 5.2 跨 API 同步（Phase 1 验证）

```
D3D11 共享纹理（SHARED|SHARED_NTHANDLE）
→ D3D12 OpenSharedHandle（同 GPU 别名）
→ DirectStorage 队列（qd.Device = D3D12 设备，必需）
→ TEXTURE_REGION 逐流请求 → GPU GDeflate 解压直写
→ 共享 fence（D3D12 Signal → D3D11 ID3D11DeviceContext4::Wait，渲染线程执行）
```

关键坑（均已解决）：
- 队列 `qd.Device` 必须设（否则 `E_DSTORAGE_INVALID_DESTINATION_TYPE`）
- `UncompressedSize` 按流未压缩大小；region 按累计未压缩字节 ÷ 行距换算像素行带
- DS 请求飞行期不释放 D3D12 别名（超时走僵尸保活，杜绝驱动 UAF）
- 非渲染线程释放 D3D11 对象一律入 `QueueRelease`，由渲染线程统一释放

### 5.3 降级

GPU 解压不可用（无 D3D12 / `dstorage.dll` 缺失 / 驱动过旧）→ 回退 CPU DDS 路径
（`.dds` 扩展名）；GDDS 依赖 DirectStorage，缺失时按失败记录日志。

## 6. 目录文件索引

```
TextureLoader.cpp      — 主逻辑：hook、替换链路、异步加载、显存淘汰、VEH 崩溃记录
dds_loader.cpp/.h      — DDS + DX10 头解析、BC1-BC7/unorm、mip/数组（自研）
gdds_interop.cpp/.h    — GDDS DirectStorage GPU 解压互操作（D3D12 别名 + DS + 共享 fence）
texture_hash.cpp/.h    — 3DMigoto 兼容哈希（GPL-3.0 移植）
mod_ini.cpp/.h         — [TextureOverride]/[Resource] ini 解析（自研，格式兼容）
crc32c/crc32c.cpp      — 硬件 CRC-32C（zlib 许可）
log.cpp/.h             — 日志
TextureLoader.ini      — 配置
third_party/dstorage/  — DirectStorage SDK（MIT + MS 条款，含 dstorage.h）
NOTICE.md / LICENSE.GPL.txt / AUTHORS.txt — 许可证与溯源
```

> 说明：开发期的测试工程与反汇编验证脚本已从仓库移除；验证结论沉淀于本文档 §5。
