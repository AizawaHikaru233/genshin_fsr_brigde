#pragma once
// 本头只放 HLSL 源码字符串，供 Ffx12Backend.cpp（native 路径）与 NgxBackend.cpp（ngx 路径）共用。
// HLSL 内容与迁移前逐字节相同，不得修改；改公式必须两边同时验证。
namespace ffx12 {

// 深度提取：out_depth[id.xy] = in_depth.Load(int3(id.xy,0)).x * g_depth_scale;   （b0.x）
// 来源：Ffx12Backend.cpp:458-471（原文逐字节迁出，含 R"( ... )" 两端的空行）
inline constexpr char g_depth_extract_hlsl[] = R"(
cbuffer DepthParams : register(b0)
{
    float g_depth_scale;
    float3 g_pad;
};
Texture2D<float4> in_depth : register(t0);
RWTexture2D<float> out_depth : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    out_depth[id.xy] = in_depth.Load(int3(id.xy, 0)).x * g_depth_scale;
}
)";

// motion 解码：float2 d = c.rg - 0.498039; float2 mv = -sign(d)*(4.0*d*d)*g_flip;
//              out_reactive[id.xy] = c.b;                                        （u1）
// 来源：Ffx12Backend.cpp:478-511（原文逐字节迁出，含 FFX12_MOTION_RAW / FFX12_MOTION_DEADZONE 两个变体宏）
inline constexpr char g_motion_decode_d11_hlsl[] = R"(
cbuffer MotionParams : register(b0)
{
    float g_flip; // XeSS/DLSS 定向——motion 方向翻转（XeSS-SR 约定 prev→curr，与 FSR 相反）
    float3 g_pad;
};
Texture2D<float4> in_mv : register(t0);
RWTexture2D<float2> out_mv : register(u0);
RWTexture2D<float> out_reactive : register(u1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 c = in_mv.Load(int3(id.xy, 0));
    #if defined(FFX12_MOTION_RAW)
    // 不解码变体：把通道按 R16G16 归一化运动矢量直接读出（只做方向翻转）。
    // 用途：验证/绕开"游戏 motion 是 R10G10B10A2 signed-in-unorm 平方编码"这一前提。
    // 历史缺陷：Ffx12MotionDecode 曾是**空开关**（g_decode_motion 除赋值外无任何读取点，
    // 平方解码无条件执行），日志却打 mv_decode=1，看起来像在生效。
    float2 mv = (c.rg * 2.0 - 1.0) * g_flip;
    #else
    float2 d = c.rg - 0.498039;
    float2 mv = -sign(d) * (4.0 * d * d) * g_flip;
    #endif
    #if defined(FFX12_MOTION_DEADZONE)
    // 静止死区——|mv| 低于阈值归零，让 FSR 进入静止锁定（lock），
    // 消除静止时残余 motion 驱动的时间累积网格（竖纹/云刺）。阈值 0.0005 UV ≈ 1px@1920。
    static const float2 k_dz = float2(0.0005, 0.0005);
    if (abs(mv.x) < k_dz.x && abs(mv.y) < k_dz.y)
        mv = float2(0.0, 0.0);
    #endif
    out_mv[id.xy] = mv;
    out_reactive[id.xy] = c.b; // motion B 通道（bits 20..29）= reactive 源（历史第 62 轮实证）
}
)";

} // namespace ffx12
