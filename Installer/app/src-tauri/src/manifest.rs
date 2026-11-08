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

/// 读包根清单；读不到返回 None（调用方据此失败关闭）。
pub fn load() -> Option<Manifest> {
    for path in candidates() {
        if !path.is_file() {
            continue;
        }
        match std::fs::read_to_string(&path) {
            Ok(text) => match serde_json::from_str::<Manifest>(&text) {
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
