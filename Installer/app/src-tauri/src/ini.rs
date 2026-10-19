use std::fs;
use std::io;
use std::path::Path;

/// 读一个 ini 值；找不到文件/段/键都返回 None。
pub fn get(path: &Path, section: &str, key: &str) -> Option<String> {
    let text = read(path)?;
    let mut current = String::new();
    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            current = trimmed[1..trimmed.len() - 1].trim().to_string();
            continue;
        }
        if trimmed.is_empty() || trimmed.starts_with(';') || trimmed.starts_with('#') {
            continue;
        }
        if !current.eq_ignore_ascii_case(section) {
            continue;
        }
        if let Some((name, value)) = trimmed.split_once('=') {
            if name.trim().eq_ignore_ascii_case(key) {
                return Some(value.trim().to_string());
            }
        }
    }
    None
}

fn read(path: &Path) -> Option<String> {
    let bytes = fs::read(path).ok()?;
    Some(String::from_utf8_lossy(&bytes).trim_start_matches('\u{feff}').to_string())
}

/// 写一个 ini 值：文件/段/键缺失都会补出来，行格式与旧脚本一致（`Key = Value`）。
/// 其余内容（注释、用户设置）原样保留。
pub fn set(path: &Path, section: &str, key: &str, value: &str) -> io::Result<()> {
    let existing = read(path);
    let crlf = existing
        .as_deref()
        .map(|t| t.contains("\r\n"))
        .unwrap_or(true);
    // 原文件末尾有没有换行也照原样保留，避免每次写都悄悄多一个空行
    let trailing_eol = existing.as_deref().map(|t| t.ends_with('\n')).unwrap_or(true);
    let mut lines: Vec<String> = existing
        .as_deref()
        .map(|t| t.split('\n').map(|l| l.trim_end_matches('\r').to_string()).collect())
        .unwrap_or_default();
    if existing.is_none() {
        lines.clear();
    }
    // 空值写成 `key =`，不留尾随空格（读回来仍然相等）
    let entry = if value.is_empty() {
        format!("{key} =")
    } else {
        format!("{key} = {value}")
    };

    // 无段 ini（如 TextureLoader.ini）：所有键都在文件根部，遇到第一个 [段] 就停
    if section.is_empty() {
        let mut replaced = false;
        for line in lines.iter_mut() {
            let trimmed = line.trim();
            if trimmed.starts_with('[') && trimmed.ends_with(']') {
                break;
            }
            if trimmed.starts_with(';') || trimmed.starts_with('#') || !trimmed.contains('=') {
                continue;
            }
            let name = trimmed.split('=').next().unwrap_or_default().trim();
            if name.eq_ignore_ascii_case(key) {
                *line = entry.clone();
                replaced = true;
                break;
            }
        }
        if !replaced {
            lines.push(entry.clone());
        }
        // 末尾的空行统一收掉，只由下面补一个换行；否则每次写都会让文件长一行
        while lines.last().map(|l| l.trim().is_empty()).unwrap_or(false) {
            lines.pop();
        }
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let eol = if crlf { "\r\n" } else { "\n" };
        let mut out = lines.join(eol);
        if trailing_eol {
            out.push_str(eol);
        }
        fs::write(path, out)?;
        return Ok(());
    }

    let mut section_start: Option<usize> = None;
    let mut section_end: usize = lines.len();
    for (i, line) in lines.iter().enumerate() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            if section_start.is_some() {
                section_end = i;
                break;
            }
            if trimmed[1..trimmed.len() - 1].trim().eq_ignore_ascii_case(section) {
                section_start = Some(i);
            }
        }
    }

    match section_start {
        Some(start) => {
            let mut replaced = false;
            for line in lines.iter_mut().take(section_end).skip(start + 1) {
                let trimmed = line.trim();
                if trimmed.starts_with(';') || trimmed.starts_with('#') || !trimmed.contains('=') {
                    continue;
                }
                let name = trimmed.split('=').next().unwrap_or_default().trim();
                if name.eq_ignore_ascii_case(key) {
                    *line = entry.clone();
                    replaced = true;
                    break;
                }
            }
            if !replaced {
                lines.insert(start + 1, entry.clone());
            }
        }
        None => {
            if !lines.is_empty() && !lines.last().map(|l| l.trim().is_empty()).unwrap_or(false) {
                lines.push(String::new());
            }
            lines.push(format!("[{section}]"));
            lines.push(entry.clone());
        }
    }

    // 末尾的空行统一收掉，只由下面补一个换行；否则每次写都会让文件长一行
    while lines.last().map(|l| l.trim().is_empty()).unwrap_or(false) {
        lines.pop();
    }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let eol = if crlf { "\r\n" } else { "\n" };
    let mut out = lines.join(eol);
    if trailing_eol {
        out.push_str(eol);
    }
    fs::write(path, out)
}
