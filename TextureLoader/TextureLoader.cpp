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
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
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
std::mutex g_pendingMutex;
std::deque<IUnknown *> g_pendingRelease;
volatile long g_pendingCount = 0;

static void QueueRelease(IUnknown *obj)
{
    if (!obj)
        return;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_pendingRelease.push_back(obj);
    }
    InterlockedIncrement(&g_pendingCount);
}

// 渲染线程调用：释放所有排队对象（仅当队列非空时锁一次）
static void FlushPendingRelease()
{
    if (InterlockedCompareExchange(&g_pendingCount, 0, 0) == 0)
        return;
    std::deque<IUnknown *> batch;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        batch.swap(g_pendingRelease);
    }
    InterlockedExchange(&g_pendingCount, 0);
    for (IUnknown *obj : batch)
        obj->Release();
}

// 快速命中表：所有已加载替换纹理的 hash 集合。绑定热路径改用一个只写不删的
// 并发集合，把"是否有替换"的查询压到接近无锁开销。
std::shared_mutex g_hitLock;
std::unordered_set<uint32_t> g_hasReplacement;

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

static void ActiveAdd(ID3D11Resource *res)
{
    if (!res)
        return;
    std::lock_guard<std::mutex> lk(g_activeMutex);
    long n = g_activeCount.load(std::memory_order_relaxed);
    if (n >= kMaxActiveResources)
        return; // 满：放弃（实际存活数远小于 4096，正常不会触发）
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

// 异步替换纹理加载：切换角色/加载场景时会瞬间命中大量纹理，若在渲染线程
// 同步 LoadDdsTexture（磁盘 IO + 建纹理）会造成帧时间尖峰。D3D11 device 方法
// 线程安全，因此把加载丢给后台线程，渲染线程只做轻量登记。
struct AsyncLoadTask {
    uint32_t hash;
    std::wstring dds_path;
};
std::mutex g_loadMutex;
std::condition_variable g_loadCv;
std::deque<AsyncLoadTask> g_loadQueue;
bool g_loadThreadRunning = false;

// resource -> 3DMigoto 兼容 hash（CreateTexture2D 时填充）
// 注意：不用裸指针 map（资源销毁后地址可能被复用导致错误替换），
// 改为通过 ID3D11Resource::SetPrivateData 把 hash 挂在资源本身上，随资源消亡。
// 替换纹理/SRV 缓存按原纹理生命周期引用计数，原纹理销毁后自动释放，避免显存积累。
// 另外提供显存感知的 LRU 淘汰：显存紧张时释放最久未用的替换缓存（释放后可从磁盘
// 重新异步加载），实现缓存随显存动态伸缩。
struct ReplacementEntry {
    ID3D11Texture2D *texture = nullptr;          // 替换纹理（缓存持有）
    ID3D11ShaderResourceView *srv = nullptr;     // 替换 SRV（缓存持有）
    uint32_t refcount = 0;                        // 活跃原纹理数
    uint64_t last_used = 0;                       // 最近绑定命中时间（GetTickCount64）
    uint64_t mem_bytes = 0;                       // 估算显存占用（纹理大小）
    uint64_t gdds_ready_fence = 0;               // GDDS 完成 fence 值（0=非 GDDS/已等待）
    DXGI_FORMAT original_format = DXGI_FORMAT_UNKNOWN; // 游戏原纹理格式（CreateTexture2D 时记录）
    UINT original_mips = 0;                       // 游戏原纹理 mip 数（0=未知）
};
std::unordered_map<uint32_t, ReplacementEntry> g_replacements;
// 从 Mods 解析出的 [TextureOverride] hash -> dds
std::unordered_map<uint32_t, TextureOverrideEntry> g_overrides;

// 显存感知缓存控制
IDXGIAdapter3 *g_dxgi_adapter3 = nullptr;        // 查询显存的适配器
volatile long g_vramMonitorRunning = 0;

// 资源私有数据 GUID（用于存储纹理 hash 与动态标记、释放跟踪器）
static const GUID TL_HASH_GUID = {0x8a2f6d4e, 0x7c31, 0x4a9b, {0x9e, 0x1c, 0x2d, 0x5f, 0x8a, 0x0b, 0x3c, 0x71}};
static const GUID TL_DYNAMIC_GUID = {0x9b3e7f5a, 0x8d42, 0x4bc1, {0xaf, 0x2d, 0x3e, 0x60, 0x9c, 0x1d, 0x4e, 0x82}};
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
        ULONG r = --ref;
        if (r == 0) {
            // 原纹理销毁：替换缓存引用计数 -1；归零则释放替换纹理/SRV。
            // 注意：D3D 调用本函数时已持有其内部锁（AB-BA 死锁风险），
            // 因此锁内只操作 map 并收集指针，锁外再 Release D3D 资源。
            ID3D11Texture2D *tex = nullptr;
            ID3D11ShaderResourceView *srv = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                auto it = g_replacements.find(hash);
                if (it != g_replacements.end()) {
                    if (it->second.refcount > 0)
                        it->second.refcount--;
                    if (it->second.refcount == 0) {
                        srv = it->second.srv;
                        tex = it->second.texture;
                        it->second.srv = nullptr;
                        it->second.texture = nullptr;
                        g_replacements.erase(it);
                        // 条目已删：同步从快速命中表移除，避免只增不删内存膨胀
                        // （VramMonitor 淘汰路径保留条目+hash 以便重新加载，此处
                        //  原纹理已全部销毁，hash 不再需要）。
                        {
                            std::unique_lock<std::shared_mutex> hl(g_hitLock);
                            g_hasReplacement.erase(hash);
                        }
                    }
                }
                // 活跃资源数组：本原纹理已销毁，清空其槽位供复用
                ActiveRemove(resource);
            }
            if (srv)
                QueueRelease(srv);
            if (tex)
                QueueRelease(tex);
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
static void MarkDynamic(ID3D11Resource *res)
{
    if (res) {
        uint32_t v = 1;
        res->SetPrivateData(TL_DYNAMIC_GUID, sizeof(v), &v);
    }
}
static bool IsDynamic(ID3D11Resource *res)
{
    if (!res)
        return false;
    UINT size = sizeof(uint32_t);
    uint32_t v = 0;
    HRESULT hr = res->GetPrivateData(TL_DYNAMIC_GUID, &size, &v);
    return SUCCEEDED(hr) && v != 0;
}

volatile long g_hook_ready = 0;
volatile long g_stats_created = 0;   // 已哈希的纹理数
volatile long g_stats_matched = 0;   // 命中的替换数
volatile long g_stats_bound = 0;     // 替换 SRV 被绑定次数
volatile bool g_observe_only = true; // 1=只记录匹配，0=真正替换

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
        gdds_fence = it->second.gdds_ready_fence;
        it->second.gdds_ready_fence = 0; // 只等待一次
        tex->AddRef(); // 锁外使用安全：防止后台加载线程/销毁同时释放
    }
    if (!g_device)
        return nullptr;

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
        srv->AddRef();
        tex->Release();
        return srv;
    }
}

// ---------------------------------------------------------------------------
// Hook: CreateTexture2D — 计算哈希、登记替换纹理
// ---------------------------------------------------------------------------

// 重入保护：LoadDdsTexture 创建替换纹理时直接转发，不再进入哈希/替换逻辑
static thread_local bool t_in_create_texture = false;

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
        const TextureOverrideEntry *ov = nullptr;
        bool matched = FindOverride(g_overrides, hash, &ov);

        if (matched && ov) {
            InterlockedIncrement(&g_stats_created);
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
                    AsyncLoadTask lt;
                    lt.hash = hash;
                    lt.dds_path = ov->dds_path;
                    {
                        std::lock_guard<std::mutex> lk(g_loadMutex);
                        g_loadQueue.push_back(std::move(lt));
                    }
                    g_loadCv.notify_one();
                }
            }
        }
    }
    return hr;
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
    // 无锁线性扫 g_activeArr（几十项），无共享锁开销。
    bool anyHit = false;
    for (UINT i = 0; i < NumViews; i++) {
        ID3D11ShaderResourceView *srv = ppShaderResourceViews[i];
        if (!srv)
            continue;
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
        // 动态纹理（创建后被 UpdateSubresource 更新过）跳过替换，避免闪烁
        if (hash && IsDynamic(res))
            hash = 0;
        // g_hasReplacement 由 AsyncLoadThread 并发写入，读必须持共享锁
        // （unordered_set 并发读写是未定义行为，高速切角色时曾崩溃）
        if (hash) {
            std::shared_lock<std::shared_mutex> hl(g_hitLock);
            if (!g_hasReplacement.count(hash))
                hash = 0;
        }
        if (hash) {
            // 检查替换缓存是否还在（可能被显存淘汰 → texture 为空）
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
                if (!dds_path.empty()) {
                    AsyncLoadTask lt;
                    lt.hash = hash;
                    lt.dds_path = std::move(dds_path);
                    {
                        std::lock_guard<std::mutex> lk(g_loadMutex);
                        g_loadQueue.push_back(std::move(lt));
                    }
                    g_loadCv.notify_one();
                }
            } else {
                // 读取原 SRV 的视图描述，创建格式/mip 一致的替换 SRV
                D3D11_SHADER_RESOURCE_VIEW_DESC origDesc;
                srv->GetDesc(&origDesc);
                ID3D11ShaderResourceView *rep = CreateReplacementSRV(hash, &origDesc);
                if (rep) {
                    views[i] = rep;
                    InterlockedIncrement(&g_stats_bound);
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

// ---------------------------------------------------------------------------
// Hook: UpdateSubresource — 标记动态纹理（创建后被更新的，动画/飘带模拟等）
// ---------------------------------------------------------------------------

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
    // 若该资源是活跃替换资源（已被我们替换），之后又被更新 → 标记为动态，
    // SetShaderResources 慢路径会跳过替换避免闪烁。
    // 先无锁判断是否为空：空则（替换未启用）直接跳过，零开销。
    if (pDstResource && g_activeCount != 0) {
        if (ActiveContains(pDstResource)) {
            uint32_t h = 0;
            std::lock_guard<std::mutex> lk(g_lock);
            if (GetResourceHash(pDstResource, &h))
                MarkDynamic(pDstResource);
        }
    }
    g_real_update_subresource(This, pDstResource, DstSubresource, pDstBox,
                              pSrcData, SrcRowPitch, SrcDepthPitch);
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
    SetShaderResources_t hooks[6] = {Hook_PS, Hook_VS, Hook_GS, Hook_HS, Hook_DS, Hook_CS};
    for (int i = 0; i < 6; i++)
        g_real_ssr[i] = funcs[i];

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (int i = 0; i < 6; i++)
        DetourAttach(&(PVOID &)g_real_ssr[i], hooks[i]);
    // UpdateSubresource：标记动态纹理
    g_real_update_subresource = (UpdateSubresource_t)vtbl->UpdateSubresource;
    DetourAttach(&(PVOID &)g_real_update_subresource, HookUpdateSubresource);
    DetourTransactionCommit();
    for (int i = 0; i < 6; i++)
        TL_LOG(L"[ok  ] hooked %ls.SetShaderResources @ %p", g_stage_name[i], g_real_ssr[i]);
    TL_LOG(L"[ok  ] hooked UpdateSubresource @ %p", g_real_update_subresource);
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
        if (g_hook_ready)
            return 0;
        if (InstallCreateDeviceHook())
            return 0;
        Sleep(5);
    }
    TL_LOG(L"[warn] bootstrap gave up waiting for d3d11.dll");
    return 0;
}

// 后台替换纹理加载线程：消费 g_loadQueue，LoadDdsTexture（磁盘 IO + 建纹理），
// 完成后更新替换表。渲染线程只做轻量登记，避免切换角色时的帧时间尖峰。
// .gdds 扩展名 → GDDS DirectStorage GPU 解压路径（gdds_interop）。
DWORD WINAPI AsyncLoadThread(LPVOID)
{
    while (true) {
        AsyncLoadTask task;
        {
            std::unique_lock<std::mutex> lk(g_loadMutex);
            g_loadCv.wait(lk, [] { return !g_loadQueue.empty(); });
            task = std::move(g_loadQueue.front());
            g_loadQueue.pop_front();
        }
        ID3D11Device *dev = g_device;
        if (!dev)
            continue;
        // 检查是否仍需要加载（可能已被其他实例抢先加载）
        {
            std::lock_guard<std::mutex> lk(g_lock);
            auto it = g_replacements.find(task.hash);
            if (it == g_replacements.end() || it->second.texture) {
                continue; // 已加载或条目已移除
            }
        }
        DdsLoadResult res;
        t_in_create_texture = true; // 后台线程也防止 LoadDdsTexture 内部重入
        HRESULT lhr = E_FAIL;
        // 按扩展名分发：.gdds → DirectStorage GPU 解压；其余 → 现有 CPU DDS 路径
        const bool is_gdds = _wcsicmp(
            wcsrchr(task.dds_path.c_str(), L'.') ? wcsrchr(task.dds_path.c_str(), L'.') : L"",
            L".gdds") == 0;
        if (is_gdds) {
            if (tloader_gdds::Initialize(dev)) {
                uint64_t ready_fence = 0;
                ID3D11Texture2D *tex = tloader_gdds::LoadGddsTexture(task.dds_path.c_str(),
                                                                     &ready_fence, &res.skipped);
                if (tex) {
                    res.texture = tex; // 已 AddRef（登记持有）
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    res.format = td.Format;
                    res.width = td.Width;
                    res.height = td.Height;
                    res.array_size = td.ArraySize;
                    res.mip_levels = td.MipLevels;
                    res.gdds_ready_fence = ready_fence; // 供渲染线程绑定前等待
                    lhr = S_OK;
                }
            } else {
                TL_LOG(L"[fail] tex hash=0x%08X gdds init failed (DirectStorage unavailable)",
                       task.hash);
            }
        } else {
            lhr = LoadDdsTexture(dev, task.dds_path.c_str(), &res);
        }
        t_in_create_texture = false;
        if (SUCCEEDED(lhr) && res.texture) {
            std::lock_guard<std::mutex> lk(g_lock);
            auto it = g_replacements.find(task.hash);
            if (it != g_replacements.end() && !it->second.texture) {
                it->second.texture = res.texture;
                it->second.gdds_ready_fence = res.gdds_ready_fence; // GDDS 完成 fence（渲染线程等待）
                // 真实显存占用估算（BC 按块/其余按像素，含 mip 链与数组）——淘汰按大小优先
                it->second.mem_bytes = EstimateTextureMemory(res.format, res.width, res.height,
                                                             res.mip_levels, res.array_size);
                // 快速命中表：替换已加载，此后该 hash 可命中
                {
                    std::unique_lock<std::shared_mutex> hl(g_hitLock);
                    g_hasReplacement.insert(task.hash);
                }
                TL_LOG_IF(1, L"[HIT ] tex hash=0x%08X (async) replaced dds=%ls",
                       task.hash, task.dds_path.c_str());
            } else {
                // 锁内不直接 Release（D3D 回调可能反向等 g_lock）——延迟到渲染线程
                QueueRelease(res.texture);
            }
        } else if (res.skipped) {
            // max_texture_side 配置跳过（非错误）——仅 level>=1 记录
            TL_LOG_IF(1, L"[skip] tex hash=0x%08X dds over max_texture_side", task.hash);
        } else {
            TL_LOG(L"[fail] tex hash=0x%08X dds load failed hr=0x%08X", task.hash, lhr);
        }
    }
    return 0;
}


// 显存感知缓存监控线程：定期查询可用显存，紧张时按 LRU 淘汰替换缓存。
// 淘汰优先 refcount==0（原纹理已销毁，最安全）的条目；若仍紧张，再淘汰
// 很久未用的活跃条目（释放后绑定会退化原纹理并触发重新加载）。
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

    while (g_dxgi_adapter3) {
        Sleep(2000);
        DXGI_QUERY_VIDEO_MEMORY_INFO info;
        if (FAILED(g_dxgi_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            continue;
        // 压力判定（显存容量驱动）：
        //   - g_vram_threshold_bytes>0  → 可用显存 < 指定容量（如 1024M/2G）
        //   - 否则                       → 可用显存 < 总显存百分比（默认 15%）
        // 未达阈值不淘汰，尽量保证游戏流畅；仅在压力持续时温和淘汰。
        uint64_t budget = info.Budget;
        uint64_t avail = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
        bool pressure = false;
        if (::tloader::g_vram_threshold_bytes > 0) {
            pressure = avail < ::tloader::g_vram_threshold_bytes;
        } else {
            int pct = ::tloader::g_vram_threshold_pct;
            if (pct <= 0)
                pct = 15;
            pressure = (budget > 0) && (avail * 100 / budget < (uint64_t)pct);
        }

        if (pressure) {
            // 淘汰替换缓存，释放显存（按大小优先：大纹理先释放，回收更多显存）
            TL_LOG(L"[vram] pressure: budget=%lluMB usage=%lluMB avail=%lluMB (threshold=%s), evicting...",
                   (unsigned long long)(budget >> 20), (unsigned long long)(info.CurrentUsage >> 20),
                   (unsigned long long)(avail >> 20),
                   ::tloader::g_vram_threshold_bytes > 0
                       ? (std::to_wstring(::tloader::g_vram_threshold_bytes >> 20) + L"MB").c_str()
                       : (std::to_wstring(::tloader::g_vram_threshold_pct) + L"%").c_str());
            // 收集候选：仅 refcount==0（原纹理已销毁，最安全），
            // 排序：mem_bytes 降序（大纹理优先）→ last_used 升序（LRU 次之）
            struct EvictCand { uint64_t mem; uint64_t last_used; uint32_t hash; };
            std::vector<EvictCand> candidates;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                for (auto &kv : g_replacements) {
                    if (kv.second.refcount == 0 && kv.second.texture)
                        candidates.push_back({kv.second.mem_bytes, kv.second.last_used, kv.first});
                }
                std::sort(candidates.begin(), candidates.end(),
                          [](const EvictCand &a, const EvictCand &b) {
                              if (a.mem != b.mem)
                                  return a.mem > b.mem; // 大纹理优先
                              return a.last_used < b.last_used; // LRU
                          });
            }
            // 逐条淘汰（每轮少量，避免一次释放过多导致卡顿）
            int evicted = 0;
            uint64_t freed_bytes = 0;
            for (auto &c : candidates) {
                if (evicted >= 4)
                    break;
                ID3D11Texture2D *tex = nullptr;
                ID3D11ShaderResourceView *srv = nullptr;
                {
                    std::lock_guard<std::mutex> lk(g_lock);
                    auto it = g_replacements.find(c.hash);
                    if (it == g_replacements.end() || it->second.refcount != 0 || !it->second.texture)
                        continue;
                    tex = it->second.texture;
                    srv = it->second.srv;
                    it->second.texture = nullptr;
                    it->second.srv = nullptr;
                    // 保留条目（refcount=0），供重新加载；从 g_hasReplacement 移除前
                    // 保留 hash 以便命中时重新加载
                    freed_bytes += it->second.mem_bytes;
                    it->second.mem_bytes = 0;
                }
                if (tex) QueueRelease(tex);
                if (srv) QueueRelease(srv);
                evicted++;
            }
            if (evicted > 0)
                TL_LOG(L"[vram] evicted %d replacement caches (freed ~%lluMB)",
                       evicted, (unsigned long long)(freed_bytes >> 20));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 轻量配置读取：DLL 同目录 TextureLoader.ini（key = value，支持 ; 注释）
// ---------------------------------------------------------------------------

static std::wstring GetIniValue(const std::wstring &dir, const std::wstring &key)
{
    std::wstring path = dir + L"TextureLoader.ini";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
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
        if (k == key)
            return v;
    }
    return L"";
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
// 无后缀数字按 MB 处理）；空值/非法（<=0、非数字）回退默认 15%。结果写入共享配置。
static void ParseVramThreshold(const std::wstring &dir)
{
    ::tloader::g_vram_threshold_pct = 15;        // 默认：可用显存 < 总显存 15%
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
        return; // 非法 → 默认

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

static void AttachToDevice(ID3D11Device *device, ID3D11DeviceContext *context)
{
    if (g_hook_ready)
        return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_hook_ready)
        return;
    g_device = device;
    g_device->AddRef();
    g_context = context;
    g_context->AddRef();
    HookDevice(device);
    HookContext(context);
    InterlockedExchange(&g_hook_ready, 1);
    // 启动后台替换纹理加载线程（仅一次）
    if (!g_loadThreadRunning) {
        g_loadThreadRunning = true;
        CreateThread(nullptr, 0, AsyncLoadThread, nullptr, 0, nullptr);
    }
    // 启动显存监控线程（仅一次）
    if (!g_vramMonitorRunning) {
        InterlockedExchange(&g_vramMonitorRunning, 1);
        CreateThread(nullptr, 0, VramMonitorThread, nullptr, 0, nullptr);
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
// 崩溃捕获：向量化异常处理器（崩溃时记录异常信息 + 模块级调用栈到日志）
// ---------------------------------------------------------------------------

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI CrashHandler(PEXCEPTION_POINTERS ep)
{
    // 只捕获访问违例/非法指令等致命异常；调试器在场时不介入
    if (IsDebuggerPresent())
        return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;

    void *addr = ep->ExceptionRecord->ExceptionAddress;
    // 记录异常信息（含线程 ID——区分异步加载线程/渲染线程/游戏线程）
    TL_LOG(L"[CRASH] exception=0x%08X addr=%p tid=0x%X", code, addr, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        TL_LOG(L"[CRASH] access violation %s address=0x%llX",
               ep->ExceptionRecord->ExceptionInformation[0] == 0 ? L"READ" : L"WRITE",
               (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    // 寄存器上下文（定位空调用来源：RIP=0 时看哪个寄存器为 0 被 call）
    if (ep->ContextRecord) {
        auto *ctx = ep->ContextRecord;
        TL_LOG(L"[CRASH] regs RAX=%p RBX=%p RCX=%p RDX=%p RSI=%p RDI=%p RSP=%p RBP=%p",
               (void *)ctx->Rax, (void *)ctx->Rbx, (void *)ctx->Rcx, (void *)ctx->Rdx,
               (void *)ctx->Rsi, (void *)ctx->Rdi, (void *)ctx->Rsp, (void *)ctx->Rbp);
        TL_LOG(L"[CRASH] regs R8=%p R9=%p R10=%p R11=%p R12=%p R13=%p R14=%p R15=%p",
               (void *)ctx->R8, (void *)ctx->R9, (void *)ctx->R10, (void *)ctx->R11,
               (void *)ctx->R12, (void *)ctx->R13, (void *)ctx->R14, (void *)ctx->R15);
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
        TL_LOG(L"[CRASH]   #%02u %s+0x%llX (%p)", i, modName,
               (unsigned long long)((uintptr_t)frames[i] - base), frames[i]);
    }

    // 原始栈扫描：CaptureStackBackTrace 在栈损坏/堆损坏时失效（只见 ntdll 分发），
    // 直接从异常上下文 RSP 逐 qword 读返回地址，识别落在已知模块内的帧，
    // 定位 use-after-free 的真实调用链（如 d3d11.dll/YuanShen.exe/TextureLoader.dll）。
    if (ep->ContextRecord) {
        uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;
        TL_LOG(L"[CRASH] raw-stack from RSP=%p:", (void *)rsp);
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
                TL_LOG(L"[CRASH]   [rsp+0x%03X] %s+0x%llX", i * 8, modName,
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
        g_observe_only = IniBool(dir, L"observe_only", true);
        TL_LOG(L"[cfg ] observe_only=%d", (int)g_observe_only);

        // 可调配置（供 gdds_interop/dds_loader/监控线程跨模块共享）
        ::tloader::g_log_level = IniInt(dir, L"log_level", 1);
        ::tloader::g_max_texture_side = IniInt(dir, L"max_texture_side", 0);
        ParseVramThreshold(dir);
        TL_LOG(L"[cfg ] log_level=%d vram_threshold_pct=%d vram_threshold_bytes=%llu max_texture_side=%d",
               ::tloader::g_log_level, ::tloader::g_vram_threshold_pct,
               (unsigned long long)::tloader::g_vram_threshold_bytes,
               ::tloader::g_max_texture_side);

        // 扫描 Mods 目录：优先用 ini 里的 mods_dir，其次 DLL 同级/上一级 Mods
        std::wstring mods_dir;
        std::wstring ini_mods = GetIniValue(dir, L"mods_dir");
        if (!ini_mods.empty()) {
            DWORD attr = GetFileAttributesW(ini_mods.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                mods_dir = ini_mods;
            else
                TL_LOG(L"[warn] configured mods_dir not found: \"%ls\"", ini_mods.c_str());
        }
        std::vector<std::wstring> candidates;
        candidates.push_back(dir + L"Mods");
        if (slash) {
            std::wstring up = std::wstring(path, slash + 1) + L"..\\Mods";
            candidates.push_back(up);
        }
        if (mods_dir.empty()) {
            for (const auto &c : candidates) {
                DWORD attr = GetFileAttributesW(c.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    mods_dir = c;
                    break;
                }
            }
        }
        if (mods_dir.empty()) {
            TL_LOG(L"[warn] Mods directory not found (tried %d paths)", (int)candidates.size() + (ini_mods.empty() ? 0 : 1));
        } else {
            size_t n = LoadModInis(mods_dir, g_overrides);
            TL_LOG(L"[ini ] loaded %zu texture overrides from \"%ls\"", n, mods_dir.c_str());
        }

        // 优先同步安装 D3D11CreateDevice hook：d3d11 通常已被 Bridge/ReShade
        // 提前加载，此时立即 hook 才能在游戏创建设备前截获。若 d3d11 尚未
        // 加载（极少数情况），退回后台线程等待。
        if (!InstallCreateDeviceHook()) {
            CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        } else {
            TL_LOG(L"[ok  ] installed synchronously in DllMain");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        ::tloader_gdds::Shutdown(); // 释放 GDDS 互操作（D3D12/DS 队列/共享 fence）
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
