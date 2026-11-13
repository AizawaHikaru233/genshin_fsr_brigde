//! 主页「帧生成」开关：只镜像已安装那份 `OptiScaler.ini` 的 `[FrameGen]` 两个键。
//!
//! 判据完全跟随 OptiScaler（`OptiScaler/framegen/` 里 `FGInput` / `FGOutput` 的读取点）：
//!   · `FGInput`  决定**谁来喂帧生成**：`Upscaler` = 用超分后的画面（OptiFG）；
//!   · `FGOutput` 决定**用哪套帧生成实现**：`DLSSG` = NVIDIA DLSS Frame Generation。
//!
//! 两者都支持 `auto`（由 OptiScaler 按显卡自己探测）。界面的开关**只在
//! 「开」和「关」之间切**，不暴露中间态：
//!   开 = `FGInput=Upscaler` + `FGOutput=DLSSG`（N 卡；A 卡应走 `FSRFG`，见下）
//!   关 = `FGInput=NoFG`     + `FGOutput=NoFG`
//!
//! A 卡的帧生成走 `FSRFG`（AMD Fluid Motion Frames），不走 DLSSG —— 这里按显卡判定。
//!
//! ★ 与 OptiScaler 的"auto"互不影响：用户不动这个开关时，安装器写回 `auto`（见
//!   `install::prepare_opti`），让 OptiScaler 自己探测；用户显式拨过开关后才写具体值。

use std::path::PathBuf;

use crate::gpu;
use crate::log;
use crate::ini;
use crate::pkg;
use crate::settings::Settings;

const SECTION: &str = "FrameGen";
const KEY_INPUT: &str = "FGInput";
const KEY_OUTPUT: &str = "FGOutput";

/// 帧生成的三种界面状态。
/// 帧生成的两态。**2026-11-14：不再有 `Auto`。**
///
/// 为什么去掉 auto：`auto` 交给 OptiScaler 按显卡探测，而它在**非 RTX 的 N 卡**
/// （GTX 10/16）上会落到 `DLSSG` —— 那些卡没有 DLSS-G，缺 Streamline 时游戏直接起不来。
/// 用户也没法从界面上判断自己是哪一种。改成只用「开 / 关」，开的时候由安装器按显卡
/// 显式写 `DLSSG`（NVIDIA RTX）或 `FSRFG`（其余），A 卡用户不会再踩到 DLSSG。
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum State {
    /// `FGInput=NoFG` / `FGOutput=NoFG`
    Off,
    /// `FGInput=Upscaler` + `FGOutput` 是具体实现（DLSSG 或 FSRFG）
    On,
}

impl State {
    pub fn as_str(self) -> &'static str {
        match self {
            State::Off => "off",
            State::On => "on",
        }
    }
}

/// 已安装那份 `OptiScaler.ini`：没装 OptiScaler（或目录里没有这份 ini）就返回 None。
pub fn ini_path(settings: &Settings) -> Option<PathBuf> {
    let version = pkg::resolve("opti", settings)?;
    let path = version.path.join("OptiScaler.ini");
    path.is_file().then_some(path)
}

/// 读当前状态。只有明确的「两个键都是 NoFG」才算关；其它（含 `auto` / 缺键）都算开。
///
/// 为什么把 `auto` / 缺键算「开」：
///   · 装过旧版安装器的用户 ini 里可能是 `auto`（旧默认值），实际帧生成是**开着**的；
///   · 界面已经没有 auto 这一档，若把它显示成「关」，用户会以为没开而去点「开」，
///     结果误改他的配置。
///   · 反过来：显示「开」且用户点「关」时，会写明确的 `NoFG`/`NoFG` —— 这也是他想要的。
pub fn current(settings: &Settings) -> State {
    let Some(path) = ini_path(settings) else {
        // 没装 OptiScaler：没有帧生成可谈，按「关」显示（开关同时是不可用的）。
        return State::Off;
    };

    let input = ini::get(&path, SECTION, KEY_INPUT);
    let output = ini::get(&path, SECTION, KEY_OUTPUT);

    match (input.as_deref(), output.as_deref()) {
        (Some(i), Some(o)) if i.eq_ignore_ascii_case("nofg") && o.eq_ignore_ascii_case("nofg") => {
            State::Off
        }
        // 其余一律「开」（含 auto / 缺键 / 一边 nofg 一边具体值这种半残状态）。
        _ => State::On,
    }
}

/// 按显卡选帧生成实现。
///
/// | 显卡 | `FGOutput` | 为什么 |
/// |---|---|---|
/// | NVIDIA | `DLSSG` | DLSS Frame Generation，需要 Streamline 运行时 |
/// | AMD | `FSRFG` | FSR Frame Generation（AMD Fluid Motion Frames 系），不需要 Streamline |
///
/// **2026-11-14 修正**：以前 N 卡以外一律给 `auto`，而 `auto` 在**非 RTX 的 N 卡**
/// （GTX 10/16）上会被 OptiScaler 判成 DLSSG，缺 Streamline 时直接进不去游戏；
/// Intel 卡更糟 —— 两边都不支持，`DLSSG` 与 `FSRFG` 都可能起不来。
/// 现在显式分流：AMD 给 `FSRFG`，其余（含 Intel / 未知）保守给 `FSRFG`
/// （OptiScaler 的 FSRFG 输出对非 AMD 卡也能走，只是效率不最优；至少不会崩在缺 DLL 上）。
fn output_for_gpu() -> &'static str {
    match gpu::vendor() {
        "nvidia" if gpu::has_rtx() => "DLSSG",
        // 非 RTX 的 N 卡（GTX 10/16 等）：没有 DLSS-G，走 FSRFG。
        "nvidia" => "FSRFG",
        // AMD（RDNA1~4 都算）/ Intel / 未知：FSRFG。
        _ => "FSRFG",
    }
}

/// 写开 / 关。
///
/// * `on = true`  → `FGInput=Upscaler` + `FGOutput=<按显卡选>`（NVIDIA RTX 走 DLSSG，其余 FSRFG）
/// * `on = false` → `FGInput=NoFG` + `FGOutput=NoFG`
///
/// **2026-11-14：不再接受「自动」**。三态里的 `auto` 已从界面移除 ——
/// 由安装器按显卡显式选实现，避免 `auto` 在非 RTX 卡上落到 DLSSG（缺 Streamline 会进不去游戏）。
pub fn write(settings: &Settings, on: bool) -> Result<State, String> {
    let path = ini_path(settings)
        .ok_or_else(|| "还没装 OptiScaler：找不到 OptiScaler.ini".to_string())?;

    let (input, output) = if on {
        ("Upscaler", output_for_gpu())
    } else {
        ("NoFG", "NoFG")
    };

    ini::set(&path, SECTION, KEY_INPUT, input)
        .map_err(|e| format!("写入 OptiScaler.ini（{SECTION}.{KEY_INPUT}）失败：{e}"))?;
    ini::set(&path, SECTION, KEY_OUTPUT, output)
        .map_err(|e| format!("写入 OptiScaler.ini（{SECTION}.{KEY_OUTPUT}）失败：{e}"))?;

    let state = if on { State::On } else { State::Off };

    log::write(&format!(
        "[install] 帧生成: {} （FGInput={input}, FGOutput={output}）",
        if on { "开启" } else { "关闭" }
    ));

    Ok(state)
}

/// 帧生成当前是否**确定开着**（供 `pkg::reshade_via_opti` 判断 ReShade 注入方式）。
///
/// 判据与 [`current`] 的 `State::On` 完全一致。
pub fn is_enabled(settings: &Settings) -> bool {
    matches!(current(settings), State::On)
}

/// 是否可能用到 **DLSSG** 输出 —— 决定要不要补 Streamline 运行时。
///
/// 判据（读 `OptiScaler.ini` 的 `[FrameGen] FGOutput`）：
///   · 显式 `DLSSG` ⇒ 是（大小写不敏感，ini 里也可能写 `dlssg`，OptiScaler 两种都认）；
///   · `auto`（或缺键）⇒ **N 卡**才算是；
///   · `NoFG` / `FSRFG` / `XeFG` ⇒ 否（这几种不吃 Streamline）。
///
/// 新装的配置由 [`write`] 写**显式**值（RTX 写 `DLSSG`，其余写 `FSRFG`），所以正常情况下
/// 走第一条或 `FSRFG` 那条。`auto` / 缺键只可能是**旧版安装器留下的 ini**
/// （旧版给 N 卡写 `auto`），那时按显卡判：RTX 上 `auto` 确实落到 DLSSG，不补就报
/// "Are you missing the streamline folder?"；A 卡上 `auto` 落到 FSRFG，补了纯属浪费 263 MB。
pub fn wants_dlssg(settings: &Settings) -> bool {
    let Some(path) = ini_path(settings) else {
        // 还没装 OptiScaler：由调用方（ensure_streamline）自己判要不要，这里保守返回 false。
        return false;
    };
    match ini::get(&path, SECTION, KEY_OUTPUT).as_deref() {
        Some(v) if v.eq_ignore_ascii_case("dlssg") => true,
        Some(v) if v.eq_ignore_ascii_case("auto") => gpu::has_rtx(),
        None => gpu::has_rtx(),
        _ => false,
    }
}

/// 界面显示用的现状（供 post_state 用）。
/// 没装 OptiScaler 时返回 `None`，UI 据此禁用开关。
pub fn snapshot(settings: &Settings) -> Option<&'static str> {
    ini_path(settings).map(|_| current(settings).as_str())
}

