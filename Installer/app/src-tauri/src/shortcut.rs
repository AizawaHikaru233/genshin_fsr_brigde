//! 桌面快捷方式：指向解帧工具本体（unlockfps_nc.exe），
//! 让它自己带着 fps_config.json 里的注入链去启动游戏。
//!
//! 中文环境叫「原神」，英文环境叫「Genshin Impact」；图标借游戏本体的 exe。

use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use winreg::enums::HKEY_CURRENT_USER;
use winreg::RegKey;

/// 让 PowerShell 子进程不弹黑窗。
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

/// 桌面目录：先读注册表的 User Shell Folders（OneDrive 重定向也能拿到），
/// 失败再回落到 %USERPROFILE%\Desktop。
pub fn desktop_dir() -> PathBuf {
    if let Some(dir) = from_registry() {
        if dir.is_dir() {
            return dir;
        }
    }
    let home = std::env::var("USERPROFILE").unwrap_or_default();
    PathBuf::from(home).join("Desktop")
}

fn from_registry() -> Option<PathBuf> {
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let key = hkcu
        .open_subkey("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders")
        .ok()?;
    let raw: String = key.get_value("Desktop").ok()?;
    Some(PathBuf::from(crate::paths::expand_env(&raw)))
}

/// 展开 %USERPROFILE% 这类变量（注册表里存的是未展开形式）。
/// 快捷方式文件名：中文环境用游戏的中文名，其它用英文名。
pub fn link_name(zh: bool) -> &'static str {
    if zh {
        "原神.lnk"
    } else {
        "Genshin Impact.lnk"
    }
}

pub fn link_path(zh: bool) -> PathBuf {
    desktop_dir().join(link_name(zh))
}

fn ps_quote(s: &str) -> String {
    s.replace('\'', "''")
}

/// 创建（或覆盖）桌面快捷方式，返回 .lnk 路径。
pub fn create(unlocker_exe: &Path, game_exe: Option<&Path>, zh: bool) -> Result<PathBuf, String> {
    if !unlocker_exe.is_file() {
        return Err(format!("找不到解帧工具：{}", unlocker_exe.display()));
    }
    let lnk = link_path(zh);
    let work_dir = unlocker_exe
        .parent()
        .map(|p| p.to_path_buf())
        .unwrap_or_default();
    // 图标优先借游戏本体，找不到就用解帧工具自己的。
    let icon_src = match game_exe {
        Some(g) if g.is_file() => g,
        _ => unlocker_exe,
    };
    let icon = format!("{},0", icon_src.display());

    let script = format!(
        "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{}');\
$s.TargetPath='{}';\
$s.WorkingDirectory='{}';\
$s.IconLocation='{}';\
$s.Description='Genshin FSR Bridge';\
$s.Save()",
        ps_quote(&lnk.display().to_string()),
        ps_quote(&unlocker_exe.display().to_string()),
        ps_quote(&work_dir.display().to_string()),
        ps_quote(&icon),
    );

    let out = Command::new("powershell")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-WindowStyle",
            "Hidden",
            "-Command",
            &script,
        ])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("调用 PowerShell 失败：{e}"))?;

    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr).trim().to_string();
        return Err(if err.is_empty() {
            "创建快捷方式失败".into()
        } else {
            err
        });
    }
    if !lnk.is_file() {
        return Err("快捷方式没有生成".into());
    }
    Ok(lnk)
}

/// 读一个 .lnk 的目标路径（读不到就返回 None）。
pub fn target_of(lnk: &Path) -> Option<String> {
    let script = format!(
        "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{}');Write-Output $s.TargetPath",
        ps_quote(&lnk.display().to_string())
    );
    let out = Command::new("powershell")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-WindowStyle",
            "Hidden",
            "-Command",
            &script,
        ])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if text.is_empty() {
        None
    } else {
        Some(text)
    }
}

/// 这个快捷方式是不是我们建的（目标就是解帧工具本体）。
/// 用户自己放的「原神.lnk」不动。
pub fn is_ours(lnk: &Path, unlocker_exe: &Path) -> bool {
    if !lnk.is_file() {
        return false;
    }
    match target_of(lnk) {
        Some(target) => target.eq_ignore_ascii_case(&unlocker_exe.display().to_string()),
        None => false,
    }
}

/// 删除我们建的快捷方式（两种语言名字都清，但只清目标对得上的）。
pub fn remove(unlocker_exe: &Path) -> Result<usize, String> {
    let mut count = 0;
    for zh in [true, false] {
        let path = link_path(zh);
        if is_ours(&path, unlocker_exe) {
            std::fs::remove_file(&path).map_err(|e| format!("删除失败：{e}"))?;
            count += 1;
        }
    }
    Ok(count)
}
