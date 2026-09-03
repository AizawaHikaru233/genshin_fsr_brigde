// SPDX-License-Identifier: GPL-3.0-or-later
// gdds_interop.cpp — GDDS DirectStorage GPU 解压互操作层实现
//
// TextureLoader is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// 链路（对齐作者 GDDS 加载器反汇编，GPU 实测通过）：
//   GDDS 解析 → D3D11 SHARED_NTHANDLE 纹理 → D3D12 OpenSharedHandle（同 GPU 别名）
//   → DirectStorage TEXTURE_REGION 逐流（GDeflate GPU 解压直写，每流 ≤8MB）
//   → 共享 fence（D3D12 Signal → D3D11 Wait）→ 替换纹理就绪
//
// GDDS 多子流格式（作者加载器反汇编破解——关键差异，旧实现只加载第一条流，
// 导致 4096² 纹理只有一半数据 → 模糊/颜色异常）：
//   @124  u32 = 子流数量 N
//   @148  N × 16B 记录 { u64 流文件偏移, u32 压缩大小, u32 未压缩大小 }
//   每条流 = 独立 GDeflate TileStream（04 FB 头），未压缩 ≤8MB
//   Σ各流未压缩 = 纹理全尺寸（4096² BC3=2×8MB；16384² BC1=16×8MB）
//   逐流独立 TEXTURE_REGION 请求 → 单请求未压缩 ≤8MB，天然规避 DS
//   单请求 32MB 上限（8192/16384 大纹理不再跳过）。
//
// 关键坑（Phase 1 已解决）：
//   - 队列 qd.Device 必须设（否则 E_DSTORAGE_INVALID_DESTINATION_TYPE 0x89240040）
//   - 跨 API 同步用共享 fence（D3D12 Signal → D3D11 ID3D11DeviceContext4::Wait）

#include "gdds_interop.h"

#include "log.h" // tloader::log_write（debug_log 同时写 TextureLoader.log）

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dstorage.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tloader_gdds
{

namespace
{

void debug_log(const char *msg, HRESULT hr = S_OK); // 前向声明（定义在下方；带默认参数）

// ---- 动态加载 dstorage.dll（不静态链接——注入场景依赖搜索坑）----
// LoadLibrary(TextureLoader.dll) 时 Windows 按 exe/系统目录搜依赖，不包含
// payload\TextureLoader\——静态链接 dstorage.lib 会让整个 DLL 加载失败
// (err=126)。改为运行时按 TextureLoader.dll 同目录绝对路径加载 +
// GetProcAddress 解析 DStorageGetFactory（COM 接口方法走 vtable 无需导入）。
typedef HRESULT(WINAPI *DStorageGetFactoryFn)(REFIID riid, void **ppv);
DStorageGetFactoryFn g_pfn_get_factory = nullptr;
HMODULE g_h_dstorage = nullptr;

bool LoadDstorageRuntime()
{
    if (g_pfn_get_factory != nullptr)
        return true;

    // TextureLoader.dll 同目录
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(GetModuleHandleW(L"TextureLoader.dll"), self, MAX_PATH);
    wchar_t *slash = wcsrchr(self, L'\\');
    if (!slash)
        return false;
    *slash = L'\0';
    std::wstring dir = self;

    // 先加载 dstoragecore.dll（dstorage.dll 的依赖），再加载 dstorage.dll
    HMODULE h_core = LoadLibraryW((dir + L"\\dstoragecore.dll").c_str());
    if (h_core != nullptr)
        g_h_dstorage = h_core; // 保持句柄存活（core 由 dstorage 持有引用）
    HMODULE h_main = LoadLibraryW((dir + L"\\dstorage.dll").c_str());
    if (h_main == nullptr)
    {
        debug_log("LoadLibrary dstorage.dll failed", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    g_h_dstorage = h_main;
    g_pfn_get_factory = reinterpret_cast<DStorageGetFactoryFn>(
        reinterpret_cast<void *>(GetProcAddress(h_main, "DStorageGetFactory")));
    if (g_pfn_get_factory == nullptr)
    {
        debug_log("GetProcAddress DStorageGetFactory failed");
        return false;
    }
    return true;
}

void UnloadDstorageRuntime()
{
    if (g_h_dstorage != nullptr)
    {
        FreeLibrary(g_h_dstorage);
        g_h_dstorage = nullptr;
    }
    g_pfn_get_factory = nullptr;
}

std::mutex g_mutex;
bool g_initialized = false;

ID3D11Device *g_game_device = nullptr;

// 自建同适配器 D3D12 设备（唯一——多实例合法但此处单设备足够）
ComPtr<ID3D12Device> g_d12dev;

// DirectStorage
ComPtr<IDStorageFactory> g_ds_factory;
ComPtr<IDStorageQueue> g_ds_queue;

// 共享 fence（D3D12 Signal → D3D11 Wait）
ComPtr<ID3D12Fence> g_shared_fence;
ComPtr<ID3D11Fence> g_shared_fence11;
UINT64 g_fence_value = 0;
ComPtr<ID3D12CommandQueue> g_signal_queue; // 专用 COPY 队列（Signal 用）

void debug_log(const char *msg, HRESULT hr)
{
    // log_level>=1 才写 GDDS 细节（0=仅关键日志）；错误（hr 失败）总是记录
    if (::tloader::g_log_level < 1 && SUCCEEDED(hr))
        return;
    char line[256];
    if (FAILED(hr))
        std::snprintf(line, sizeof(line), "[gdds] %s (hr=0x%08X)\n", msg, (unsigned)hr);
    else
        std::snprintf(line, sizeof(line), "[gdds] %s\n", msg);
    OutputDebugStringA(line);
    fputs(line, stderr);
    // 同时写入 TextureLoader.log（line 已含 "[gdds] " 前缀，log_write 不再重复加）
    wchar_t wline[320];
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 320);
    ::tloader::log_write(L"%ls", wline);
}

// ---- GDDS 解析（多子流格式，作者加载器反汇编对齐）----
struct GddsStream
{
    uint64_t offset = 0;   // 流在文件中的绝对偏移
    uint32_t compressed = 0;   // 压缩数据大小（字节）
    uint32_t uncompressed = 0; // 该流解压后大小（字节）
};

struct GddsInfo
{
    UINT width = 0, height = 0, mips = 1, array = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<GddsStream> streams; // 子流表（记录数组 @148）
    uint64_t total_uncompressed = 0; // Σ各流未压缩 = 纹理全尺寸
};

bool ParseGdds(const std::vector<uint8_t> &data, GddsInfo &out)
{
    if (data.size() < 200 || memcmp(data.data(), "GDDS", 4) != 0)
        return false;
    const uint8_t *dds = data.data() + 4;
    out.height = *(uint32_t *)(dds + 8);
    out.width = *(uint32_t *)(dds + 12);
    out.mips = *(uint32_t *)(dds + 24);
    const uint8_t *dxt = data.data() + 128;
    out.format = (DXGI_FORMAT) * (uint32_t *)dxt;
    out.array = *(uint32_t *)(dxt + 12);

    // 子流数量：DDS_HEADER 尾部 dwReserved2（@124）
    const uint32_t stream_count = *(uint32_t *)(data.data() + 124);
    if (stream_count == 0 || stream_count > 64)
        return false;
    const size_t need = 148 + (size_t)stream_count * 16;
    if (data.size() < need)
        return false;

    out.streams.clear();
    out.total_uncompressed = 0;
    const size_t records_end = 148 + (size_t)stream_count * 16;
    for (uint32_t i = 0; i < stream_count; i++)
    {
        const uint8_t *rec = data.data() + 148 + (size_t)i * 16;
        GddsStream s;
        s.offset = *(uint64_t *)(rec + 0);
        s.compressed = *(uint32_t *)(rec + 8);
        s.uncompressed = *(uint32_t *)(rec + 12);
        // 记录区之后才是流数据；流偏移不能落在记录区内。
        if (s.offset < records_end || s.compressed == 0 || s.uncompressed == 0)
            return false;
        // 仅当流偏移落在已读头部缓冲内才校验 GDeflate magic（04 FB）——
        // 大纹理后段流的偏移远超 4096 头缓冲（如 Gorou 流1 @1281176），
        // 不能越界读取；其余流信任记录表（作者加载器同样只读记录数组）。
        if (s.offset + 2 <= data.size() &&
            (data[s.offset] != 0x04 || data[s.offset + 1] != 0xFB))
        {
            return false;
        }
        out.streams.push_back(s);
        out.total_uncompressed += s.uncompressed;
    }
    return true;
}

std::vector<uint8_t> ReadFileHeader(const wchar_t *path, size_t want, uint64_t *file_size_out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (file_size_out)
        *file_size_out = (uint64_t)sz.QuadPart;
    size_t to_read = sz.QuadPart < (LONGLONG)want ? (size_t)sz.QuadPart : want;
    std::vector<uint8_t> buf(to_read);
    DWORD got = 0;
    if (!ReadFile(h, buf.data(), (DWORD)to_read, &got, nullptr))
        got = 0;
    CloseHandle(h);
    buf.resize(got);
    return buf;
}

// 块压缩格式判定（BC 格式行距按“块行”计，region 像素坐标需 ×4）
bool IsBlockCompressed(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

// 创建 SHARED_NTHANDLE 替换纹理 + D3D12 别名（Phase 1 实测链路）
// 格式：用 GDDS 文件头格式（Phase 1 实测；SRGB 变体映射曾尝试——若游戏按
// UNORM 采样会变暗，日志记录实际视图格式后再定方案）。
bool CreateReplacementTexture(const GddsInfo &info, ComPtr<ID3D11Texture2D> &d11,
                              ComPtr<ID3D12Resource> &d12)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = info.width;
    d.Height = info.height;
    // GDDS 只含 mip0（Phase 1 验证文件 mips=1）；强制单 mip，避免
    // 多 mip 纹理的 1+ 级未初始化垃圾被采样（黑/花屏甚至驱动异常）。
    d.MipLevels = 1;
    d.ArraySize = 1; // 仅支持单切片（TEXTURE_REGION 只填 subresource 0）
    d.Format = info.format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(g_game_device->CreateTexture2D(&d, nullptr, &d11)))
        return false;
    ComPtr<IDXGIResource1> r1;
    if (FAILED(d11.As(&r1)))
        return false;
    HANDLE h = nullptr;
    if (FAILED(r1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)))
        return false;
    HRESULT hr = g_d12dev->OpenSharedHandle(h, IID_PPV_ARGS(&d12));
    CloseHandle(h);
    return SUCCEEDED(hr) && d12 != nullptr;
}

// DS 队列完成 → Signal 共享 fence（D3D12 COPY 队列，不占渲染路径）
void SignalSharedFence()
{
    const UINT64 value = ++g_fence_value;
    g_signal_queue->Signal(g_shared_fence.Get(), value);
}

} // namespace (anonymous 内部实现)

bool Initialize(ID3D11Device *game_device)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized)
        return true;
    if (!game_device)
        return false;
    g_game_device = game_device;

    // 1. 同适配器 D3D12 设备（共享纹理互操作必需同一 LUID）
    {
        ComPtr<IDXGIDevice> dxgi_dev;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(game_device->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) ||
            FAILED(dxgi_dev->GetAdapter(&adapter)))
        {
            debug_log("get adapter failed");
            return false;
        }
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                     IID_PPV_ARGS(&g_d12dev))))
        {
            debug_log("D3D12CreateDevice failed");
            return false;
        }
    }

    // 2. DirectStorage 工厂 + 队列（qd.Device 必须设——GPU 解压目标）
    {
        if (!LoadDstorageRuntime() || g_pfn_get_factory == nullptr)
        {
            debug_log("DStorageGetFactory unavailable (dstorage.dll not loaded)");
            return false;
        }
        if (FAILED(g_pfn_get_factory(IID_PPV_ARGS(&g_ds_factory))))
        {
            debug_log("DStorageGetFactory failed (dstorage.dll missing?)");
            return false;
        }
        DSTORAGE_QUEUE_DESC qd = {};
        qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qd.Priority = DSTORAGE_PRIORITY_NORMAL;
        qd.Name = "gdds-tloader";
        qd.Device = g_d12dev.Get(); // REQUIRED for GPU decompression
        if (FAILED(g_ds_factory->CreateQueue(&qd, IID_PPV_ARGS(&g_ds_queue))))
        {
            debug_log("CreateQueue failed");
            return false;
        }
    }

    // 3. 共享 fence（D3D12 Signal → D3D11 Wait）
    {
        if (FAILED(g_d12dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                         IID_PPV_ARGS(&g_shared_fence))))
        {
            debug_log("CreateFence shared failed");
            return false;
        }
        HANDLE fh = nullptr;
        if (FAILED(g_d12dev->CreateSharedHandle(g_shared_fence.Get(), nullptr,
                                                GENERIC_ALL, nullptr, &fh)))
        {
            debug_log("fence CreateSharedHandle failed");
            return false;
        }
        ComPtr<ID3D11Device5> d11v5;
        if (FAILED(game_device->QueryInterface(IID_PPV_ARGS(&d11v5))) ||
            FAILED(d11v5->OpenSharedFence(fh, IID_PPV_ARGS(&g_shared_fence11))))
        {
            CloseHandle(fh);
            debug_log("D3D11 OpenSharedFence failed");
            return false;
        }
        CloseHandle(fh);
    }

    // 4. 专用 COPY 队列（Signal 共享 fence）
    {
        D3D12_COMMAND_QUEUE_DESC cqd = {};
        cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        if (FAILED(g_d12dev->CreateCommandQueue(&cqd, IID_PPV_ARGS(&g_signal_queue))))
        {
            debug_log("create signal queue failed");
            return false;
        }
    }

    g_initialized = true;
    debug_log("GDDS interop initialized (self D3D12 + DirectStorage)");
    return true;
}

ID3D11Texture2D *LoadGddsTexture(const wchar_t *gdds_path, uint64_t *out_ready_fence,
                                 bool *out_skipped)
{
    if (out_skipped)
        *out_skipped = false; // 默认非跳过；仅 max_texture_side 分支置 true
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || !g_ds_queue)
        return nullptr;

    // 1. 读头 + 解析 GDDS（多子流：流数 @124，记录数组 @148）
    const auto header = ReadFileHeader(gdds_path, 4096, nullptr);
    GddsInfo info;
    if (!ParseGdds(header, info))
    {
        debug_log("not GDDS / parse failed");
        return nullptr;
    }

    // 仅支持单 mip + 单数组切片：GDDS 若含多子资源（mips>1/array>1），
    // 其子流映射规则不同（按子资源而非行条带），后续再实现。
    if (info.mips != 1 || info.array != 1)
    {
        debug_log("unsupported GDDS (mips/array != 1)");
        return nullptr;
    }

    // 可调上限：max_texture_side>0 时跳过超大纹理（保留原版，避免显存压力）
    if (::tloader::g_max_texture_side > 0 &&
        ((int)info.width > ::tloader::g_max_texture_side ||
         (int)info.height > ::tloader::g_max_texture_side))
    {
        if (out_skipped)
            *out_skipped = true;
        char big[256];
        std::snprintf(big, sizeof(big), "skip GDDS over max_texture_side %ux%u (>%d)",
                      info.width, info.height, ::tloader::g_max_texture_side);
        debug_log(big);
        return nullptr;
    }

    // 2. 替换纹理（SHARED_NTHANDLE）+ D3D12 别名
    ComPtr<ID3D11Texture2D> d11;
    ComPtr<ID3D12Resource> d12;
    if (!CreateReplacementTexture(info, d11, d12))
    {
        debug_log("create replacement texture failed");
        return nullptr;
    }

    // 3. DS 打开 GDDS 文件
    ComPtr<IDStorageFile> ds_file;
    if (FAILED(g_ds_factory->OpenFile(gdds_path, IID_PPV_ARGS(&ds_file))))
    {
        debug_log("OpenFile failed");
        return nullptr;
    }

    // 4. 逐流 EnqueueRequest（作者加载器同款：每流独立 TEXTURE_REGION）。
    //    每条流未压缩 ≤8MB → 单请求远低于 DS 32MB 上限，8192/16384 纹理
    //    无需跳过（旧实现单请求 64MB 报 REQUEST_TOO_LARGE 被跳过）。
    //    流按未压缩字节量连续切分纹理行带：region.top/bottom 由累计
    //    未压缩字节 ÷ 行距换算（块压缩格式 ×4 像素/块行）。
    //
    //    注意：q1 As() 必须在入队之前完成——入队后任何提前返回都会在 DS
    //    请求仍飞行时释放 d12（驱动 UAF，快速切换角色崩溃现场 amdxx64）。
    ComPtr<IDStorageQueue1> q1;
    if (FAILED(g_ds_queue.As(&q1)))
        return nullptr;

    D3D12_RESOURCE_DESC texdesc = d12->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total_bytes = 0;
    g_d12dev->GetCopyableFootprints(&texdesc, 0, 1, 0, &fp, nullptr, nullptr, &total_bytes);

    // 校验：Σ各流未压缩 == 纹理 footprint（防止记录表与纹理不匹配）
    if (info.total_uncompressed != total_bytes)
    {
        char bad[256];
        std::snprintf(bad, sizeof(bad),
                      "GDDS stream sum mismatch: sum=%llu footprint=%llu (%ux%u fmt=%d n=%zu)",
                      (unsigned long long)info.total_uncompressed,
                      (unsigned long long)total_bytes,
                      info.width, info.height, (int)info.format, info.streams.size());
        debug_log(bad);
        return nullptr;
    }

    const UINT64 row_pitch = fp.Footprint.RowPitch;      // 每块行字节（BC 格式=块行）
    const UINT block_h = IsBlockCompressed(info.format) ? 4u : 1u; // 像素/块行
    if (row_pitch == 0 || info.total_uncompressed % row_pitch != 0)
    {
        char bad2[256];
        std::snprintf(bad2, sizeof(bad2),
                      "GDDS row-pitch mismatch: rowPitch=%llu total=%llu (%ux%u)",
                      (unsigned long long)row_pitch,
                      (unsigned long long)info.total_uncompressed,
                      info.width, info.height);
        debug_log(bad2);
        return nullptr;
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < info.streams.size(); i++)
    {
        const GddsStream &s = info.streams[i];
        const uint64_t rows = s.uncompressed / (row_pitch ? row_pitch : 1);
        const UINT top_px = (UINT)((acc / (row_pitch ? row_pitch : 1)) * block_h);
        const UINT bot_px = (UINT)(((acc + s.uncompressed) / (row_pitch ? row_pitch : 1)) * block_h);

        DSTORAGE_REQUEST req = {};
        req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
        req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        req.Source.File.Source = ds_file.Get();
        req.Source.File.Offset = (UINT32)s.offset;
        req.Source.File.Size = s.compressed;
        req.Destination.Texture.Resource = d12.Get();
        req.Destination.Texture.SubresourceIndex = 0;
        req.Destination.Texture.Region = {0, top_px, 0, info.width, bot_px, 1};
        req.UncompressedSize = s.uncompressed; // 该流解压后大小（≤8MB）

        g_ds_queue->EnqueueRequest(&req);
        acc += s.uncompressed;
    }

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    q1->EnqueueSetEvent(ev);
    g_ds_queue->Submit();
    DWORD wr = WaitForSingleObject(ev, 30000);
    if (wr != WAIT_OBJECT_0)
    {
        // 超时：DS 请求可能仍在飞行——绝不能在此释放 d11/d12（驱动 UAF）。
        // 僵尸保活：把对象挂到静态列表，直到进程退出才释放（请求终将完成）。
        static std::mutex zombie_mx;
        static std::vector<ComPtr<ID3D11Texture2D>> zombie_tex;
        static std::vector<ComPtr<ID3D12Resource>> zombie_res;
        {
            std::lock_guard<std::mutex> lk(zombie_mx);
            zombie_tex.push_back(d11);
            zombie_res.push_back(d12);
        }
        debug_log("DS wait timeout (texture zombie-kept)");
        return nullptr;
    }
    CloseHandle(ev);
    HANDLE errEvt = g_ds_queue->GetErrorEvent();
    if (errEvt && WaitForSingleObject(errEvt, 0) == WAIT_OBJECT_0)
    {
        DSTORAGE_ERROR_RECORD rec = {};
        g_ds_queue->RetrieveErrorRecord(&rec);
        // 记录文件上下文，定位超限/异常文件
        char ctx[512];
        std::snprintf(ctx, sizeof(ctx),
                      "DS ERROR file=%ls %ux%u mips=%u arr=%u fmt=%d streams=%zu sum=%llu",
                      gdds_path, info.width, info.height, info.mips, info.array,
                      (int)info.format, info.streams.size(),
                      (unsigned long long)info.total_uncompressed);
        debug_log(ctx, rec.FirstFailure.HResult);
        return nullptr;
    }

    // 5. Signal 共享 fence（D3D12 COPY 队列）——GPU 侧完成标记。
    //    不做显式 resource barrier：DirectStorage 的 TEXTURE_REGION 由驱动
    //    内部管理目标状态，且 barrier 的 allocator 生命周期在异步加载线程
    //    中释放会与 GPU 执行竞争（曾导致 ntdll 崩溃）。D3D11 侧共享纹理
    //    由渲染线程 WaitOnRenderThread 建立读写可见性（Phase 1 已验证）。
    SignalSharedFence();
    if (out_ready_fence != nullptr)
        *out_ready_fence = g_fence_value;

    d11.Get()->AddRef();
    return d11.Get();
}

void WaitOnRenderThread(ID3D11DeviceContext *immediate_ctx, uint64_t fence_value)
{
    // 渲染线程专用：立即上下文 Wait 与游戏提交串行，无竞态。
    // DS 完成事件已保证 GPU 写入完成——此处通常立即返回（fence 已 Signal）。
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (immediate_ctx != nullptr && SUCCEEDED(immediate_ctx->QueryInterface(IID_PPV_ARGS(&ctx4))))
    {
        if (g_shared_fence11 != nullptr)
            ctx4->Wait(g_shared_fence11.Get(), fence_value);
    }
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
        return;
    g_signal_queue.Reset();
    g_shared_fence11.Reset();
    g_shared_fence.Reset();
    g_ds_queue.Reset();
    g_ds_factory.Reset();
    g_d12dev.Reset();
    g_game_device = nullptr;
    g_initialized = false;
    UnloadDstorageRuntime(); // 释放动态加载的 dstorage.dll
    debug_log("GDDS interop shutdown");
}

bool Active()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialized;
}

} // namespace tloader_gdds
