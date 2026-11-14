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
use crate::{log, paths, pkg};

use std::fs;

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
    /// GitHub API 给该附件的 sha256 摘要（缺失时调用方必须拒绝安装）。
    pub sha256: Option<String>,
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
    let mut best: Option<(u8, String, String, Option<String>)> = None;
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
        let digest = asset["digest"]
            .as_str()
            .map(|v| v.trim().trim_start_matches("sha256:").to_uppercase())
            .filter(|v| v.len() == 64);
        if best.as_ref().map(|b| asset_score > b.0).unwrap_or(true) {
            best = Some((asset_score, name.to_string(), url.to_string(), digest));
        }
    }
    match best {
        Some((_, name, url, sha256)) => Ok(Release { tag, name, url, sha256 }),
        None => Err("最新 release 里没有可用的附件".to_string()),
    }
}

/// 从 GitHub 最新 release 里挑一个整包（优先 7z，其次 zip）。
fn release_asset() -> Result<Release, String> {
    latest_release()
}

/// 校验从本项目 GitHub release 下载的整包：只认 API 给的 sha256 摘要（缺就失败关闭）。
fn verify_release_archive(rel: &Release, archive: &Path) -> Result<(), String> {
    let expected = rel.sha256.as_ref().ok_or_else(|| {
        "GitHub 没有给该附件提供 SHA-256 摘要，已拒绝安装未校验的压缩包".to_string()
    })?;
    crate::verify::check_sha256(archive, expected, "插件包")
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


/* ── ReShade64.dll 自签名 ── */

/// 给 `ReShade64.dll` 打自签名证书。
///
/// 为什么需要：ReShade 官方发行版的 `ReShade64.dll` 是 **NotSigned**（无 Authenticode 签名）。
/// OptiScaler 在运行期用 `LoadLibraryExW_Ldr` 加载它 —— 反作弊看到"运行期加载一个无签名模块"
/// 会拦截，表现为勾了 ReShade 但游戏起不来。签名后该模块有可验证的链，反作弊放行。
///
/// 为什么不提交带签名的二进制进仓库：证书只在本机受信任根里有效，换机无效；
/// 带签名二进制还会增大仓库体积、且证书有 5 年有效期。
///
/// 失败**不阻断安装**：ReShade 仍可用，只是要走外部注入路径（见 pkg::dll_list）。
pub fn sign_reshade_dll(dll: &Path) -> Result<(), String> {
    if !dll.is_file() {
        return Err("ReShade64.dll 不存在".to_string());
    }

    // 已经签过（签名有效）就直接返回：重复签名没有意义。
    if let Some(true) = has_valid_signature(dll) {
        log::write("[reshade] ReShade64.dll 已有有效签名，跳过");
        return Ok(());
    }

    let signtool = find_signtool().ok_or_else(|| "找不到 signtool.exe（需要 Windows SDK）".to_string())?;
    log::write(&format!("[reshade] 使用 signtool: {}", signtool.display()));

    let thumbprint = ensure_signing_cert()?;
    log::write(&format!("[reshade] 证书指纹: {thumbprint}"));

    let out = hidden_command(&signtool)
        .args(["sign", "/sha1", &thumbprint, "/fd", "SHA256", "/v"])
        .arg(dll)
        .output()
        .map_err(|e| format!("调用 signtool 失败：{e}"))?;

    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let stdout = String::from_utf8_lossy(&out.stdout);
        return Err(format!("signtool 签名失败：{stderr}{stdout}"));
    }

    log::write("[reshade] ReShade64.dll 签名完成");
    Ok(())
}

/// 建命令时不弹黑框（Windows 上不加 CREATE_NO_WINDOW 会闪一个控制台）。
fn hidden_command(program: &Path) -> std::process::Command {
    #[allow(unused_mut)]
    let mut cmd = std::process::Command::new(program);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    cmd
}

/// 找 `signtool.exe`：先看 PATH，再扫 Windows Kits\10\bin\<ver>\x64。
fn find_signtool() -> Option<PathBuf> {
    if let Ok(out) = hidden_command(Path::new("where")).arg("signtool.exe").output() {
        if out.status.success() {
            let text = String::from_utf8_lossy(&out.stdout);
            if let Some(first) = text.lines().next() {
                let p = PathBuf::from(first.trim());
                if p.is_file() {
                    return Some(p);
                }
            }
        }
    }

    let kits = PathBuf::from(
        std::env::var("ProgramFiles(x86)").unwrap_or_else(|_| "C:\\Program Files (x86)".to_string()),
    )
    .join("Windows Kits")
    .join("10")
    .join("bin");
    let mut versions: Vec<PathBuf> = fs::read_dir(&kits)
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    versions.sort();
    versions.reverse();
    versions
        .into_iter()
        .map(|v| v.join("x64").join("signtool.exe"))
        .find(|p| p.is_file())
}

/// 确保「本机自签名代码签名证书」存在，返回指纹。
///
/// **Subject 刻意用本项目自己的标识，不冒充 ReShade 官方作者。**
///
/// 2026-11-14 复审修正：原实现用 `CN=crosire, O=ReShade, ...`（ReShade 作者的身份）。
/// 那会带来两个真实风险：
///   · **身份冒充**：用户证书根里会长期存在一个名为 "crosire / ReShade" 的可信锚，
///     任何用这把私钥签的二进制都被本机当可信 —— 而私钥就留在用户的证书存储里，
///     泄露即可伪造「ReShade 官方签名」。
///   · **用户不可见**：安装器会静默向 `CurrentUser\Root` 写入一个根证书，卸载也不移除。
///
/// 现在用 `CN=GenshinFsrBridge Local Self-Sign`：一眼能看出是本机自签，
/// 不与上游身份混淆；有效期缩到 **1 年**（配合安装器的移除入口，见 `remove_signing_cert`）。
///
/// ⚠️ 注意：换 Subject 后，**旧版本安装器签过的 ReShade64.dll 不会再被 `has_valid_signature`
///   认作同一张证书** —— 那不影响功能（`has_valid_signature` 只看 Status 是否 Valid，
///   不看颁发者），但会遗留一个旧的 "crosire" 根证书。`remove_signing_cert` 会一并清掉。
fn ensure_signing_cert() -> Result<String, String> {
    const SUBJECT: &str = "CN=GenshinFsrBridge Local Self-Sign";
    const VALID_YEARS: i32 = 1;

    // 1) 已有就复用
    let find = format!(
        "$c = Get-ChildItem Cert:\\\\CurrentUser\\\\My | Where-Object {{ $_.Subject -eq '{SUBJECT}' -and $_.HasPrivateKey }} | Sort-Object NotAfter -Descending | Select-Object -First 1; if ($c) {{ $c.Thumbprint }}"
    );
    if let Ok(out) = hidden_command(Path::new("powershell"))
        .args(["-NoProfile", "-NonInteractive", "-Command", find.as_str()])
        .output()
    {
        if out.status.success() {
            let tp = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if !tp.is_empty() {
                return Ok(tp);
            }
        }
    }

    // 2) 没有就建一个，并装进 CurrentUser\\Root
    let create = format!(
        "$c = New-SelfSignedCertificate -Type CodeSigningCert -Subject '{SUBJECT}' -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -CertStoreLocation 'Cert:\\\\CurrentUser\\\\My' -NotAfter (Get-Date).AddYears({VALID_YEARS}); $s = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root','CurrentUser'); $s.Open('ReadWrite'); $s.Add($c); $s.Close(); $c.Thumbprint"
    );
    let out = hidden_command(Path::new("powershell"))
        .args(["-NoProfile", "-NonInteractive", "-Command", create.as_str()])
        .output()
        .map_err(|e| format!("创建证书失败：{e}"))?;

    if !out.status.success() {
        return Err(format!("创建证书失败：{}", String::from_utf8_lossy(&out.stderr)));
    }
    let tp = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if tp.is_empty() {
        return Err("创建证书后没有拿到指纹".to_string());
    }
    Ok(tp)
}

/// 从当前用户的 `My` 与 `Root` 里移除本项目的自签名证书（卸载 / 用户主动清理时调用）。
///
/// 为什么需要：自签名证书会向 `CurrentUser\Root` 写入一个**用户看不见**的可信根，
/// 且**不会随安装器卸载自动消失**。没有移除入口就等于在用户机器上留了一个永久信任锚。
///
/// 一并清掉旧版本用过的 `CN=crosire, O=ReShade ...`（那时冒充了 ReShade 作者身份）。
/// 返回移除的证书数量。
pub fn remove_signing_cert() -> Result<usize, String> {
    const SUBJECTS: [&str; 2] = [
        "CN=GenshinFsrBridge Local Self-Sign",
        "CN=crosire, O=ReShade, L=Boston, S=Massachusetts, C=US",
    ];
    let mut removed = 0usize;
    for subject in SUBJECTS {
        let script = format!(
            "$n = 0; foreach ($store in @('My','Root')) {{ \
               $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store,'CurrentUser'); \
               $s.Open('ReadWrite'); \
               foreach ($c in @($s.Certificates | Where-Object {{ $_.Subject -eq '{subject}' }})) {{ $s.Remove($c); $n++ }}; \
               $s.Close() }}; $n"
        );
        if let Ok(out) = hidden_command(Path::new("powershell"))
            .args(["-NoProfile", "-NonInteractive", "-Command", script.as_str()])
            .output()
        {
            if out.status.success() {
                if let Ok(n) = String::from_utf8_lossy(&out.stdout).trim().parse::<usize>() {
                    removed += n;
                }
            }
        }
    }
    log::write(&format!("[reshade] 已移除 {removed} 个自签名证书"));
    Ok(removed)
}

/// `Get-AuthenticodeSignature` 判断签名状态。
/// `Some(true)` = Valid；`Some(false)` = 有签名但无效；`None` = 查不出来。
///
/// **用 `-LiteralPath` 而不是把路径塞进单引号字符串**：路径里含 `'` 时
/// （例如 `C:\Users\O'Brien\...`）原写法会把 PowerShell 命令截断，得到一个语法错误
/// 而被当成「查不出来」。`-LiteralPath` 走参数传递，不经过 PowerShell 的解析器。
fn has_valid_signature(dll: &Path) -> Option<bool> {
    let cmd = "(Get-AuthenticodeSignature -LiteralPath $env:GFSR_SIGN_TARGET).Status";
    let out = hidden_command(Path::new("powershell"))
        .args(["-NoProfile", "-NonInteractive", "-Command", cmd])
        .env("GFSR_SIGN_TARGET", dll)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let status = String::from_utf8_lossy(&out.stdout).trim().to_string();
    Some(status.eq_ignore_ascii_case("Valid"))
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
        // 期望值全部来自包内清单；清单缺失/下不到预期文件就失败关闭。
        let (setup_url, setup_sha, dll_sha) = crate::manifest::reshade_expectation()?;
        report("正在从 ReShade 官网下载官方安装器…");
        let dir = temp_dir("reshade")?;
        let setup = dir.join("ReShade_Setup.exe");
        download_direct(&setup_url, &setup, " ReShade 官方安装器", report)?;
        crate::verify::check_sha256(&setup, &setup_sha, "ReShade 官方安装器")?;
        report("正在从官方安装器里解出 ReShade64.dll…");
        extract_reshade_dll(&setup, &reshade_dll)?;
        if let Some(expected) = dll_sha.as_deref() {
            crate::verify::check_sha256(&reshade_dll, expected, "ReShade64.dll")?;
        }

        // 2026-11-14：给 ReShade64.dll 打自签名证书。
        // 官方二进制是 NotSigned；OptiScaler 运行期加载无签名模块会被反作弊拦截。
        // 失败不阻断：仍可用外部注入路径（见 pkg::dll_list 的智能判断）。
        report("正在给 ReShade64.dll 签名…");
        match sign_reshade_dll(&reshade_dll) {
            Ok(()) => {
                report("ReShade64.dll 已签名");
            }
            Err(err) => {
                log::write(&format!("[reshade] 签名失败（不影响安装）：{err}"));
                report("签名失败，将使用外部注入方式加载 ReShade");
            }
        }
        // ReShade 官方二进制本身不带 Authenticode 签名，这里只认清单里的 SHA-256。
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
    let rel = release_asset()?;
    let dir = temp_dir("release")?;
    let archive = dir.join(&rel.name);
    download_mirrored(&rel.url, &archive, &format!(" {}", rel.name), report)?;
    verify_release_archive(&rel, &archive)?;
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

/// 把本包内置的 DLSS 运行时复制到目标目录（启动 / 保存 / 安装 / 切版本共用）。
/// 只在 payload 里确实有文件时才动手；没有就让调用方走联网那条路。
pub fn install_bundled(dest: &Path) -> Result<(), String> {
    let bundled = paths::payload_dir().join("NVIDIA").join("DLSS").join("nvngx_dlss.dll");
    if !bundled.is_file() {
        return Err("payload 里没有 NVIDIA\\DLSS\\nvngx_dlss.dll".to_string());
    }
    // 包内文件也要对得上清单（防打包错、防被替换）；清单缺失则不做这项检查。
    if let Ok((_, _, _, Some(expected))) = crate::manifest::dlss_expectation() {
        crate::verify::check_sha256(&bundled, &expected, "内置 nvngx_dlss.dll")?;
    }
    copy_dlss(&bundled, dest)
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

    // 版本与摘要全部锁定在包内清单：不去"取最新"，也不接受没有摘要的下载。
    let (tag, asset, archive_sha, dll_sha) = crate::manifest::dlss_expectation()?;
    let url = format!(
        "https://github.com/NVIDIA-RTX/Streamline/releases/download/{tag}/{asset}"
    );
    let dir = temp_dir("dlss")?;
    let archive = dir.join(&asset);
    download_mirrored(&url, &archive, &format!(" NVIDIA DLSS（{tag}）"), report)?;
    crate::verify::check_sha256(&archive, &archive_sha, "NVIDIA Streamline 压缩包")?;
    report("正在解压 NVIDIA Streamline 组件包…");
    let unpacked = dir.join("unpacked");
    extract_archive(&archive, &unpacked)?;
    let dll = find_nvngx_dlss(&unpacked)
        .ok_or_else(|| "NVIDIA Streamline 官方包中没有找到 nvngx_dlss.dll".to_string())?;
    if let Some(expected) = dll_sha.as_deref() {
        crate::verify::check_sha256(&dll, expected, "nvngx_dlss.dll")?;
    }
    crate::verify::require_authenticode(&dll, "nvngx_dlss.dll")?;
    copy_dlss(&dll, dest)?;
    let _ = fs::remove_dir_all(&dir);
    Ok(format!("Streamline {tag}"))
}

/// OptiScaler 的 DLSSG 帧生成需要 Streamline 运行时。它从**自己 DLL 旁边的 `streamline\`**
/// 加载这几个文件（路径硬编码，见 OptiScaler/proxies/Streamline_Proxy.h:89-103 与 181/215/254）：
///
/// ```text
/// <OptiScaler 组件目录>\streamline\
///   sl.interposer.dll   ← 入口（LoadStreamline 直接 LoadLibraryExW_Ldr 它）
///   sl.common.dll       ← presentCommon 等公共接口
///   nvngx_dlssg.dll     ← DLSS 帧生成本体
///   sl.dlss_g.dll       ← DLSSG 插件
///   sl.reflex.dll       ← Reflex 插件
///   sl.pcl.dll          ← PCL 插件
/// ```
///
/// 缺 `sl.interposer.dll` 时 OptiScaler 报 "Streamline proxy can't find sl.interposer.dll!"，
/// 界面提示 "Can't init DLSSG Output / Are you missing the streamline folder?"
///
/// 这些文件都在 NVIDIA 官方 Streamline SDK 压缩包的 `bin\x64\` 下，与 `nvngx_dlss.dll`
/// 同一个包 —— 所以复用清单里的同一个 URL 与 ArchiveSha256，不额外引入校验项。
///
/// 为什么只认这 6 个：OptiScaler 只读这 6 个路径；多放（比如 sl.dlss.dll / sl.nis.dll）
/// 会让包变大而没有任何用处。
const STREAMLINE_FILES: [&str; 6] = [
    "sl.interposer.dll",
    "sl.common.dll",
    "nvngx_dlssg.dll",
    "sl.dlss_g.dll",
    "sl.reflex.dll",
    "sl.pcl.dll",
];

/// `streamline` 子目录是否已经齐全（任一缺失都要补）。
pub fn streamline_ready(dir: &Path) -> bool {
    STREAMLINE_FILES.iter().all(|name| dir.join(name).is_file())
}

/// 从已解压的 Streamline SDK 里挑出需要的文件复制过去。
///
/// 只在 `bin\x64\` 这一层找（发行版布局）；`development\` 子目录里有同名但不同构建的
/// DLL，**不能**误用（那是 debug/development 版，与发行版 ABI 与依赖都不同）。
fn copy_streamline_from(unpacked: &Path, dest: &Path) -> Result<Vec<String>, String> {
    let bin = unpacked.join("bin").join("x64");
    if !bin.is_dir() {
        return Err("Streamline 压缩包里没有 bin\\x64 目录".to_string());
    }

    fs::create_dir_all(dest).map_err(|e| format!("无法创建 streamline 目录：{e}"))?;

    let mut copied = Vec::new();
    for name in STREAMLINE_FILES {
        let source = bin.join(name);
        if !source.is_file() {
            return Err(format!("Streamline 包里缺少 {name}"));
        }
        let target = dest.join(name);
        fs::copy(&source, &target).map_err(|e| format!("复制 {name} 失败：{e}"))?;
        copied.push(paths::to_display(&target));
    }
    Ok(copied)
}

/// 把 Streamline 运行时放进 `<OptiScaler 组件目录>\streamline\`。
///
/// 先看 payload 有没有现成的（`payload\OptiScaler\streamline\` 或随包同位置），
/// 没有就从 NVIDIA 官方 SDK 取。返回一句来源说明，供日志/界面显示。
pub fn fetch_streamline(dest: &Path, report: Report) -> Result<String, String> {
    // ① 包内已带（发布包可以预置，省一次 263 MB 下载）
    let bundled = paths::payload_dir()
        .join("OptiScaler")
        .join("streamline");
    if streamline_ready(&bundled) {
        report("正在安装现成的 Streamline 组件…");
        fs::create_dir_all(dest).map_err(|e| format!("无法创建 streamline 目录：{e}"))?;
        for name in STREAMLINE_FILES {
            fs::copy(bundled.join(name), dest.join(name))
                .map_err(|e| format!("复制 {name} 失败：{e}"))?;
        }
        return Ok("payload 里已有".to_string());
    }

    // ② 联网取。版本、URL、摘要全部来自清单（与 nvngx_dlss.dll 同一个压缩包）。
    let (tag, asset, archive_sha, _) = crate::manifest::dlss_expectation()?;
    let url =
        format!("https://github.com/NVIDIA-RTX/Streamline/releases/download/{tag}/{asset}");
    let dir = temp_dir("streamline")?;
    let archive = dir.join(&asset);
    // 这个包 ~263 MB，比 nvngx_dlss.dll 大得多；文案要说明在干什么，否则用户以为卡住。
    download_mirrored(
        &url,
        &archive,
        &format!(" NVIDIA Streamline 运行时（{tag}，用于 DLSS 帧生成）"),
        report,
    )?;
    crate::verify::check_sha256(&archive, &archive_sha, "NVIDIA Streamline 压缩包")?;
    report("正在解压 Streamline 组件包…");
    let unpacked = dir.join("unpacked");
    extract_archive(&archive, &unpacked)?;
    copy_streamline_from(&unpacked, dest)?;
    let _ = fs::remove_dir_all(&dir);
    Ok(format!("Streamline {tag}"))
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
