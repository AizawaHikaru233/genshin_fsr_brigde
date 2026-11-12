//! 主页「超分输入类型」：只镜像已安装那份 `Dx11FsrBridge.ini` 的 `Ffx12Backend`。
//!
//! 判据完全跟随桥（`Dx11FsrBridge.cpp` 里 Ffx12Backend 的读取点）：只认精确小写的
//! `auto` / `native` / `ngx`；缺键 = `auto`；其它任何值（空、大写、拼错）桥会回退
//! `native` 并告警 `backend_invalid`。界面只做「读现值 / 写合法值」，不替桥判定，
//! 也不改桥的行为。显示名与后端值的对应：Auto -> auto、FFX -> native、NVNGX -> ngx。

use std::path::PathBuf;

use crate::gpu;
use crate::ini;
use crate::pkg;
use crate::settings::Settings;

const SECTION: &str = "Dx11FsrBridge";
const KEY: &str = "Ffx12Backend";

/// 桥认识的后端值，顺序即界面顺序。
pub const VALUES: [&str; 3] = ["auto", "native", "ngx"];

/// 已安装那份 `Dx11FsrBridge.ini`：桥没装（或版本目录里没有这份 ini）就返回 None。
pub fn ini_path(settings: &Settings) -> Option<PathBuf> {
    let version = pkg::resolve("bridge", settings)?;
    let path = version.path.join("Dx11FsrBridge.ini");
    path.is_file().then_some(path)
}

/// 桥实际会用的后端：缺键 = auto；非法值 = native（与桥的 backend_invalid 回退一致）。
pub fn effective(settings: &Settings) -> String {
    match ini_path(settings) {
        Some(path) => match ini::get(&path, SECTION, KEY).as_deref() {
            Some("auto") => "auto".to_string(),
            Some("native") => "native".to_string(),
            Some("ngx") => "ngx".to_string(),
            Some(_) => "native".to_string(),
            None => "auto".to_string(),
        },
        None => "auto".to_string(),
    }
}

/// 桥会不会走 NGX：显式 `ngx`，或 `auto` 且这张卡能跑 DLSS（桥的 auto 判据在
/// `GpuBackendPick.h`：vendor 必须是 0x10DE 且型号在 RTX/Ampere 及以上）。
/// 用来决定「只装桥」时要不要把 `nvngx_dlss.dll` 也补齐。
pub fn uses_ngx(settings: &Settings) -> bool {
    match effective(settings).as_str() {
        "ngx" => true,
        "auto" => gpu::has_rtx(),
        _ => false,
    }
}

/// 写用户选的后端。只接受三个合法值：桥只认精确小写，安装器不替用户纠错（写错等于
/// 悄悄把桥打回 native）。
pub fn write(settings: &Settings, value: &str) -> Result<(), String> {
    if !VALUES.contains(&value) {
        return Err(format!("不支持的超分输入类型：{value}"));
    }
    let path = ini_path(settings)
        .ok_or_else(|| "还没装 Dx11FsrBridge：找不到 Dx11FsrBridge.ini".to_string())?;
    ini::set(&path, SECTION, KEY, value).map_err(|e| format!("写入 Dx11FsrBridge.ini 失败：{e}"))
}
