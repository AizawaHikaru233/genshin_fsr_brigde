//! 包内固定版本清单 ``upstream-versions.json``：下载物校验的**唯一期望值来源**。
//!
//! 清单随包发布（包根，与 ``Package-Version.txt`` 同级）。清单缺失或字段缺失时
//! 一律**失败关闭**——宁可让用户手动补文件，也不静默接受未校验的下载。

use std::path::PathBuf;

use serde::Deserialize;

use crate::paths;

#[derive(Deserialize, Default, Debug)]
pub struct Manifest {
    pub dlss: Option<Dlss>,
    pub reshade: Option<Reshade>,
}

#[derive(Deserialize, Debug)]
pub struct Dlss {
    #[serde(rename = "Tag", default)]
    pub tag: Option<String>,
    #[serde(rename = "Asset", default)]
    pub asset: Option<String>,
    #[serde(rename = "ArchiveSha256", default)]
    pub archive_sha256: Option<String>,
    #[serde(rename = "DllSha256", default)]
    pub dll_sha256: Option<String>,
}

#[derive(Deserialize, Debug)]
pub struct Reshade {
    #[serde(rename = "SetupUrl", default)]
    pub setup_url: Option<String>,
    #[serde(rename = "SetupSha256", default)]
    pub setup_sha256: Option<String>,
    #[serde(rename = "DllSha256", default)]
    pub dll_sha256: Option<String>,
}

fn candidates() -> Vec<PathBuf> {
    let mut list = vec![paths::exe_dir().join("upstream-versions.json")];
    if let Some(root) = paths::payload_dir().parent() {
        list.push(root.join("upstream-versions.json"));
    }
    list
}

/// 解析清单文本，容忍 UTF-8 BOM。
///
/// 2026-11-26（清单 BOM 事故）：清单原先由 `Set-Content -Encoding UTF8` 生成，该参数在
/// PowerShell 5.1 下写的是「UTF-8 **带** BOM」，而 `serde_json` 不接受 BOM（报
/// `expected value at line 1 column 1`）⇒ `load()` 返回 None ⇒ `dlss_expectation()` /
/// `reshade_expectation()` 全部 Err ⇒ 安装器的 DLSS/ReShade 联网下载**永远失败关闭**，
/// 用户没有任何可恢复路径。生成端已改为无 BOM 落盘，这里再容忍一次：已经发出去的旧包
/// 与被手工改过的清单都不该因此报废。
///
/// 容忍的只有 BOM；空串、截断 JSON 等坏内容仍必须报错（失败关闭的语义不变），
/// 见下方 `parse_rejects_garbage`。
fn parse(text: &str) -> Result<Manifest, serde_json::Error> {
    serde_json::from_str(text.trim_start_matches('\u{feff}'))
}

/// 读包根清单；读不到返回 None（调用方据此失败关闭）。
pub fn load() -> Option<Manifest> {
    for path in candidates() {
        if !path.is_file() {
            continue;
        }
        match std::fs::read_to_string(&path) {
            Ok(text) => match parse(&text) {
                Ok(manifest) => return Some(manifest),
                Err(err) => {
                    crate::log::write(&format!("[verify] 清单解析失败 {}: {err}", path.display()))
                }
            },
            Err(err) => {
                crate::log::write(&format!("[verify] 清单读取失败 {}: {err}", path.display()))
            }
        }
    }
    None
}

/// DLSS 的期望值；缺任何一项都当作"不可校验"。
pub fn dlss_expectation() -> Result<(String, String, String, Option<String>), String> {
    let manifest =
        load().ok_or_else(|| "包里缺少 upstream-versions.json，无法校验下载内容".to_string())?;
    let dlss = manifest
        .dlss
        .ok_or_else(|| "upstream-versions.json 里没有 dlss 段".to_string())?;
    let tag = dlss
        .tag
        .filter(|v| !v.trim().is_empty())
        .ok_or_else(|| "清单里没有 dlss.Tag".to_string())?;
    let asset = dlss
        .asset
        .filter(|v| !v.trim().is_empty())
        .ok_or_else(|| "清单里没有 dlss.Asset".to_string())?;
    let archive = dlss
        .archive_sha256
        .filter(|v| !v.trim().is_empty())
        .ok_or_else(|| "清单里没有 dlss.ArchiveSha256".to_string())?;
    Ok((tag, asset, archive, dlss.dll_sha256))
}

/// ReShade 官方安装器的期望值。
pub fn reshade_expectation() -> Result<(String, String, Option<String>), String> {
    let manifest =
        load().ok_or_else(|| "包里缺少 upstream-versions.json，无法校验下载内容".to_string())?;
    let reshade = manifest
        .reshade
        .ok_or_else(|| "upstream-versions.json 里没有 reshade 段".to_string())?;
    let url = reshade
        .setup_url
        .filter(|v| !v.trim().is_empty())
        .ok_or_else(|| "清单里没有 reshade.SetupUrl".to_string())?;
    let sha = reshade
        .setup_sha256
        .filter(|v| !v.trim().is_empty())
        .ok_or_else(|| "清单里没有 reshade.SetupSha256".to_string())?;
    Ok((url, sha, reshade.dll_sha256))
}

/// 清单实际路径（日志/自检用）。
pub fn manifest_path() -> Option<PathBuf> {
    candidates().into_iter().find(|p| p.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;

    // 与真实清单同形（键名 PascalCase），只留判定需要的字段。
    const MANIFEST: &str = r#"{
        "dlss": { "Tag": "v2.14.16", "Asset": "streamline.zip", "ArchiveSha256": "AA" },
        "reshade": { "SetupUrl": "https://reshade.me/Setup.exe", "SetupSha256": "BB" }
    }"#;

    #[test]
    fn parse_accepts_plain_manifest() {
        let manifest = parse(MANIFEST).expect("无 BOM 的清单必须能解析");
        assert!(manifest.dlss.is_some());
        assert!(manifest.reshade.is_some());
    }

    #[test]
    fn parse_accepts_bom_prefixed_manifest() {
        // 回归用例：带 BOM 时 serde_json 直接报错 ⇒ 下载永远失败关闭（本次事故）。
        let manifest = parse(&format!("\u{feff}{MANIFEST}")).expect("带 BOM 的清单必须能解析");
        assert_eq!(manifest.dlss.unwrap().tag.as_deref(), Some("v2.14.16"));
        assert_eq!(manifest.reshade.unwrap().setup_sha256.as_deref(), Some("BB"));
    }

    #[test]
    fn parse_rejects_garbage() {
        // 容忍 BOM ≠ 容忍坏内容：清单读不出来时仍要失败关闭，不许静默当成"空清单"。
        assert!(parse("").is_err(), "空串必须报错");
        assert!(parse("\u{feff}").is_err(), "只有 BOM 也必须报错");
        assert!(parse("\u{feff}{\"dlss\":").is_err(), "截断的 JSON 必须报错");
    }
}
