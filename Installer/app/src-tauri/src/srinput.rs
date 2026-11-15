//! 主页「超分输入类型」：只镜像已安装那份 `Dx11FsrBridge.ini` 的 `DlssInput`。
//!
//! 判据完全跟随桥（`Dx11FsrBridge.cpp` 的 `dlss_gate::resolve_path`）：
//!
//!   · `1` = auto（默认）—— 能拿到 nvngx / OptiScaler 在场时走 DLSS 直连，否则退回 ffx-api
//!   · `2` = force    —— 强制走 DLSS 直连
//!
//! ⚠️ **没有 `0` 这一档**。旧设计里 0 表示「用户显式选 FFX」，桥会把
//! `user_preference=0` 传给 `route_output` 得到 `user_forced_ffx` —— 那会让桥**自己**
//! 跑 FFX 超分；而 OptiScaler 侧通常还开着 `Dx11Upscaler=dlss`，两套超分同时接管
//! 同一条 draw ⇒ 实机卡死。现在桥把 0 和任何未知值都按 auto 处理。
//!
//! 界面只做「读现值 / 写合法值」，不替桥判定，也不改桥的行为。
//! 显示名与值的对应：Auto -> 1、Force DLSS -> 2。

use std::path::PathBuf;

use crate::gpu;
use crate::ini;
use crate::pkg;
use crate::settings::Settings;

const SECTION: &str = "Dx11FsrBridge";
const KEY: &str = "DlssInput";

/// 桥认识的值，顺序即界面顺序。
pub const VALUES: [&str; 2] = ["1", "2"];

/// 已安装那份 `Dx11FsrBridge.ini`：桥没装（或版本目录里没有这份 ini）就返回 None。
pub fn ini_path(settings: &Settings) -> Option<PathBuf> {
    let version = pkg::resolve("bridge", settings)?;
    let path = version.path.join("Dx11FsrBridge.ini");
    path.is_file().then_some(path)
}

/// 桥实际会用的模式：缺键 = auto（"1"）；非法值也按 auto —— 桥那边的映射是
/// `mode == 2 ? force : auto`，所以除 "2" 之外的一切都等价于 auto。
pub fn effective(settings: &Settings) -> String {
    match ini_path(settings) {
        Some(path) => match ini::get(&path, SECTION, KEY).as_deref() {
            Some("2") => "2".to_string(),
            _ => "1".to_string(),
        },
        None => "1".to_string(),
    }
}

/// 桥会不会走 DLSS 直连。auto 的判据见 `DlssInput.cpp` 的 `route_output`：
/// ① 已加载 NGX 且（N 卡 或 OptiScaler 在场）；② OptiScaler 在场。
///
/// 用来决定「只装桥」时要不要把 `nvngx_dlss.dll` 也补齐 —— 只要能让
/// `dlss_runtime_present` 成立，auto 就会落到直连路径。
pub fn uses_dlss(settings: &Settings) -> bool {
    match effective(settings).as_str() {
        "2" => true,
        // auto：桥的判据要求「NGX 运行时可加载」或「OptiScaler 在场」。安装器这边
        // 只能按显卡推断第一项 —— OptiScaler 是否装由别的路径决定，不由这里判断。
        _ => gpu::has_rtx(),
    }
}

/// 写用户选的模式。只接受两个合法值：桥只认精确的 "1"/"2"，安装器不替用户纠错
/// （写错等于悄悄把桥打回 auto）。
pub fn write(settings: &Settings, value: &str) -> Result<(), String> {
    if !VALUES.contains(&value) {
        return Err(format!("不支持的超分输入类型：{value}"));
    }
    let path = ini_path(settings)
        .ok_or_else(|| "还没装 Dx11FsrBridge：找不到 Dx11FsrBridge.ini".to_string())?;
    ini::set(&path, SECTION, KEY, value).map_err(|e| format!("写入 Dx11FsrBridge.ini 失败：{e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 只有 "2" 是 force，其余全部归 auto —— 必须与桥的
    /// `g_config.dlss_input_mode == 2 ? 1 : -1` 逐字对应。
    #[test]
    fn values_have_no_off_tier() {
        assert_eq!(VALUES, ["1", "2"]);
        // 这个断言固定住「没有 0」这件事：桥侧 0 会走 FFX 并与 OptiScaler 抢超分。
        assert!(!VALUES.contains(&"0"));
    }
}
