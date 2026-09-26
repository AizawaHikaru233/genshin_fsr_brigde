// JitterFlagProbeTest.cpp — JitterFlagProbe 的独立自测（不需要游戏、不需要 GPU）。
//
// 覆盖五件事（对应交付里"怎么证明"的每一条）：
//   ① 定位：在**真实机器码形状**（取自两份实机构建的反汇编）上验证签名发现；
//      含负例（找不到 / 并列候选）与 Tier-2 裁决。
//   ② 只读（force=0）：装钩后调用假 setter，验证 observer 记录的实参正确、
//      **函数仍然正常执行且返回值/副作用不变**（= 行为零改动）。
//   ③ 【强制 true】（force=1）：**假 setter 体内真的收到 true**，
//      而 observer 记录的仍是**原始 false**（日志能同时给出"原值"与"传出的值"）。
//   ④ 限流：decide_emit 状态机（首次/变化/心跳/不刷屏/帧数上限）。
//   ⑤ 关闭与还原：enabled=false 时零字节改动；shutdown() 还原原始序言。
// ASCII-only（与仓库既有测试一致）。
#include "JitterFlagProbe.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{

// 锚点序言（真实字节，取自 D:\Dump\cn.exe / global.exe 的
// FFX_FSR2.ConfigureJitteredProjectionMatrix @0x6B558D0 / 0x6B54420）
constexpr std::uint8_t k_camera_head[14] = {
    0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53,
    0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00,
};
// Matrix4x4 setter 序言（真实字节：Camera.set_projectionMatrix @0x13DC510）
constexpr std::uint8_t k_matrix_head[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9,
};
// bool setter 序言（真实字节：Camera.set_useJitteredProjectionMatrixForTransparentRendering
// @0x13DC5F0 —— 两份构建里**同一地址、同一字节**）
constexpr std::uint8_t k_flag_head[19] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
    0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
};

// 调用点形状（真实字节）：`mov rcx,r14 ; xor edx,edx ; call rel32`
// —— `xor edx,edx` 就是 Unity 源码里 `= false` 那一行的机器码。
constexpr std::uint8_t k_lea_rdx[5] = {0x4C, 0x8D, 0x54, 0x24, 0x70}; // lea rdx,[rsp+0x70]
constexpr std::uint8_t k_mov_rcx_r14[3] = {0x4C, 0x89, 0xF1};          // mov rcx,r14
constexpr std::uint8_t k_xor_edx_edx[2] = {0x31, 0xD2};                // xor edx,edx

constexpr std::uint32_t k_image_size = 0x8000;
constexpr std::uint32_t k_anchor_rva = 0x1000;
constexpr std::uint32_t k_matrix_rva = 0x3000;
constexpr std::uint32_t k_flag_rva = 0x3080;
// 锚点内的两处调用点偏移。**必须与实机布局逐字节一致**（这是本测试的关键：
// 它同时验证 RVA 计算与"调用点相邻"判据）：
//   0x100: lea rdx,[rsp+0x70]      (5) -> 0x104
//   0x105: mov rcx,r14             (3) -> 0x107
//   0x108: call matrix setter      (5) -> 0x10C
//   0x10D: mov rcx,r14             (3) -> 0x10F
//   0x110: xor edx,edx             (2) -> 0x111
//   0x112: call bool setter        (5)
constexpr std::size_t k_anchor_pre_off = 0x100;
constexpr std::size_t k_anchor_call1_off = 0x108; // call matrix setter
constexpr std::size_t k_anchor_call2_off = 0x112; // call bool setter（与实机同为 +10 字节）

int failures = 0;
#define CHECK(cond, msg)                                    \
    do                                                      \
    {                                                       \
        if (!(cond))                                        \
        {                                                   \
            std::printf("FAIL: %s\n", msg);                 \
            ++failures;                                     \
        }                                                   \
        else                                                \
        {                                                   \
            std::printf("ok:   %s\n", msg);                 \
        }                                                   \
    } while (0)

std::uint8_t *g_image = nullptr;

void write_u32(std::uint8_t *p, std::uint32_t v)
{
    std::memcpy(p, &v, 4);
}

// 在锚点函数体内 code[off] 写 `E8 rel32`，目标是**绝对 RVA** target（相对 g_image）。
// ⚠️ rel32 必须相对"调用指令的下一条"算，而 code 指向 g_image + k_anchor_rva
// —— 漏掉 k_anchor_rva 会让目标整体偏 0x1000（本测试首版就踩了这个）。
void write_call(std::uint8_t *code, std::size_t off, std::uint32_t target_rva)
{
    code[off] = 0xE8;
    const std::int32_t rel = static_cast<std::int32_t>(target_rva) -
        static_cast<std::int32_t>(k_anchor_rva + off + 5);
    write_u32(code + off + 1, static_cast<std::uint32_t>(rel));
}

// 假 bool setter 体（接在 16 字节序言之后；序言已 push rdi + sub rsp,0x20）：
//   mov byte ptr [rcx], dil      ; 把**函数体真正收到的** bool 写进 test 给的 camera 字节
//   movzx eax, dil               ; 返回收到的 bool（证明 dl 没被 stub 破坏）
//   add rsp, 0x20 ; pop rdi ; mov rbx,[rsp+8] ; ret
// 刻意**不用 RIP 相对寻址**写全局：test exe 的全局与 VirtualAlloc 返回的地址可能相距
// 超过 2 GB，disp32 截断会写到错误地址（本测试首版就是这么 AV 的）。
std::size_t build_flag_body(std::uint8_t *at)
{
    std::size_t p = 0;
    at[p++] = 0x40; at[p++] = 0x88; at[p++] = 0x39;                 // mov [rcx], dil
    at[p++] = 0x40; at[p++] = 0x0F; at[p++] = 0xB6; at[p++] = 0xC7; // movzx eax, dil
    at[p++] = 0x48; at[p++] = 0x83; at[p++] = 0xC4; at[p++] = 0x20; // add rsp, 0x20
    at[p++] = 0x5F;                                                 // pop rdi
    at[p++] = 0x48; at[p++] = 0x8B; at[p++] = 0x5C; at[p++] = 0x24; at[p++] = 0x08; // mov rbx,[rsp+8]
    at[p++] = 0xC3;                                                 // ret
    return p;
}

void build_image()
{
    g_image = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, k_image_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    std::memset(g_image, 0xCC, k_image_size);

    // 锚点函数：序言 + 两处调用点（矩阵 setter → bool setter，间隔 10 字节同实机）
    std::memcpy(g_image + k_anchor_rva, k_camera_head, sizeof(k_camera_head));
    std::uint8_t *anchor = g_image + k_anchor_rva;
    std::memcpy(anchor + k_anchor_pre_off, k_lea_rdx, sizeof(k_lea_rdx));
    std::memcpy(anchor + k_anchor_pre_off + 5, k_mov_rcx_r14, sizeof(k_mov_rcx_r14));
    write_call(anchor, k_anchor_call1_off, k_matrix_rva);
    std::memcpy(anchor + k_anchor_call2_off - 5, k_mov_rcx_r14, sizeof(k_mov_rcx_r14));
    std::memcpy(anchor + k_anchor_call2_off - 2, k_xor_edx_edx, sizeof(k_xor_edx_edx));
    write_call(anchor, k_anchor_call2_off, k_flag_rva);

    // 假矩阵 setter（只用于被识别；不会被调用）
    std::memcpy(g_image + k_matrix_rva, k_matrix_head, sizeof(k_matrix_head));
    const std::uint8_t matrix_tail[] = {0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3};
    std::memcpy(g_image + k_matrix_rva + 16, matrix_tail, sizeof(matrix_tail));

    // 假 bool setter（这是**被装钩**的目标）。
    // ⚠️ 序言必须写满 19 字节、函数体从 +19 开始：签名匹配看的是 19 字节
    // （含 `48 85 C9` test rcx,rcx），若把体紧接在 16 字节后写，就会盖掉签名的
    // 第 16–18 字节 ⇒ 定位必然失败（本测试首版就是这么错的）。
    std::memcpy(g_image + k_flag_rva, k_flag_head, sizeof(k_flag_head));
    build_flag_body(g_image + k_flag_rva + sizeof(k_flag_head));
}

using flag_fn = bool (*)(void *camera, bool value);

const char *g_last_line = nullptr;
char g_lines[16][256] {};
std::size_t g_line_count = 0;

void test_sink(const char *line)
{
    if (g_line_count < 16)
    {
        std::snprintf(g_lines[g_line_count], sizeof(g_lines[0]), "%s", line);
        g_last_line = g_lines[g_line_count];
    }
    ++g_line_count;
}

// 在所有已记录行里找子串
bool any_line_has(const char *needle)
{
    for (std::size_t i = 0; i < g_line_count && i < 16; ++i)
    {
        if (std::strstr(g_lines[i], needle) != nullptr)
            return true;
    }
    return false;
}

std::uint64_t g_fake_frame = 0;
std::uint64_t frame_provider()
{
    return g_fake_frame;
}

// ---- ① 定位 ----
void run_locate_tests()
{
    std::uint32_t setter = 0;
    int imm = -2;
    const char *reason = nullptr;
    const bool ok = jitter_flag_probe::locate_flag_setter(g_image, k_image_size, k_anchor_rva,
                                                          jitter_flag_probe::k_scan_window, &setter,
                                                          &imm, &reason);
    CHECK(ok, "locate: 找到 bool setter");
    CHECK(setter == k_flag_rva, "locate: 目标 RVA 正确 (0x3080)");
    CHECK(imm == 0, "locate: 调用点实参识别为常量 0（= 源码里的 false）");

    // 负例：锚点序言/窗口非法
    CHECK(!jitter_flag_probe::locate_flag_setter(g_image, k_image_size, 0, 0x1000, &setter, &imm, &reason),
          "locate: camera_rva=0 被拒");
    CHECK(reason != nullptr && std::strcmp(reason, "bad_rva") == 0, "locate: 原因是 bad_rva");
    CHECK(!jitter_flag_probe::locate_flag_setter(g_image, k_image_size, k_anchor_rva, 0, &setter, &imm, &reason),
          "locate: scan_window=0 被拒");

    // 负例：把 bool setter 的序言改掉 → 找不到
    std::uint8_t saved[19] {};
    std::memcpy(saved, g_image + k_flag_rva, sizeof(saved));
    g_image[k_flag_rva + 10] = 0x48; // movzx edi,dl -> mov rdi,rdx（不再是 bool setter）
    CHECK(!jitter_flag_probe::locate_flag_setter(g_image, k_image_size, k_anchor_rva, 0x1000, &setter,
                                                 &imm, &reason),
          "locate: 序言被改后找不到（拒绝误判）");
    CHECK(reason != nullptr && std::strcmp(reason, "flag_setter_not_found") == 0,
          "locate: 原因是 flag_setter_not_found");
    std::memcpy(g_image + k_flag_rva, saved, sizeof(saved));

    // Tier-2：再加一处「不与矩阵 setter 相邻」的 bool setter 调用 → 仍应唯一定位原目标
    std::uint8_t *anchor = g_image + k_anchor_rva;
    std::memcpy(anchor + 0x200, k_mov_rcx_r14, sizeof(k_mov_rcx_r14));
    write_call(anchor, 0x203, k_flag_rva);
    CHECK(jitter_flag_probe::locate_flag_setter(g_image, k_image_size, k_anchor_rva, 0x1000, &setter,
                                                &imm, &reason) &&
              setter == k_flag_rva,
          "locate: Tier-2 在并列候选中选中「紧跟矩阵 setter 且实参为常量」的那个");

    // Tier-2 无法裁决：再加一处同样紧跟矩阵 setter 的 bool 调用 → 如实失败
    std::memcpy(anchor + 0x300, k_lea_rdx, sizeof(k_lea_rdx));
    std::memcpy(anchor + 0x305, k_mov_rcx_r14, sizeof(k_mov_rcx_r14));
    write_call(anchor, 0x308, k_matrix_rva);
    std::memcpy(anchor + 0x30D, k_mov_rcx_r14, sizeof(k_mov_rcx_r14));
    std::memcpy(anchor + 0x310, k_xor_edx_edx, sizeof(k_xor_edx_edx));
    write_call(anchor, 0x312, k_flag_rva);
    CHECK(!jitter_flag_probe::locate_flag_setter(g_image, k_image_size, k_anchor_rva, 0x1000, &setter,
                                                 &imm, &reason),
          "locate: 两处等价候选 → 如实失败（不猜）");
    CHECK(reason != nullptr && std::strcmp(reason, "flag_setter_ambiguous") == 0,
          "locate: 原因是 flag_setter_ambiguous");

    // 复原锚点，避免影响后续 install 测试
    std::memset(anchor + 0x200, 0xCC, 0x200);
}

// ---- ② 只读 + ④ 关闭/还原 ----
void run_install_tests()
{
    auto *flag = reinterpret_cast<flag_fn>(g_image + k_flag_rva);

    // 关闭时：零字节改动
    jitter_flag_probe::Config off_cfg;
    off_cfg.enabled = false;
    off_cfg.camera_rva = k_anchor_rva;
    off_cfg.image_size = k_image_size;
    const char *reason = nullptr;
    CHECK(!jitter_flag_probe::install(reinterpret_cast<std::uint64_t>(g_image), off_cfg, &reason),
          "install: enabled=false 返回 false");
    CHECK(reason != nullptr && std::strcmp(reason, "disabled") == 0, "install: 原因是 disabled");
    CHECK(!jitter_flag_probe::active(), "install: enabled=false 时 active() 为 false");
    CHECK(std::memcmp(g_image + k_flag_rva, k_flag_head, sizeof(k_flag_head)) == 0,
          "install: enabled=false 时目标字节**一个都没改**");

    // 打开
    jitter_flag_probe::set_log_sink(&test_sink);
    jitter_flag_probe::set_frame_provider(&frame_provider);
    jitter_flag_probe::Config cfg;
    cfg.enabled = true;
    cfg.camera_rva = k_anchor_rva;
    cfg.image_size = k_image_size;
    CHECK(jitter_flag_probe::install(reinterpret_cast<std::uint64_t>(g_image), cfg, &reason),
          "install: 打开后安装成功");
    CHECK(jitter_flag_probe::active(), "install: active() 为 true");
    CHECK(g_image[k_flag_rva] == 0x48 && g_image[k_flag_rva + 1] == 0xB8,
          "install: 目标序言已被绝对跳转替换");

    // 只读观测：false。camera 字节既证明函数体执行了，也记录函数体**实际收到**的实参。
    std::uint8_t camera_byte = 0xAA;
    void *camera = &camera_byte;
    g_fake_frame = 7;
    const bool r1 = flag(camera, false);
    CHECK(r1 == false, "pass-through: 传 false 时函数仍返回 false");
    CHECK(camera_byte == 0x00, "pass-through: 原函数体**确实执行了**，且收到的实参是 false");
    std::uint8_t value = 0xFF;
    std::uint64_t cam = 0, frame = 0;
    CHECK(jitter_flag_probe::last_observation(&value, &cam, &frame), "observe: 有观测记录");
    CHECK(value == 0, "observe: 记录到 value=false（这就是要证的实参）");
    CHECK(cam == reinterpret_cast<std::uint64_t>(camera), "observe: 记录到 camera 指针");
    CHECK(frame == 7, "observe: frame 来自注入的帧号来源");
    CHECK(jitter_flag_probe::saw_false(), "observe: saw_false() 为 true");
    CHECK(!jitter_flag_probe::saw_true(), "observe: 此时尚未见过 true");

    // 只读观测：true（证伪分支）
    g_fake_frame = 8;
    const bool r2 = flag(camera, true);
    CHECK(r2 == true, "pass-through: 传 true 时函数仍返回 true（实参未被改写）");
    CHECK(camera_byte == 0x01, "pass-through: 函数体收到的实参是 true（未被 stub 改成 false）");
    CHECK(jitter_flag_probe::last_observation(&value, &cam, &frame) && value == 1,
          "observe: 记录到 value=true");
    CHECK(jitter_flag_probe::saw_true(), "observe: saw_true() 为 true");

    // 日志：至少出现首次行与变化行，且形如 jitter_flag value=...
    bool saw_first = false, saw_change = false, saw_installed = false;
    for (std::size_t i = 0; i < g_line_count && i < 16; ++i)
    {
        if (std::strncmp(g_lines[i], "jitter_flag value=", 18) == 0)
        {
            if (std::strstr(g_lines[i], "reason=first") != nullptr)
                saw_first = true;
            if (std::strstr(g_lines[i], "reason=change") != nullptr)
                saw_change = true;
        }
        if (std::strstr(g_lines[i], "jitter_flag_probe_installed") != nullptr)
            saw_installed = true;
    }
    CHECK(saw_installed, "log: 安装证据行（含 setter_rva / callsite_imm）");
    CHECK(saw_first, "log: 首次观测行（证明钩子确实装上了）");
    CHECK(saw_change, "log: 值变化行（false -> true）");
    CHECK(any_line_has("value=false forced=false"),
          "log: force=0 时观测行的 forced 与 value 一致（= 没改任何东西）");

    // 还原
    jitter_flag_probe::shutdown();
    CHECK(!jitter_flag_probe::active(), "shutdown: active() 变回 false");
    CHECK(std::memcmp(g_image + k_flag_rva, k_flag_head, sizeof(k_flag_head)) == 0,
          "shutdown: 原始序言已还原（16 字节）");
    // 刻意**不**在这里"顺手修好"目标字节：下面的调用必须验证 `shutdown()` 真的还原了，
    // 而不是被测试自己补回来的。
    const std::uint64_t before = jitter_flag_probe::observed_count();
    std::uint8_t camera_after = 0xAA;
    CHECK(flag(&camera_after, true) == true, "shutdown: 还原后函数仍可正常调用");
    CHECK(camera_after == 0x01, "shutdown: 还原后函数体仍执行并收到正确实参");
    CHECK(jitter_flag_probe::observed_count() == before, "shutdown: 还原后不再产生观测");
}

// ---- ③ 限流状态机 ----
// ⚠️ decide_emit 现在同时看 value（原值）与 forwarded（转发值），因此下面每次调用都
// 显式给出两者；"只读观测"场景里二者相等。
void run_emit_tests()
{
    using namespace jitter_flag_probe;
    EmitState st;
    EmitDecision d = decide_emit(st, 0, 0, 0, 1, 0, 300);
    CHECK(d.emit && std::strcmp(d.reason, "first") == 0, "emit: 首次必打");

    bool spam = false;
    for (std::uint64_t call = 2; call <= 300; ++call)
    {
        d = decide_emit(st, 0, 0, call - 1, call, 0, 300);
        if (d.emit)
            spam = true;
    }
    CHECK(!spam, "emit: 值不变时 300 次调用内**一行都不多打**（不刷屏）");

    d = decide_emit(st, 0, 0, 300, 301, 0, 300);
    CHECK(d.emit && std::strcmp(d.reason, "heartbeat") == 0, "emit: 第 301 次调用打心跳");

    d = decide_emit(st, 1, 1, 301, 302, 0, 300);
    CHECK(d.emit && std::strcmp(d.reason, "change") == 0, "emit: 原值变化立刻打（不等心跳）");

    // ★ force 翻转：原值**没变**（游戏仍传 false），只有转发值变了 0->1 ⇒ 也必须立刻打。
    // 否则按了热键之后要等最多 300 次调用才能在日志里看到 forced=true，
    // 而"force 到底生效了没"正是 A/B 最需要立刻确认的一项。
    EmitState st_force;
    decide_emit(st_force, 0, 0, 0, 1, 0, 300);
    d = decide_emit(st_force, 0, 1, 1, 2, 0, 300);
    CHECK(d.emit && std::strcmp(d.reason, "change") == 0,
          "emit: force 翻转（原值不变、转发值 0->1）立刻打一行");
    d = decide_emit(st_force, 0, 0, 2, 3, 0, 300);
    CHECK(d.emit && std::strcmp(d.reason, "change") == 0,
          "emit: force 翻回（转发值 1->0）同样立刻打一行");

    // 值不变时的不刷屏性：600 次调用里只应有 1 行心跳（第 301 次）
    EmitState st2;
    decide_emit(st2, 0, 0, 0, 1, 0, 300);
    int heartbeats = 0;
    for (std::uint64_t call = 2; call <= 600; ++call)
    {
        if (decide_emit(st2, 0, 0, call - 1, call, 0, 300).emit)
            ++heartbeats;
    }
    CHECK(heartbeats == 1, "emit: 600 次等值调用里只有 1 行心跳（绝不刷屏）");

    // 帧数上限
    EmitState st3;
    d = decide_emit(st3, 0, 0, 10, 1, 10, 300);
    CHECK(d.emit, "emit: frame <= frames_limit 时正常记录");
    d = decide_emit(st3, 1, 1, 11, 2, 10, 300);
    CHECK(!d.emit, "emit: frame > frames_limit 后一律不打（即使值变化）");

    // 帧号冻结时心跳仍应发生（心跳用调用计数，不用帧号）
    EmitState st4;
    decide_emit(st4, 0, 0, 0, 1, 0, 5);
    bool heartbeat = false;
    for (std::uint64_t call = 2; call <= 6; ++call)
    {
        if (decide_emit(st4, 0, 0, 0, call, 0, 5).emit)
            heartbeat = true;
    }
    CHECK(heartbeat, "emit: 帧号冻结（恒为 0）时心跳仍会发生");
}

// ---- ④ 【强制 true】端到端 ----
// 关键断言不是"我们自己说改了"，而是**假 setter 体内真的收到 true**
// （假 setter 体会把收到的 dil 写回 camera 字节，并把它当返回值返回）。
void run_force_tests()
{
    using namespace jitter_flag_probe;
    auto *flag = reinterpret_cast<flag_fn>(g_image + k_flag_rva);

    // 纯函数：force 的**全部语义**
    CHECK(decide_forwarded_value(false, 0) == 0, "forward: force=0 原样返回 0");
    CHECK(decide_forwarded_value(false, 0xDEADBEEFCAFEF00Dull) == 0xDEADBEEFCAFEF00Dull,
          "forward: force=0 **逐位原样**返回（连 rdx 高位也不动）");
    CHECK(decide_forwarded_value(true, 0) == 1, "forward: force=1 时 false 被改成 1");
    CHECK(decide_forwarded_value(true, 0xDEADBEEFCAFEF00Dull) == 1,
          "forward: force=1 时**无论**原值是什么都变成 1");

    // 重装（前面的 shutdown 已还原）—— 初值 force=false
    Config cfg;
    cfg.enabled = true;
    cfg.force = false;
    cfg.camera_rva = k_anchor_rva;
    cfg.image_size = k_image_size;
    const char *reason = nullptr;
    CHECK(install(reinterpret_cast<std::uint64_t>(g_image), cfg, &reason), "force: 重新安装成功");
    CHECK(!force_enabled(), "force: 初值来自 cfg.force=false");
    CHECK(any_line_has("jitter_flag_probe_installed") && any_line_has("force=0"),
          "log: 安装行带 force 初值（force=0）");

    // force=0：行为与只读时完全一致
    std::uint8_t camera = 0xAA;
    g_fake_frame = 100;
    const std::uint64_t over_before = overridden_count();
    CHECK(flag(&camera, false) == false, "force=0: 返回值不变（false）");
    CHECK(camera == 0x00, "force=0: setter 体内收到的仍是 false");
    CHECK(overridden_count() == over_before, "force=0: 没有任何一次改写被记入 overridden_count");
    std::uint8_t orig = 0xFF, fwd = 0xFF;
    CHECK(last_forwarding(&orig, &fwd) && orig == 0 && fwd == 0, "force=0: 原值=转发值=0");

    // 切到 force=1（模拟热键）
    const std::size_t lines_before_toggle = g_line_count;
    CHECK(set_force(true, "hotkey", 1234), "force: set_force(true) 返回 true（状态确实变了）");
    CHECK(force_enabled(), "force: force_enabled() 为 true");
    CHECK(g_line_count > lines_before_toggle, "log: 切换必打一行");
    CHECK(any_line_has("jitter_flag_force toggled on=true source=hotkey frame=1234"),
          "log: 切换行格式为 jitter_flag_force toggled on=... source=... frame=...");

    // ★ 核心断言：游戏传 false，但 setter 体内**真的收到 true**
    camera = 0xAA;
    g_fake_frame = 101;
    CHECK(flag(&camera, false) == true, "force=1: 返回值变成 true（实参确实被改成 true）");
    CHECK(camera == 0x01, "force=1: **setter 体内收到的实参是 true**");
    CHECK(last_observation(&orig, nullptr, nullptr) && orig == 0,
          "force=1: observer 记录的**仍是原始值 false**（日志能给出原值）");
    CHECK(last_forwarding(&orig, &fwd) && orig == 0 && fwd == 1,
          "force=1: 原值=0 而转发值=1（原值与传出的值可同时观察）");
    CHECK(overridden_count() == over_before + 1, "force=1: overridden_count 记到 1 次真实改写");
    CHECK(any_line_has("value=false forced=true"),
          "log: 观测行同时给出 value=false（原值）与 forced=true（实际传出的值）");
    CHECK(any_line_has("callsite_imm=0"), "log: 观测行给出 callsite_imm=0（静态原值）");

    // 幂等切换：状态没变就不打行（防止被重复调用时刷屏）
    const std::size_t lines_before_idempotent = g_line_count;
    CHECK(!set_force(true, "hotkey", 1235), "force: 重复置 true 返回 false（状态未变）");
    CHECK(g_line_count == lines_before_idempotent, "log: 状态未变时不打行");

    // 翻回 force=0
    CHECK(set_force(false, "hotkey", 1300), "force: 翻回 false");
    CHECK(!force_enabled(), "force: force_enabled() 变回 false");
    camera = 0xAA;
    g_fake_frame = 102;
    CHECK(flag(&camera, false) == false, "force: 翻回后返回值恢复 false");
    CHECK(camera == 0x00, "force: 翻回后 setter 体内收到的又是 false（热键可随时撤销）");
    CHECK(overridden_count() == over_before + 1, "force: 翻回后不再改写（改写次数停在 1）");

    // 还原：仍然只在字节是我们的 jmp 时还原；且 force 状态复位
    CHECK(set_force(true, "hotkey", 1400), "force: 还原前先重新打开");
    shutdown();
    CHECK(!active(), "force: shutdown 后 active() 为 false");
    CHECK(!force_enabled(), "force: shutdown 把 force 状态复位为 false");
    CHECK(std::memcmp(g_image + k_flag_rva, k_flag_head, sizeof(k_flag_head)) == 0,
          "force: shutdown 后原始序言逐字节还原");
    camera = 0xAA;
    CHECK(flag(&camera, false) == false && camera == 0x00,
          "force: 还原后即使 force 曾开着也不再改写真参");
}

} // namespace

int main()
{
    std::printf("JitterFlagProbeTest\n");
    build_image();
    run_locate_tests();
    run_install_tests();
    run_force_tests();
    run_emit_tests();
    if (failures == 0)
        std::printf("ALL PASS\n");
    else
        std::printf("%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
