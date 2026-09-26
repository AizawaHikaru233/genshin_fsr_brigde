// FSR2 输入纹理转储（诊断，默认关闭）
//
// 目的：回答"某一小块区域（角色薇斯纳的翅膀骨架）为什么拿不到时间抗锯齿"。
// 超分参数分两层——全局（渲染分辨率/抖动/锐化）每帧只有一份，**不可能局部不同**；
// 只有**逐像素输入纹理**（运动矢量 / 深度 / reactive / 透明）才能局部不同。
// 所以把这几张输入原样落盘，就能直接看出那块区域的哪张输入异常。
//
// 安全约束（本仓库实测过的坑）：在游戏 draw 内做 CPU 同步读回
// （CreateTexture2D → Copy → Flush → Map(READ)），在 **D3D11On12 共享队列**路径上
// 可能永远不返回 ⇒ 游戏整体无响应（见 Dx11FsrBridge.cpp 里 append_tex_samples 的注释）。
// 因此本模块：
//   1) 只做**跨帧延迟**读回——第 N 帧只发 GPU 拷贝，第 N+1 帧才 Map（那时 GPU 早已完成）；
//   2) 调用方传入 allow_readback；为 false（On12 路径）时**直接拒绝并记一次日志**。
//
// 全部键都在 [Dx11FsrBridge] 段：
//   Fsr2InputDump      = 0/1   总开关（默认 0）
//   Fsr2InputDumpFrames= N     落盘帧数（默认 3）
//   Fsr2InputDumpDir   = path  输出目录（默认 <模块目录>\fsr2dump）
//   Fsr2InputDumpRaw   = 0/1   写 .raw（精确原始数据，供离线统计，默认 1）
//   Fsr2InputDumpPng   = 0/1   写 .png（可视化，默认 1）
//   Fsr2InputDumpMaxDim= N     最长边超过 N 时整数抽稀（0=不抽稀，默认 0）
//
// 依赖约定：仅 D3D11；不修改任何绑定/状态（只 CopySubresourceRegion 到自己的 staging）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace fsr2dump
{

// 一次 dispatch 的输入与标量参数（由调用方在 ffx12::dispatch 之前填好）
struct FrameDesc
{
    ID3D11Texture2D *color = nullptr;
    ID3D11Texture2D *depth = nullptr;
    ID3D11Texture2D *motion = nullptr;
    ID3D11Texture2D *transparency = nullptr;
    ID3D11Texture2D *output = nullptr;
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
    std::uint64_t frame_index = 0;
    std::uint64_t instance = 0;
    // 由 ffx12 后端报告的实际输入格式（日志用；0 表示未知）
    std::uint32_t fmt_color = 0;
    std::uint32_t fmt_depth = 0;
    std::uint32_t fmt_motion = 0;
    std::uint32_t fmt_output = 0;
};

// 读一次 INI 完成配置（幂等）。ini_path 为 Bridge ini 的完整路径；为空则视为未启用。
void configure(const wchar_t *ini_path);

// 每个 accumulate dispatch 调用一次（**无论开关是否打开**）。
// 必须无条件调用：热键/延时触发本身要在这里轮询，若先判 enabled() 再调用，
// 触发就永远不会发生（开关关闭时 enabled() 为 false ⇒ 死锁在"未启用"上）。
// 关闭且未配置触发时内部走一条原子快路径直接返回，开销可忽略。
//   ctx            —— 游戏 D3D11 上下文
//   desc           —— 本帧输入与参数
//   allow_readback —— false 表示当前是 D3D11On12 共享队列路径，读回不安全（直接拒绝）
void on_dispatch(ID3D11DeviceContext *ctx, const FrameDesc &desc, bool allow_readback);

// 进程退出/卸载时释放未落盘的 staging
void shutdown();

// —— 触发（延时自动 / 热键），纯状态机，可单测 ——

// 语义对齐本仓库既有的 TextureTrace（同一套 GetTickCount64 计时 + GetAsyncKeyState 轮询），
// 但**不复用它的代码块**：那段代码在 `#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)` 里，
// 而发布构建恰恰定义了该宏（CMakeLists 默认 ON）⇒ 放在那里热键在发布版里根本不编译。
struct TriggerConfig
{
    std::uint32_t frames = 3;        // 每轮抓几帧
    std::uint32_t interval_ms = 0;   // 帧间间隔（0 = 连抓）
    std::uint32_t autostart_sec = 0; // > 0：启动后 N 秒自动触发一次（只一次）
    std::uint32_t hotkey = 0;        // > 0：VK 码，按下即触发（可重复触发）
    bool require_trigger = false;    // 上面两项都没配时为 false ⇒ 立刻开抓（保持旧行为）
};

struct TriggerRuntime
{
    std::uint64_t start_tick = 0;
    bool autostart_fired = false;
    bool session_active = false;
    std::uint32_t frames_done = 0;
    std::uint64_t next_capture_tick = 0;
    std::uint64_t session_count = 0;
};

struct TriggerOut
{
    bool capture_now = false; // 本帧该抓
    bool started = false;     // 本轮刚开
    bool finished = false;    // 本轮刚结束
    const char *source = "";  // "hotkey" / "timer" / "immediate"
};

// 纯函数：给定配置、运行态、当前 tick 与"热键是否刚按下"，推进状态机。
// 保证：会话进行中忽略新的热键（不混轮次）；一轮抓满 frames 帧即结束；
//       自动触发只发生一次；会话结束后热键仍可再次触发。
TriggerOut trigger_update(const TriggerConfig &cfg, TriggerRuntime &rt, std::uint64_t now_tick,
                          bool hotkey_edge);

// —— 以下为可单测的纯函数（无 D3D 依赖）——

// 半精度浮点转单精度
float half_to_float(std::uint16_t bits);

// 游戏 motion 的 R10G10B10A2 平方编码解码（与 Ffx12Backend.cpp 的 CS 一致）：
//   d = raw - 0.498039;  mv = -sign(d) * 4 * d^2
void decode_motion_rgba10(float raw_r, float raw_g, float *out_x, float *out_y);

// 生成合法 PNG（8bit RGBA，非隔行）。内部用 zlib 的 stored 块，无需外部依赖。
std::vector<std::uint8_t> encode_png_rgba(const std::uint8_t *rgba, std::uint32_t w, std::uint32_t h);

// PNG 用的 CRC32（多项式 0xEDB88320）
std::uint32_t crc32_bytes(const std::uint8_t *data, std::size_t size);

// .raw 文件头（64 字节，小端）：magic "FDMP" / version / width / height / dxgi_format /
// row_pitch / frame_index / reserved。供离线脚本解析。
std::vector<std::uint8_t> make_raw_header(std::uint32_t w, std::uint32_t h, std::uint32_t dxgi_format,
                                          std::uint32_t row_pitch, std::uint64_t frame_index);

} // namespace fsr2dump
