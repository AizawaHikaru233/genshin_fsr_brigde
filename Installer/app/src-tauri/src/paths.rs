use std::path::{Path, PathBuf};

/// 安装器自身所在目录：payload 就放在它旁边。
pub fn exe_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_default()
}

pub fn local_app_data() -> PathBuf {
    std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_default()
}

/// %LOCALAPPDATA%\GenshinFsrBridge —— 与 C++ 版保持同一位置，老配置能直接沿用。
pub fn config_dir() -> PathBuf {
    local_app_data().join("GenshinFsrBridge")
}

pub fn settings_file() -> PathBuf {
    config_dir().join("settings.json")
}

pub fn log_file() -> PathBuf {
    config_dir().join("installer.log")
}

/// 解帧工具就在 payload 里跑：配置写在它旁边，不再往 %LOCALAPPDATA% 复制。
pub fn unlocker_dir() -> PathBuf {
    payload_dir().join("FpsUnlocker")
}

/// payload 目录：优先环境变量，其次 exe 同级、再往上两级（开发时在 target\debug 里）。
/// 返回值会把 `..` 解析掉：这个路径会写进 fps_config.json 的 DllList，
/// 带 `..` 的写法会让「和上次装的是不是同一个路径」判断失效。
pub fn payload_dir() -> PathBuf {
    if let Some(v) = std::env::var_os("GFSR_PAYLOAD") {
        return clean(PathBuf::from(v));
    }
    let exe = exe_dir();
    let candidates = [
        exe.join("payload"),
        exe.join("..").join("payload"),
        exe.join("..").join("..").join("payload"),
        exe.join("..").join("..").join("..").join("payload"),
    ];
    for cand in candidates {
        if looks_like_payload(&cand) {
            return clean(cand);
        }
    }
    clean(exe.join("payload"))
}

/// payload 的判据：有 FpsUnlocker 目录，或有任意 `Bridge*` 版本目录
/// （Bridge 从 2.2.0 起改名成 `Bridge_v<版本>`，不能再硬编码 `Bridge`）。
fn looks_like_payload(dir: &Path) -> bool {
    if dir.join("FpsUnlocker").is_dir() {
        return true;
    }
    match std::fs::read_dir(dir) {
        Ok(entries) => entries.flatten().any(|e| {
            e.file_name()
                .to_string_lossy()
                .to_ascii_lowercase()
                .starts_with("bridge")
        }),
        Err(_) => false,
    }
}

/// 解析 `..` 并把 Windows 的 `\\?\` 前缀去掉（要拿去显示和写 JSON）。
fn clean(path: PathBuf) -> PathBuf {
    match std::fs::canonicalize(&path) {
        Ok(canonical) => {
            let text = canonical.to_string_lossy();
            match text.strip_prefix(r"\\?\") {
                Some(rest) => PathBuf::from(rest),
                None => canonical,
            }
        }
        Err(_) => path,
    }
}

/// 包版本号：包根目录的 Package-Version.txt（与旧脚本包同一约定）。
pub fn package_version() -> String {
    std::fs::read_to_string(exe_dir().join("Package-Version.txt"))
        .map(|text| text.trim().to_string())
        .unwrap_or_default()
}

/// 把路径转成界面能显示、能存进 JSON 的字符串。
pub fn to_display(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

/// 展开 `%VAR%`（ini 里常见 `%TEMP%`）。不认识的变量原样保留，方便用户看出写错了。
pub fn expand_env(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut rest = value;
    while let Some(start) = rest.find('%') {
        out.push_str(&rest[..start]);
        let after = &rest[start + 1..];
        match after.find('%') {
            Some(end) => {
                let name = &after[..end];
                match std::env::var(name) {
                    Ok(val) => out.push_str(&val),
                    Err(_) => {
                        out.push('%');
                        out.push_str(name);
                        out.push('%');
                    }
                }
                rest = &after[end + 1..];
            }
            None => {
                out.push_str(&rest[start..]);
                rest = "";
            }
        }
    }
    out.push_str(rest);
    out
}