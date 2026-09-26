// FSR2 输入纹理转储 —— 实现（诊断，默认关闭）
// 设计与安全约束见 Fsr2InputDump.h 顶部注释。
#include "Fsr2InputDump.h"

#if !defined(FSR2DUMP_NO_LOGGER)
#include "BridgeLogger.h"
// 单测（FSR2DUMP_NO_LOGGER）不链接 BridgeLogger；此时日志变成空操作。
#define FSR2DUMP_LOG(lvl, msg) LOG_##lvl(::blog::cat::upscale, msg)
#else
#define FSR2DUMP_LOG(lvl, msg) ((void)0)
#endif

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fsr2dump
{
namespace
{

// ---------------- 纯工具 ----------------

void put_u32le(std::vector<std::uint8_t> &v, std::uint32_t x)
{
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
}

void put_u32be(std::vector<std::uint8_t> &v, std::uint32_t x)
{
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

void put_u64le(std::vector<std::uint8_t> &v, std::uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}

std::uint8_t clamp_u8(float x)
{
    if (!(x > 0.0f)) // 含 NaN
        return 0;
    if (x >= 1.0f)
        return 255;
    return static_cast<std::uint8_t>(x * 255.0f + 0.5f);
}

// ---------------- 像素解码 ----------------

// 从原始字节解出最多 4 个通道的浮点值（按格式语义）。
// 返回 false 表示格式未知（调用方走"原样字节"兜底）。
bool decode_channels(DXGI_FORMAT fmt, const std::uint8_t *p, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        out[0] = p[0] / 255.0f;
        out[1] = p[1] / 255.0f;
        out[2] = p[2] / 255.0f;
        out[3] = p[3] / 255.0f;
        return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        out[0] = p[2] / 255.0f;
        out[1] = p[1] / 255.0f;
        out[2] = p[0] / 255.0f;
        out[3] = p[3] / 255.0f;
        return true;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
        out[0] = out[1] = out[2] = p[0] / 255.0f;
        out[3] = 1.0f;
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    {
        const std::uint32_t v = *reinterpret_cast<const std::uint32_t *>(p);
        out[0] = static_cast<float>(v & 0x3FFu) / 1023.0f;
        out[1] = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
        out[2] = static_cast<float>((v >> 20) & 0x3FFu) / 1023.0f;
        out[3] = static_cast<float>((v >> 30) & 0x3u) / 3.0f;
        return true;
    }
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS:
        out[0] = out[1] = out[2] = half_to_float(*reinterpret_cast<const std::uint16_t *>(p));
        out[3] = 1.0f;
        return true;
    case DXGI_FORMAT_R16G16_FLOAT:
    {
        const std::uint16_t *h = reinterpret_cast<const std::uint16_t *>(p);
        out[0] = half_to_float(h[0]);
        out[1] = half_to_float(h[1]);
        out[2] = 0.0f;
        out[3] = 1.0f;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    {
        const std::uint16_t *h = reinterpret_cast<const std::uint16_t *>(p);
        out[0] = half_to_float(h[0]);
        out[1] = half_to_float(h[1]);
        out[2] = half_to_float(h[2]);
        out[3] = half_to_float(h[3]);
        return true;
    }
    case DXGI_FORMAT_R16_UNORM:
        out[0] = out[1] = out[2] = *reinterpret_cast<const std::uint16_t *>(p) / 65535.0f;
        out[3] = 1.0f;
        return true;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        out[0] = out[1] = out[2] = *reinterpret_cast<const float *>(p);
        out[3] = 1.0f;
        return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        // 深度在前 4 字节（与 Ffx12Backend 的提取路径一致）
        out[0] = out[1] = out[2] = *reinterpret_cast<const float *>(p);
        out[3] = 1.0f;
        return true;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    {
        const std::uint32_t v = (*reinterpret_cast<const std::uint32_t *>(p)) & 0x00FFFFFFu;
        out[0] = out[1] = out[2] = static_cast<float>(v) / 16777215.0f;
        out[3] = 1.0f;
        return true;
    }
    case DXGI_FORMAT_R11G11B10_FLOAT:
    {
        const std::uint32_t v = *reinterpret_cast<const std::uint32_t *>(p);
        auto f11 = [](std::uint32_t x) -> float
        {
            const std::uint32_t e = (x >> 6) & 0x1Fu;
            const std::uint32_t m = x & 0x3Fu;
            if (e == 0)
                return static_cast<float>(m) / 64.0f * std::pow(2.0f, -14.0f);
            if (e == 31)
                return m ? 0.0f : 1e30f;
            return (1.0f + static_cast<float>(m) / 64.0f) * std::pow(2.0f, static_cast<float>(e) - 15.0f);
        };
        auto f10 = [](std::uint32_t x) -> float
        {
            const std::uint32_t e = (x >> 5) & 0x1Fu;
            const std::uint32_t m = x & 0x1Fu;
            if (e == 0)
                return static_cast<float>(m) / 32.0f * std::pow(2.0f, -14.0f);
            if (e == 31)
                return m ? 0.0f : 1e30f;
            return (1.0f + static_cast<float>(m) / 32.0f) * std::pow(2.0f, static_cast<float>(e) - 15.0f);
        };
        out[0] = f11(v & 0x7FFu);
        out[1] = f11((v >> 11) & 0x7FFu);
        out[2] = f10((v >> 22) & 0x3FFu);
        out[3] = 1.0f;
        return true;
    }
    default:
        return false;
    }
}

std::uint32_t bytes_per_pixel(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return 8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
        return 1;
    default:
        return 0;
    }
}

const char *format_name(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    default: return "?";
    }
}

bool is_depth_like(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        return true;
    default:
        return false;
    }
}

// ---------------- 状态 ----------------

struct CaptureItem
{
    std::string name;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT w = 0;
    UINT h = 0;
    UINT bpp = 0;
    ID3D11Texture2D *staging = nullptr; // 拥有
    ID3D11Texture2D *resolve = nullptr; // 仅 MSAA 时使用（本版本直接跳过 MSAA，保留字段）
};

struct Pending
{
    bool used = false;
    std::uint64_t frame_index = 0;
    std::uint64_t instance = 0;
    std::uint32_t render_w = 0;
    std::uint32_t render_h = 0;
    std::uint32_t display_w = 0;
    std::uint32_t display_h = 0;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float motion_scale_x = 1.0f;
    float motion_scale_y = 1.0f;
    float frame_time_delta_ms = 16.7f;
    bool use_reactive_mask = false;
    bool use_transparency_mask = false;
    bool enable_sharpening = false;
    float sharpness = 0.0f;
    bool reset = false;
    std::uint32_t fmt_color = 0;
    std::uint32_t fmt_depth = 0;
    std::uint32_t fmt_motion = 0;
    std::uint32_t fmt_output = 0;
    std::vector<CaptureItem> items;
};

struct State
{
    bool configured = false;
    bool enabled = false;
    bool refused_unsafe = false;
    bool logged_ready = false;
    std::uint32_t frames_wanted = 3;
    std::uint32_t frames_done = 0;
    std::uint32_t max_dim = 0;
    bool save_raw = true;
    bool save_png = true;
    std::filesystem::path dir;
    TriggerConfig trigger;
    TriggerRuntime trigger_rt;
    Pending pending;
};

// 默认配置（无开关、无触发）下的每帧快路径：一次 relaxed 原子读即返回。
std::atomic_bool g_maybe_active { false };

State &state()
{
    static State s;
    return s;
}

std::mutex &state_mutex()
{
    static std::mutex m;
    return m;
}

void release_pending(Pending &p)
{
    for (CaptureItem &it : p.items)
    {
        if (it.staging)
            it.staging->Release();
        if (it.resolve)
            it.resolve->Release();
    }
    p.items.clear();
    p.used = false;
}

// ---------------- 落盘 ----------------

bool write_file(const std::filesystem::path &path, const std::vector<std::uint8_t> &data)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    return out.good();
}

// 把一张已 Map 的纹理写成 .raw（精确）与若干 .png（可视化）。
// kind: 0=普通颜色/未知, 1=motion（额外输出解码图与 reactive(B) 图）, 2=depth（自动 min/max 归一）
void dump_mapped_texture(const std::filesystem::path &dir, const Pending &p, const CaptureItem &item,
                         const D3D11_MAPPED_SUBRESOURCE &mapped, bool save_raw, bool save_png,
                         std::uint32_t max_dim, std::vector<std::string> &written)
{
    const std::string stem = "f" + std::to_string(p.frame_index) + "_" + item.name;
    const std::uint8_t *base = static_cast<const std::uint8_t *>(mapped.pData);
    if (!base)
        return;

    if (save_raw)
    {
        std::vector<std::uint8_t> raw = make_raw_header(item.w, item.h, static_cast<std::uint32_t>(item.fmt),
                                                       mapped.RowPitch, p.frame_index);
        for (UINT y = 0; y < item.h; ++y)
        {
            const std::uint8_t *row = base + static_cast<std::size_t>(y) * mapped.RowPitch;
            raw.insert(raw.end(), row, row + static_cast<std::size_t>(item.w) * item.bpp);
        }
        if (write_file(dir / (stem + ".raw"), raw))
            written.push_back(stem + ".raw");
    }

    if (!save_png)
        return;

    // PNG 尺寸（可抽稀）
    UINT stride = 1;
    if (max_dim > 0)
    {
        UINT longest = std::max(item.w, item.h);
        while (longest / (stride * 2) >= max_dim)
            stride *= 2;
    }
    const UINT ow = std::max<UINT>(1, item.w / stride);
    const UINT oh = std::max<UINT>(1, item.h / stride);

    const bool motion_like = (item.fmt == DXGI_FORMAT_R10G10B10A2_UNORM ||
                              item.fmt == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    const bool depth_like = is_depth_like(item.fmt);

    // 两遍：先求范围（深度用 min/max，motion 解码用最大幅度），再出图
    float dmin = 1e30f, dmax = -1e30f, mvmax = 0.0f;
    if (depth_like || motion_like)
    {
        for (UINT y = 0; y < item.h; y += stride)
        {
            const std::uint8_t *row = base + static_cast<std::size_t>(y) * mapped.RowPitch;
            for (UINT x = 0; x < item.w; x += stride)
            {
                float c[4];
                if (!decode_channels(item.fmt, row + static_cast<std::size_t>(x) * item.bpp, c))
                    continue;
                if (depth_like)
                {
                    dmin = std::min(dmin, c[0]);
                    dmax = std::max(dmax, c[0]);
                }
                else
                {
                    float mx, my;
                    decode_motion_rgba10(c[0], c[1], &mx, &my);
                    mvmax = std::max(mvmax, std::max(std::fabs(mx), std::fabs(my)));
                }
            }
        }
    }

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(ow) * oh * 4, 0);
    std::vector<std::uint8_t> mv_rgba, reactive_rgba;
    if (motion_like)
    {
        mv_rgba.assign(static_cast<std::size_t>(ow) * oh * 4, 0);
        reactive_rgba.assign(static_cast<std::size_t>(ow) * oh * 4, 0);
    }
    for (UINT oy = 0; oy < oh; ++oy)
    {
        const UINT sy = std::min(item.h - 1, oy * stride);
        const std::uint8_t *row = base + static_cast<std::size_t>(sy) * mapped.RowPitch;
        for (UINT ox = 0; ox < ow; ++ox)
        {
            const UINT sx = std::min(item.w - 1, ox * stride);
            float c[4];
            std::uint8_t *dst = rgba.data() + (static_cast<std::size_t>(oy) * ow + ox) * 4;
            if (!decode_channels(item.fmt, row + static_cast<std::size_t>(sx) * item.bpp, c))
            {
                // 未知格式：按字节兜底（前 3 字节当 RGB）
                const std::uint8_t *p = row + static_cast<std::size_t>(sx) * item.bpp;
                dst[0] = p[0];
                dst[1] = item.bpp > 1 ? p[1] : p[0];
                dst[2] = item.bpp > 2 ? p[2] : p[0];
                dst[3] = 255;
                if (motion_like)
                {
                    std::memcpy(mv_rgba.data() + (static_cast<std::size_t>(oy) * ow + ox) * 4, dst, 4);
                    std::memcpy(reactive_rgba.data() + (static_cast<std::size_t>(oy) * ow + ox) * 4, dst, 4);
                }
                continue;
            }

            if (depth_like)
            {
                const float span = (dmax > dmin) ? (dmax - dmin) : 1.0f;
                const std::uint8_t g = clamp_u8((c[0] - dmin) / span);
                dst[0] = dst[1] = dst[2] = g;
                dst[3] = 255;
            }
            else if (motion_like)
            {
                // raw 视图：直接看 10/10/10/2 原始通道
                dst[0] = clamp_u8(c[0]);
                dst[1] = clamp_u8(c[1]);
                dst[2] = clamp_u8(c[2]);
                dst[3] = 255;
                // 解码视图：0.5 灰 = 无位移；幅度按本帧最大值归一，异常区域一眼可见
                float mx, my;
                decode_motion_rgba10(c[0], c[1], &mx, &my);
                const float k = (mvmax > 1e-6f) ? (0.5f / mvmax) : 1.0f;
                std::uint8_t *mvd = mv_rgba.data() + (static_cast<std::size_t>(oy) * ow + ox) * 4;
                mvd[0] = clamp_u8(0.5f + mx * k);
                mvd[1] = clamp_u8(0.5f + my * k);
                mvd[2] = 128;
                mvd[3] = 255;
                // reactive 源 = motion 的 B 通道（Ffx12Backend.cpp CS 实证）
                std::uint8_t *rvd = reactive_rgba.data() + (static_cast<std::size_t>(oy) * ow + ox) * 4;
                rvd[0] = rvd[1] = rvd[2] = clamp_u8(c[2]);
                rvd[3] = 255;
            }
            else
            {
                // 颜色/未知：超 1 的按 soft 压缩，便于看结构
                auto tone = [](float x) -> std::uint8_t
                {
                    if (x > 1.0f)
                        x = x / (1.0f + x);
                    return clamp_u8(x);
                };
                dst[0] = tone(c[0]);
                dst[1] = tone(c[1]);
                dst[2] = tone(c[2]);
                dst[3] = 255;
            }
        }
    }

    auto emit = [&](const std::string &suffix, const std::vector<std::uint8_t> &px)
    {
        const std::vector<std::uint8_t> png = encode_png_rgba(px.data(), ow, oh);
        if (write_file(dir / (stem + suffix + ".png"), png))
            written.push_back(stem + suffix + ".png");
    };
    emit("", rgba);
    if (motion_like)
    {
        emit("_mvdec", mv_rgba);
        emit("_reactiveB", reactive_rgba);
    }
}

void write_meta(const std::filesystem::path &dir, const Pending &p, const std::vector<std::string> &written)
{
    std::vector<std::string> lines;
    lines.push_back("frame_index=" + std::to_string(p.frame_index));
    lines.push_back("instance=" + std::to_string(p.instance));
    lines.push_back("render=" + std::to_string(p.render_w) + "x" + std::to_string(p.render_h));
    lines.push_back("display=" + std::to_string(p.display_w) + "x" + std::to_string(p.display_h));
    lines.push_back("jitter=" + std::to_string(p.jitter_x) + "," + std::to_string(p.jitter_y));
    lines.push_back("motion_scale=" + std::to_string(p.motion_scale_x) + "," +
                    std::to_string(p.motion_scale_y));
    lines.push_back("frame_time_delta_ms=" + std::to_string(p.frame_time_delta_ms));
    lines.push_back("reset=" + std::string(p.reset ? "1" : "0"));
    lines.push_back("use_reactive_mask=" + std::string(p.use_reactive_mask ? "1" : "0"));
    lines.push_back("use_transparency_mask=" + std::string(p.use_transparency_mask ? "1" : "0"));
    lines.push_back("sharpening=" + std::string(p.enable_sharpening ? "1" : "0") + " sharpness=" +
                    std::to_string(p.sharpness));
    lines.push_back("fmt_color=" + std::to_string(p.fmt_color));
    lines.push_back("fmt_depth=" + std::to_string(p.fmt_depth));
    lines.push_back("fmt_motion=" + std::to_string(p.fmt_motion));
    lines.push_back("fmt_output=" + std::to_string(p.fmt_output));
    for (const CaptureItem &it : p.items)
    {
        lines.push_back(std::string("input ") + it.name + ": " + format_name(it.fmt) + " (" +
                        std::to_string(static_cast<int>(it.fmt)) + ") " + std::to_string(it.w) + "x" +
                        std::to_string(it.h) + " bpp=" + std::to_string(it.bpp));
    }
    lines.push_back("files:");
    for (const std::string &f : written)
        lines.push_back("  " + f);

    std::string body;
    for (const std::string &l : lines)
    {
        body += l;
        body += "\r\n";
    }
    std::vector<std::uint8_t> data(body.begin(), body.end());
    write_file(dir / ("f" + std::to_string(p.frame_index) + "_meta.txt"), data);
}

// 把一张源纹理排进本帧的捕获（只发 GPU 拷贝，不做任何 Map/Flush）
void queue_texture(ID3D11Device *dev, ID3D11DeviceContext *ctx, Pending &p, const char *name,
                   ID3D11Texture2D *tex, std::string &skip_reason)
{
    if (!tex)
    {
        skip_reason = std::string(name) + "=null";
        return;
    }
    D3D11_TEXTURE2D_DESC td {};
    tex->GetDesc(&td);
    if (td.SampleDesc.Count > 1)
    {
        // staging 不支持 MSAA；FSR2 输入实测也不是 MSAA。遇到就跳过并如实记原因。
        skip_reason = std::string(name) + "=msaa" + std::to_string(td.SampleDesc.Count);
        return;
    }
    const UINT bpp = bytes_per_pixel(td.Format);
    if (bpp == 0)
    {
        skip_reason = std::string(name) + "=fmt_unknown(" + std::to_string(static_cast<int>(td.Format)) + ")";
        return;
    }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging)) || !staging)
    {
        skip_reason = std::string(name) + "=staging_create_failed";
        return;
    }
    // 统一走 CopySubresourceRegion（staging 的 ArraySize/MipLevels 被强制为 1，
    // 与源不一致时 CopyResource 会失败）。只发 GPU 拷贝，不做 Map/Flush。
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
    CaptureItem it;
    it.name = name;
    it.fmt = td.Format;
    it.w = td.Width;
    it.h = td.Height;
    it.bpp = bpp;
    it.staging = staging;
    p.items.push_back(it);
}

} // namespace

// ---------------- 纯函数实现 ----------------

float half_to_float(std::uint16_t bits)
{
    const std::uint32_t sign = (static_cast<std::uint32_t>(bits) >> 15) & 0x1u;
    const std::uint32_t exp = (static_cast<std::uint32_t>(bits) >> 10) & 0x1Fu;
    const std::uint32_t man = static_cast<std::uint32_t>(bits) & 0x3FFu;
    float v;
    if (exp == 0)
    {
        v = (man == 0) ? 0.0f : (static_cast<float>(man) / 1024.0f) * std::pow(2.0f, -14.0f);
    }
    else if (exp == 31)
    {
        v = (man == 0) ? 1e30f : 0.0f; // Inf 用大数表示，NaN 记 0（可视化用）
    }
    else
    {
        v = (1.0f + static_cast<float>(man) / 1024.0f) * std::pow(2.0f, static_cast<float>(exp) - 15.0f);
    }
    return sign ? -v : v;
}

void decode_motion_rgba10(float raw_r, float raw_g, float *out_x, float *out_y)
{
    const float dx = raw_r - 0.498039f;
    const float dy = raw_g - 0.498039f;
    auto sq = [](float d) -> float
    {
        const float m = -4.0f * d * d;
        return (d >= 0.0f) ? m : -m;
    };
    if (out_x)
        *out_x = sq(dx);
    if (out_y)
        *out_y = sq(dy);
}

std::uint32_t crc32_bytes(const std::uint8_t *data, std::size_t size)
{
    static std::uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, []()
    {
        for (std::uint32_t n = 0; n < 256; ++n)
        {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
    });
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> encode_png_rgba(const std::uint8_t *rgba, std::uint32_t w, std::uint32_t h)
{
    std::vector<std::uint8_t> out;
    if (!rgba || w == 0 || h == 0)
        return out;
    const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    auto chunk = [&](const char type[4], const std::vector<std::uint8_t> &payload)
    {
        put_u32be(out, static_cast<std::uint32_t>(payload.size()));
        const std::size_t crc_start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), payload.begin(), payload.end());
        const std::uint32_t crc = crc32_bytes(out.data() + crc_start, out.size() - crc_start);
        put_u32be(out, crc);
    };

    // IHDR
    {
        std::vector<std::uint8_t> ihdr;
        put_u32be(ihdr, w);
        put_u32be(ihdr, h);
        ihdr.push_back(8);  // bit depth
        ihdr.push_back(6);  // color type RGBA
        ihdr.push_back(0);  // compression
        ihdr.push_back(0);  // filter
        ihdr.push_back(0);  // interlace
        chunk("IHDR", ihdr);
    }

    // 原始数据：每行前置 filter 字节 0
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + static_cast<std::size_t>(w) * 4));
    for (std::uint32_t y = 0; y < h; ++y)
    {
        raw.push_back(0);
        const std::uint8_t *row = rgba + static_cast<std::size_t>(y) * w * 4;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(w) * 4);
    }

    // zlib：stored 块（无需外部依赖），CMF/FLG = 0x78 0x01
    std::vector<std::uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    std::size_t off = 0;
    while (off < raw.size() || raw.empty())
    {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = (off + n) >= raw.size();
        z.push_back(last ? 1 : 0);
        const std::uint16_t len = static_cast<std::uint16_t>(n);
        z.push_back(static_cast<std::uint8_t>(len & 0xFF));
        z.push_back(static_cast<std::uint8_t>(len >> 8));
        const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
        z.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
        z.push_back(static_cast<std::uint8_t>(nlen >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
                 raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (last)
            break;
    }
    // Adler-32
    {
        std::uint32_t a = 1, b = 0;
        for (std::uint8_t byte : raw)
        {
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }
        put_u32be(z, (b << 16) | a);
    }
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

std::vector<std::uint8_t> make_raw_header(std::uint32_t w, std::uint32_t h, std::uint32_t dxgi_format,
                                          std::uint32_t row_pitch, std::uint64_t frame_index)
{
    std::vector<std::uint8_t> v;
    v.reserve(64);
    v.push_back('F');
    v.push_back('D');
    v.push_back('M');
    v.push_back('P');
    put_u32le(v, 1); // version
    put_u32le(v, w);
    put_u32le(v, h);
    put_u32le(v, dxgi_format);
    put_u32le(v, row_pitch);
    put_u64le(v, frame_index);
    while (v.size() < 64)
        v.push_back(0);
    return v;
}

TriggerOut trigger_update(const TriggerConfig &cfg, TriggerRuntime &rt, std::uint64_t now_tick,
                          bool hotkey_edge)
{
    TriggerOut out;
    if (rt.start_tick == 0)
        rt.start_tick = now_tick;

    // 未配置任何触发 ⇒ 保持旧行为：立刻开抓（配置者显然就是想要最近这些帧）。
    if (!cfg.require_trigger && rt.session_count == 0 && !rt.session_active)
    {
        rt.session_active = true;
        rt.frames_done = 0;
        rt.next_capture_tick = now_tick;
        ++rt.session_count;
        out.started = true;
        out.source = "immediate";
    }

    // 会话进行中忽略新的触发：不混轮次（抓满即结束，之后热键可再触发）。
    if (!rt.session_active)
    {
        bool start = false;
        const char *source = "";
        if (hotkey_edge && cfg.hotkey != 0)
        {
            start = true;
            source = "hotkey";
        }
        else if (!rt.autostart_fired && cfg.autostart_sec > 0 &&
                 now_tick - rt.start_tick >= static_cast<std::uint64_t>(cfg.autostart_sec) * 1000u)
        {
            // 自动触发只发生一次
            start = true;
            source = "timer";
        }
        if (cfg.autostart_sec > 0 && !rt.autostart_fired && now_tick - rt.start_tick >=
                static_cast<std::uint64_t>(cfg.autostart_sec) * 1000u)
            rt.autostart_fired = true;
        if (start)
        {
            rt.session_active = true;
            rt.frames_done = 0;
            rt.next_capture_tick = now_tick;
            ++rt.session_count;
            out.started = true;
            out.source = source;
        }
    }

    if (rt.session_active && now_tick >= rt.next_capture_tick)
    {
        out.capture_now = true;
        ++rt.frames_done;
        rt.next_capture_tick = now_tick + cfg.interval_ms;
        if (rt.frames_done >= (cfg.frames == 0 ? 1u : cfg.frames))
        {
            rt.session_active = false;
            out.finished = true;
        }
    }
    return out;
}

// ---------------- 对外流程 ----------------

void configure(const wchar_t *ini_path)
{
    if (!ini_path || ini_path[0] == L'\0')
        return;
    std::lock_guard<std::mutex> lock(state_mutex());
    State &s = state();
    s.configured = true;
    s.enabled = GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDump", 0, ini_path) != 0;
    s.frames_wanted = static_cast<std::uint32_t>(
        std::max<UINT>(1u, GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDumpFrames", 3, ini_path)));
    s.max_dim = static_cast<std::uint32_t>(
        std::max<UINT>(0u, GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDumpMaxDim", 0, ini_path)));
    s.save_raw = GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDumpRaw", 1, ini_path) != 0;
    s.save_png = GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDumpPng", 1, ini_path) != 0;

    wchar_t dir_buf[1024] = {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"Fsr2InputDumpDir", L"", dir_buf,
                             static_cast<DWORD>(std::size(dir_buf)), ini_path);
    std::filesystem::path dir;
    if (dir_buf[0] != L'\0')
    {
        dir = std::filesystem::path(dir_buf);
    }
    else
    {
        std::error_code ec;
        dir = std::filesystem::path(ini_path).parent_path() / L"fsr2dump";
    }
    s.dir = dir;

    // 触发：延时自动开始 / 热键 / 帧间间隔。
    // 语义对齐既有 TextureTrace（TextureTraceHotkey / TextureTraceAutoStartSec），
    // 但那段代码在发布构建里被 #if 编译掉，故这里自成一套。
    s.trigger.frames = s.frames_wanted;
    s.trigger.interval_ms = static_cast<std::uint32_t>(
        std::max<UINT>(0u, GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDumpIntervalMs", 0,
                                                 ini_path)));
    s.trigger.autostart_sec = static_cast<std::uint32_t>(
        std::max<UINT>(0u, GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDumpAutoStartSec", 0,
                                                 ini_path)));
    s.trigger.hotkey = static_cast<std::uint32_t>(
        std::max<UINT>(0u, GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2InputDumpHotkey", 0,
                                                 ini_path)));
    // 两个触发都没配 ⇒ 立刻开抓（保持既有行为，避免"配了开关却什么都不抓"）
    s.trigger.require_trigger = (s.trigger.autostart_sec > 0) || (s.trigger.hotkey > 0);
    s.trigger_rt = TriggerRuntime {};
    s.trigger_rt.start_tick = GetTickCount64();

    // 只在"开关打开"时才让每帧快路径放行
    g_maybe_active.store(s.enabled, std::memory_order_relaxed);

    if (s.enabled)
    {
        FSR2DUMP_LOG(INFO,
                 "fsr2_input_dump armed frames=" + std::to_string(s.frames_wanted) +
                     " interval_ms=" + std::to_string(s.trigger.interval_ms) +
                     " autostart_sec=" + std::to_string(s.trigger.autostart_sec) +
                     " hotkey=" + std::to_string(s.trigger.hotkey) +
                     " trigger=" + std::string(s.trigger.require_trigger
                                                   ? (s.trigger.hotkey ? "hotkey" : "timer")
                                                   : "immediate") +
                     " dir=" + s.dir.string());
    }
}

void on_dispatch(ID3D11DeviceContext *ctx, const FrameDesc &desc, bool allow_readback)
{
    if (!ctx)
        return;
    // 快路径：默认配置（开关关闭）下每帧只花一次 relaxed 原子读。
    if (!g_maybe_active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(state_mutex());
    State &s = state();
    if (!s.configured || !s.enabled)
        return;

    if (!allow_readback)
    {
        if (!s.refused_unsafe)
        {
            s.refused_unsafe = true;
            FSR2DUMP_LOG(WARN,
                     "fsr2_input_dump refused: D3D11On12 shared-queue path (in-draw CPU readback "
                     "may hang the game). Set Dx11On12=0 to use the native-D3D11 path, or leave "
                     "Fsr2InputDump=0.");
        }
        return;
    }

    // 轮询热键（与 TextureTrace 同法：GetAsyncKeyState 的 bit0 = 自上次调用后按下过）
    bool hotkey_edge = false;
    if (s.trigger.hotkey != 0)
        hotkey_edge = (GetAsyncKeyState(static_cast<int>(s.trigger.hotkey)) & 1) != 0;

    const TriggerOut out = trigger_update(s.trigger, s.trigger_rt, GetTickCount64(), hotkey_edge);
    if (out.started)
    {
        FSR2DUMP_LOG(INFO, std::string("fsr2_input_dump triggered source=") + out.source +
                     " session=" + std::to_string(s.trigger_rt.session_count) +
                     " frames=" + std::to_string(s.trigger.frames) +
                     " interval_ms=" + std::to_string(s.trigger.interval_ms));
    }
    if (out.finished)
    {
        FSR2DUMP_LOG(INFO, "fsr2_input_dump session done frames=" +
                     std::to_string(s.trigger_rt.frames_done) +
                     " total_frames=" + std::to_string(s.frames_done) +
                     " dir=" + s.dir.string());
    }

    // ① 先落盘上一帧排队的捕获（那时 GPU 早已完成 ⇒ Map 不会长时间阻塞）。
    //    注意：即使本轮已抓满也必须做完，否则最后一帧会丢。
    if (s.pending.used)
    {
        std::vector<std::string> written;
        std::error_code ec;
        std::filesystem::create_directories(s.dir, ec);
        for (const CaptureItem &it : s.pending.items)
        {
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (FAILED(ctx->Map(it.staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
                continue;
            dump_mapped_texture(s.dir, s.pending, it, mapped, s.save_raw, s.save_png, s.max_dim, written);
            ctx->Unmap(it.staging, 0);
        }
        write_meta(s.dir, s.pending, written);
        ++s.frames_done;
        FSR2DUMP_LOG(INFO,
                 "fsr2_input_dump frame=" + std::to_string(s.pending.frame_index) +
                     " render=" + std::to_string(s.pending.render_w) + "x" +
                     std::to_string(s.pending.render_h) +
                     " files=" + std::to_string(written.size()) +
                     " dir=" + s.dir.string());
        release_pending(s.pending);
    }

    // ② 只有触发状态机说"本帧该抓"时才排新捕获
    if (!out.capture_now)
        return;

    // 排队本帧捕获（只发 GPU 拷贝，不 Map/Flush）
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev)
    {
        FSR2DUMP_LOG(WARN, "fsr2_input_dump cannot get device; disabled");
        s.enabled = false;
        g_maybe_active.store(false, std::memory_order_relaxed);
        return;
    }
    Pending p;
    p.used = true;
    p.frame_index = desc.frame_index;
    p.instance = desc.instance;
    p.render_w = desc.render_w;
    p.render_h = desc.render_h;
    p.display_w = desc.display_w;
    p.display_h = desc.display_h;
    p.jitter_x = desc.jitter_x;
    p.jitter_y = desc.jitter_y;
    p.motion_scale_x = desc.motion_scale_x;
    p.motion_scale_y = desc.motion_scale_y;
    p.frame_time_delta_ms = desc.frame_time_delta_ms;
    p.use_reactive_mask = desc.use_reactive_mask;
    p.use_transparency_mask = desc.use_transparency_mask;
    p.enable_sharpening = desc.enable_sharpening;
    p.sharpness = desc.sharpness;
    p.reset = desc.reset;
    p.fmt_color = desc.fmt_color;
    p.fmt_depth = desc.fmt_depth;
    p.fmt_motion = desc.fmt_motion;
    p.fmt_output = desc.fmt_output;

    std::string reasons;
    auto add_reason = [&reasons](const std::string &r)
    {
        if (r.empty())
            return;
        if (!reasons.empty())
            reasons += ",";
        reasons += r;
    };
    std::string r;
    queue_texture(dev, ctx, p, "color", desc.color, r);
    add_reason(r);
    r.clear();
    queue_texture(dev, ctx, p, "depth", desc.depth, r);
    add_reason(r);
    r.clear();
    queue_texture(dev, ctx, p, "motion", desc.motion, r);
    add_reason(r);
    r.clear();
    queue_texture(dev, ctx, p, "transparency", desc.transparency, r);
    add_reason(r);
    r.clear();
    queue_texture(dev, ctx, p, "output", desc.output, r);
    add_reason(r);
    dev->Release();

    if (!s.logged_ready)
    {
        s.logged_ready = true;
        FSR2DUMP_LOG(INFO,
                 "fsr2_input_dump first capture raw=" + std::to_string(s.save_raw ? 1 : 0) +
                     " png=" + std::to_string(s.save_png ? 1 : 0) +
                     " max_dim=" + std::to_string(s.max_dim) +
                     (reasons.empty() ? "" : (" skipped=" + reasons)));
    }
    if (p.items.empty())
    {
        FSR2DUMP_LOG(WARN, "fsr2_input_dump no input captured; disabled. reasons=" + reasons);
        s.enabled = false;
        g_maybe_active.store(false, std::memory_order_relaxed);
        return;
    }
    release_pending(s.pending);
    s.pending = std::move(p);
}

void shutdown()
{
    std::lock_guard<std::mutex> lock(state_mutex());
    release_pending(state().pending);
}

} // namespace fsr2dump
