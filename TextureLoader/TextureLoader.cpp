// SPDX-License-Identifier: GPL-3.0-or-later
// TextureLoader.cpp — 独立轻量 D3D11 纹理替换 DLL。
//
// 设计目标（相对完整 3DMigoto proxy 的精简重实现）：
//   * 不包装整个 ID3D11Device/Context vtable，只 hook 两个点：
//       - ID3D11Device::CreateTexture2D            → 用 3DMigoto 兼容哈希算出纹理 hash
//       - ID3D11DeviceContext::PSSetShaderResources → 绑定时按 hash 匹配 [TextureOverride]
//   * 因此与 ReShade / Dx11FsrBridge（各自 hook 渲染链/FSR）可共存。
//   * 哈希算法与 GIMI(texture_hash=0) 逐字一致，可匹配 Texture++ 自带的
//     [TextureOverride] hash= 与 [Resource] filename= 映射表。
//
// 许可证：GPL-3.0-or-later。哈希算法移植自 bo3b/3Dmigoto (GPL-3.0)，
// 见 LICENSE.GPL.txt 与 NOTICE.md；crc32c 为 Mark Adler / Robert Vazan 作品。

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log.h"
#include "texture_hash.h"
#include "mod_ini.h"
#include "dds_loader.h"
#include "gdds_interop.h"

#include "detours.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------

std::mutex g_lock;                  // 写锁：登记/释放/建 SRV（低频）
ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_context = nullptr;

// 渲染线程延迟释放队列：所有非渲染线程（替换跟踪回调/显存监控/异步加载）
// 不得直接 Release D3D11 对象——AMD 驱动 worker 可能在对象释放后仍引用
// （快速切换角色时崩溃现场 amdxx64 + RIP=垃圾地址 = 驱动内 UAF）。统一入队，
// 由渲染线程在 SetShaderResources hook 入口批量释放（与游戏提交串行）。
//
// ⚠️ 竞态修复：旧实现用 `g_pendingCount` 与队列**分离**维护——
//   QueueRelease：{ 加锁 push_back } 解锁；然后 InterlockedIncrement(count)
//   FlushPendingRelease：先读 count，为 0 就 return；否则锁内 swap、再 count=0
// 中间那段"解锁之后、自增之前"的窗口里，Flush 可能：
//   - 读到 count==0 直接返回（刚 push 的对象留在队列里，本次不释放，尚可等下次）
//   - 更糟：读到非 0 → swap 拿到队列 → 把 count 置 0；此时并发的 QueueRelease
//     已 push 完但**还没自增** → 该对象被 swap 走却没计入任何计数，而随后那次
//     InterlockedIncrement 让 count 变成 1 而队列**实际为空** → 计数与队列永久失配，
//     后续 Flush 反复进入却拿到空 batch，对象**永不释放**（泄漏）。
//
// 修复分两层：
// 1) **计数器移除**，队列本身成为唯一真相（判空与取走在同一把锁内，不可能失配）。
// 2) `g_pendingMaybe` 只做**无锁门闩**，用于恢复热路径快速返回：
//    FlushPendingRelease 由 SetShaderResources 钩子入口**每次调用**，若每次都加解锁
//    会带来可测开销（实测约 2~3%）。门闩的语义是"**可能**有排队项"——
//    producer 在**入队之前**先置位（exchange），consumer 在锁内 swap 后才清位，
//    因此不存在"漏看"：只要队列里真有东西，门闩必然已置位。
//    代价仅是偶尔多进一次锁拿到空 batch（无害）。这与旧实现"计数器可能与队列失配"
//    有本质区别：门闩是**保守方向**的近似（宁可真、不可漏），不会导致泄漏。
std::mutex g_pendingMutex;
std::deque<IUnknown *> g_pendingRelease;
std::atomic<bool> g_pendingMaybe{false};

static void QueueRelease(IUnknown *obj)
{
    if (!obj)
        return;
    // 先置门闩再入队：保证 consumer 只要能看到队列非空，就一定也能看到门闩。
    g_pendingMaybe.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    g_pendingRelease.push_back(obj);
}

// 渲染线程调用：释放所有排队对象。判空与取走在同一锁内，无失配窗口。
static void FlushPendingRelease()
{
    // 快速路径：门闩未置位 ⇒ 队列必空（见上方语义说明），零成本返回。
    if (!g_pendingMaybe.load(std::memory_order_acquire))
        return;
    std::deque<IUnknown *> batch;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        if (g_pendingRelease.empty())
            return;
        batch.swap(g_pendingRelease);
        // swap 之后队列已空 → 清门闩（在锁内，避免与 producer 的置位交错）
        g_pendingMaybe.store(false, std::memory_order_release);
    }
    // 锁外释放：Release 可能回调进 D3D/驱动，持锁会放大争用。
    for (IUnknown *obj : batch)
        obj->Release();
}

// 快速命中表已由 g_replacements 本身承担（find 即判定"是否有替换"），
// 无需独立集合——慢路径一次 g_lock find 同时完成"有无条目/是否需重载/取缓存"。

// 活跃替换资源：紧凑数组（写低频：纹理创建/销毁；读高频：绑定热路径）。
// 用固定数组替代 unordered_set：读时无锁线性扫 [0, count)，count = 实际存活数
// （几十~几百项 ~20ns），消除共享锁开销和 unordered_set 并发读写 UB。
// 写方（ActiveAdd/ActiveRemove）用专用互斥锁串行，读方无锁：
//   - 追加：先写槽位再发布 count（读者可能短暂漏看新项 → 走慢路径，无害）
//   - 移除：swap-remove 保持紧凑，再递减 count（读者最多多扫一个旧槽，只比
//     指针不解引用，无害）
// 纹理销毁后指针残留无害：销毁的 res 不会被游戏再绑定；地址被复用时慢路径
// 会因 GetResourceHash 查不到而跳过（只多走慢路径，不产生错误替换）。
const int kMaxActiveResources = 4096;
std::atomic<ID3D11Resource *> g_activeArr[kMaxActiveResources] = {};
std::atomic<long> g_activeCount{0}; // 存活条目数（写方持锁更新；读方 acquire 读）
std::mutex g_activeMutex;           // 写互斥（低频）；读方不取锁

// 满员丢弃计数（2026-09-22）：原实现在写满时**静默 return** ——
// 一旦真的写满，新资源再也无法登记，替换会**静默失效而日志毫无痕迹**。
// 这是最难查的一类故障，必须留下证据。
std::atomic<long> g_activeOverflow{0};
std::atomic<long> g_repSrvOverflow{0};

static void ActiveAdd(ID3D11Resource *res)
{
    if (!res)
        return;
    std::lock_guard<std::mutex> lk(g_activeMutex);
    long n = g_activeCount.load(std::memory_order_relaxed);
    if (n >= kMaxActiveResources)
    {
        // 满：计数 + 限流告警（首次与每 1024 次）—— 见 g_activeOverflow 的说明
        const long dropped = ++g_activeOverflow;
        if (dropped == 1 || (dropped % 1024) == 0)
            TL_LOG(L"[warn] activeArr full (%d) — replacement silently misses; dropped=%ld",
                   kMaxActiveResources, dropped);
        return;
    }
    g_activeArr[n].store(res, std::memory_order_release); // 先写槽位
    g_activeCount.store(n + 1, std::memory_order_release); // 后发布 count
}

// 原纹理销毁时移除（swap-remove 保持紧凑；比较不解引用，同地址复用由慢路径兜底）
static void ActiveRemove(ID3D11Resource *res)
{
    if (!res)
        return;
    std::lock_guard<std::mutex> lk(g_activeMutex);
    long n = g_activeCount.load(std::memory_order_relaxed);
    for (long i = 0; i < n; i++) {
        if (g_activeArr[i].load(std::memory_order_relaxed) == res) {
            g_activeArr[i].store(g_activeArr[n - 1].load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            g_activeArr[n - 1].store(nullptr, std::memory_order_relaxed);
            g_activeCount.store(n - 1, std::memory_order_release);
            return;
        }
    }
}
static bool ActiveContains(ID3D11Resource *res)
{
    long n = g_activeCount.load(std::memory_order_acquire);
    if (n > kMaxActiveResources)
        n = kMaxActiveResources;
    for (long i = 0; i < n; i++) {
        if (g_activeArr[i].load(std::memory_order_acquire) == res)
            return true;
    }
    return false;
}

// 替换 SRV 快速判定数组：绑定热路径先比 SRV 指针——命中说明该视图就是
// 替换视图（其资源不在 activeArr），无需 GetResource/ActiveContains，直接透传。
// 与 g_activeArr 同模式：写低频（SRV 创建/释放）互斥串行，读高频无锁扫。
const int kMaxReplacementSrvs = 4096;
std::atomic<ID3D11ShaderResourceView *> g_repSrvArr[kMaxReplacementSrvs] = {};
std::atomic<long> g_repSrvCount{0};
std::mutex g_repSrvMutex;

static void RepSrvAdd(ID3D11ShaderResourceView *srv)
{
    if (!srv)
        return;
    std::lock_guard<std::mutex> lk(g_repSrvMutex);
    long n = g_repSrvCount.load(std::memory_order_relaxed);
    if (n >= kMaxReplacementSrvs)
    {
        // 满：计数 + 限流告警（同 ActiveAdd —— 静默丢弃会让替换无声失效）
        const long dropped = ++g_repSrvOverflow;
        if (dropped == 1 || (dropped % 1024) == 0)
            TL_LOG(L"[warn] repSrvArr full (%d) — bound-SRV fast path degrades; dropped=%ld",
                   kMaxReplacementSrvs, dropped);
        return;
    }
    g_repSrvArr[n].store(srv, std::memory_order_release);
    g_repSrvCount.store(n + 1, std::memory_order_release);
}
static void RepSrvRemove(ID3D11ShaderResourceView *srv)
{
    if (!srv)
        return;
    std::lock_guard<std::mutex> lk(g_repSrvMutex);
    long n = g_repSrvCount.load(std::memory_order_relaxed);
    for (long i = 0; i < n; i++) {
        if (g_repSrvArr[i].load(std::memory_order_relaxed) == srv) {
            g_repSrvArr[i].store(g_repSrvArr[n - 1].load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            g_repSrvArr[n - 1].store(nullptr, std::memory_order_relaxed);
            g_repSrvCount.store(n - 1, std::memory_order_release);
            return;
        }
    }
}
static bool RepSrvContains(ID3D11ShaderResourceView *srv)
{
    long n = g_repSrvCount.load(std::memory_order_acquire);
    if (n > kMaxReplacementSrvs)
        n = kMaxReplacementSrvs;
    for (long i = 0; i < n; i++) {
        if (g_repSrvArr[i].load(std::memory_order_acquire) == srv)
            return true;
    }
    return false;
}

// 异步替换纹理加载：切换角色/加载场景时会瞬间命中大量纹理，若在渲染线程
// 同步加载（磁盘 IO + 建纹理）会造成帧时间尖峰。D3D11 device 方法线程安全，
// 因此把加载丢给后台线程，渲染线程只做轻量登记。
//
// 双队列 + 双线程（GDDS / DDS 互不干扰）：
//   - GDDS 加载走 DirectStorage GPU 解压（大纹理 8-128MB，单任务耗时高）
//   - DDS 走 CPU 磁盘加载（小纹理，低延迟）
//   两者各自独立消费，大 GDDS 任务不再阻塞 DDS 小纹理；登记/淘汰仍共享 g_lock。
struct AsyncLoadTask {
    uint32_t hash;
    std::wstring path;
    const TextureLoaderEntry *loader = nullptr; // 入队时选定（替代消费时扩展名判断）
};
std::mutex g_loadMutex;
std::condition_variable g_loadCv[2];
std::deque<AsyncLoadTask> g_loadQueue[2];
std::unordered_set<uint32_t> g_inflight; // 已出队正在加载的 hash（防在途重复加载）

// ---- 停机协作 ----------------------------------------------------------
// ⚠️ 旧实现的两个后台线程（AsyncLoadThread / VramMonitorThread）**没有退出条件**，
// 且 DLL_PROCESS_DETACH 既不停线程也不 DetourDetach —— 卸载时它们可能仍在
// ExecuteLoadTask 里访问已释放的替换缓存（下面的 detach 会清空 g_replacements），
// 属于 use-after-free。这里加显式停机信号 + 线程句柄，detach 时先停后清。
std::atomic<bool> g_shutdownRequested{ false };
HANDLE g_loadThread[2] = { nullptr, nullptr };
HANDLE g_vramThread = nullptr;
HANDLE g_bootstrapThread = nullptr;
bool g_loadThreadRunning[2] = {false, false};

// 前置声明（定义在 HookCreateTexture2D 之后，同步/异步共用）
static void ExecuteLoadTask(const AsyncLoadTask &task);

// 加载器注册表（扩展名 → 实现；nullptr 条目为默认 DDS 兜底）
static const TextureLoaderEntry g_loaders[] = {
    {L"gdds", ::tloader_gdds::LoadGddsTextureEntry},
    {L"dds", LoadDdsTexture},
    {nullptr, LoadDdsTexture}, // 默认兜底：未注册扩展名走 CPU DDS
};

// ⚠️ 2026-09-22：判断"是不是 GDDS 加载器"必须按**加载器函数**，
// 不能按 `&g_loaders[0]` 下标。
//
// 原实现两处都用下标 0 认 GDDS。一旦调整注册表顺序、或在 GDDS 之前插入
// 新加载器，判定就会**静默错位** —— 把 DDS 当成 GDDS（于是 DDS 被
// `gdds_enabled=0`/`Failed()` 误拦，替换整体失效），或反之（GDDS 任务
// 投进 DDS 队列）。两种都很难从日志看出来。
// 按函数指针判定与顺序无关：默认兜底项用的也是 LoadDdsTexture，所以
// `load == LoadGddsTextureEntry` 唯一标识 GDDS。
static bool IsGddsLoader(const TextureLoaderEntry *entry)
{
    return entry != nullptr && entry->load == ::tloader_gdds::LoadGddsTextureEntry;
}

// 入队：选定加载器并按类型投递到对应队列
static void EnqueueLoad(uint32_t hash, std::wstring path)
{
    AsyncLoadTask lt;
    lt.hash = hash;
    lt.path = std::move(path);
    lt.loader = SelectTextureLoader(lt.path.c_str(), g_loaders,
                                    _countof(g_loaders));
    if (!lt.loader)
        return; // 无匹配且无默认（注册表为空）——不可能，兜底恒存在
    // GDDS 失败终态隔离：DirectStorage 不可用时 GDDS 任务不再入队，
    // 避免队列堆积与 [fail] 刷屏——DDS 路径完全不受影响。
    // gdds_enabled=0：配置禁用 GDDS（N 卡驱动缺陷规避等场景），.gdds 直接跳过。
    // 2026-09-22：改用 IsGddsLoader 判定，不再依赖注册表下标。
    const bool is_gdds = IsGddsLoader(lt.loader);
    if (is_gdds &&
        (!::tloader::g_gdds_enabled || ::tloader_gdds::Failed()))
        return;
    // 同步模式（async_load=0）：渲染线程直接执行加载——与 3DMigoto/旧版一致，
    // D3D11 纹理创建在渲染线程（规避 NVIDIA 驱动对后台线程建纹理的缺陷）。
    if (!::tloader::g_async_load) {
        ExecuteLoadTask(lt);
        return;
    }
    int q = is_gdds ? 0 : 1; // 0=GDDS 1=DDS
    {
        std::lock_guard<std::mutex> lk(g_loadMutex);
        // 去重（含在途）：同 hash 已在队列（未消费）或正在加载（已出队）时
        // 不再入队——快速切角色时同一纹理被反复创建，避免队列堆积、重复
        // 唤醒与重复加载（消费端/登记端仍有兜底检查）。
        if (g_inflight.count(hash))
            return;
        for (const auto &t : g_loadQueue[q]) {
            if (t.hash == hash)
                return;
        }
        g_loadQueue[q].push_back(std::move(lt));
    }
    g_loadCv[q].notify_one();
}

// resource -> 3DMigoto 兼容 hash（CreateTexture2D 时填充）
// 注意：不用裸指针 map（资源销毁后地址可能被复用导致错误替换），
// 改为通过 ID3D11Resource::SetPrivateData 把 hash 挂在资源本身上，随资源消亡。
// 替换纹理/SRV 缓存**全局持久化**（与 3DMigoto CustomResource 同策略）：
// 原纹理销毁只减 refcount，不释放替换缓存——同 hash 再创建直接复用，避免
// 反复重载与释放（NVIDIA 驱动对释放路径敏感）。显存压力时由 VramMonitor
// 淘汰（释放 texture/srv 保留条目，命中时重新加载），进程退出时统一释放。
struct ReplacementEntry {
    ID3D11Texture2D *texture = nullptr;          // 替换纹理（缓存持有）
    ID3D11ShaderResourceView *srv = nullptr;     // 替换 SRV（缓存持有）
    uint32_t refcount = 0;                        // 活跃原纹理数
    uint64_t last_used = 0;                       // 最近绑定命中时间（GetTickCount64）
    uint64_t mem_bytes = 0;                       // 估算显存占用（纹理大小）
    uint64_t ready_fence = 0;                      // GDDS 完成 fence 值（0=非 GDDS/已等待）
    DXGI_FORMAT original_format = DXGI_FORMAT_UNKNOWN; // 游戏原纹理格式（CreateTexture2D 时记录）
    UINT original_mips = 0;                       // 游戏原纹理 mip 数（0=未知）
};
std::unordered_map<uint32_t, ReplacementEntry> g_replacements;
// 从 Mods 解析出的 [TextureOverride] hash -> dds
std::unordered_map<uint32_t, TextureOverrideEntry> g_overrides;

// 显存感知缓存控制
IDXGIAdapter3 *g_dxgi_adapter3 = nullptr;        // 查询显存的适配器
uint64_t g_total_vram = 0;                       // 物理显存字节（DXGI_ADAPTER_DESC.DedicatedVideoMemory）
uint32_t g_gpu_vendor = 0;                       // GPU 厂商 ID（0x10DE=NVIDIA 0x1002=AMD 0x8086=Intel）
volatile long g_vramMonitorRunning = 0;

// ---------------------------------------------------------------------------
// 闲置候选索引（显存回收的低开销数据源）
// 监控线程不再每轮全表扫描——维护"可淘汰候选"增量索引：
//   g_idleSet:      std::set<{last_used, hash}>，按 last_used 升序（最久未用在前）
//   g_idleLastUsed: hash → 索引内 last_used（惰性修正定位旧键）
// 维护点全部低频（登记/tracker 归零/淘汰/豁免到期），在 g_lock 内调用；
// 绑定热路径更新 last_used 不碰索引（零开销），由监控巡检验证时惰性修正。
// 以下函数必须在持有 g_lock 时调用。
// ---------------------------------------------------------------------------
static std::set<std::pair<uint64_t, uint32_t>> g_idleSet;
static std::unordered_map<uint32_t, uint64_t> g_idleLastUsed;

static void IdleIndexUpdate(uint32_t hash, uint64_t last_used)
{
    auto it = g_idleLastUsed.find(hash);
    if (it != g_idleLastUsed.end()) {
        g_idleSet.erase({it->second, hash});
        g_idleLastUsed.erase(it);
    }
    g_idleSet.insert({last_used, hash});
    g_idleLastUsed[hash] = last_used;
}
static void IdleIndexRemove(uint32_t hash)
{
    auto it = g_idleLastUsed.find(hash);
    if (it != g_idleLastUsed.end()) {
        g_idleSet.erase({it->second, hash});
        g_idleLastUsed.erase(it);
    }
}

// 缓存总占用（原子维护：登记 += mem，淘汰 -= mem；监控线程 O(1) 读）
static std::atomic<uint64_t> g_cacheBytesTotal{0};

// 资源私有数据 GUID（用于存储纹理 hash、释放跟踪器）
// 2026-11-23：原 TL_DYNAMIC_GUID（「最后更新时刻」TTL 载荷）已删除 —— 见下方
// HookUpdateSubresource 的「为什么不标记」注释。**旧 GUID 数值故意不留在这里**：
// 探针 probe/tloader-nv/probe.cpp 仍用同一个 GUID 数值读私有数据，它需要能读到
// 「(none)」来证明新构建一个字节都没写；DLL 侧不再需要这个符号。
static const GUID TL_HASH_GUID = {0x8a2f6d4e, 0x7c31, 0x4a9b, {0x9e, 0x1c, 0x2d, 0x5f, 0x8a, 0x0b, 0x3c, 0x71}};
static const GUID TL_TRACKER_GUID = {0x2c4a8e60, 0x1d53, 0x4f9a, {0xb8, 0x4e, 0x7a, 0x2f, 0x6c, 0x09, 0xd5, 0x33}};

// 释放跟踪器：经 SetPrivateDataInterface 挂到原纹理上，原纹理被销毁时
// D3D 回调本对象 Release；ref 归零时把替换缓存引用计数 -1，归零则释放替换资源。
class ReplacementTracker : public IUnknown
{
    std::atomic_ulong ref;
    uint32_t hash;
    ID3D11Resource *resource; // 被跟踪的原纹理
public:
    ReplacementTracker(uint32_t h, ID3D11Resource *r) : hash(h), resource(r) { ref = 0; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (ppvObject && IsEqualIID(riid, IID_IUnknown)) {
            AddRef();
            *ppvObject = this;
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        // ⚠️ 2026-09-23：下溢防护。
        //
        // `ref` 初值为 0，本对象能否正确触发**完全依赖** `SetPrivateDataInterface`
        // 按 COM 约定 AddRef。若它没有 AddRef（或 D3D 内部多调一次 Release），
        // 原来的 `--ref` 会**回绕到 ULONG_MAX**，`r == 0` 永不成立
        // ⇒ tracker 永不触发 ⇒ `g_replacements[hash].refcount` 永不递减
        // ⇒ 该条目**永不可淘汰**（静默显存泄漏，且现象与"缓存策略正常"无法区分）。
        //
        // 改用 CAS：归零后再 Release 只返回 0，既不下溢也不重复触发。
        ULONG prev = ref.load(std::memory_order_relaxed);
        for (;;)
        {
            if (prev == 0)
                return 0; // 已归零：不再下溢，也不再触发
            if (ref.compare_exchange_weak(prev, prev - 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
                break;
        }
        const ULONG r = prev - 1;
        if (r == 0) {
            // 原纹理销毁：替换缓存引用计数 -1。
            // 缓存全局持久化（与 3DMigoto CustomResource 同策略）：原纹理销毁
            // 不释放替换纹理/SRV、不删除条目——同 hash 再创建时直接复用缓存
            // （need_load 判定 texture!=nullptr 即跳过加载）。替换纹理仅在
            // 显存压力淘汰（VramMonitorThread）或进程退出时释放，避免频繁
            // 创建/销毁替换资源（NVIDIA 驱动对释放路径也敏感，且切角色时
            // 反复重载浪费 IO）。
            // 注意：D3D 调用本函数时已持有其内部锁（AB-BA 死锁风险），
            // 因此锁内只操作 map，锁外不触碰 D3D 对象（缓存继续持有）。
            {
                std::lock_guard<std::mutex> lk(g_lock);
                auto it = g_replacements.find(hash);
                if (it != g_replacements.end()) {
                    if (it->second.refcount > 0)
                        it->second.refcount--;
                    // refcount 归零 → 重新可淘汰：加入闲置索引（若已有则更新）
                    if (it->second.refcount == 0 && it->second.texture)
                        IdleIndexUpdate(hash, it->second.last_used);
                }
                // 活跃资源数组：本原纹理已销毁，清空其槽位供复用
                ActiveRemove(resource);
            }
            delete this;
        }
        return r;
    }
};

// 通过 SetPrivateData 存储/读取纹理 hash（随资源消亡，杜绝裸指针复用误替换）
static void SetResourceHash(ID3D11Resource *res, uint32_t hash)
{
    if (res)
        res->SetPrivateData(TL_HASH_GUID, sizeof(hash), &hash);
}
static bool GetResourceHash(ID3D11Resource *res, uint32_t *out)
{
    if (!res)
        return false;
    UINT size = sizeof(uint32_t);
    uint32_t h = 0;
    HRESULT hr = res->GetPrivateData(TL_HASH_GUID, &size, &h);
    if (SUCCEEDED(hr) && size == sizeof(uint32_t)) {
        *out = h;
        return true;
    }
    return false;
}
// ---------------------------------------------------------------------------
// 「被 UpdateSubresource 更新过」这件事已经不记在任何地方（2026-11-23）
// ---------------------------------------------------------------------------
// 这里曾经有两代实现，均已删除，留记录是因为它们各自出过事：
//   · <= 2.5.0.0：写 4 字节常量 1 =「永久动态」，绑定慢路径的 IsDynamic() 一读到就
//     **永久**跳过该纹理的替换 —— D3D11 私有数据没有删除语义（SetPrivateData 只能
//     覆盖），所以这个标记**不可撤销**。后果是「这张贴图的 mod 从此不生效」，且日志里
//     没有任何痕迹（标记本身不打日志、绑定热路径又是无日志快速判定）。
//     E1 证据：探针 G / G′ 两例（2026-11-22 22:15），更新后中心像素由绿变蓝
//     `(0,255,0,255)` → `(0,0,255,255)`、私有数据 `dynamic=(none)` → `dynamic=1`，
//     分属两个构建、同一探针，唯一变量是被测 DLL。
//   · 2026-11-22：改成 8 字节 GetTickCount64 时间戳的 TTL 窗口（配置键
//     update_suppress_ms），窗口过后自动恢复替换。**已回滚**，理由是实机实测
//     （见 HookUpdateSubresource 上方）：该窗口在本游戏版本上命中 0 次，没有作用对象。
//
// 现在两条钩子（CopyResource 自 f07bb04、UpdateSubresource 自 2026-11-23）都**不写
// 任何私有数据**，只做计数转发。因此本文件不再需要时间戳读写/窗口判定这组辅助函数，
// 也不再需要 NowMs()。

std::atomic<long> g_hook_ready{0};   // 双检锁用；必须严格原子（旧为 volatile long，非原子读）
volatile long g_stats_created = 0;   // 已哈希的纹理数（**所有**带初始数据的纹理）
volatile long g_stats_matched = 0;   // 命中的替换数（g_stats_created 的子集）
volatile long g_stats_bound = 0;     // 替换 SRV 被绑定次数
volatile bool g_observe_only = true; // 1=只记录匹配，0=真正替换
// ⚠️ 2026-09-22：`g_stats_created` 原在 matched 分支内自增，
// 与 `g_stats_matched` **恒等** —— 是日志说谎（"created" 从未反映哈希量）。
// 已移到哈希之后、判定之前。`created - matched` = 哈希了但未命中。

// HookCopyResource 命中计数（2026-11-22）。**口径必须与 [stat] 行一起读**：
//   · 仅当 dst 当前位于活跃替换集（ActiveContains 通过）时才自增；未通过不计；
//   · **不是** CopyResource 的调用总次数，也**不是**被触及的纹理数 ——
//     同一条资源每帧被拷贝一次就重复计入一次；
//   · 属**累计**计数（进程生命周期内单调递增，不在任何地方清零）。
// 用途：判别「实机上这条路径到底会不会发生」。它是判定是否保留该钩子的唯一依据，
// 所以口径必须窄 —— 计宽了就回答不了这个问题。
// 三态解释（行缺失 / 行在但计 0 / 计 >0）见 [stat] 打印点前的注释。
// 为什么用 std::atomic 而不是同区域的 volatile long + InterlockedIncrement：
// 见 AGENTS.md §二.4（volatile 不是同步原语）。此处是渲染线程自增、退出时读，
// 用 relaxed 顺序即可（只要求不丢增量，不需要跨变量的可见性顺序）。
std::atomic<long> copyresource_hit_calls{0};

// 两个钩子的【总调用次数】（2026-11-23 新增；**保留**，见其用途）。
// 口径：都是【累计】、都与 dst 是否在活跃集无关（计数在钩子第一行、无条件）。
// 为什么值得留这两个整数：它们是「API 被调用了吗」的唯一直接答案，而这个问题在
// 2026-11-23 的实机实测里一锤定音 —— `upd_calls=166018`（107 秒）而当时用于「命中
// 替换集」的计数为 0 ⇒ 证明 UpdateSubresource 与可替换纹理是两个不相交的集合，
// TTL 抑制功能因此被回滚。没有 upd_calls 这个数，只看到「命中 0 次」就无法区分
//   ① 游戏从不调用 UpdateSubresource；
//   ② 游戏频繁调用，但目标纹理都不在替换集。
// 两者对「要不要保留这个钩子」给出相反结论，所以这一对计数必须留着。
// ⚠️ 代价**未量化**：实测该 API 约 1550 次/秒，每次一个 relaxed 原子自增；若需要
// 具体帧数影响，必须用无诊断构建 + 外部帧时间工具（AGENTS.md §二.5 禁止热路径计时探针）。
std::atomic<long> g_update_subresource_calls{0};
std::atomic<long> g_copy_resource_calls{0};

// ---------------------------------------------------------------------------
// 原始函数指针
// ---------------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE *CreateTexture2D_t)(ID3D11Device *,
    const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

CreateTexture2D_t RealCreateTexture2D = nullptr;

// ---------------------------------------------------------------------------
// 工具：用替换纹理创建 SRV（复用原 SRV 的视图描述结构，格式取替换纹理）
// ---------------------------------------------------------------------------

// 格式家族判定：视图格式必须与纹理实际数据格式同家族，否则 GPU 会按错误
// 的块压缩格式解码（如 Finale 4K 的 GDDS 数据是 BC3(78) 而游戏原纹理是
// BC7(99)——用 99 建视图 hr 成功但解码错乱 → 颜色/模糊错误）。
static int BlockFamily(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return 1;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: return 2;
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return 3;
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: return 4;
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: return 5;
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16: return 6;
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return 7;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return 10;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 20;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return 30;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 40;
    default: return 100;
    }
}

// 估算纹理显存占用（字节）：BC 块压缩按 4x4 块计，其余按每像素字节，
// 含完整 mip 链与数组切片（用于淘汰策略按大小排序）。
static uint64_t EstimateTextureMemory(DXGI_FORMAT fmt, UINT w, UINT h,
                                      UINT mips, UINT array)
{
    uint64_t blockBytes = 0; // BC：每 4x4 块字节（0=非块压缩）
    uint64_t bpp = 0;        // 非 BC：每像素字节
    switch (BlockFamily(fmt)) {
        case 1: case 4:                       blockBytes = 8;  break; // BC1/BC4
        case 2: case 3: case 5: case 6: case 7: blockBytes = 16; break; // BC2/3/5/6/7
        case 10:                              bpp = 4;  break; // R8G8B8A8/B8G8R8A8
        case 20:                              bpp = 8;  break; // R16G16B16A16
        case 30:                              bpp = 4;  break; // R10G10B10A2
        case 40:                              bpp = 16; break; // R32G32B32A32
        default:
            switch (fmt) {
                case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS:
                case DXGI_FORMAT_R8_UINT:   case DXGI_FORMAT_R8_SNORM:      bpp = 1;  break;
                case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
                case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_SNORM:     bpp = 2;  break;
                case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_FLOAT: bpp = 4; break;
                case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT:
                case DXGI_FORMAT_R32_TYPELESS:                              bpp = 4;  break;
                case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: bpp = 8; break;
                default:                                                    bpp = 4;  break;
            }
            break;
    }
    if (mips == 0) mips = 1;
    if (array == 0) array = 1;
    uint64_t total = 0;
    for (UINT m = 0; m < mips; ++m) {
        UINT mw = w >> m; if (mw == 0) mw = 1;
        UINT mh = h >> m; if (mh == 0) mh = 1;
        if (blockBytes) {
            uint64_t blocks = ((uint64_t)(mw + 3) / 4) * ((uint64_t)(mh + 3) / 4);
            total += blocks * blockBytes;
        } else {
            total += (uint64_t)mw * mh * bpp;
        }
    }
    return total * array;
}

// 用替换纹理创建 SRV：保留原 SRV 的视图描述（格式/mip/数组切片），
// 只在原格式为 TYPELESS/UNKNOWN 或与替换纹理不兼容时改用替换纹理格式。
ID3D11ShaderResourceView *CreateReplacementSRV(uint32_t hash,
    const D3D11_SHADER_RESOURCE_VIEW_DESC *origDesc)
{
    ID3D11Texture2D *tex = nullptr;
    uint64_t gdds_fence = 0; // GDDS 完成 fence（渲染线程等待——立即上下文非线程安全）
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_replacements.find(hash);
        if (it == g_replacements.end())
            return nullptr;
        tex = it->second.texture;
        if (!tex)
            return nullptr;
        it->second.last_used = GetTickCount64(); // 更新 LRU 时间戳
        if (it->second.srv) {
            it->second.srv->AddRef();
            return it->second.srv;
        }
        // GDDS 纹理：DS 完成后 Signal 共享 fence，须在渲染线程（本函数由
        // SetShaderResources hook 调用，即游戏渲染线程）等待一次建立可见性。
        // 严禁在后台加载线程调用立即上下文 Wait——与游戏提交竞态会损坏
        // GPU 命令流导致 TDR/全暗。
        gdds_fence = it->second.ready_fence;
        it->second.ready_fence = 0; // 只等待一次
        tex->AddRef(); // 锁外使用安全：防止后台加载线程/销毁同时释放
    }
    if (!g_device) {
        // ⚠️ 必须释放上面（锁内）为"锁外使用安全"而取的 AddRef：
        // 旧实现此处直接 return，是唯一一条漏掉 Release 的返回路径 →
        // 每次走到这里都泄漏一个替换纹理引用，导致替换缓存**永远无法被淘汰**
        // （淘汰要求 refcount 归零，见 VramMonitorThread 的"绝不淘汰使用中"约束）。
        tex->Release();
        return nullptr;
    }

    // 渲染线程：等待 GDDS GPU 解压完成（fence 通常已 Signal——立即返回）
    if (gdds_fence != 0 && g_context != nullptr)
        ::tloader_gdds::WaitOnRenderThread(g_context, gdds_fence);

    ID3D11ShaderResourceView *srv = nullptr;
    HRESULT hr = E_FAIL;

    if (origDesc) {
        // 复制原视图描述，仅修正格式字段
        D3D11_SHADER_RESOURCE_VIEW_DESC desc = *origDesc;
        D3D11_TEXTURE2D_DESC tdesc;
        tex->GetDesc(&tdesc);
        // 原格式是 TYPELESS / UNKNOWN 或与替换纹理不兼容时，用替换纹理的实际格式
        DXGI_FORMAT viewFmt = desc.Format;
        if (viewFmt == DXGI_FORMAT_UNKNOWN)
            viewFmt = tdesc.Format;
        else if (viewFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) viewFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (viewFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) viewFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (viewFmt == DXGI_FORMAT_BC1_TYPELESS) viewFmt = DXGI_FORMAT_BC1_UNORM;
        else if (viewFmt == DXGI_FORMAT_BC2_TYPELESS) viewFmt = DXGI_FORMAT_BC2_UNORM;
        else if (viewFmt == DXGI_FORMAT_BC3_TYPELESS) viewFmt = DXGI_FORMAT_BC3_UNORM;
        else if (viewFmt == DXGI_FORMAT_BC4_TYPELESS) viewFmt = DXGI_FORMAT_BC4_UNORM;
        else if (viewFmt == DXGI_FORMAT_BC5_TYPELESS) viewFmt = DXGI_FORMAT_BC5_UNORM;
        else if (viewFmt == DXGI_FORMAT_BC7_TYPELESS) viewFmt = DXGI_FORMAT_BC7_UNORM;
        else if (viewFmt == DXGI_FORMAT_R16G16B16A16_TYPELESS) viewFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
        else if (viewFmt == DXGI_FORMAT_R10G10B10A2_TYPELESS) viewFmt = DXGI_FORMAT_R10G10B10A2_UNORM;
        // 格式家族预检：原 SRV 格式与替换纹理实际数据格式家族不同（如 BC7(99)
        // 原纹理 vs BC3(78) GDDS 数据）时，视图必须用纹理实际格式——否则 GPU
        // 按错误块压缩格式解码（CreateShaderResourceView 对 BC 家族不校验，hr
        // 成功但颜色/模糊错误）。
        if (BlockFamily(viewFmt) != BlockFamily(tdesc.Format))
            viewFmt = tdesc.Format;
        // 修正 mip/数组切片越界（替换纹理可能 mip 更少）
        switch (desc.ViewDimension) {
            case D3D11_SRV_DIMENSION_TEXTURE2D:
                desc.Texture2D.MipLevels = std::min(desc.Texture2D.MipLevels,
                    std::max(1u, tdesc.MipLevels) - desc.Texture2D.MostDetailedMip);
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                desc.Texture2DArray.MipLevels = std::min(desc.Texture2DArray.MipLevels,
                    std::max(1u, tdesc.MipLevels) - desc.Texture2DArray.MostDetailedMip);
                desc.Texture2DArray.ArraySize = std::min(desc.Texture2DArray.ArraySize,
                    std::max(1u, tdesc.ArraySize) - desc.Texture2DArray.FirstArraySlice);
                break;
            case D3D11_SRV_DIMENSION_TEXTURECUBE:
                desc.TextureCube.MipLevels = std::min(desc.TextureCube.MipLevels,
                    std::max(1u, tdesc.MipLevels) - desc.TextureCube.MostDetailedMip);
                break;
            default:
                break;
        }
        desc.Format = viewFmt;
        hr = g_device->CreateShaderResourceView(tex, &desc, &srv);
        if (FAILED(hr)) {
            // 格式视图仍不兼容时退回默认全视图
            hr = g_device->CreateShaderResourceView(tex, nullptr, &srv);
        }
        // 诊断：记录原视图格式 vs 替换纹理格式 vs 最终视图格式（定位颜色/格式不匹配）
        TL_LOG_IF(1, L"[srv ] hash=0x%08X origFmt=%d texFmt=%d viewFmt=%d hr=0x%08X mips=%u/%u",
               hash, (int)origDesc->Format, (int)tdesc.Format, (int)viewFmt, (unsigned)hr,
               origDesc->Texture2D.MipLevels, tdesc.MipLevels);
    } else {
        hr = g_device->CreateShaderResourceView(tex, nullptr, &srv);
    }

    if (FAILED(hr)) {
        tex->Release();
        return nullptr;
    }

    // 双重检查：可能另一线程已创建；缓存由替换条目持有，随原纹理销毁释放
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_replacements.find(hash);
        if (it == g_replacements.end() || !it->second.texture) {
            srv->Release();
            tex->Release();
            return nullptr;
        }
        if (it->second.srv) {
            srv->Release();
            srv = it->second.srv;
            srv->AddRef();
            tex->Release();
            return srv;
        }
        it->second.srv = srv; // 缓存持有引用
        RepSrvAdd(srv);       // 登记替换视图：绑定热路径可无 COM 快速判定
        srv->AddRef();
        tex->Release();
        return srv;
    }
}

// ---------------------------------------------------------------------------
// 诊断：导出游戏纹理哈希（配置键 dump_hashes，2026-11-22 新增）
// ---------------------------------------------------------------------------
// 为什么需要它：本 DLL 的**每一条** `hash=` 日志行（[HIT]/[srv]/[skip]/[fail]/[bind]/[upd]）
// 都出现在 `matched && ov` 之后，**没命中 mod 的哈希被直接丢弃**（HookCreateTexture2D 里
// CalcTexture2DDescHash 算完只在命中分支用）。于是 Mods 目录为空时**拿不到任何游戏纹理
// 哈希** —— 想造一个最小 mod 做端到端验证都无从下手；报告者那类「mod 装了不生效」也
// 无法定案（他的 `hashed=7205 matched=42` 到底是哈希不匹配还是别的原因，没有数据能回答）。
//
// ⚠️ 为什么这**不是** AGENTS.md §二.5 禁止的「热路径探针」（本条目的硬性要求之一）：
//   · HookCreateTexture2D 是**每次创建纹理**调用一次，不是每帧 / 每 draw —— 同一张纹理只在
//     加载时创建一次，之后逐帧走的是 HookSetShaderResourcesCommon。量级参照：报告者实测
//     47 秒 hashed=7205（一局几千行）。渲染线程上确实会发生（哈希在渲染线程同步算），
//     但那是「每个资源一次」，不是「每帧一次」。
//   · §二.5 的红线是「每帧/每 draw 加计时探针」（每 draw 2×QueryPerformanceCounter + 4 个
//     原子操作，80,000 draw/s 时吃掉 7~8% 帧数）；本条**不做任何计时**，只打一行日志。
//   · 而且**默认关闭**：dump_hashes=0 时连去重容器都不碰（见调用点，取锁之前就 return）。
//
// 去重：同一张纹理在不同场景会被反复创建，重复行只是噪音；诊断要问的是「这个游戏版本里
// 有哪些**不同**的纹理哈希」，故每个不同 hash 只导出一行。
// 写法照同文件既有的 g_bind_logged（见 HookSetShaderResourcesCommon 上方）：**static**（内部链接）
// + 专用 std::mutex + `insert().second` 判首次。
// （2026-11-23：原注释还并列引用了 g_upd_logged，它随 [upd] recovered 日志一起删除 ——
//   引用一个已不存在的符号会让注释指错地方，故同步收敛为单一出处。）
// 容器取 unordered_set 而不是 g_bind_logged 的 std::set：本处规模可达数万，只做插入/判重，
// 不需要有序遍历，平均 O(1) 优于 O(log n)。
static std::mutex g_hash_dump_mutex;
static std::unordered_set<uint32_t> g_hash_dump_logged;

// 前置条件：调用方已确认 g_dump_hashes != 0。本函数**故意不做**该判断 ——
// 判断必须发生在**取锁之前**（条目 §六.1：不能为了一行不打的日志去抢一把全局锁）。
static void DumpTextureHash(uint32_t hash, const D3D11_TEXTURE2D_DESC *desc)
{
    if (!desc)
        return;
    {
        // 锁只保护集合本身。日志必须放在锁**外**：log_write 内部还要取自己的 g_mutex，
        // 且在渲染线程上做 fflush（AGENTS.md §二.4「不要在持锁期间做 I/O」）。
        std::lock_guard<std::mutex> lk(g_hash_dump_mutex);
        if (!g_hash_dump_logged.insert(hash).second)
            return; // 这个 hash 已经导出过
    }
    // 格式口径（条目 §4.4）：fmt / usage 用 DXGI_FORMAT / D3D11_USAGE 的**枚举数值**，
    // 不是字符串 —— 数值稳定，可直接写进 DDS 的 DX10 头（dds_loader 的口径同源）。
    // 前导 [hash] 便于 Select-String 抽取；行内必须含 hash/宽/高/格式，否则不足以合成 mod。
    TL_LOG(L"[hash] 0x%08X %ux%u fmt=%d mips=%u usage=%d",
           hash, desc->Width, desc->Height, (int)desc->Format, desc->MipLevels,
           (int)desc->Usage);
}

// ---------------------------------------------------------------------------
// Hook: CreateTexture2D — 计算哈希、登记替换纹理
// ---------------------------------------------------------------------------

// 重入保护：LoadDdsTexture 创建替换纹理时直接转发，不再进入哈希/替换逻辑
static thread_local bool t_in_create_texture = false;

// ⚠️ 2026-09-22：重入标志必须用 RAII 管理。
//
// 原实现是手工 `t_in_create_texture = true;` / `= false;` 夹住 loader->load()。
// 只要 load() 以**异常**离开（其内部用到 std::vector/std::wstring 等会分配的
// 类型），那句 `= false` 就永不执行 → 该线程**永久**停留在重入态 →
// 此后该线程创建的**所有**纹理都直接转发、绕过哈希与替换 →
// **mod 整体静默失效，且日志里没有任何异常痕迹**。
// RAII 保证任何离开路径（return / throw）都会复位。
struct CreateTextureReentryGuard
{
    CreateTextureReentryGuard() { t_in_create_texture = true; }
    ~CreateTextureReentryGuard() { t_in_create_texture = false; }
    CreateTextureReentryGuard(const CreateTextureReentryGuard &) = delete;
    CreateTextureReentryGuard &operator=(const CreateTextureReentryGuard &) = delete;
};

static HRESULT STDMETHODCALLTYPE HookCreateTexture2D(
    ID3D11Device *This,
    const D3D11_TEXTURE2D_DESC *pDesc,
    const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Texture2D **ppTexture2D)
{
    if (t_in_create_texture)
        return RealCreateTexture2D(This, pDesc, pInitialData, ppTexture2D);

    HRESULT hr = RealCreateTexture2D(This, pDesc, pInitialData, ppTexture2D);
    if (FAILED(hr) || !ppTexture2D || !*ppTexture2D)
        return hr;

    // 仅对带初始数据的静态纹理计算哈希（动态/渲染目标纹理无初始数据则跳过）。
    // 哈希在渲染线程同步计算（保持正确性：pInitialData 此刻有效），
    // 替换纹理加载已异步（AsyncLoadThread），避免切角色时磁盘 IO 阻塞。
    if (pInitialData && pInitialData->pSysMem) {
        uint32_t data_hash = CalcTexture2DDataHash(pDesc, pInitialData);
        uint32_t hash = CalcTexture2DDescHash(data_hash, pDesc);
        // ⚠️ 2026-09-22：`g_stats_created` 必须在这里自增。
        //
        // 原实现把它放在下面的 `if (matched && ov)` 分支**内**，紧邻
        // `g_stats_matched` —— 于是两个计数**恒等**，`[stat]` 行里的
        // "created" 从来没有反映过实际哈希量（是**日志说谎**，不是冗余）。
        // 移到判定之前，让它真正表示"已哈希的纹理数"，
        // 从而 `created - matched` 就是"哈希了但没命中"的数量。
        InterlockedIncrement(&g_stats_created);
        // 纹理哈希导出（2026-11-22，配置键 dump_hashes）：算完 hash、查 override **之前**。
        // 放在这里是因为「没命中 mod 的哈希」正是要导出的对象 —— 放到 FindOverride 之后
        // 就只剩下命中项，等于把本功能的目的（看这个游戏版本有哪些哈希）废掉。
        // **零副作用**（条目 §六.1）：dump_hashes=0 时这里只多一次 relaxed 原子读，
        // 不取 g_hash_dump_mutex、不查 g_hash_dump_logged、不产生任何字符串/日志 —— 判断在取锁之前。
        // 为什么用 relaxed：它只在 DllMain 里写一次、之后被渲染线程读，只要求「读到 0 或读到 1」。
        if (::tloader::g_dump_hashes.load(std::memory_order_relaxed))
            DumpTextureHash(hash, pDesc);
        const TextureOverrideEntry *ov = nullptr;
        bool matched = FindOverride(g_overrides, hash, &ov);

        if (matched && ov) {
            {
                std::lock_guard<std::mutex> lk(g_lock);
                SetResourceHash(*ppTexture2D, hash);
            }
            InterlockedIncrement(&g_stats_matched);
            if (g_observe_only) {
                TL_LOG_IF(1, L"[HIT ] tex hash=0x%08X \"%ls\" (%ux%u) observe-only (would use %ls)",
                       hash, ov->ini_section.c_str(), pDesc->Width, pDesc->Height,
                       ov->dds_path.c_str());
            } else {
                bool need_load = false;
                {
                    std::lock_guard<std::mutex> lk(g_lock);
                    auto &entry = g_replacements[hash];
                    entry.refcount++;
                    need_load = (entry.texture == nullptr);
                    // 记录游戏原纹理格式/mip——用于与替换纹理对比（SRV 视图格式诊断）
                    if (entry.original_format == DXGI_FORMAT_UNKNOWN) {
                        entry.original_format = pDesc->Format;
                        entry.original_mips = pDesc->MipLevels;
                    }
                }
                ReplacementTracker *tr = new ReplacementTracker(hash, *ppTexture2D);
                HRESULT thr = (*ppTexture2D)->SetPrivateDataInterface(TL_TRACKER_GUID, tr);
                if (FAILED(thr)) {
                    std::lock_guard<std::mutex> lk(g_lock);
                    auto it = g_replacements.find(hash);
                    if (it != g_replacements.end() && it->second.refcount > 0)
                        it->second.refcount--;
                    delete tr;
                }
                if (SUCCEEDED(thr)) {
                    ActiveAdd(*ppTexture2D);
                }
                if (need_load) {
                    EnqueueLoad(hash, ov->dds_path);
                }
            }
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// 执行单个加载任务（异步线程与同步模式共用）：
// 经统一加载器注册表执行加载，完成后更新替换表。
// 同步模式（async_load=0）在渲染线程调用——与 3DMigoto/旧版一致，
// D3D11 纹理创建回到渲染线程，规避 NVIDIA 驱动对后台线程建纹理的缺陷。
// 注意：调用方不得持有 g_lock（内部会取锁登记）。
// ---------------------------------------------------------------------------
static void ExecuteLoadTask(const AsyncLoadTask &task)
{
    ID3D11Device *dev = g_device;
    if (!dev || !task.loader)
        return;
    // 检查是否仍需要加载（可能已被其他实例抢先加载）
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_replacements.find(task.hash);
        if (it == g_replacements.end() || it->second.texture)
            return; // 已加载或条目已移除
    }
    TextureLoadResult res;
    HRESULT lhr;
    {
        // 2026-09-22：原为手工置位/复位，load() 抛异常会让该线程
        // 永久处于重入态 → 之后所有纹理创建都绕过替换（静默失效）。改 RAII。
        CreateTextureReentryGuard reentry_guard; // 防止加载内部重入（建纹理）
        lhr = task.loader->load(dev, task.path.c_str(), &res);
    }
    if (SUCCEEDED(lhr) && res.texture) {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_replacements.find(task.hash);
        if (it != g_replacements.end() && !it->second.texture) {
            it->second.texture = res.texture;
            it->second.ready_fence = res.ready_fence; // GDDS 完成 fence（渲染线程等待）
            // 真实显存占用估算（BC 按块/其余按像素，含 mip 链与数组）——淘汰按大小优先
            it->second.mem_bytes = EstimateTextureMemory(res.format, res.width, res.height,
                                                         res.mip_levels, res.array_size);
            // 加载即视为活动：last_used 初始化当前时间——刚加载未绑定的缓存
            // 不会立即被常态回收（防加载-回收抖动）；长期未绑定的自然老化排最前
            it->second.last_used = GetTickCount64();
            g_cacheBytesTotal.fetch_add(it->second.mem_bytes, std::memory_order_relaxed);
            IdleIndexUpdate(task.hash, it->second.last_used); // 可淘汰候选入索引
            TL_LOG_IF(1, L"[HIT ] tex hash=0x%08X %lsreplaced dds=%ls",
                   task.hash, ::tloader::g_async_load ? L"(async) " : L"",
                   task.path.c_str());
        } else {
            // 锁内不直接 Release（D3D 回调可能反向等 g_lock）——延迟到渲染线程
            QueueRelease(res.texture);
        }
    } else if (res.skipped) {
        // max_texture_side 配置跳过（非错误）——仅 level>=1 记录
        TL_LOG_IF(1, L"[skip] tex hash=0x%08X dds over max_texture_side", task.hash);
    } else {
        TL_LOG(L"[fail] tex hash=0x%08X load failed hr=0x%08X (%ls)",
               task.hash, lhr, task.path.c_str());
    }
}

// ---------------------------------------------------------------------------
// 通用 SRV 替换 hook（应用于全部着色器阶段）
// 飘带/布料模拟可能在 VS/CS 阶段采样纹理，只替换 PS 会导致阶段间数据不一致
// （VS 读原纹理、PS 读替换纹理 → 摆动异常 + 闪烁）。因此所有阶段统一替换。
// ---------------------------------------------------------------------------

typedef void(STDMETHODCALLTYPE *SetShaderResources_t)(ID3D11DeviceContext *,
    UINT, UINT, ID3D11ShaderResourceView *const *);

// 每个阶段独立的原函数指针（Detours 按地址挂钩）
SetShaderResources_t g_real_ssr[6] = {nullptr};
static const wchar_t *g_stage_name[6] = {
    L"PS", L"VS", L"GS", L"HS", L"DS", L"CS"
};

// [bind] 等级 2 诊断的"每个 hash 只记首次"状态（2026-09-24）。
// 见 HookSetShaderResourcesCommon 内的说明：原来每次绑定都记一行，
// 实机 140 秒 899,670 行 / 75 MB，且 log.cpp 每行 fflush ⇒ 渲染线程被写盘拖住。
static std::mutex g_bind_log_mutex;
static std::set<uint32_t> g_bind_logged;
static std::atomic<uint64_t> g_bind_log_suppressed{0};

static void STDMETHODCALLTYPE HookSetShaderResourcesCommon(
    ID3D11DeviceContext *This,
    UINT StartSlot, UINT NumViews,
    ID3D11ShaderResourceView *const *ppShaderResourceViews,
    SetShaderResources_t real,
    int stageIdx)
{
    // 渲染线程批量释放：非渲染线程排队的 D3D11 对象在此释放，与游戏提交串行
    // （避免 AMD 驱动 worker 在对象释放后仍引用 → 快速切换角色崩溃）。
    FlushPendingRelease();

    // 极速通道：未启用替换（g_activeCount==0）时，单条内存读直接透传。
    // 这是 93万次/秒调用下成本最低的形态：1 次 volatile 读 + 1 次间接跳转。
    if (g_activeCount == 0) {
        real(This, StartSlot, NumViews, ppShaderResourceViews);
        return;
    }

    // 有活跃替换资源：遍历判断本次绑定是否含替换资源。
    // 两级无锁判定（均不解引用/无 COM 调用）：
    //   1) SRV 指针即替换视图（我们创建过）→ 无需处理，直接透传
    //   2) 否则 GetResource 查原纹理是否在 activeArr（需替换）
    // 对"反复绑定替换 SRV"的常见场景省掉每次 GetResource/Release COM 调用。
    bool anyHit = false;
    for (UINT i = 0; i < NumViews; i++) {
        ID3D11ShaderResourceView *srv = ppShaderResourceViews[i];
        if (!srv)
            continue;
        if (RepSrvContains(srv))
            continue; // 替换视图：无需再替换
        ID3D11Resource *res = nullptr;
        srv->GetResource(&res);
        if (!res)
            continue;
        if (ActiveContains(res)) {
            anyHit = true;
            res->Release();
            break;
        }
        res->Release();
    }

    // 本绑定无替换资源 → 直接透传
    if (!anyHit) {
        real(This, StartSlot, NumViews, ppShaderResourceViews);
        return;
    }

    // 慢路径：拷贝数组，逐项替换
    ID3D11ShaderResourceView **views =
        (ID3D11ShaderResourceView **)_malloca(sizeof(void *) * NumViews);
    memcpy(views, ppShaderResourceViews, sizeof(void *) * NumViews);
    // 2026-11-23：这里原本有一段 TTL 抑制判定（窗口值 + 本帧 GetTickCount64 + 逐 slot
    // 读私有数据 + 恢复时清零并打 [upd] 日志），随 update_suppress_ms 一起删除。
    // 删它的依据是实机实测：该 API 107 秒被调用 166,018 次，命中替换集 **0 次**，
    // 因此「游戏会更新一张已被替换的纹理」这个前提在本游戏版本上不成立，这整段
    // 从来没有执行过有效分支（只承担了每 slot 一次 GetPrivateData 的代价）。
    // 现在的语义回到最简单的一条：hash 命中就替换，不看这张纹理是否被更新过。
    // 为什么这不会退回到「永久失效」：判断依据不再是任何资源级标记，而是「有没有命中
    // 替换表」—— 替换表由 mod 驱动，与游戏怎么写纹理无关。
    for (UINT i = 0; i < NumViews; i++) {
        ID3D11ShaderResourceView *srv = views[i];
        if (!srv)
            continue;
        ID3D11Resource *res = nullptr;
        srv->GetResource(&res);
        if (!res)
            continue;
        uint32_t hash = 0;
        if (!GetResourceHash(res, &hash))
            hash = 0;
        if (hash) {
            // 一次锁内判定：条目存在且纹理已加载 → 建 SRV；
            // 条目存在但纹理为空（显存淘汰）→ 重新异步加载；
            // 条目不存在（原纹理全销毁）→ 跳过（保留原纹理）。
            // （移除独立 g_hasReplacement 集：g_replacements.find 即判定，
            //   省一次 shared_lock + unordered_set 查询。）
            bool need_reload = false;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                auto it = g_replacements.find(hash);
                if (it != g_replacements.end() && !it->second.texture)
                    need_reload = true;
            }
            if (need_reload) {
                // 重新异步加载被淘汰的替换纹理
                std::wstring dds_path;
                {
                    auto oit = g_overrides.find(hash);
                    if (oit != g_overrides.end())
                        dds_path = oit->second.dds_path;
                }
                if (!dds_path.empty())
                    EnqueueLoad(hash, std::move(dds_path));
            } else {
                // 读取原 SRV 的视图描述，创建格式/mip 一致的替换 SRV
                D3D11_SHADER_RESOURCE_VIEW_DESC origDesc;
                srv->GetDesc(&origDesc);
                ID3D11ShaderResourceView *rep = CreateReplacementSRV(hash, &origDesc);
                if (rep) {
                    views[i] = rep;
                    InterlockedIncrement(&g_stats_bound);
                    // 2026-09-22：`stageIdx` 形参与 `g_stage_name[]`
                    // 此前都是**死代码**（形参从未使用、名字表从未被引用）。
                    // 这里把它们用于**等级 2** 的绑定诊断。
                    //
                    // ⚠️ 2026-09-24：原实现在**每次绑定**都记一行，注释断言
                    // "只有排查时才打开，因此不影响热路径成本" —— **该判断不成立**：
                    // 实机日志 140 秒产出 899,670 行 / 75 MB（≈6,400 行/秒），
                    // 而 log.cpp 是**每行 fflush** ⇒ 渲染线程每秒上万次写盘系统调用。
                    // 现改为**每个 hash 只记首次绑定**（替换条目有限 ⇒ 行数有限），
                    // 已足够回答"替换是在哪个着色器阶段生效的"；被抑制的行数计入
                    // [stat] 的 bind_log_suppressed，**不会静默丢失**。
                    bool first_bind = false;
                    {
                        std::lock_guard<std::mutex> lk(g_bind_log_mutex);
                        first_bind = g_bind_logged.insert(hash).second;
                    }
                    if (first_bind) {
                        const wchar_t *stage = (stageIdx >= 0 && stageIdx < 6)
                            ? g_stage_name[stageIdx] : L"?";
                        TL_LOG_IF(2, L"[bind] stage=%ls slot=%u hash=0x%08X -> replacement SRV",
                                  stage, StartSlot + i, hash);
                    } else {
                        g_bind_log_suppressed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
        res->Release();
    }

    real(This, StartSlot, NumViews, views);

    // 释放我们 AddRef 过的替换 SRV 引用，避免引用计数无限增长
    for (UINT i = 0; i < NumViews; i++) {
        if (views[i] && views[i] != ppShaderResourceViews[i])
            views[i]->Release();
    }
    _freea(views);
}

// 六个阶段的薄包装：转发给通用实现 + 各自的原函数指针
static void STDMETHODCALLTYPE Hook_PS(ID3D11DeviceContext *t, UINT a, UINT b,
    ID3D11ShaderResourceView *const *c) { HookSetShaderResourcesCommon(t, a, b, c, g_real_ssr[0], 0); }
static void STDMETHODCALLTYPE Hook_VS(ID3D11DeviceContext *t, UINT a, UINT b,
    ID3D11ShaderResourceView *const *c) { HookSetShaderResourcesCommon(t, a, b, c, g_real_ssr[1], 1); }
static void STDMETHODCALLTYPE Hook_GS(ID3D11DeviceContext *t, UINT a, UINT b,
    ID3D11ShaderResourceView *const *c) { HookSetShaderResourcesCommon(t, a, b, c, g_real_ssr[2], 2); }
static void STDMETHODCALLTYPE Hook_HS(ID3D11DeviceContext *t, UINT a, UINT b,
    ID3D11ShaderResourceView *const *c) { HookSetShaderResourcesCommon(t, a, b, c, g_real_ssr[3], 3); }
static void STDMETHODCALLTYPE Hook_DS(ID3D11DeviceContext *t, UINT a, UINT b,
    ID3D11ShaderResourceView *const *c) { HookSetShaderResourcesCommon(t, a, b, c, g_real_ssr[4], 4); }
static void STDMETHODCALLTYPE Hook_CS(ID3D11DeviceContext *t, UINT a, UINT b,
    ID3D11ShaderResourceView *const *c) { HookSetShaderResourcesCommon(t, a, b, c, g_real_ssr[5], 5); }

// 六个阶段的钩子入口表。提为文件级，供 HookContext 安装与 UninstallHooks 卸载共用。
// 旧实现把这张表放在 HookContext 函数内（局部数组），卸载时无从取得 →
// 六个 SetShaderResources 钩子**无法 DetourDetach**，DLL 卸载后游戏仍会跳进
// 已解除映射的 trampoline。
static SetShaderResources_t const g_stage_hooks[6] = {Hook_PS, Hook_VS, Hook_GS, Hook_HS, Hook_DS, Hook_CS};

// ---------------------------------------------------------------------------
// Hook: UpdateSubresource —— 只计数，**不改任何替换行为**（2026-11-23 重写）
// ---------------------------------------------------------------------------
//
// ---- 为什么删掉 TTL 抑制（回滚的决定依据是实机实测，不是推测）----
// 2026-11-22 曾把它改成「TTL 抑制」：把最后一次更新时间戳写进原纹理私有数据，
// 绑定慢路径在窗口 (update_suppress_ms) 内跳过替换、窗口过后自动恢复。
// 2026-11-23 实机（原神，`update_suppress_ms=5000`，8 张 2048×2048 场景贴图已替换）实测：
//   [stat] hashed=28019 matched=19 bound=11732 active=8 upd_suppressed=0 upd_recovered=0
//          upd_calls=166018 copy_calls=37054          （107 秒）
// ⇒ 游戏 107 秒调用 UpdateSubresource **166,018 次**（约 1550 次/秒），
//   其中命中替换集 **0 次**。该功能**没有作用对象**。
// 机制原因（唯一哈希入口，见 HookCreateTexture2D）：只有**带初始数据**的纹理才被哈希、
// 才会进替换集（它们是加载期创建的静态纹理）；而游戏用 UpdateSubresource 更新的纹理
// 是运行期创建、无初始数据的，从没被哈希过 ⇒ 两个集合不相交。
//
// ---- 为什么不标记（三条理由）----
// 1. **不标记 ⇒ 替换在更新之后保持**。旧的 MarkDynamic 走 SetPrivateData 打在**原纹理**
//    上，而 D3D11 私有数据没有删除语义（只能覆盖、不能删）⇒ 标记**不可撤销**，绑定路径
//    的 IsDynamic() 于是**永久**跳过该纹理 ⇒ 替换永久失效。这正是探针 G / G′ 测到的缺陷
//    （G = HEAD 测得蓝、G′ = 2.2.0 测得蓝；`f07bb04` 只修了 CopyResource 一处）。
//    删掉标记是**唯一**能保证「更新之后替换仍在」的形态。
// 2. **实测该分支从不命中**：166,018 次调用、0 次命中替换集 ⇒ 标记与否对画面**零影响**，
//    但「不标记」避免了永久失效的潜在风险。即使将来某个游戏/场景真的会更新被替换的纹理，
//    不标记的后果也只是「短暂显示过期内容」，而不是「这张贴图的 mod 永久不生效」。
// 3. 与 `f07bb04` 对 CopyResource 的取舍一致：**内容可能过期 ≪ 替换永久失效**。
//    同一产品两处同类事件不该给相反的取舍 —— 而 HEAD 时期恰恰就是相反的。
//
// ---- 与 `f07bb04` 的关系 ----
// `f07bb04` 只把 CopyResource 那条路径改成「不标记」，UpdateSubresource 这条一直是
// 旧形态（永久标记）。本条把它对齐，两条路径现在同构：钩子只计数、零行为改动。
//
// ---- 保留钩子的唯一理由 ----
// 低成本判别「实机上这条路径到底会不会发生」（计数见 g_update_subresource_calls）。
// 它**不影响任何替换行为**，也不写任何私有数据。
//
// ⚠️ 2026-11-23 实测限制（诚实记录，未变）：本钩子只覆盖
// ID3D11DeviceContext::UpdateSubresource。UpdateSubresource1（ID3D11DeviceContext1）与
// CopySubresourceRegion **未 hook** ⇒ 走这两条入口的写入连计数都拿不到。
// 游戏是否用到它们**未核实**（缺游戏侧调用证据）。
// ⚠️ 计数本身的 CPU 代价**未量化**：实测约 1550 次/秒，每次一个 relaxed 原子自增。
// 要量化必须用无诊断构建 + 外部帧时间工具（AGENTS.md §二.5 禁止在热路径加计时探针）。

typedef void(STDMETHODCALLTYPE *UpdateSubresource_t)(ID3D11DeviceContext *,
    ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
UpdateSubresource_t g_real_update_subresource = nullptr;

static void STDMETHODCALLTYPE HookUpdateSubresource(
    ID3D11DeviceContext *This,
    ID3D11Resource *pDstResource,
    UINT DstSubresource,
    const D3D11_BOX *pDstBox,
    const void *pSrcData,
    UINT SrcRowPitch,
    UINT SrcDepthPitch)
{
    // 只计数，不改行为（2026-11-23）。
    // 计数放在最前、无条件：它回答「这个 API 被调用了吗」，与配置无关（本钩子现在
    // 也没有任何配置键）。删掉其它判定之后，AGENTS.md §二.6 的「最便宜的检查放最前」
    // 自然满足 —— 这里只剩一次 relaxed 原子自增加一次转发。
    g_update_subresource_calls.fetch_add(1, std::memory_order_relaxed);
    g_real_update_subresource(This, pDstResource, DstSubresource, pDstBox,
                              pSrcData, SrcRowPitch, SrcDepthPitch);
}

// ---------------------------------------------------------------------------
// Hook: CopyResource —— 保留钩子，但**不再**标记动态（2026-11-22，有意取舍）
// ---------------------------------------------------------------------------
//
// 钩子保留的唯一目的：低成本判别「实机上这条路径到底会不会发生」（计数见
// copyresource_hit_calls）。它**不再**影响任何替换行为。
//
// ---- 为什么保留钩子，却不再 MarkDynamic ----
// 2026-09-23 加这个钩子的意图是防「替换纹理过期」：纹理创建时被我们哈希并替换，
// 之后游戏用 CopyResource 覆写它的内容；若不标记动态，绑定时仍会替换
// ⇒ 画面与游戏状态不一致。该意图本身没错，错的是选的手段：
// MarkDynamic() 经 SetPrivateData 把标记打在**原纹理**上，而 D3D11 的私有数据
// **不可撤销**（SetPrivateData 只能覆盖，没有删除语义），绑定慢路径的 IsDynamic()
// 于是**永久**跳过该纹理的替换。结果与意图正好相反：不是「不再过期」，
// 而是**替换被永久放弃**。
//
// ---- 失效形态 ----
// 该原纹理从第一次 CopyResource 起就不再被替换，直到这张纹理被销毁重建为止。
// 玩家看到的就是「这张贴图的 mod 不生效」。
// 而日志里**没有任何痕迹**：MarkDynamic 只写资源私有数据、本身不打日志，绑定热路径
// 又是无日志的快速判定 —— 这正是该问题长期被误判成「N 卡驱动的纹理创建/上载时机」的原因。
//
// ---- 与 25678cc 的原注释相反，这是【有意】取舍，不要改回去 ----
// 原注释断言「不标记动态 ⇒ 替换过期」。该断言的前提成立，但它默认了「过期」比
// 「永久不替换」轻；实测结论相反：过期只是内容不跟手，永久不替换是 mod 整体失效。
// 可验证依据（同一台机器、同一份 Mod、GPU 像素读回，A/B 两个构建）：
//   · 含本钩子（HEAD 2.5.0.0）：CopyResource 之后读回**游戏原色** ⇒ 替换丢失；
//   · 无本钩子（2.2.0）：同样操作后读回 **mod 颜色** ⇒ 替换保持；
//   · 对照：不做 CopyResource 时两个构建都保持替换；observe_only=1 时不替换
//     ⇒ 读回链路本身不是「恒绿」，红色确实来自替换丢失。
// 若要重新引入「跳过替换」，必须改用**可撤销**的判据
// （例如按内容版本号在有限时间内过期），不能再用不可撤销的资源级标记。
//
// ---- 注意：想按 desc 对账时，Usage 会改哈希 ----
// CalcTexture2DDescHash() 对**整个** D3D11_TEXTURE2D_DESC 做 CRC-32C
//（texture_hash.cpp：crc32c_hw(data_hash, pDesc, sizeof(D3D11_TEXTURE2D_DESC))），
// Usage 字段就在被哈希的范围内 ⇒ 「改 Usage 再比哈希」必然对不上：同一张纹理
// 的 IMMUTABLE 与 DEFAULT 哈希并不相同（实测 0x63EAE7C3 / 0x91E6EA3D）。
// 要对账只能用**实际提交创建时的那份 desc** 重算。
//
// 为什么**不**同时 hook `Map`/`Unmap`（沿用 2026-09-23 的结论，未变）：
// 纹理经 `Map` 更新要求 `D3D11_USAGE_DYNAMIC`/`STAGING`，而 `HookCreateTexture2D`
// 只对**带初始数据**的纹理算哈希 —— 这类纹理极少同时满足；反观 `Map`/`Unmap` 是
// **极热路径**（每帧大量常量缓冲/顶点缓冲都走它），为覆盖罕见情形而给它加一次
// 线性扫描不划算。该限制在此显式记录。

typedef void(STDMETHODCALLTYPE *CopyResource_t)(ID3D11DeviceContext *,
    ID3D11Resource *, ID3D11Resource *);
CopyResource_t g_real_copy_resource = nullptr;

static void STDMETHODCALLTYPE HookCopyResource(
    ID3D11DeviceContext *This,
    ID3D11Resource *pDstResource,
    ID3D11Resource *pSrcResource)
{
    // 无条件计数（2026-11-23）：与 upd_calls 同口径 —— 总调用次数，与 dst 是否在活跃集无关。
    // 下面 copyresource_hit_calls 是「命中活跃集」的次数，两者相除才知道命中率。
    g_copy_resource_calls.fetch_add(1, std::memory_order_relaxed);
    // 先无锁判空（替换未启用时零开销）。本分支**不再**标记动态、也不再移出活跃数组：
    // 见上方注释 —— 那个标记不可撤销，会让该纹理永久失去替换。
    // 这里只累计 copyresource_hit_calls，口径见其声明处：**仅** ActiveContains 通过才计，
    // 不是 CopyResource 调用总数、也不是被触及的纹理数（同一资源每帧可重复计入）。
    if (pDstResource && g_activeCount != 0) {
        if (ActiveContains(pDstResource)) {
            // relaxed 即可：只要求增量不丢，不需要与其他变量建立可见性顺序（AGENTS.md §二.4）。
            const long n = copyresource_hit_calls.fetch_add(1, std::memory_order_relaxed) + 1;
            // 限流：首次与每 1024 次各打一行（与 ActiveAdd 的 dropped 同写法）。
            // **不能**每次命中都打 —— 本钩子可能每帧命中多次，而 log.cpp 每行 fflush，
            // 那就是渲染线程上每秒上万次写盘系统调用（同文件 [bind] 行 2026-09-24 的实测教训）。
            if (n == 1 || (n % 1024) == 0)
                TL_LOG(L"[copy] CopyResource hit on replacement dst: n=%ld (cumulative, not call count)", n);
        }
    }
    g_real_copy_resource(This, pDstResource, pSrcResource);
}

// ---------------------------------------------------------------------------
// vtable hook 安装
// ---------------------------------------------------------------------------
// 槽位号一律不硬编码：通过下面的镜像 vtable 结构体按成员名取函数指针，
// 由编译器计算偏移，版本迭代（d3d11.h 不变更 ABI）依然安全。

// ---------------------------------------------------------------------------
// 镜像 vtable 结构体：按成员名访问（编译期定偏移，零硬编码槽位号）
// 布局与 d3d11.h 的 ID3D11DeviceContext 虚函数声明顺序一致。
// ---------------------------------------------------------------------------

struct D3D11DeviceContextVtblMirror
{
    // IUnknown
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D11DeviceContext *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ID3D11DeviceContext *);
    ULONG   (STDMETHODCALLTYPE *Release)(ID3D11DeviceContext *);
    // ID3D11DeviceChild
    void    (STDMETHODCALLTYPE *GetDevice)(ID3D11DeviceContext *, ID3D11Device **);
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(ID3D11DeviceContext *, REFGUID, UINT *, void *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(ID3D11DeviceContext *, REFGUID, UINT, const void *);
    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(ID3D11DeviceContext *, REFGUID, const IUnknown *);
    // ID3D11DeviceContext
    void (STDMETHODCALLTYPE *VSSetConstantBuffers)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
    void (STDMETHODCALLTYPE *PSSetShaderResources)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
    void (STDMETHODCALLTYPE *PSSetShader)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
    void (STDMETHODCALLTYPE *PSSetSamplers)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
    void (STDMETHODCALLTYPE *VSSetShader)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
    void (STDMETHODCALLTYPE *DrawIndexed)(ID3D11DeviceContext *, UINT, UINT, INT);
    void (STDMETHODCALLTYPE *Draw)(ID3D11DeviceContext *, UINT, UINT);
    HRESULT (STDMETHODCALLTYPE *Map)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
    void (STDMETHODCALLTYPE *Unmap)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
    void (STDMETHODCALLTYPE *PSSetConstantBuffers)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
    void (STDMETHODCALLTYPE *IASetInputLayout)(ID3D11DeviceContext *, ID3D11InputLayout *);
    void (STDMETHODCALLTYPE *IASetVertexBuffers)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *, const UINT *, const UINT *);
    void (STDMETHODCALLTYPE *IASetIndexBuffer)(ID3D11DeviceContext *, ID3D11Buffer *, DXGI_FORMAT, UINT);
    void (STDMETHODCALLTYPE *DrawIndexedInstanced)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
    void (STDMETHODCALLTYPE *DrawInstanced)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
    void (STDMETHODCALLTYPE *GSSetConstantBuffers)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
    void (STDMETHODCALLTYPE *GSSetShader)(ID3D11DeviceContext *, ID3D11GeometryShader *, ID3D11ClassInstance *const *, UINT);
    void (STDMETHODCALLTYPE *IASetPrimitiveTopology)(ID3D11DeviceContext *, D3D11_PRIMITIVE_TOPOLOGY);
    void (STDMETHODCALLTYPE *VSSetShaderResources)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
    void (STDMETHODCALLTYPE *VSSetSamplers)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
    void (STDMETHODCALLTYPE *Begin)(ID3D11DeviceContext *, ID3D11Asynchronous *);
    void (STDMETHODCALLTYPE *End)(ID3D11DeviceContext *, ID3D11Asynchronous *);
    HRESULT (STDMETHODCALLTYPE *GetData)(ID3D11DeviceContext *, ID3D11Asynchronous *, void *, UINT, UINT);
    void (STDMETHODCALLTYPE *SetPredication)(ID3D11DeviceContext *, ID3D11Predicate *, BOOL);
    void (STDMETHODCALLTYPE *GSSetShaderResources)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
    void (STDMETHODCALLTYPE *GSSetSamplers)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
    void (STDMETHODCALLTYPE *OMSetRenderTargets)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
    void (STDMETHODCALLTYPE *OMSetRenderTargetsAndUnorderedAccessViews)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
    void (STDMETHODCALLTYPE *OMSetBlendState)(ID3D11DeviceContext *, ID3D11BlendState *, const FLOAT *, UINT);
    void (STDMETHODCALLTYPE *OMSetDepthStencilState)(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);
    void (STDMETHODCALLTYPE *SOSetTargets)(ID3D11DeviceContext *, UINT, ID3D11Buffer *const *, const UINT *);
    void (STDMETHODCALLTYPE *DrawAuto)(ID3D11DeviceContext *);
    void (STDMETHODCALLTYPE *DrawIndexedInstancedIndirect)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
    void (STDMETHODCALLTYPE *DrawInstancedIndirect)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
    void (STDMETHODCALLTYPE *Dispatch)(ID3D11DeviceContext *, UINT, UINT, UINT);
    void (STDMETHODCALLTYPE *DispatchIndirect)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
    void (STDMETHODCALLTYPE *RSSetState)(ID3D11DeviceContext *, ID3D11RasterizerState *);
    void (STDMETHODCALLTYPE *RSSetViewports)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
    void (STDMETHODCALLTYPE *RSSetScissorRects)(ID3D11DeviceContext *, UINT, const D3D11_RECT *);
    void (STDMETHODCALLTYPE *CopySubresourceRegion)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
    void (STDMETHODCALLTYPE *CopyResource)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
    void (STDMETHODCALLTYPE *UpdateSubresource)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
    void (STDMETHODCALLTYPE *CopyStructureCount)(ID3D11DeviceContext *, ID3D11Buffer *, UINT, ID3D11UnorderedAccessView *);
    void (STDMETHODCALLTYPE *ClearRenderTargetView)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT *);
    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewUint)(ID3D11DeviceContext *, ID3D11UnorderedAccessView *, const UINT *);
    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewFloat)(ID3D11DeviceContext *, ID3D11UnorderedAccessView *, const FLOAT *);
    void (STDMETHODCALLTYPE *ClearDepthStencilView)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
    void (STDMETHODCALLTYPE *GenerateMips)(ID3D11DeviceContext *, ID3D11ShaderResourceView *);
    void (STDMETHODCALLTYPE *SetResourceMinLOD)(ID3D11DeviceContext *, ID3D11Resource *, FLOAT);
    FLOAT (STDMETHODCALLTYPE *GetResourceMinLOD)(ID3D11DeviceContext *, ID3D11Resource *);
    void (STDMETHODCALLTYPE *ResolveSubresource)(ID3D11DeviceContext *, ID3D11Resource *, UINT, ID3D11Resource *, UINT, DXGI_FORMAT);
    void (STDMETHODCALLTYPE *ExecuteCommandList)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
    void (STDMETHODCALLTYPE *HSSetShaderResources)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
    void (STDMETHODCALLTYPE *HSSetShader)(ID3D11DeviceContext *, ID3D11HullShader *, ID3D11ClassInstance *const *, UINT);
    void (STDMETHODCALLTYPE *HSSetSamplers)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
    void (STDMETHODCALLTYPE *HSSetConstantBuffers)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
    void (STDMETHODCALLTYPE *DSSetShaderResources)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
    void (STDMETHODCALLTYPE *DSSetShader)(ID3D11DeviceContext *, ID3D11DomainShader *, ID3D11ClassInstance *const *, UINT);
    void (STDMETHODCALLTYPE *DSSetSamplers)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
    void (STDMETHODCALLTYPE *DSSetConstantBuffers)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
    void (STDMETHODCALLTYPE *CSSetShaderResources)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
};

// ID3D11Device 的镜像 vtable（按成员名访问，编译期定偏移）
struct D3D11DeviceVtblMirror
{
    // IUnknown
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D11Device *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ID3D11Device *);
    ULONG   (STDMETHODCALLTYPE *Release)(ID3D11Device *);
    // ID3D11Device
    HRESULT (STDMETHODCALLTYPE *CreateBuffer)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
    HRESULT (STDMETHODCALLTYPE *CreateTexture1D)(ID3D11Device *, const D3D11_TEXTURE1D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture1D **);
    HRESULT (STDMETHODCALLTYPE *CreateTexture2D)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
};

static void HookDevice(ID3D11Device *device)
{
    // 通过镜像 vtable 按成员名取函数指针，不硬编码槽位号
    const auto *vtbl = *(D3D11DeviceVtblMirror *const *)device;
    RealCreateTexture2D = (CreateTexture2D_t)vtbl->CreateTexture2D;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)RealCreateTexture2D, HookCreateTexture2D);
    DetourTransactionCommit();
    TL_LOG(L"[ok  ] hooked CreateTexture2D @ %p", RealCreateTexture2D);
}

static void HookContext(ID3D11DeviceContext *context)
{
    // 通过镜像 vtable 按成员名取函数指针，杜绝硬编码槽位号错误
    const auto *vtbl = *(D3D11DeviceContextVtblMirror *const *)context;

    SetShaderResources_t funcs[6] = {
        (SetShaderResources_t)vtbl->PSSetShaderResources, // PS
        (SetShaderResources_t)vtbl->VSSetShaderResources, // VS
        (SetShaderResources_t)vtbl->GSSetShaderResources, // GS
        (SetShaderResources_t)vtbl->HSSetShaderResources, // HS
        (SetShaderResources_t)vtbl->DSSetShaderResources, // DS
        (SetShaderResources_t)vtbl->CSSetShaderResources, // CS
    };
    for (int i = 0; i < 6; i++)
        g_real_ssr[i] = funcs[i];

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (int i = 0; i < 6; i++)
        DetourAttach(&(PVOID &)g_real_ssr[i], g_stage_hooks[i]);
    // UpdateSubresource：只计数（2026-11-23 起不写私有数据、不改行为，见其上方注释）
    g_real_update_subresource = (UpdateSubresource_t)vtbl->UpdateSubresource;
    DetourAttach(&(PVOID &)g_real_update_subresource, HookUpdateSubresource);
    // CopyResource：同上（2026-09-23 补，见 HookCopyResource 的说明）
    g_real_copy_resource = (CopyResource_t)vtbl->CopyResource;
    DetourAttach(&(PVOID &)g_real_copy_resource, HookCopyResource);
    DetourTransactionCommit();
    for (int i = 0; i < 6; i++)
        TL_LOG(L"[ok  ] hooked %ls.SetShaderResources @ %p", g_stage_name[i], g_real_ssr[i]);
    TL_LOG(L"[ok  ] hooked UpdateSubresource @ %p", g_real_update_subresource);
    TL_LOG(L"[ok  ] hooked CopyResource @ %p", g_real_copy_resource);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

HMODULE g_self = nullptr;

} // namespace

static bool InstallCreateDeviceHook();

// 后台线程：d3d11 尚未加载时紧轮询安装 hook（兜底路径，避免错过设备创建）
DWORD WINAPI BootstrapThread(LPVOID)
{
    for (int i = 0; i < 4000; ++i) {
        if (g_hook_ready.load(std::memory_order_acquire))
            return 0;
        if (InstallCreateDeviceHook())
            return 0;
        Sleep(5);
    }
    TL_LOG(L"[warn] bootstrap gave up waiting for d3d11.dll");
    return 0;
}

// 后台替换纹理加载线程（两条：idx 0=GDDS，idx 1=DDS）：
// 消费对应队列，经统一加载器注册表执行加载，完成后更新替换表。
// 渲染线程只做轻量登记，避免切换角色时的帧时间尖峰。
// GDDS/DDS 各自独立消费——大 GDDS 任务不阻塞 DDS 小纹理；登记共享 g_lock。
DWORD WINAPI AsyncLoadThread(LPVOID param)
{
    const int idx = (int)(intptr_t)param;
    while (true) {
        AsyncLoadTask task;
        {
            std::unique_lock<std::mutex> lk(g_loadMutex);
            // 停机信号也唤醒等待：否则线程会永远卡在 cv.wait 上，
            // detach 时的 WaitForSingleObject 就会一直等到超时。
            g_loadCv[idx].wait(lk, [idx] {
                return !g_loadQueue[idx].empty() || g_shutdownRequested.load(std::memory_order_acquire);
            });
            if (g_loadQueue[idx].empty() && g_shutdownRequested.load(std::memory_order_acquire))
                return 0;   // 队列已排空且要求停机 → 退出
            task = std::move(g_loadQueue[idx].front());
            g_loadQueue[idx].pop_front();
            g_inflight.insert(task.hash); // 在途标记：同 hash 新任务不再入队
        }
        ExecuteLoadTask(task);
        {
            std::lock_guard<std::mutex> lk(g_loadMutex);
            g_inflight.erase(task.hash); // 完成（成功/失败/跳过统一解除）
        }
    }
    return 0;
}


// ---------------------------------------------------------------------------
// v2 显存回收：四分区状态机 + 压力系数驱动 + 防抖动
//
//   舒适区（Comfort）   avail > 总显存×25%          不主动回收（仅缓存超预算时轻度回收）
//   温和区（Mild）      C% < avail ≤ 25%            轻度回收（闲置 10min，每轮 2~4 条）
//   压力区（Pressure）  C%×0.5 < avail ≤ C%         中度回收（闲置 2~5min，每轮 8~16 条）
//   危急区（Critical）  avail ≤ C%×0.5              强力回收（缺口自适应，闲置 ≥30s，每轮 ≤32）
//
//   C = 临界百分比，随显存容量自适应（小卡留更多余量）：
//       C = clamp(20 − (总GB − 4)×0.35, 8, 20) %
//       4GB→20%  8GB→18.6%  16GB→15.8%  24GB→13%
//       用户显式 vram_threshold（百分比或容量）优先覆盖 C。
//
//   压力系数 P（0..1）驱动全部回收参数：
//       P = clamp((25%总显存 − avail) / (25%总显存 − C), 0, 1)
//       闲置阈值 T = 10min → 30s 线性；每轮 N = 2 → 32 条线性
//       排序主序：P<0.5 LRU 优先（最久未用）；P≥0.5 大纹理优先（快速释放）
//
//   防抖动：
//       - 热缓存豁免：淘汰后 30s 内被重新加载 → 豁免回收 5 分钟
//       - 缓存总量预算：替换缓存 > 总显存×20% 时舒适区也回收（闲置最久优先）
//       - 绝不淘汰使用中（refcount>0）
// ---------------------------------------------------------------------------

// 淘汰/热豁免记录（g_lock 保护）：hash -> 时间戳
struct EvictRecord {
    uint64_t evict_time = 0;  // 最近淘汰时刻（GetTickCount64）
    uint64_t hot_until = 0;   // 热缓存豁免截止（0=未豁免）
};
static std::unordered_map<uint32_t, EvictRecord> g_evictRecords;

DWORD WINAPI VramMonitorThread(LPVOID)
{
    // 从 device 获取 DXGI adapter3 用于查询显存
    if (!g_device)
        return 0;
    IDXGIDevice *dxgi_dev = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev))) {
        IDXGIAdapter *adapter = nullptr;
        if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter))) {
            if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&g_dxgi_adapter3)))
                g_dxgi_adapter3 = nullptr;
            adapter->Release();
        }
        dxgi_dev->Release();
    }

    // 缓存总量预算（总显存 20%）
    const uint64_t kCacheBudgetPct = 20;

    enum class Zone { Comfort, Mild, Pressure, Critical };
    Zone zone = Zone::Comfort;
    Zone last_zone = Zone::Comfort;
    uint64_t last_record_cleanup = 0;

    while (g_dxgi_adapter3 && !g_shutdownRequested.load(std::memory_order_acquire)) {
        // 状态化动态探针频率：按当前 zone 定频（压力越小探针越稀疏，低开销）
        DWORD sleep_ms = 5000;
        switch (last_zone) {
            case Zone::Comfort:  sleep_ms = 5000; break;
            case Zone::Mild:     sleep_ms = 3000; break;
            case Zone::Pressure: sleep_ms = 1500; break;
            case Zone::Critical: sleep_ms = 800;  break;
        }
        Sleep(sleep_ms);

        DXGI_QUERY_VIDEO_MEMORY_INFO info;
        if (FAILED(g_dxgi_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            continue;

        uint64_t total = g_total_vram ? g_total_vram : info.Budget; // 物理显存基准
        uint64_t avail = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;

        // 临界可用显存 C（用户显式设置优先，否则规格自适应）
        uint64_t critical_avail = 0;
        if (::tloader::g_vram_threshold_bytes > 0) {
            critical_avail = ::tloader::g_vram_threshold_bytes;
        } else {
            int pct = ::tloader::g_vram_threshold_pct;
            if (pct <= 0) {
                // 自适应：C = clamp(20 - (GB-4)*0.35, 8, 20)
                double gb = (double)total / (1024.0 * 1024.0 * 1024.0);
                double c = 20.0 - (gb - 4.0) * 0.35;
                if (c < 8.0) c = 8.0;
                if (c > 20.0) c = 20.0;
                pct = (int)(c + 0.5);
            }
            critical_avail = total * (uint64_t)pct / 100;
        }
        uint64_t mild_top = total * 25 / 100;      // 温和区上界（25%）
        uint64_t pressure_bottom = critical_avail / 2; // 压力区下界（危急区起点）

        // 压力系数 P（0..1）
        uint64_t span = mild_top > critical_avail ? mild_top - critical_avail : 1;
        uint64_t below = avail > critical_avail ? mild_top - avail : span;
        uint64_t progress = span ? below * 100 / span : 100;
        if (progress > 100)
            progress = 100;
        double P = (double)progress / 100.0;

        // 分区；zone 变化立即生效（下轮按新频率探针）
        if (avail <= pressure_bottom)
            zone = Zone::Critical;
        else if (avail <= critical_avail)
            zone = Zone::Pressure;
        else if (avail <= mild_top)
            zone = Zone::Mild;
        else
            zone = Zone::Comfort;
        if (zone != last_zone) {
            last_zone = zone;
            continue; // 分区刚变化：立即再探（高频响应压力突变）
        }

        uint64_t now = GetTickCount64();

        // 清理过期淘汰记录（节流：每 10 分钟一次，避免每轮全扫）
        if (now > last_record_cleanup + 10ull * 60 * 1000) {
            last_record_cleanup = now;
            std::lock_guard<std::mutex> lk(g_lock);
            for (auto it = g_evictRecords.begin(); it != g_evictRecords.end();) {
                if (now > it->second.evict_time + 10ull * 60 * 1000 &&
                    (it->second.hot_until == 0 || now > it->second.hot_until)) {
                    // 豁免到期且条目仍可淘汰：重新加入闲置索引
                    auto rit = g_replacements.find(it->first);
                    if (rit != g_replacements.end() && rit->second.refcount == 0 &&
                        rit->second.texture)
                        IdleIndexUpdate(it->first, rit->second.last_used);
                    it = g_evictRecords.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 回收参数（按 P 线性驱动；危急区走缺口自适应单独处理）
        uint64_t idle_ms = 0;
        int evict_limit = 0;
        bool size_primary = false; // true = 大纹理优先排序
        switch (zone) {
            case Zone::Comfort:
                idle_ms = 10ull * 60 * 1000; // 10 分钟（缓存超预算时才用）
                evict_limit = 2;
                size_primary = false;
                break;
            case Zone::Mild:
                idle_ms = 10ull * 60 * 1000 - (uint64_t)(P * (10ull * 60 * 1000 - 2ull * 60 * 1000));
                evict_limit = 2 + (int)(P * 6);
                size_primary = false;
                break;
            case Zone::Pressure:
                idle_ms = 2ull * 60 * 1000 - (uint64_t)(P * (2ull * 60 * 1000 - 30ull * 1000));
                evict_limit = 8 + (int)(P * 8);
                size_primary = (P >= 0.5);
                break;
            case Zone::Critical:
                idle_ms = 30ull * 1000; // 30 秒兜底
                evict_limit = 32;
                size_primary = true;
                break;
        }

        // 缓存总量（O(1) 原子读）
        uint64_t cache_bytes = g_cacheBytesTotal.load(std::memory_order_relaxed);
        uint64_t cache_budget = total * kCacheBudgetPct / 100;

        // 从闲置索引取候选（最多取 64 个验证，淘汰上限由 evict_limit 控制）
        // 惰性修正：索引键过期（被重新绑定）→ 重插到新位置；不可淘汰 → 移除。
        struct EvictCand { uint64_t mem; uint64_t last_used; uint32_t hash; };
        std::vector<EvictCand> candidates;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            int scanned = 0;
            for (auto it = g_idleSet.begin();
                 it != g_idleSet.end() && scanned < 64;
                 ++it, ++scanned) {
                uint32_t hash = it->second;
                auto rit = g_replacements.find(hash);
                if (rit == g_replacements.end() || !rit->second.texture) {
                    IdleIndexRemove(hash); // 条目已释放
                    continue;
                }
                // 惰性修正：索引键 != 实际 last_used → 被重新绑定过，重插
                if (it->first != rit->second.last_used) {
                    IdleIndexUpdate(hash, rit->second.last_used);
                    continue;
                }
                if (rit->second.refcount != 0) {
                    IdleIndexRemove(hash); // 使用中：移出候选（tracker 归零时加回）
                    continue;
                }
                // 热缓存豁免
                auto er = g_evictRecords.find(hash);
                if (er != g_evictRecords.end() && er->second.hot_until != 0 &&
                    now < er->second.hot_until) {
                    IdleIndexRemove(hash);
                    continue;
                }
                // 闲置时长未达标：索引按 last_used 升序，后面的更不可能达标 → 停止
                if (rit->second.last_used != 0 &&
                    now <= rit->second.last_used + idle_ms)
                    break;
                candidates.push_back({rit->second.mem_bytes, rit->second.last_used, hash});
            }
            // 排序：P<0.5 LRU 优先；P>=0.5 大纹理优先
            std::sort(candidates.begin(), candidates.end(),
                      [size_primary](const EvictCand &a, const EvictCand &b) {
                          if (size_primary) {
                              if (a.mem != b.mem)
                                  return a.mem > b.mem;
                              return a.last_used < b.last_used;
                          }
                          if (a.last_used != b.last_used)
                              return a.last_used < b.last_used;
                          return a.mem > b.mem;
                      });
        }

        // 执行回收
        int evicted = 0;
        uint64_t freed_bytes = 0;
        uint64_t gap = 0;

        if (zone == Zone::Comfort) {
            // 舒适区：仅缓存超预算时轻度回收（闲置最久优先）
            if (cache_bytes <= cache_budget || candidates.empty()) {
                TL_LOG_IF(2, L"[vram] zone=comfort cache=%llu/%lluMB evict=0",
                       (unsigned long long)(cache_bytes >> 20),
                       (unsigned long long)(cache_budget >> 20));
                continue;
            }
            TL_LOG(L"[vram] zone=comfort cache=%llu/%lluMB over budget, recycling idle",
                   (unsigned long long)(cache_bytes >> 20),
                   (unsigned long long)(cache_budget >> 20));
        } else if (zone == Zone::Critical) {
            // 危急区：缺口自适应（回收 >= gap 即停）
            gap = critical_avail > avail ? critical_avail - avail : 0;
            TL_LOG(L"[vram] zone=critical avail=%lluMB gap=%lluMB P=%u%% cache=%llu/%lluMB",
                   (unsigned long long)(avail >> 20), (unsigned long long)(gap >> 20),
                   (unsigned)progress, (unsigned long long)(cache_bytes >> 20),
                   (unsigned long long)(cache_budget >> 20));
        } else {
            // 温和/压力区：按 P 驱动参数回收
            TL_LOG(L"[vram] zone=%s avail=%lluMB P=%u%% idle>%llus N=%d cache=%llu/%lluMB",
                   zone == Zone::Mild ? L"mild" : L"pressure",
                   (unsigned long long)(avail >> 20), (unsigned)progress,
                   (unsigned long long)(idle_ms / 1000), evict_limit,
                   (unsigned long long)(cache_bytes >> 20),
                   (unsigned long long)(cache_budget >> 20));
        }

        for (auto &c : candidates) {
            if (evicted >= evict_limit)
                break;
            if (zone == Zone::Critical && freed_bytes >= gap)
                break;
            ID3D11Texture2D *tex = nullptr;
            ID3D11ShaderResourceView *srv = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                auto it = g_replacements.find(c.hash);
                if (it == g_replacements.end() || it->second.refcount != 0 || !it->second.texture)
                    continue;
                // 双重确认热豁免（锁内再查一次）
                auto er = g_evictRecords.find(c.hash);
                if (er != g_evictRecords.end() && er->second.hot_until != 0 &&
                    now < er->second.hot_until)
                    continue;
                // 二次校验：候选收集后刚被重新绑定（last_used 已更新）→ 跳过
                if (it->second.last_used != c.last_used)
                    continue;
                tex = it->second.texture;
                srv = it->second.srv;
                it->second.texture = nullptr;
                it->second.srv = nullptr;
                RepSrvRemove(srv);
                const uint64_t this_mem = it->second.mem_bytes; // 本条占用
                freed_bytes += this_mem;
                it->second.mem_bytes = 0;
                IdleIndexRemove(c.hash); // 已淘汰：移出候选索引
                g_cacheBytesTotal.fetch_sub(this_mem, std::memory_order_relaxed);
                // 记录淘汰时刻（热豁免判定依据）
                g_evictRecords[c.hash].evict_time = now;
            }
            if (tex) QueueRelease(tex);
            if (srv) QueueRelease(srv);
            evicted++;
        }

        if (evicted > 0) {
            if (zone == Zone::Critical)
                TL_LOG(L"[vram] evicted %d (freed ~%lluMB of gap %lluMB)",
                       evicted, (unsigned long long)(freed_bytes >> 20),
                       (unsigned long long)(gap >> 20));
            else
                TL_LOG(L"[vram] evicted %d (freed ~%lluMB)",
                       evicted, (unsigned long long)(freed_bytes >> 20));
        } else if (zone != Zone::Comfort) {
            TL_LOG(L"[vram] no evictable cache (all in use)");
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 轻量配置读取：DLL 同目录 TextureLoader.ini（key = value，支持 ; 注释）
// ---------------------------------------------------------------------------

// ⚠️ 2026-09-23：ini **只读一次**，不再每个键都把整个文件重读一遍。
//
// 原实现每次 `GetIniValue` 都走 `CreateFileW` + 整文件 `ReadFile` + UTF-8 转码
// + 逐行解析。而 `DllMain` 的初始化块里有 **8 次**读取
// （observe_only / log_level / max_texture_side / gdds_enabled /
//  async_load ×2 / vram_threshold / mods_dir）
// ⇒ 同一份 73 行的 ini 被完整读入并解析 8 遍。
//
// 关键点不只是浪费 I/O：这些调用**全部发生在 loader lock 持有期间**（DllMain），
// 属于已知的死锁模式（与已修的 `log_init` 内 fopen 同性质）——
// 该窗口应尽量缩短。8 次文件打开/读取/关闭收窄为 1 次。
//
// 语义**原样保留**：同名键取**最后一次**出现（"后出现覆盖先前"）、
// `;` 行内注释截断、键与值的首尾空白裁剪、UTF-8 失败时退回按字节扩展。
struct IniCache
{
    std::mutex mutex;
    bool loaded = false;
    std::wstring source; // 已解析的文件路径；dir 变化时重载
    std::unordered_map<std::wstring, std::wstring> values;
};
IniCache g_ini_cache;

// 调用方必须已持有 g_ini_cache.mutex。
static void reload_ini_locked(const std::wstring &path)
{
    g_ini_cache.values.clear();
    g_ini_cache.source = path;
    g_ini_cache.loaded = true;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::string data;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        data.append(buf, got);
    CloseHandle(h);

    // UTF-8 / ASCII 转宽字符
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), nullptr, 0);
    std::wstring wdata;
    if (wlen > 0) {
        wdata.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), &wdata[0], wlen);
    } else {
        wdata.assign(data.begin(), data.end());
    }

    size_t pos = 0;
    while (pos < wdata.size()) {
        size_t eol = wdata.find(L'\n', pos);
        if (eol == std::wstring::npos)
            eol = wdata.size();
        std::wstring line = wdata.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        size_t sem = line.find(L';');
        if (sem != std::wstring::npos)
            line = line.substr(0, sem);
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;
        std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.front() == L' ' || k.front() == L'\t')) k.erase(0, 1);
        while (!k.empty() && (k.back() == L' ' || k.back() == L'\t')) k.pop_back();
        while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(0, 1);
        while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
        // 自研无 section 行解析：同名键允许出现多次（如外部工具追加 [General] 段），
        // 取最后一个匹配——后出现的键覆盖先前的空值/旧值。
        g_ini_cache.values[k] = v;
    }
}

static std::wstring GetIniValue(const std::wstring &dir, const std::wstring &key)
{
    const std::wstring path = dir + L"TextureLoader.ini";
    std::lock_guard<std::mutex> lk(g_ini_cache.mutex);
    if (!g_ini_cache.loaded || g_ini_cache.source != path)
        reload_ini_locked(path);
    auto it = g_ini_cache.values.find(key);
    return it == g_ini_cache.values.end() ? std::wstring() : it->second;
}

static bool IniBool(const std::wstring &dir, const std::wstring &key, bool def)
{
    std::wstring v = GetIniValue(dir, key);
    if (v.empty())
        return def;
    return v == L"1" || v == L"true" || v == L"yes" || v == L"on";
}

static int IniInt(const std::wstring &dir, const std::wstring &key, int def)
{
    std::wstring v = GetIniValue(dir, key);
    if (v.empty())
        return def;
    wchar_t *end = nullptr;
    long n = wcstol(v.c_str(), &end, 10);
    if (end == v.c_str())
        return def;
    return (int)n;
}

// 解析 vram_threshold：支持百分比（"15%"）或具体容量（"1024M"/"2G"，M/G 大小写均可，
// 无后缀数字按 MB 处理）。空值/非法 → 自适应模式（pct=0：按显存容量动态推导临界值）。
// 显式设置时覆盖自适应值（用户自定义优先级最高）。结果写入共享配置。
static void ParseVramThreshold(const std::wstring &dir)
{
    ::tloader::g_vram_threshold_pct = 0;         // 0 = 自适应（按显存容量推导）
    ::tloader::g_vram_threshold_bytes = 0;       // 0 = 百分比模式

    std::wstring v = GetIniValue(dir, L"vram_threshold");
    if (v.empty())
        return;
    // 去首尾空白
    size_t b = v.find_first_not_of(L" \t");
    if (b == std::wstring::npos)
        return;
    size_t e = v.find_last_not_of(L" \t");
    v = v.substr(b, e - b + 1);
    if (v.empty())
        return;

    bool is_pct = false;
    double mult = 1024.0 * 1024.0; // 默认 MB
    wchar_t last = v.back();
    if (last == L'%') {
        is_pct = true;
        v.pop_back();
    } else if (last == L'G' || last == L'g') {
        mult = 1024.0 * 1024.0 * 1024.0;
        v.pop_back();
    } else if (last == L'M' || last == L'm') {
        mult = 1024.0 * 1024.0;
        v.pop_back();
    }
    // 去尾随空白（"1024M " 之类）
    while (!v.empty() && (v.back() == L' ' || v.back() == L'\t'))
        v.pop_back();
    if (v.empty())
        return;

    wchar_t *end = nullptr;
    double val = wcstod(v.c_str(), &end);
    if (end == v.c_str() || val <= 0)
        return; // 非法 → 自适应

    if (is_pct) {
        if (val > 100.0)
            val = 100.0;
        ::tloader::g_vram_threshold_pct = (int)(val + 0.5);
        if (::tloader::g_vram_threshold_pct < 1)
            ::tloader::g_vram_threshold_pct = 1;
        ::tloader::g_vram_threshold_bytes = 0;
    } else {
        ::tloader::g_vram_threshold_bytes = (uint64_t)(val * mult);
        if (::tloader::g_vram_threshold_bytes == 0)
            ::tloader::g_vram_threshold_bytes = 1; // 极小合法值至少非零
    }
}

// ---------------------------------------------------------------------------
// D3D11CreateDevice hook — 获取设备实例（DllList 注入方式下游戏不调用我们的导出）
// ---------------------------------------------------------------------------

typedef HRESULT(WINAPI *D3D11CreateDevice_t)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);
typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(IDXGIAdapter *, D3D_DRIVER_TYPE,
    HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
    IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static D3D11CreateDevice_t RealD3D11CreateDevice = nullptr;
static D3D11CreateDeviceAndSwapChain_t RealD3D11CreateDeviceAndSwapChain = nullptr;

// 记录 GPU 型号与驱动版本（崩溃定位关键信息：驱动内部崩溃需对照驱动版本）
static void LogGpuInfo(ID3D11Device *device)
{
    IDXGIDevice *dxgi_dev = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev)))
        return;
    IDXGIAdapter *adapter = nullptr;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) {
        dxgi_dev->Release();
        return;
    }
    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
        const wchar_t *vendor = L"?";
        switch (desc.VendorId) {
            case 0x10DE: vendor = L"NVIDIA"; break;
            case 0x1002: vendor = L"AMD"; break;
            case 0x8086: vendor = L"Intel"; break;
            default: break;
        }
        TL_LOG(L"[gpu ] %ls (vendor=0x%04X %ls, device=0x%04X, rev=%u, vram=%lluMB)",
               desc.Description, (unsigned)desc.VendorId, vendor,
               (unsigned)desc.DeviceId, (unsigned)desc.Revision,
               (unsigned long long)(desc.DedicatedVideoMemory >> 20));
        g_total_vram = desc.DedicatedVideoMemory; // 物理显存（淘汰阈值基准，非动态 Budget）
        g_gpu_vendor = desc.VendorId;             // 厂商 ID（async_load 自动调整依据）
    }
    // 驱动版本：注册表显示类（{4d36e968-...} 下的 DriverVersion）
    HKEY hClass = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ, &hClass) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < 32; i++) {
            wchar_t sub[16];
            swprintf_s(sub, L"%04u", i);
            HKEY hDev = nullptr;
            if (RegOpenKeyExW(hClass, sub, 0, KEY_READ, &hDev) != ERROR_SUCCESS)
                continue;
            wchar_t ver[128] = {}, ddesc[256] = {};
            DWORD sz = sizeof(ver);
            bool hasVer = RegQueryValueExW(hDev, L"DriverVersion", nullptr, nullptr,
                                           (LPBYTE)ver, &sz) == ERROR_SUCCESS;
            sz = sizeof(ddesc);
            bool hasDesc = RegQueryValueExW(hDev, L"DriverDesc", nullptr, nullptr,
                                            (LPBYTE)ddesc, &sz) == ERROR_SUCCESS;
            RegCloseKey(hDev);
            if (hasVer && hasDesc && wcsstr(ddesc, desc.Description)) {
                TL_LOG(L"[gpu ] driver: %ls (ver %ls)", ddesc, ver);
                break;
            }
        }
        RegCloseKey(hClass);
    }
    adapter->Release();
    dxgi_dev->Release();
}

static void AttachToDevice(ID3D11Device *device, ID3D11DeviceContext *context)
{
    if (g_hook_ready.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_hook_ready.load(std::memory_order_acquire))
        return;
    g_device = device;
    g_device->AddRef();
    g_context = context;
    g_context->AddRef();
    HookDevice(device);
    HookContext(context);
    g_hook_ready.store(1, std::memory_order_release);
    LogGpuInfo(device);
    // async_load 未显式设置时按 GPU 厂商自动选择：
    //   NVIDIA → 同步（驱动对后台线程建纹理有缺陷，实测 566.64 崩溃/卡死）
    //   其他（AMD/Intel）→ 异步（后台线程加载，避免渲染线程阻塞 GPU 空转）
    if (!::tloader::g_async_load_explicit) {
        int want = (g_gpu_vendor == 0x10DE) ? 0 : 1;
        if (::tloader::g_async_load != want) {
            ::tloader::g_async_load = want;
            TL_LOG(L"[cfg ] async_load auto-adjusted to %d (vendor=0x%04X)",
                   want, (unsigned)g_gpu_vendor);
        }
    }
    // 启动后台替换纹理加载线程：0=GDDS（DirectStorage GPU 解压），1=DDS（CPU）
    for (int i = 0; i < 2; i++) {
        if (!g_loadThreadRunning[i]) {
            g_loadThreadRunning[i] = true;
            // 保存句柄：detach 时需要等它们退出后才能释放替换缓存（见 DllMain）。
            // 旧实现丢弃句柄 → 既无法等待退出，也无法 CloseHandle（句柄泄漏）。
            g_loadThread[i] = CreateThread(nullptr, 0, AsyncLoadThread, (LPVOID)(intptr_t)i, 0, nullptr);
        }
    }
    // 启动显存监控线程（仅一次）
    if (!g_vramMonitorRunning) {
        InterlockedExchange(&g_vramMonitorRunning, 1);
        g_vramThread = CreateThread(nullptr, 0, VramMonitorThread, nullptr, 0, nullptr);
    }
    TL_LOG(L"[ok  ] attached: device=%p context=%p", device, context);
}

static HRESULT WINAPI HookD3D11CreateDevice(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext)
{
    HRESULT hr = RealD3D11CreateDevice(pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
        ppImmediateContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        ID3D11DeviceContext *ctx = nullptr;
        (*ppDevice)->GetImmediateContext(&ctx);
        if (!ctx && ppImmediateContext)
            ctx = *ppImmediateContext;
        AttachToDevice(*ppDevice, ctx);
        if (ctx)
            ctx->Release();
    }
    return hr;
}

static HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
{
    HRESULT hr = RealD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
        ppDevice, pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        ID3D11DeviceContext *ctx = nullptr;
        (*ppDevice)->GetImmediateContext(&ctx);
        if (!ctx && ppImmediateContext)
            ctx = *ppImmediateContext;
        AttachToDevice(*ppDevice, ctx);
        if (ctx)
            ctx->Release();
    }
    return hr;
}

// 安装 D3D11CreateDevice hook（d3d11 已加载时同步安装；未加载返回 false 由后台线程重试）
static bool InstallCreateDeviceHook()
{
    HMODULE real = GetModuleHandleW(L"d3d11.dll");
    if (!real)
        real = LoadLibraryW(L"d3d11.dll");
    if (!real) {
        TL_LOG(L"[warn] d3d11.dll not loadable yet");
        return false;
    }
    RealD3D11CreateDevice = (D3D11CreateDevice_t)GetProcAddress(real, "D3D11CreateDevice");
    RealD3D11CreateDeviceAndSwapChain =
        (D3D11CreateDeviceAndSwapChain_t)GetProcAddress(real, "D3D11CreateDeviceAndSwapChain");
    if (!RealD3D11CreateDevice || !RealD3D11CreateDeviceAndSwapChain) {
        TL_LOG(L"[warn] d3d11 exports not found");
        return false;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)RealD3D11CreateDevice, HookD3D11CreateDevice);
    DetourAttach(&(PVOID &)RealD3D11CreateDeviceAndSwapChain, HookD3D11CreateDeviceAndSwapChain);
    LONG r = DetourTransactionCommit();
    TL_LOG(L"[ok  ] D3D11CreateDevice hook installed (commit=%d)", r);
    return true;
}

// ---------------------------------------------------------------------------
// 卸载钩子（DLL_PROCESS_DETACH 时调用）
//
// ⚠️ 为什么必须做：Detours 的 DetourAttach 会把目标函数**改写**成跳转到我们的
// trampoline，而 trampoline 位于本 DLL 的地址空间内。若 DLL 卸载时不 DetourDetach，
// 游戏后续仍会跳进已解除映射的内存 → 立即崩溃。
// 旧实现从不卸载，只在进程终结时才"侥幸"没出事（那时游戏也不再调用这些函数）。
//
// 顺序：必须在线程停掉**之后**调用——否则卸载瞬间仍有线程在钩子内执行。
// 注意 D3D11CreateDevice 系列在 d3d11.dll 里（不在本模块），DetourDetach 会把
// 原字节写回；失败也不致命（进程即将退出）。
// ---------------------------------------------------------------------------
static void UninstallHooks()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (RealCreateTexture2D != nullptr)
        DetourDetach(&(PVOID &)RealCreateTexture2D, HookCreateTexture2D);
    for (int i = 0; i < 6; ++i) {
        if (g_real_ssr[i] != nullptr)
            DetourDetach(&(PVOID &)g_real_ssr[i], g_stage_hooks[i]);
    }
    if (g_real_update_subresource != nullptr)
        DetourDetach(&(PVOID &)g_real_update_subresource, HookUpdateSubresource);
    // ⚠️ 新增钩子必须在此同步摘除：漏摘会让 DLL 卸载后游戏仍跳进已释放的
    // trampoline（`c9bb8df` 修过的正是这类缺陷）。
    if (g_real_copy_resource != nullptr)
        DetourDetach(&(PVOID &)g_real_copy_resource, HookCopyResource);
    if (RealD3D11CreateDevice != nullptr)
        DetourDetach(&(PVOID &)RealD3D11CreateDevice, HookD3D11CreateDevice);
    if (RealD3D11CreateDeviceAndSwapChain != nullptr)
        DetourDetach(&(PVOID &)RealD3D11CreateDeviceAndSwapChain, HookD3D11CreateDeviceAndSwapChain);
    const LONG r = DetourTransactionCommit();
    TL_LOG(L"[ok  ] hooks uninstalled (commit=%d)", r);
}

// ---------------------------------------------------------------------------
// 崩溃捕获：向量化异常处理器（崩溃时记录异常信息 + 模块级调用栈到日志）
// ---------------------------------------------------------------------------

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI CrashHandler(PEXCEPTION_POINTERS ep)
{
    // ⚠️ 2026-09-23：重入防护。
    //
    // 处理器自身也会访问内存（CaptureStackBackTrace 走栈、VirtualQuery、
    // memcpy、GetModuleFileNameW）。若它**自己**再触发异常（栈已损坏、
    // 页已失效），VEH 会被重新进入 → 无限递归 → 栈溢出。
    // thread_local 标志让第二次进入立刻退出，保证只产出一份现场。
    static thread_local bool t_in_crash_handler = false;
    if (t_in_crash_handler)
        return EXCEPTION_CONTINUE_SEARCH;
    t_in_crash_handler = true;

    // 只捕获访问违例/非法指令等致命异常；调试器在场时不介入
    //
    // ⚠️ 2026-09-23（诊断补强）：这两条原先**静默**返回，于是"日志里没有 [CRASH] 行"
    // 永远无法区分"异常真的没发生"与"被这里跳过" —— 实机排查时已经因此困惑过一次
    // （新基线日志无 [CRASH] 行 ⇒ 究竟没崩，还是被拦？）。现在把跳过原因写进日志。
    // **注意：重入那条必须保持静默**（它自己也会走 TLC_LOG，否则无限递归）。
    if (IsDebuggerPresent()) {
        TLC_LOG(L"[CRASH] skipped reason=debugger_present");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH; // 静默：唯一必须不记录的分支（防递归）
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED) {
        TLC_LOG(L"[CRASH] skipped reason=non_fatal_code code=0x%08X addr=%p", code,
                ep->ExceptionRecord->ExceptionAddress);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void *addr = ep->ExceptionRecord->ExceptionAddress;

    // 故障地址归属模块。原先只解析**栈帧**归属，故障地址与 R13 都是裸值 ——
    // 而实机案例里出错指令在反作弊模块（MHYPBase.dll）、R13 是它正在扫描的
    // 另一个模块基址，两者都只能用裸地址表示 ⇒ **无法指名"在扫谁的内存"**。
    // 这里补上故障地址的模块解析；R13 在下方寄存器块之后单独解析。
    {
        HMODULE amod = nullptr;
        wchar_t aname[MAX_PATH] = L"<unknown>";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)addr, &amod)) {
            GetModuleFileNameW(amod, aname, MAX_PATH);
            wchar_t *slash = wcsrchr(aname, L'\\');
            if (slash) wcscpy_s(aname, slash + 1);
        }
        TLC_LOG(L"[CRASH] fault_module %s+0x%llX", aname,
                (unsigned long long)((uintptr_t)addr - (uintptr_t)amod));
    }
    // ⚠️ 2026-09-23：全部改用 TLC_LOG（= log_crash_write），**不能用 TL_LOG**。
    // TL_LOG 会阻塞在非递归的 g_mutex 上；若崩溃正发生在持有该锁的代码里
    // （fwprintf/fflush 内部违例、堆损坏），就会自死锁 ——
    // 崩溃报告永远写不出来，进程还挂在那里。详见 log.h 的说明。
    TLC_LOG(L"[CRASH] exception=0x%08X addr=%p tid=0x%X", code, addr, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        TLC_LOG(L"[CRASH] access violation %s address=0x%llX",
                ep->ExceptionRecord->ExceptionInformation[0] == 0 ? L"READ" : L"WRITE",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    // 寄存器上下文（定位空调用来源：RIP=0 时看哪个寄存器为 0 被 call）
    if (ep->ContextRecord) {
        auto *ctx = ep->ContextRecord;
        TLC_LOG(L"[CRASH] regs RAX=%p RBX=%p RCX=%p RDX=%p RSI=%p RDI=%p RSP=%p RBP=%p",
                (void *)ctx->Rax, (void *)ctx->Rbx, (void *)ctx->Rcx, (void *)ctx->Rdx,
                (void *)ctx->Rsi, (void *)ctx->Rdi, (void *)ctx->Rsp, (void *)ctx->Rbp);
        TLC_LOG(L"[CRASH] regs R8=%p R9=%p R10=%p R11=%p R12=%p R13=%p R14=%p R15=%p",
                (void *)ctx->R8, (void *)ctx->R9, (void *)ctx->R10, (void *)ctx->R11,
                (void *)ctx->R12, (void *)ctx->R13, (void *)ctx->R14, (void *)ctx->R15);
        // R13 在实机案例里是"被扫描模块的基址"（出错地址 = R13 + 0x93xxxx）。
        // 裸值无法指名模块 ⇒ 解析它，直接说出反作弊在扫谁的内存。
        {
            HMODULE rmod = nullptr;
            wchar_t rname[MAX_PATH] = L"<unknown>";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)ctx->R13, &rmod)) {
                GetModuleFileNameW(rmod, rname, MAX_PATH);
                wchar_t *slash = wcsrchr(rname, L'\\');
                if (slash) wcscpy_s(rname, slash + 1);
            }
            TLC_LOG(L"[CRASH] r13_module %s+0x%llX", rname,
                    (unsigned long long)(ctx->R13 - (uintptr_t)rmod));
        }
    }

    // 模块级调用栈（无符号解析——输出 module+0xOFFSET 便于定位所在模块）
    void *frames[32] = {};
    USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = nullptr;
        wchar_t modName[MAX_PATH] = L"<unknown>";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &mod)) {
            GetModuleFileNameW(mod, modName, MAX_PATH);
            wchar_t *slash = wcsrchr(modName, L'\\');
            if (slash) wcscpy_s(modName, slash + 1);
        }
        uintptr_t base = mod ? (uintptr_t)mod : 0;
        TLC_LOG(L"[CRASH]   #%02u %s+0x%llX (%p)", i, modName,
                (unsigned long long)((uintptr_t)frames[i] - base), frames[i]);
    }

    // 原始栈扫描：CaptureStackBackTrace 在栈损坏/堆损坏时失效（只见 ntdll 分发），
    // 直接从异常上下文 RSP 逐 qword 读返回地址，识别落在已知模块内的帧，
    // 定位 use-after-free 的真实调用链（如 d3d11.dll/YuanShen.exe/TextureLoader.dll）。
    if (ep->ContextRecord) {
        uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;
        TLC_LOG(L"[CRASH] raw-stack from RSP=%p:", (void *)rsp);
        for (int i = 0; i < 96; i++) {
            uintptr_t addr = rsp + (uintptr_t)i * 8;
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
                break;
            if ((mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
                break;
            uintptr_t val = 0;
            memcpy(&val, (void *)addr, sizeof(val));
            HMODULE mod = nullptr;
            wchar_t modName[MAX_PATH] = L"";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)val, &mod)) {
                GetModuleFileNameW(mod, modName, MAX_PATH);
                wchar_t *slash = wcsrchr(modName, L'\\');
                if (slash) wcscpy_s(modName, slash + 1);
                uintptr_t base = (uintptr_t)mod;
                TLC_LOG(L"[CRASH]   [rsp+0x%03X] %s+0x%llX", i * 8, modName,
                        (unsigned long long)(val - base));
            }
        }
    }
    // 不吞异常——让系统走 WER/LocalDumps 流程（日志已足够定位模块）
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------

static void *g_crash_handler = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        g_crash_handler = AddVectoredExceptionHandler(1, CrashHandler);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hModule, path, MAX_PATH);
        wchar_t *slash = wcsrchr(path, L'\\');
        std::wstring dir = slash ? std::wstring(path, slash + 1) : L"";
        ::tloader::log_init(dir);
        TL_LOG(L"TextureLoader init.");
        // ⚠️ 缺省值必须与发布 ini / README 一致（均为 0 = 真正替换）。
        // 旧代码缺省 true（只观察不替换），而 TextureLoader.ini 与 README 都写
        // `observe_only = 0`（默认真正替换）→ 只要用户的 ini 没有这一键（例如从旧版
        // 升级、或用了精简模板），就会**静默退化为"完全不替换"**，且日志只打
        // `[cfg ] observe_only=1`，用户很难意识到纹理根本没被替换。
        g_observe_only = IniBool(dir, L"observe_only", false);
        TL_LOG(L"[cfg ] observe_only=%d", (int)g_observe_only);

        // 可调配置（供 gdds_interop/dds_loader/监控线程跨模块共享）
        ::tloader::g_log_level = IniInt(dir, L"log_level", 1);
        ::tloader::g_max_texture_side = IniInt(dir, L"max_texture_side", 0);
        ::tloader::g_gdds_enabled = IniInt(dir, L"gdds_enabled", 1);
        // async_load：ini 显式设置则按值；未设置默认异步（后台线程建纹理，
        // 避免渲染线程被磁盘 IO/纹理创建阻塞导致 GPU 空转），N 卡在
        // AttachToDevice（已知 GPU 厂商）时自动切同步规避驱动缺陷。
        {
            // ⚠️ 2026-09-22：改用 IniBool，与其它布尔键语义一致。
            //
            // 原实现是 `(av == L"1") ? 1 : 0` —— **只认字面量 "1"**。于是
            // `async_load = true` / `= yes` / `= on` 全部被当成 **0（同步）**，
            // 与 `IniBool` 的语义（以及同文件其它布尔键）不一致；
            // 而 async_load 直接决定纹理在哪个线程创建（N 卡驱动规避的关键开关），
            // 静默走错分支代价很大。
            // 注意 `IniBool` 需要键存在与否的区分，故仍用 GetIniValue 判空。
            std::wstring av = GetIniValue(dir, L"async_load");
            if (av.empty()) {
                ::tloader::g_async_load = 1; // 默认异步
                ::tloader::g_async_load_explicit = 0;
            } else {
                ::tloader::g_async_load = IniBool(dir, L"async_load", true) ? 1 : 0;
                ::tloader::g_async_load_explicit = 1;
            }
        }
        ParseVramThreshold(dir);
        // 2026-11-23：原先这里读 update_suppress_ms（UpdateSubresource 的 TTL 抑制窗口）。
        // 该键已删除 —— 实机实测其前提不成立（166,018 次调用、0 次命中替换集），
        // 详见 HookUpdateSubresource 上方的「为什么删掉 TTL 抑制」。
        // 删除后的兼容性：旧用户的 ini 里可能仍留着这一行，它会被当成**未知键**忽略
        // （ini 解析只按名字取值，不认识的键不报错、不影响其它键）。
        // dump_hashes（2026-11-22 新增）：纹理哈希导出开关，0 = 关（默认），1 = 每个不同 hash 导出一行。
        // 为什么用 IniBool 而不是 IniInt：它是纯布尔语义（条目 §4.5「取值域 0/1」），
        // 与同文件 observe_only / async_load 一致（IniBool 认 1/true/yes/on，缺键取默认）。
        // 为什么默认必须是 0：诊断默认关（§二.5）；且**存量用户升级拿不到这个键**
        // （Apply-PackageUpdate.ps1 保留用户 ini、不补新键）⇒ 代码默认值就是线上唯一行为。
        //  ⇒ 因此 [cfg ] 行**必须**打印它：它是「键被读到」的唯一可见处。
        ::tloader::g_dump_hashes.store(IniBool(dir, L"dump_hashes", false) ? 1 : 0,
                                      std::memory_order_relaxed);
        TL_LOG(L"[cfg ] log_level=%d vram_threshold_pct=%d vram_threshold_bytes=%llu max_texture_side=%d gdds_enabled=%d async_load=%d",
               ::tloader::g_log_level, ::tloader::g_vram_threshold_pct,
               (unsigned long long)::tloader::g_vram_threshold_bytes,
               ::tloader::g_max_texture_side, ::tloader::g_gdds_enabled,
               ::tloader::g_async_load);
        // 2026-11-23：原 `[cfg ] update_suppress_ms=%d` 单独一行随该键一起删除。
        // 保留「单独一行」这个形态给 dump_hashes 的理由不变（下面两行仍是独立 TL_LOG）。
        // dump_hashes 同样**单独一行**：拼进上面那行会超过 140 字符（AGENTS.md §二.1 目标 ≤120）。
        // 它也是「存量用户拿不到这个键」场景下唯一能看到取值的行 —— grep 一行即可确认。
        TL_LOG(L"[cfg ] dump_hashes=%d",
               ::tloader::g_dump_hashes.load(std::memory_order_relaxed));

        // 扫描 Mods 目录：优先用 ini 里的 mods_dir；未配置时默认 DLL 所在目录的
        // Mods 子目录（不存在则自动创建空的，用户把 mod 放进去即可）。
        std::wstring mods_dir;
        std::wstring ini_mods = GetIniValue(dir, L"mods_dir");
        if (!ini_mods.empty()) {
            DWORD attr = GetFileAttributesW(ini_mods.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                mods_dir = ini_mods;
            else
                TL_LOG(L"[warn] configured mods_dir not found: \"%ls\"", ini_mods.c_str());
        }
        if (mods_dir.empty()) {
            std::wstring def = dir + L"Mods";
            DWORD attr = GetFileAttributesW(def.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (CreateDirectoryW(def.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
                    TL_LOG(L"[ini ] created default Mods directory: \"%ls\"", def.c_str());
            }
            mods_dir = def;
        }
        {
            size_t n = LoadModInis(mods_dir, g_overrides);
            TL_LOG(L"[ini ] loaded %zu texture overrides from \"%ls\"", n, mods_dir.c_str());
        }

        // 优先同步安装 D3D11CreateDevice hook：d3d11 通常已被 Bridge/ReShade
        // 提前加载，此时立即 hook 才能在游戏创建设备前截获。若 d3d11 尚未
        // 加载（极少数情况），退回后台线程等待。
        if (!InstallCreateDeviceHook()) {
            g_bootstrapThread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        } else {
            TL_LOG(L"[ok  ] installed synchronously in DllMain");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // ⚠️ 顺序至关重要：**先停后台线程，再释放它们会访问的数据**。
        // 旧实现直接释放 g_replacements，而 AsyncLoadThread 可能正在 ExecuteLoadTask
        // 里写这张表 → use-after-free；VramMonitorThread 也可能正在淘汰条目。
        // 线程也没有退出条件，会一直跑到进程终结。
        g_shutdownRequested.store(true, std::memory_order_release);
        {
            // 唤醒两个加载线程（它们可能正卡在 cv.wait 上）
            std::lock_guard<std::mutex> lk(g_loadMutex);
            g_loadCv[0].notify_all();
            g_loadCv[1].notify_all();
        }
        // 等待退出（有界：加载线程在任务边界检查停机信号，最多等一个任务时长）
        HANDLE waits[3] = { g_loadThread[0], g_loadThread[1], g_vramThread };
        for (HANDLE h : waits) {
            if (h != nullptr)
                WaitForSingleObject(h, 3000);
        }
        for (HANDLE &h : waits) {
            if (h != nullptr) {
                CloseHandle(h);
                h = nullptr;
            }
        }
        if (g_bootstrapThread != nullptr) {
            WaitForSingleObject(g_bootstrapThread, 1000);
            CloseHandle(g_bootstrapThread);
            g_bootstrapThread = nullptr;
        }
        // 线程已停 → 现在释放钩子与缓存才是安全的。
        // 旧实现从不 DetourDetach：DLL 卸载后游戏仍会跳进已释放的 trampoline。
        UninstallHooks();
        ::tloader_gdds::Shutdown(); // 释放 GDDS 互操作（D3D12/DS 队列/共享 fence）
        // ⚠️ 2026-09-23：退出时打印统计汇总 —— 必须在下面释放缓存**之前**，
        // 否则 cache_entries/cache_MB 全是 0。
        //
        // 为什么现在才加：`g_stats_created/matched/bound` 三个计数**一直被自增
        // 但从未被任何地方读取**（write-only 死存储）。于是上一批
        // "修正 g_stats_created 语义（原来与 matched 恒等）" 的改动
        // **没有任何可观测效果，也无法验收**。把汇总打出来，三个计数才有意义：
        //   hashed  = 带初始数据、被哈希过的纹理数【累计】
        //   matched = 其中命中 mod 覆盖的【累计】
        //   bound   = 替换 SRV 实际被绑定的次数【累计】
        //   hashed - matched = 哈希了但没命中（判断 mod 覆盖范围的关键量）
        //
        // 2026-11-22 追加的四个字段，各自口径必须连同本行一起读（AGENTS.md §五.6）：
        //   copy_hits = copyresource_hit_calls【累计】—— 仅当 CopyResource 的 dst
        //               当时位于活跃替换集时才计（见其声明处）；**不是** CopyResource
        //               调用总次数，**不是**被触及的纹理数（同一资源每帧可重复计入）。
        //   active    = g_activeCount【**当前状态·退出时刻快照**】—— 此刻活跃替换资源
        //               的条目数。⚠️ 它**不是**「本局是否有过活跃替换」的证据：原纹理
        //               销毁时 ReplacementTracker 会调 ActiveRemove，所以一局里替换全程
        //               生效、退出时仍可能读到 0（实测：bound>0 与 active=0 共存）。
        //               要证明「替换在该局生效过」，用同行的【累计】字段 matched>0 且 bound>0。
        //   overflow  = g_activeOverflow【累计】—— activeArr(4096) 写满而**静默丢弃**
        //               的登记次数，进程内不清零。
        //   observe   = g_observe_only【**当前状态**】—— 1=只记录匹配、不真正替换；
        //               它是配置值、整局恒定，故当前值即整局口径。
        //
        // 2026-11-23：原「TTL 抑制路径」的两个字段（upd_suppressed / upd_recovered）
        // 随 update_suppress_ms 一起删除，配套的 [upd] recovered 限流日志也一并删除。
        // 原因见 HookUpdateSubresource 上方：实机 166,018 次调用、0 次命中替换集，
        // 该路径没有作用对象。**这两个字段的删除不影响下面任何判据** —— 它们是
        // 「抑制发生过几次」，与「替换是否生效」无关。
        // 要读「UpdateSubresource 这个 API 被调用了吗」，用行尾的 upd_calls。
        //
        // ⚠️ 2026-11-23 口径复原（受 TTL 回滚影响）：2026-11-22 曾因删掉 HookUpdateSubresource
        //   里的 ActiveRemove 而在此补一段「被抑制过的纹理留在活跃集 ⇒ copy_hits 变浊」。
        //   现钩子已不再碰活跃集（既不标记也不移出），**该补充说明随之作废**：
        //   copy_hits 的口径回到与改动前完全一致，下面四个前置条件即为完整的判据。
        //
        // ⚠️ 读 copy_hits=0 时必须先排除三种「本来就不会计数」的情形，否则会把
        //   「替换没启用」读成「这条路径没发生」。前置条件一律用【累计】字段判，
        //   **不要**用 active（见上：退出时为 0 不代表没生效）：
        //   observe=1 ⇒ 替换根本没启用（ActiveAdd 只在非 observe-only 分支调用）
        //     ⇒ copy_hits 必然 0，**不构成任何证据**；
        //   overflow>0 ⇒ 有条目被静默丢弃、活跃集不完整 ⇒ 不构成证据；
        //   matched=0 或 bound=0 ⇒ 本局没命中过替换 / 从未绑定替换 SRV ⇒ 不构成证据。
        //   只有 observe=0 且 overflow=0 且 matched>0 且 bound>0 时，copy_hits=0 才
        //   说明本局该路径**未发生**（≠「不是这个机制」，仍需查其他原因）。
        // 注：observe 用 (int) 显式转换传入 varargs，使口径一眼可见。
        //
        // 2026-11-22 末尾再追加一个字段（纹理哈希导出，配置键 dump_hashes）：
        //   hashes_unique = g_hash_dump_logged.size()【**当前状态·退出时刻快照**】—— 本进程内
        //                **不同**纹理哈希的个数，也就是日志里 `[hash]` 行的条数。
        //                ⚠️ 它**同时受配置影响**：dump_hashes=0（默认）时去重容器根本不填
        //                ⇒ 恒为 0，**这本身就是「默认零副作用」的证据**，不是「本局没有纹理」。
        //                要读「本局到底哈希了多少个不同纹理」，必须先在 ini 里开 dump_hashes=1。
        //                与 `hashed=` 的关系：hashed 是**次数**（含同一纹理的重复创建），
        //                hashes_unique 是**不同哈希个数**；hashed > hashes_unique 是正常现象，
        //                且此时 `[hash]` 行数必须 = hashes_unique（不是 hashed）—— 这是去重判据。
        //
        // 2026-11-23 末尾再追加两个字段（钩子总调用次数）：
        //   upd_calls  = g_update_subresource_calls【**累计**】—— UpdateSubresource 钩子的
        //                **总调用次数**，与 dst 是否在活跃集无关（计数在钩子第一行、无条件）。
        //   copy_calls = g_copy_resource_calls【**累计**】—— 同型。
        //   ⚠️ 这两个数**不是**「命中替换集的次数」，那是 copy_hits（UpdateSubresource 侧
        //   自 2026-11-23 起不再有对应的命中计数 —— 它随 TTL 一起删除，实测该侧命中恒 0）。
        //   存在的唯一目的：区分实机上两种相反的世界 ——
        //     upd_calls==0      ⇒ 游戏**从不**调用该 API；
        //     upd_calls>0       ⇒ 调用了（此时再看同行的 matched/bound/copy_hits 判它是否
        //                          触及过替换集；2026-11-23 实测为 166018 次而命中 0 次）。
        // 只**追加**在行尾，既有字段的语义与顺序一律不动。
        // 取 size() 必须持 g_hash_dump_mutex（写方在渲染线程），且**在 g_lock 之外**取：
        // 虽然 DumpTextureHash 从不取 g_lock（不构成环），但没有必要把两把锁叠在一起。
        size_t hashes_unique = 0;
        {
            std::lock_guard<std::mutex> lk(g_hash_dump_mutex);
            hashes_unique = g_hash_dump_logged.size();
        }
        {
            std::lock_guard<std::mutex> lk(g_lock);
            TL_LOG(L"[stat] hashed=%ld matched=%ld bound=%ld cache_entries=%u cache_MB=%u "
                   L"bind_log_suppressed=%llu copy_hits=%ld active=%ld overflow=%ld observe=%d "
                   L"hashes_unique=%zu upd_calls=%ld copy_calls=%ld",
                   g_stats_created, g_stats_matched, g_stats_bound,
                   (unsigned)g_replacements.size(),
                   (unsigned)(g_cacheBytesTotal.load(std::memory_order_relaxed) / (1024ull * 1024ull)),
                   (unsigned long long)g_bind_log_suppressed.load(std::memory_order_relaxed),
                   copyresource_hit_calls.load(std::memory_order_relaxed),
                   g_activeCount.load(std::memory_order_relaxed),
                   g_activeOverflow.load(std::memory_order_relaxed),
                   (int)g_observe_only,
                   hashes_unique,
                   g_update_subresource_calls.load(std::memory_order_relaxed),
                   g_copy_resource_calls.load(std::memory_order_relaxed));
        }
        // 进程退出：统一释放全局持久化的替换缓存（正常运行时不随原纹理销毁，
        // 退出时一次清空；进程卸载后驱动侧资源由系统回收，此处显式 Release）
        {
            std::lock_guard<std::mutex> lk(g_lock);
            for (auto &kv : g_replacements) {
                if (kv.second.srv) {
                    RepSrvRemove(kv.second.srv);
                    kv.second.srv->Release();
                }
                if (kv.second.texture)
                    kv.second.texture->Release();
            }
            g_replacements.clear();
            g_idleSet.clear();
            g_idleLastUsed.clear();
            g_evictRecords.clear();
            g_cacheBytesTotal.store(0, std::memory_order_relaxed);
        }
        ::tloader::log_shutdown();
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 导出：供宿主（Starward 插件加载器 / 其他 DLL）在 D3D11 设备创建后调用。
// 若宿主直接加载本 DLL，可调用此函数完成 hook。
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) void TextureLoader_Attach(ID3D11Device *device,
                                                           ID3D11DeviceContext *context)
{
    AttachToDevice(device, context);
}
