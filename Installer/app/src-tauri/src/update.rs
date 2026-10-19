//! 插件包自更新。
//!
//! 流程：查 GitHub Releases 的最新 tag → 比版本 → 下载整包（7z/zip，走系统代理与加速线路
//! 测速）→ 解到临时目录 → 把整包里的 `payload\` 覆盖到本地 payload → 根目录文件
//! （`Package-Version.txt` / `upstream-versions.json` / `README.md` / `Feedback.txt` / `license\`）
//! 一并更新，**安装器 exe 自己不覆盖**（正在运行，覆盖会失败）→ 覆盖前备份的「实际在用」ini
//! 回填回去 → 重新写一遍注入链。
//!
//! 版本化组件（Bridge / OptiScaler）覆盖后目录名会变，这里顺手把当前启用版本切到最新的那个，
//! 否则用户更新完跑的还是旧目录里的 DLL。

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use serde_json::{json, Value};

use crate::fetch::{self, Report};
use crate::settings::Settings;
use crate::{gamepath, install, paths, pkg};

/// 本地包版本（包根 `Package-Version.txt`）。
pub fn local_version() -> String {
    let v = paths::package_version();
    if v.is_empty() {
        "未知".to_string()
    } else {
        v
    }
}

/// 从字符串里抽数字段：`v2.3.1` → [2,3,1]。
fn numbers(text: &str) -> Vec<u64> {
    let mut out = Vec::new();
    let mut cur = String::new();
    for ch in text.chars() {
        if ch.is_ascii_digit() {
            cur.push(ch);
        } else if !cur.is_empty() {
            out.push(cur.parse().unwrap_or(0));
            cur.clear();
        }
    }
    if !cur.is_empty() {
        out.push(cur.parse().unwrap_or(0));
    }
    out
}

/// remote 是否比 local 新（按数字段逐位比较）。
pub fn newer(remote: &str, local: &str) -> bool {
    let a = numbers(remote);
    let b = numbers(local);
    if a.is_empty() {
        return false;
    }
    for i in 0..a.len().max(b.len()) {
        let x = *a.get(i).unwrap_or(&0);
        let y = *b.get(i).unwrap_or(&0);
        if x != y {
            return x > y;
        }
    }
    false
}

/// 查 GitHub 上的最新版本，与本地比。返回给界面的结构。
pub fn check() -> Value {
    let local = local_version();
    match fetch::latest_release() {
        Ok(rel) => json!({
            "ok": true,
            "local": local,
            "remote": rel.tag,
            "newer": newer(&rel.tag, &local),
            "asset": rel.name,
        }),
        Err(error) => json!({ "ok": false, "local": local, "error": error }),
    }
}

/// 覆盖型更新。成功返回 `{version, files, inis, bridgeDir, optiDir}`。
pub fn apply(s: &mut Settings, report: Report) -> Result<Value, String> {
    let local = local_version();
    report("正在查询最新版本…");
    let rel = fetch::latest_release()?;
    if !newer(&rel.tag, &local) {
        return Err(format!("已经是最新版本（{local}）"));
    }
    report(&format!("发现新版本 {}（当前 {local}），开始下载", rel.tag));

    let tmp = fetch::temp_dir("update")?;
    let _cleanup = TempDir(tmp.clone());

    let archive = tmp.join(&rel.name);
    let size = fetch::download_mirrored(&rel.url, &archive, "插件包", report)?;
    report(&format!("已下载 {}，正在解压", crate::fetch::human(size)));

    let extracted = tmp.join("x");
    fetch::extract_archive(&archive, &extracted)?;
    let new_payload = fetch::find_payload_root(&extracted)
        .ok_or_else(|| "下载到的包里没有 payload 目录".to_string())?;
    let new_root = new_payload
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| extracted.clone());

    // 先把「实际在用」的 ini 存到临时目录
    let backup = tmp.join("ini");
    fs::create_dir_all(&backup).map_err(|e| format!("无法创建临时目录：{e}"))?;
    let mut saved: Vec<&'static str> = Vec::new();
    for kind in KINDS {
        if let Some(src) = live_path(kind, s) {
            if src.is_file() && fs::copy(&src, backup.join(backup_name(kind))).is_ok() {
                saved.push(kind);
            }
        }
    }
    report(&format!("已备份 {} 个在用的 ini", saved.len()));

    report("正在覆盖组件文件…");
    let files = copy_tree(&new_payload, &paths::payload_dir())?;
    let exe_dir = paths::exe_dir();
    for name in [
        "Package-Version.txt",
        "upstream-versions.json",
        "README.md",
        "Feedback.txt",
    ] {
        let src = new_root.join(name);
        if src.is_file() {
            let _ = fs::copy(&src, exe_dir.join(name));
        }
    }
    let license = new_root.join("license");
    if license.is_dir() {
        let _ = copy_tree(&license, &exe_dir.join("license"));
    }

    // 版本化组件：切到最新的版本目录，否则更新完跑的还是旧 DLL
    let mut switched = Vec::new();
    if let Some(best) = newest_dir("bridge") {
        if best != s.bridge_dir {
            switched.push(format!("Bridge → {best}"));
            s.bridge_dir = best;
        }
    }
    if let Some(best) = newest_dir("opti") {
        if best != s.opti_dir {
            switched.push(format!("OptiScaler → {best}"));
            s.opti_dir = best;
        }
    }

    report("正在回填 ini 配置…");
    let mut restored: Vec<String> = Vec::new();
    for kind in &saved {
        let src = backup.join(backup_name(kind));
        if let Some(dst) = live_path(kind, s) {
            if let Some(parent) = dst.parent() {
                let _ = fs::create_dir_all(parent);
            }
            if fs::copy(&src, &dst).is_ok() {
                restored.push(dst.display().to_string());
            }
        }
    }

    // 重新写一遍托管键与注入链（新版本目录的路径要落进 fps_config.json）
    if !s.game_dir.is_empty() {
        if let Some(exe) = gamepath::find_game_exe(std::path::Path::new(&s.game_dir)) {
            let cancel = AtomicBool::new(false);
            let selected = pkg::with_mandatory(&s.selected);
            install::apply(&exe, s.fps, &selected, s, &cancel, |_| {});
        }
    }

    let version = paths::package_version();
    crate::log::write(&format!(
        "插件包已更新: {} → {}（覆盖 {} 个文件，回填 {} 个 ini）",
        local,
        version,
        files,
        restored.len()
    ));
    Ok(json!({
        "version": if version.is_empty() { rel.tag.clone() } else { version },
        "files": files,
        "inis": restored,
        "switched": switched,
    }))
}

/// 需要备份/回填的 ini 种类。
const KINDS: &[&str] = &["reshade", "reshade_preset", "bridge", "opti", "texture"];

fn backup_name(kind: &str) -> &'static str {
    match kind {
        "reshade" => "ReShade.ini",
        "reshade_preset" => "ReShadePreset.ini",
        "bridge" => "Bridge.ini",
        "opti" => "OptiScaler.ini",
        _ => "TextureLoader.ini",
    }
}

/// 某种 ini 在「实际使用位置」上的路径。
fn live_path(kind: &str, s: &Settings) -> Option<PathBuf> {
    match kind {
        "reshade" => {
            if s.game_dir.is_empty() {
                None
            } else {
                Some(PathBuf::from(&s.game_dir).join("ReShade.ini"))
            }
        }
        "reshade_preset" => {
            if s.game_dir.is_empty() {
                None
            } else {
                Some(PathBuf::from(&s.game_dir).join("ReShadePreset.ini"))
            }
        }
        "bridge" => Some(pkg::dll_path_of("bridge", s).parent()?.join("Dx11FsrBridge.ini")),
        "opti" => Some(pkg::dll_path_of("opti", s).parent()?.join("OptiScaler.ini")),
        _ => Some(paths::payload_dir().join("TextureLoader").join("TextureLoader.ini")),
    }
}

/// payload 里当前版本号最大的版本目录（按目录名里的数字比）。
fn newest_dir(id: &str) -> Option<String> {
    pkg::versions_of(id)
        .into_iter()
        .max_by(|a, b| numbers(&a.dir).cmp(&numbers(&b.dir)))
        .map(|v| v.dir)
}

/// 递归覆盖复制，返回复制的文件数。
fn copy_tree(src: &Path, dst: &Path) -> Result<usize, String> {
    if src.is_file() {
        if let Some(parent) = dst.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("无法创建目录：{e}"))?;
        }
        fs::copy(src, dst).map_err(|e| format!("覆盖 {} 失败：{e}", dst.display()))?;
        return Ok(1);
    }
    fs::create_dir_all(dst).map_err(|e| format!("无法创建目录：{e}"))?;
    let mut count = 0;
    let entries = fs::read_dir(src).map_err(|e| format!("读取 {} 失败：{e}", src.display()))?;
    for entry in entries.flatten() {
        count += copy_tree(&entry.path(), &dst.join(entry.file_name()))?;
    }
    Ok(count)
}

struct TempDir(PathBuf);

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
