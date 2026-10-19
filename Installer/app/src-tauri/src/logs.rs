//! 组件日志的定位与打包。
//!
//! 四个来源（都在管理器自己的目录树里，不需要进游戏目录找）：
//! - 安装器    %LOCALAPPDATA%\GenshinFsrBridge\installer.log（含 .old.log 轮转）
//! - Bridge    当前启用版本目录里的 Dx11FsrBridge.log（含 .1/.2 轮转）
//! - OptiScaler 当前启用版本目录里的 OptiScaler.log（SingleFile 关闭时会是 OptiScaler_<ticks>.log）
//! - TextureLoader payload\TextureLoader\TextureLoader.log 与 TextureLoader.crash.log
//!
//! 日志文件名与目录取自各组件的实现：
//! - `Dx11FsrBridge.cpp` 里 `log_cfg.directory_w = g_module_dir`，
//! - `TextureLoader\log.cpp``log_init(dll_dir)` 写 `<dll_dir>\TextureLoader.log`，
//! - OptiScaler `Config.cpp` 里相对路径的 LogFileName 落在 `DllPath().parent_path()`。

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};

use crate::paths;
use crate::pkg;
use crate::settings::Settings;

pub struct Source {
    pub id: &'static str,
    pub name: &'static str,
    pub dir: PathBuf,
    pub prefix: &'static str,
}

/// 四个日志来源；版本化组件取当前启用的那个目录。
pub fn sources(s: &Settings) -> Vec<Source> {
    let mut out = vec![Source {
        id: "installer",
        name: "安装器",
        dir: paths::config_dir(),
        prefix: "installer",
    }];
    if let Some(v) = pkg::resolve("bridge", s) {
        out.push(Source {
            id: "bridge",
            name: "Dx11FsrBridge",
            dir: v.path,
            prefix: "Dx11FsrBridge",
        });
    }
    if let Some(v) = pkg::resolve("opti", s) {
        out.push(Source {
            id: "opti",
            name: "OptiScaler",
            dir: v.path,
            prefix: "OptiScaler",
        });
    }
    out.push(Source {
        id: "texture",
        name: "TextureLoader",
        dir: paths::payload_dir().join("TextureLoader"),
        prefix: "TextureLoader",
    });
    out
}

/// 某个来源下所有日志文件，新的在前。
fn files_of(src: &Source) -> Vec<PathBuf> {
    let mut files: Vec<PathBuf> = Vec::new();
    if let Ok(rd) = fs::read_dir(&src.dir) {
        for entry in rd.flatten() {
            let path = entry.path();
            if !path.is_file() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if !name.starts_with(&src.prefix.to_ascii_lowercase()) || !name.contains(".log") {
                continue;
            }
            files.push(path);
        }
    }
    let primary = format!("{}.log", src.prefix.to_ascii_lowercase());
    let mtime = |p: &PathBuf| {
        fs::metadata(p)
            .and_then(|m| m.modified())
            .unwrap_or(UNIX_EPOCH)
    };
    // 主文件（Dx11FsrBridge.log / TextureLoader.log）排在最前，
    // 轮转文件与 crash 日志只是附带——否则「打开」会命中轮转出来的旧文件。
    files.sort_by(|a, b| {
        let pa = a.file_name().map(|n| n.to_string_lossy().to_ascii_lowercase()) == Some(primary.clone());
        let pb = b.file_name().map(|n| n.to_string_lossy().to_ascii_lowercase()) == Some(primary.clone());
        pb.cmp(&pa).then_with(|| mtime(b).cmp(&mtime(a)))
    });
    files
}

/// 发给界面的日志清单。
pub fn info(s: &Settings) -> Value {
    let mut list = Vec::new();
    for src in sources(s) {
        let files = files_of(&src);
        let newest = files.first();
        let meta = newest.and_then(|p| fs::metadata(p).ok());
        let bytes = meta.as_ref().map(|m| m.len()).unwrap_or(0);
        let stamp = meta
            .as_ref()
            .and_then(|m| m.modified().ok())
            .map(fmt_stamp)
            .unwrap_or_default();
        list.push(json!({
            "id": src.id,
            "name": src.name,
            "dir": paths::to_display(&src.dir),
            "file": newest.map(|p| paths::to_display(p)).unwrap_or_default(),
            "exists": newest.is_some(),
            "bytes": bytes,
            "time": stamp,
            "count": files.len(),
        }));
    }
    json!(list)
}

/// 某个来源最新一份日志（打开用）。
pub fn newest_file(id: &str, s: &Settings) -> Option<PathBuf> {
    sources(s)
        .into_iter()
        .find(|src| src.id == id)
        .and_then(|src| files_of(&src).into_iter().next())
}

/// 某个来源所在目录（没有日志时也能打开目录）。
pub fn source_dir(id: &str, s: &Settings) -> Option<PathBuf> {
    sources(s).into_iter().find(|src| src.id == id).map(|src| src.dir)
}

/// 把所有日志打成一个 zip。entries 里是 `来源id/文件名`。
pub fn bundle(dest: &Path, s: &Settings) -> Result<(usize, Vec<String>), String> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("无法创建目录：{}", e))?;
    }
    let file = fs::File::create(dest).map_err(|e| format!("无法创建压缩包：{}", e))?;
    let mut zip = zip::ZipWriter::new(file);
    let options =
        zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    let mut count = 0usize;
    let mut names = Vec::new();
    for src in sources(s) {
        for path in files_of(&src) {
            let name = path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| "log".to_string());
            if let Ok(text) = fs::read(&path) {
                let entry = format!("{}/{}", src.id, name);
                zip.start_file(entry.clone(), options)
                    .map_err(|e| format!("写入压缩包失败：{}", e))?;
                zip.write_all(&text)
                    .map_err(|e| format!("写入压缩包失败：{}", e))?;
                count += 1;
                names.push(entry);
            }
        }
    }
    zip.finish().map_err(|e| format!("收尾压缩包失败：{}", e))?;
    if count == 0 {
        let _ = fs::remove_file(dest);
        return Err("这四个组件都还没有生成日志".to_string());
    }
    Ok((count, names))
}

/// 打包时的默认文件名：`GenshinFSRBridge-logs-1019-004233.zip`。
pub fn archive_name() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
        + 8 * 3600;
    let days = secs.div_euclid(86_400);
    let tod = secs.rem_euclid(86_400);
    let (month, day) = civil_from_days(days);
    format!(
        "GenshinFSRBridge-logs-{:02}{:02}-{:02}{:02}{:02}.zip",
        month,
        day,
        tod / 3600,
        (tod % 3600) / 60,
        tod % 60
    )
}

/// `MM-DD HH:MM`（本地时间按东八区算，与日志行同源）。
fn fmt_stamp(time: SystemTime) -> String {
    let secs = time
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
        + 8 * 3600;
    let days = secs.div_euclid(86_400);
    let tod = secs.rem_euclid(86_400);
    let (month, day) = civil_from_days(days);
    format!(
        "{:02}-{:02} {:02}:{:02}",
        month,
        day,
        tod / 3600,
        (tod % 3600) / 60
    )
}

/// 1970-01-01 起的天数 → (月, 日)，Howard Hinnant 的算法，省掉 chrono。
fn civil_from_days(days: i64) -> (i64, i64) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let _ = y;
    (m, d)
}
