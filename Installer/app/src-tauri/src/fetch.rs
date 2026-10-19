//! 组件缺失时联网补齐：ReShade 走官网安装器，其余从本项目的 GitHub Releases 取整包。
//!
//! - 跟随系统代理：读 HKCU 的 Internet Settings（ProxyEnable / ProxyServer / ProxyOverride），
//!   与浏览器、.NET 用的是同一套设置（本机 WinHTTP 是直连，但 IE 代理是开着的，
//!   所以不能走 WinHTTP 的「自动代理」）。
//! - GitHub 走旧脚本那套加速前缀，下载前先测速挑最快的一条。

use std::io::{Cursor, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::Value;
use winreg::enums::HKEY_CURRENT_USER;
use winreg::RegKey;

use crate::settings::Settings;
use crate::{paths, pkg};

use std::fs;

const RESHADE_SETUP_URL: &str = "https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe";
const RELEASE_API: &str =
    "https://api.github.com/repos/AizawaHikaru233/genshin_fsr_brigde/releases/latest";
/// 与旧脚本 Configure.ps1 相同的候选线路：直连 + 三个 GitHub 加速前缀。
const MIRRORS: &[&str] = &["", "https://ghfast.top/", "https://gh-proxy.com/", "https://ghproxy.net/"];
const UA: &str = "GenshinFSRBridgeInstaller";

pub type Report<'a> = &'a dyn Fn(&str);

/// 统一的字节数显示（下载进度、日志都用它）。
pub fn human(size: u64) -> String {
    if size >= 1024 * 1024 {
        format!("{:.1} MB", size as f64 / 1048576.0)
    } else if size >= 1024 {
        format!("{} KB", size / 1024)
    } else {
        format!("{size} B")
    }
}

/* ── 系统代理 ── */

#[derive(Default, Clone)]
pub struct Proxy {
    /// 形如 `192.168.31.6:7890`；None 表示系统未启用固定代理。
    pub server: Option<String>,
    pub bypass: Vec<String>,
}

fn reg_string(key: &RegKey, name: &str) -> String {
    key.get_value::<String, _>(name).unwrap_or_default()
}

/// 读当前用户的 WinINet 代理设置（和浏览器同一份）。
pub fn system_proxy() -> Proxy {
    let Ok(key) = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey("Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings")
    else {
        return Proxy::default();
    };
    let enabled: u32 = key.get_value("ProxyEnable").unwrap_or(0);
    if enabled == 0 {
        return Proxy::default();
    }
    let raw = reg_string(&key, "ProxyServer");
    let raw = raw.trim();
    if raw.is_empty() {
        return Proxy::default();
    }
    // 可能是 "host:port"，也可能是 "http=host:port;https=host:port"
    let picked = if raw.contains('=') {
        let mut chosen: Option<String> = None;
        for part in raw.split(';') {
            let Some((scheme, value)) = part.split_once('=') else {
                continue;
            };
            if scheme.trim().eq_ignore_ascii_case("https") || chosen.is_none() {
                chosen = Some(value.trim().to_string());
                if scheme.trim().eq_ignore_ascii_case("https") {
                    break;
                }
            }
        }
        chosen
    } else {
        Some(raw.to_string())
    };
    let bypass = reg_string(&key, "ProxyOverride")
        .split(';')
        .map(str::trim)
        .filter(|item| !item.is_empty() && *item != "<local>")
        .map(|item| item.to_string())
        .collect();
    Proxy {
        server: picked.filter(|value| !value.is_empty()),
        bypass,
    }
}

fn host_of(url: &str) -> String {
    url.split("://")
        .nth(1)
        .unwrap_or(url)
        .split(['/', '?'])
        .next()
        .unwrap_or("")
        .split('@')
        .last()
        .unwrap_or("")
        .split(':')
        .next()
        .unwrap_or("")
        .to_lowercase()
}

fn pattern_matches(pattern: &str, host: &str) -> bool {
    let pattern = pattern.trim().to_lowercase();
    if pattern.is_empty() {
        return false;
    }
    if pattern == "<local>" {
        return !host.contains('.');
    }
    if let Some(suffix) = pattern.strip_prefix('*') {
        return host.ends_with(suffix);
    }
    if let Some(prefix) = pattern.strip_suffix('*') {
        return host.starts_with(prefix);
    }
    pattern == host
}

fn proxy_for(url: &str) -> Option<String> {
    let proxy = system_proxy();
    let server = proxy.server?;
    let host = host_of(url);
    if proxy.bypass.iter().any(|item| pattern_matches(item, &host)) {
        return None;
    }
    Some(if server.contains("://") {
        server
    } else {
        format!("http://{server}")
    })
}

/* ── ureq ── */

fn agent_with(url: &str, connect: u64, read: u64) -> Result<ureq::Agent, String> {
    let connector = ureq::native_tls::TlsConnector::new().map_err(|e| format!("初始化 TLS 失败：{e}"))?;
    let mut builder = ureq::AgentBuilder::new()
        .tls_connector(Arc::new(connector))
        .user_agent(UA)
        .timeout_connect(Duration::from_secs(connect))
        .timeout_read(Duration::from_secs(read))
        .timeout_write(Duration::from_secs(30))
        .timeout(Duration::from_secs(1800));
    if let Some(server) = proxy_for(url) {
        let proxy = ureq::Proxy::new(&server).map_err(|e| format!("代理地址无效（{server}）：{e}"))?;
        builder = builder.proxy(proxy);
    }
    Ok(builder.build())
}

fn agent(url: &str) -> Result<ureq::Agent, String> {
    agent_with(url, 20, 60)
}

/// 下载到 dest，按已写字节数回报进度。
fn http_download(url: &str, dest: &Path, label: &str, report: Report) -> Result<u64, String> {
    http_download_with(url, dest, label, report, 20, 60, None)
}

/// slow_guard：(至少要有多少字节, 多少时间内) —— 期限内没到这个量就判这条线路太慢。
fn http_download_with(
    url: &str,
    dest: &Path,
    label: &str,
    report: Report,
    connect: u64,
    read: u64,
    slow_guard: Option<(u64, Duration)>,
) -> Result<u64, String> {
    let started = Instant::now();
    let resp = agent_with(url, connect, read)?
        .get(url)
        .call()
        .map_err(|e| format!("请求失败：{e}"))?;
    let total: Option<u64> = resp
        .header("Content-Length")
        .and_then(|value| value.parse().ok());
    let mut reader = resp.into_reader();
    let mut file = fs::File::create(dest).map_err(|e| format!("无法写入临时文件：{e}"))?;
    let mut buffer = vec![0u8; 256 * 1024];
    let mut done = 0u64;
    let mut next_report = Instant::now();
    loop {
        let read_bytes = match reader.read(&mut buffer) {
            Ok(n) => n,
            Err(e) => return Err(format!("下载中断（{}）：{e}", human(done))),
        };
        if read_bytes == 0 {
            break;
        }
        file.write_all(&buffer[..read_bytes])
            .map_err(|e| format!("写入临时文件失败：{e}"))?;
        done += read_bytes as u64;
        if let Some((min_bytes, limit)) = slow_guard {
            if done < min_bytes && started.elapsed() > limit {
                return Err(format!(
                    "线路太慢（{} 秒只拿到 {}）",
                    limit.as_secs(),
                    human(done)
                ));
            }
        }
        if Instant::now() >= next_report {
            next_report = Instant::now() + Duration::from_millis(400);
            match total {
                Some(total) if total > 0 => report(&format!(
                    "正在下载{label}… {} / {}",
                    human(done),
                    human(total)
                )),
                _ => report(&format!("正在下载{label}… {}", human(done))),
            }
        }
    }
    Ok(done)
}

fn http_text(url: &str) -> Result<String, String> {
    let resp = agent(url)?
        .get(url)
        .call()
        .map_err(|e| format!("请求失败：{e}"))?;
    resp.into_string().map_err(|e| format!("读取响应失败：{e}"))
}

/// 拉一小段量带宽，返回字节/秒。探测用短超时，避免某条线路挂住拖慢选择。
fn probe_speed(url: &str) -> u64 {
    let Ok(agent) = agent_with(url, 4, 3) else { return 0 };
    let started = Instant::now();
    let Ok(resp) = agent
        .get(url)
        .set("Range", "bytes=0-524287")
        .call()
    else {
        return 0;
    };
    let mut reader = resp.into_reader();
    let mut buffer = vec![0u8; 64 * 1024];
    let mut done = 0u64;
    while started.elapsed() < Duration::from_secs(3) && done < 512 * 1024 {
        match reader.read(&mut buffer) {
            Ok(0) => break,
            Ok(n) => done += n as u64,
            Err(_) => break,
        }
    }
    let seconds = started.elapsed().as_secs_f64().max(0.05);
    (done as f64 / seconds) as u64
}

/// 四条线路并行测速（串行要十几秒，太像卡住了），按速度从快到慢排。
fn rank_mirrors(url: &str) -> Vec<(u64, &'static str)> {
    let mut results: Vec<(u64, &'static str)> = Vec::new();
    std::thread::scope(|scope| {
        let handles: Vec<_> = MIRRORS
            .iter()
            .map(|prefix| {
                let full = format!("{prefix}{url}");
                let prefix = *prefix;
                scope.spawn(move || (probe_speed(&full), prefix))
            })
            .collect();
        for handle in handles {
            results.push(handle.join().unwrap_or((0, "")));
        }
    });
    results.sort_by(|a, b| b.0.cmp(&a.0));
    results
}

/* ── 线路选择 ── */

fn download_direct(url: &str, dest: &Path, label: &str, report: Report) -> Result<u64, String> {
    http_download(url, dest, label, report)
}

/// 先测速挑最快的线路，失败就顺着速度往下换。
pub fn download_mirrored(url: &str, dest: &Path, label: &str, report: Report) -> Result<u64, String> {
    report("正在测速选择下载线路…");
    let ranked = rank_mirrors(url);
    match ranked.first() {
        Some((speed, prefix)) if *speed > 0 => {
            let name = if prefix.is_empty() { "直连" } else { prefix.trim_end_matches('/') };
            report(&format!("走 {name}（{}/s）", human(*speed)));
        }
        _ => report("线路都没测通，先直接试一次…"),
    }
    let mut last = String::from("没有可用线路");
    for (_, prefix) in ranked {
        let full = format!("{prefix}{url}");
        let guard = Some((2 * 1024 * 1024, Duration::from_secs(30)));
        match http_download_with(&full, dest, label, report, 15, 20, guard) {
            Ok(size) if size > 0 => return Ok(size),
            Ok(_) => last = format!("{full}: 服务器返回空文件"),
            Err(err) => last = format!("{full}: {err}"),
        }
    }
    Err(format!("下载失败（{last}）"))
}

/* ── GitHub release ── */

fn rank(name: &str) -> u8 {
    let lower = name.to_lowercase();
    if lower.ends_with(".7z") {
        2
    } else if lower.ends_with(".zip") {
        1
    } else {
        0
    }
}

/// 最新 release 的版本号与整包附件。
pub struct Release {
    pub tag: String,
    pub name: String,
    pub url: String,
}

/// 查 GitHub 最新 release：版本号 + 优先 7z 的整包地址。走系统代理与加速线路。
pub fn latest_release() -> Result<Release, String> {
    latest_release_matching(RELEASE_API, &rank)
}

/// 查任意仓库的最新 release，用 `score` 给附件打分（0 = 不要）。
fn latest_release_matching(api: &str, score: &dyn Fn(&str) -> u8) -> Result<Release, String> {
    let mut last = String::from("没有可用的线路");
    let mut body = String::new();
    for prefix in MIRRORS {
        match http_text(&format!("{prefix}{api}")) {
            Ok(text) if !text.trim().is_empty() => {
                body = text;
                break;
            }
            Ok(_) => last = format!("{prefix}{api}: 返回空内容"),
            Err(err) => last = format!("{prefix}{api}: {err}"),
        }
    }
    if body.is_empty() {
        return Err(format!("查询 GitHub Releases 失败（{last}）"));
    }
    if !body.trim_start().starts_with('{') {
        return Err("GitHub Releases 返回的不是 JSON（可能被限流）".to_string());
    }
    let json: Value = serde_json::from_str(&body).map_err(|e| format!("解析 Releases 响应失败：{e}"))?;
    let tag = json["tag_name"]
        .as_str()
        .or_else(|| json["name"].as_str())
        .unwrap_or_default()
        .to_string();
    let assets = json["assets"]
        .as_array()
        .ok_or_else(|| "最新 release 里没有附件".to_string())?;
    let mut best: Option<(u8, String, String)> = None;
    for asset in assets {
        let (Some(name), Some(url)) = (
            asset["name"].as_str(),
            asset["browser_download_url"].as_str(),
        ) else {
            continue;
        };
        let asset_score = score(name);
        if asset_score == 0 {
            continue;
        }
        if best.as_ref().map(|b| asset_score > b.0).unwrap_or(true) {
            best = Some((asset_score, name.to_string(), url.to_string()));
        }
    }
    match best {
        Some((_, name, url)) => Ok(Release { tag, name, url }),
        None => Err("最新 release 里没有可用的附件".to_string()),
    }
}

/// 从 GitHub 最新 release 里挑一个整包（优先 7z，其次 zip）。
fn release_asset() -> Result<(String, String), String> {
    latest_release().map(|r| (r.url, r.name))
}

/* ── 解压 ── */

pub fn extract_archive(archive: &Path, dest: &Path) -> Result<(), String> {
    fs::create_dir_all(dest).map_err(|e| format!("无法创建临时目录：{e}"))?;
    let name = archive
        .file_name()
        .map(|s| s.to_string_lossy().to_lowercase())
        .unwrap_or_default();
    if name.ends_with(".7z") {
        sevenz_rust::decompress_file(archive, dest).map_err(|e| format!("解压 7z 失败：{e}"))
    } else if name.ends_with(".zip") {
        let file = fs::File::open(archive).map_err(|e| format!("打不开压缩包：{e}"))?;
        let mut zip = zip::ZipArchive::new(file).map_err(|e| format!("解压 zip 失败：{e}"))?;
        zip.extract(dest).map_err(|e| format!("解压 zip 失败：{e}"))
    } else {
        Err("不支持的压缩格式".to_string())
    }
}

/// 官方 ReShade 安装器是 PE 里挂了 zip；按 512 对齐扫到 PK\x03\x04 再当 zip 读。
fn extract_reshade_dll(setup: &Path, dest: &Path) -> Result<(), String> {
    let bytes = fs::read(setup).map_err(|e| format!("读取 ReShade 安装器失败：{e}"))?;
    let mut offset = None;
    let mut pos = 0usize;
    while pos + 4 <= bytes.len() {
        if bytes[pos] == b'P' && bytes[pos + 1] == b'K' && bytes[pos + 2] == 3 && bytes[pos + 3] == 4 {
            offset = Some(pos);
            break;
        }
        pos += 512;
    }
    let offset = offset.ok_or_else(|| "官方安装器里没找到内嵌压缩包".to_string())?;
    let mut archive = zip::ZipArchive::new(Cursor::new(&bytes[offset..]))
        .map_err(|e| format!("解压官方安装器失败：{e}"))?;
    let mut entry = archive
        .by_name("ReShade64.dll")
        .map_err(|_| "官方安装器里没有 ReShade64.dll".to_string())?;
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("无法创建目录：{e}"))?;
    }
    let mut out = fs::File::create(dest).map_err(|e| format!("无法写入 ReShade64.dll：{e}"))?;
    std::io::copy(&mut entry, &mut out).map_err(|e| format!("解出 ReShade64.dll 失败：{e}"))?;
    Ok(())
}

/* ── 组件补齐 ── */

fn payload_prefixes(id: &str) -> Vec<&'static str> {
    match id {
        "fpsun" => vec!["FpsUnlocker"],
        "bridge" => vec!["Bridge", "AMD"],
        "opti" => vec!["OptiScaler"],
        "reshade" => vec!["ReShade"],
        "texture" => vec!["TextureLoader"],
        "mosaic" => vec!["AntiPlayerMosaic"],
        _ => Vec::new(),
    }
}

/// 在解压结果里找 `payload` 目录（允许套一层包装目录）。
pub fn find_payload_root(dir: &Path) -> Option<PathBuf> {
    if !dir.is_dir() {
        return None;
    }
    let direct = dir.join("payload");
    if direct.is_dir() {
        return Some(direct);
    }
    for entry in fs::read_dir(dir).ok()?.flatten() {
        let path = entry.path();
        if path.is_dir() {
            let nested = path.join("payload");
            if nested.is_dir() {
                return Some(nested);
            }
        }
    }
    None
}

fn copy_missing(src: &Path, dst: &Path, copied: &mut Vec<String>) -> Result<(), String> {
    if src.is_file() {
        if dst.exists() {
            return Ok(());
        }
        if let Some(parent) = dst.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("无法创建目录：{e}"))?;
        }
        fs::copy(src, dst).map_err(|e| format!("复制 {} 失败：{e}", src.display()))?;
        copied.push(dst.display().to_string());
        return Ok(());
    }
    if !src.is_dir() {
        return Ok(());
    }
    fs::create_dir_all(dst).map_err(|e| format!("无法创建目录：{e}"))?;
    for entry in fs::read_dir(src).map_err(|e| format!("读取 {} 失败：{e}", src.display()))? {
        let entry = entry.map_err(|e| e.to_string())?;
        copy_missing(&entry.path(), &dst.join(entry.file_name()), copied)?;
    }
    Ok(())
}

/// 把整包里匹配本组件的目录（`Bridge` / `Bridge_v2.3.1` 等）与 default_config 补进 payload。
fn copy_component_from(
    archive_payload: &Path,
    id: &str,
    copied: &mut Vec<String>,
) -> Result<(), String> {
    let payload = paths::payload_dir();
    let prefixes = payload_prefixes(id);
    let entries = fs::read_dir(archive_payload).map_err(|e| format!("读取 payload 失败：{e}"))?;
    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();
        let matched = prefixes.iter().any(|prefix| {
            let lower = name.to_lowercase();
            let prefix = prefix.to_lowercase();
            lower == prefix || lower.starts_with(&format!("{prefix}_"))
        });
        if matched && path.is_dir() {
            copy_missing(&path, &payload.join(&name), copied)?;
        }
    }
    let templates = archive_payload.join("default_config");
    if templates.is_dir() {
        copy_missing(&templates, &payload.join("default_config"), copied)?;
    }
    Ok(())
}

pub fn temp_dir(tag: &str) -> Result<PathBuf, String> {
    let dir = std::env::temp_dir().join(format!("gfsr-fetch-{tag}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).map_err(|e| format!("无法创建临时目录：{e}"))?;
    Ok(dir)
}

/// 补齐某个组件在 payload 里缺失的文件，返回补了哪些文件。
pub fn fetch_component(id: &str, settings: &Settings, report: Report) -> Result<Vec<String>, String> {
    let mut copied = Vec::new();
    let payload = paths::payload_dir();
    let reshade_dll = payload.join("ReShade\\ReShade64.dll");
    if id == "reshade" && !reshade_dll.is_file() {
        report("正在从 ReShade 官网下载官方安装器…");
        let dir = temp_dir("reshade")?;
        let setup = dir.join("ReShade_Setup.exe");
        download_direct(RESHADE_SETUP_URL, &setup, " ReShade 官方安装器", report)?;
        report("正在从官方安装器里解出 ReShade64.dll…");
        extract_reshade_dll(&setup, &reshade_dll)?;
        copied.push(reshade_dll.display().to_string());
        let _ = fs::remove_dir_all(&dir);
    }

    // 还缺的其它文件（以及 ReShade 的 shaders 之类）从本项目的 GitHub 整包补
    let still_missing: Vec<String> = pkg::missing_files(&[id.to_string()], settings)
        .into_iter()
        .filter(|line| line.starts_with(&format!("{id}:")))
        .collect();
    if still_missing.is_empty() {
        return Ok(copied);
    }

    report("正在查询 GitHub Releases 里的最新整包…");
    let (url, name) = release_asset()?;
    let dir = temp_dir("release")?;
    let archive = dir.join(&name);
    download_mirrored(&url, &archive, &format!(" {name}"), report)?;
    report("正在解压…");
    let unpacked = dir.join("unpacked");
    extract_archive(&archive, &unpacked)?;
    let root =
        find_payload_root(&unpacked).ok_or_else(|| "整包里没有找到 payload 目录".to_string())?;
    report("正在补进管理器目录…");
    copy_component_from(&root, id, &mut copied)?;
    let _ = fs::remove_dir_all(&dir);
    Ok(copied)
}

/* ── NVIDIA DLSS 运行时（RTX 显卡 + OptiScaler 才需要） ── */

const STREAMLINE_API: &str = "https://api.github.com/repos/NVIDIA-RTX/Streamline/releases/latest";

/// 递归收集目录下所有 `nvngx_dlss.dll`，跳过 development 目录、优先 `bin\x64\` 下的生产版。
fn find_nvngx_dlss(root: &Path) -> Option<PathBuf> {
    fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
        let Ok(entries) = fs::read_dir(dir) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                let name = path
                    .file_name()
                    .map(|s| s.to_string_lossy().to_lowercase())
                    .unwrap_or_default();
                if name == "development" {
                    continue;
                }
                walk(&path, out);
            } else if path
                .file_name()
                .map(|s| s.to_string_lossy().eq_ignore_ascii_case("nvngx_dlss.dll"))
                .unwrap_or(false)
            {
                out.push(path);
            }
        }
    }
    let mut found = Vec::new();
    walk(root, &mut found);
    found.sort_by_key(|p| {
        let lower = p.to_string_lossy().to_lowercase().replace('/', "\\");
        let production = !lower.ends_with("\\bin\\x64\\nvngx_dlss.dll");
        (production, lower)
    });
    found.into_iter().next()
}

fn copy_dlss(src: &Path, dest: &Path) -> Result<(), String> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("无法创建目录：{e}"))?;
    }
    fs::copy(src, dest).map_err(|e| format!("复制 nvngx_dlss.dll 失败：{e}"))?;
    let license = src.with_file_name("nvngx_dlss.license.txt");
    if license.is_file() {
        let _ = fs::copy(&license, dest.with_file_name("nvngx_dlss.license.txt"));
    }
    Ok(())
}

/// 把 nvngx_dlss.dll 放进 OptiScaler 组件目录（ini 的 `NvngxDlssPath = auto` 相对它解析）。
/// 先看 payload 里有没有现成的 DLSS 组件，没有就从 NVIDIA 官方 Streamline 取。
pub fn fetch_dlss(dest: &Path, report: Report) -> Result<String, String> {
    let bundled = paths::payload_dir()
        .join("NVIDIA")
        .join("DLSS")
        .join("nvngx_dlss.dll");
    if bundled.is_file() {
        report("正在安装现成的 NVIDIA DLSS 组件…");
        copy_dlss(&bundled, dest)?;
        return Ok("payload 里已有".to_string());
    }

    report("正在查询 NVIDIA Streamline 最新版本…");
    let release = latest_release_matching(STREAMLINE_API, &|name| {
        let lower = name.to_lowercase();
        if lower.starts_with("streamline-sdk-v") && lower.ends_with(".zip") {
            1
        } else {
            0
        }
    })?;
    let dir = temp_dir("dlss")?;
    let archive = dir.join(&release.name);
    download_mirrored(
        &release.url,
        &archive,
        &format!(" NVIDIA DLSS（{}）", release.tag),
        report,
    )?;
    report("正在解压 NVIDIA Streamline 组件包…");
    let unpacked = dir.join("unpacked");
    extract_archive(&archive, &unpacked)?;
    let dll = find_nvngx_dlss(&unpacked)
        .ok_or_else(|| "NVIDIA Streamline 官方包中没有找到 nvngx_dlss.dll".to_string())?;
    copy_dlss(&dll, dest)?;
    let _ = fs::remove_dir_all(&dir);
    Ok(format!("Streamline {}", release.tag))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn count_files(dir: &Path) -> usize {
        let mut total = 0;
        if let Ok(entries) = fs::read_dir(dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    total += count_files(&path);
                } else {
                    total += 1;
                }
            }
        }
        total
    }

    #[test]
    fn print_system_proxy() {
        let proxy = system_proxy();
        println!("[proxy] server={:?} bypass={:?}", proxy.server, proxy.bypass);
        println!("[proxy] github → {:?}", proxy_for("https://github.com/x"));
        println!("[proxy] local → {:?}", proxy_for("http://127.0.0.1:9222/json"));
    }

    /// 用 env GFSR_TEST_URL 指向一个 URL，验证下载链路。
    #[test]
    fn download_probe() {
        let Ok(url) = std::env::var("GFSR_TEST_URL") else {
            return;
        };
        let dest = std::env::temp_dir().join("gfsr-download-probe.bin");
        let _ = fs::remove_file(&dest);
        let report = |text: &str| println!("[probe] {text}");
        let started = Instant::now();
        let size = download_direct(&url, &dest, " probe", &report).unwrap();
        println!(
            "[probe] 完成 {} 用时 {:.1}s（{}/s）",
            human(size),
            started.elapsed().as_secs_f64(),
            human((size as f64 / started.elapsed().as_secs_f64().max(0.05)) as u64)
        );
        assert!(size > 1024);
        let _ = fs::remove_file(&dest);
    }

    /// 用 env GFSR_TEST_DLSS=1 打开：验证 NVIDIA DLSS 运行时的获取链路。
    #[test]
    fn dlss_probe() {
        if std::env::var("GFSR_TEST_DLSS").is_err() {
            return;
        }
        let out = std::env::temp_dir().join("gfsr-dlss-test");
        let _ = fs::remove_dir_all(&out);
        fs::create_dir_all(&out).unwrap();
        let dest = out.join("nvngx_dlss.dll");
        let report = |text: &str| println!("[dlss] {text}");
        let started = Instant::now();
        let source = fetch_dlss(&dest, &report).unwrap();
        let size = fs::metadata(&dest).unwrap().len();
        println!(
            "[dlss] 来源 {source}，{} 用时 {:.1}s",
            human(size),
            started.elapsed().as_secs_f64()
        );
        assert!(size > 1024 * 1024, "nvngx_dlss.dll 太小：{size}");
        let _ = fs::remove_dir_all(&out);
    }

    /// 用 env GFSR_TEST_7Z 指向一个真实的 7z 包，验证 7z 解压链路。
    #[test]
    fn decompress_local_7z() {
        let Ok(path) = std::env::var("GFSR_TEST_7Z") else {
            return;
        };
        let out = std::env::temp_dir().join("gfsr-7z-test");
        let _ = fs::remove_dir_all(&out);
        fs::create_dir_all(&out).unwrap();
        let started = Instant::now();
        extract_archive(Path::new(&path), &out).unwrap();
        let files = count_files(&out);
        println!("[7z] {} 个文件，用时 {:.1}s", files, started.elapsed().as_secs_f64());
        assert!(files > 5, "解压出的文件太少：{files}");
        let _ = fs::remove_dir_all(&out);
    }
}
