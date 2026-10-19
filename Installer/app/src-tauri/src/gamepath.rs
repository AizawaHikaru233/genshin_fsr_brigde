use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
use winreg::RegKey;

const GAME_EXES: &[&str] = &["YuanShen.exe", "GenshinImpact.exe"];

/// 各盘符下常见的安装层。
const COMMON_SUBDIRS: &[&str] = &[
    "Genshin Impact",
    "Games\\Genshin Impact",
    "Games\\Genshin Impact Game",
    "Program Files\\Genshin Impact",
    "Program Files\\Genshin Impact Game",
    "miHoYo\\Genshin Impact",
    "miHoYo\\Genshin Impact Game",
    "HoYoPlay\\games\\Genshin Impact Game",
    "Program Files\\HoYoPlay\\games\\Genshin Impact Game",
];

const REGISTRY_CANDIDATES: &[(isize, &str, &str)] = &[
    (
        HKEY_CURRENT_USER,
        "Software\\miHoYo\\原神",
        "GamePath",
    ),
    (
        HKEY_CURRENT_USER,
        "Software\\miHoYo\\Genshin Impact",
        "GamePath",
    ),
    (
        HKEY_CURRENT_USER,
        "Software\\miHoYo\\原神",
        "InstallPath",
    ),
    (
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\原神",
        "InstallLocation",
    ),
    (
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Genshin Impact",
        "InstallLocation",
    ),
];

pub fn find_game_exe(dir: &Path) -> Option<PathBuf> {
    GAME_EXES
        .iter()
        .map(|name| dir.join(name))
        .find(|path| path.is_file())
}

pub fn is_game_dir(dir: &Path) -> bool {
    find_game_exe(dir).is_some()
}

/// 允许用户选到上一层目录：先看它自己，再看常见子目录，最后看它的直接子目录。
pub fn normalize(picked: &Path) -> Option<PathBuf> {
    if picked.as_os_str().is_empty() {
        return None;
    }
    if picked.is_file() {
        let parent = picked.parent()?;
        return if is_game_dir(parent) {
            Some(parent.to_path_buf())
        } else {
            None
        };
    }
    if is_game_dir(picked) {
        return Some(picked.to_path_buf());
    }
    for sub in COMMON_SUBDIRS {
        let candidate = picked.join(sub);
        if is_game_dir(&candidate) {
            return Some(candidate);
        }
    }
    fs::read_dir(picked).ok()?.flatten().find_map(|entry| {
        let path = entry.path();
        if path.is_dir() && is_game_dir(&path) {
            Some(path)
        } else {
            None
        }
    })
}

fn registry_value(root: isize, subkey: &str, value: &str) -> Option<PathBuf> {
    let key = RegKey::predef(root).open_subkey(subkey).ok()?;
    let text: String = key.get_value(value).ok()?;
    if text.trim().is_empty() {
        None
    } else {
        Some(PathBuf::from(text))
    }
}

fn logical_drives() -> Vec<char> {
    let mask = unsafe { windows_sys::Win32::Storage::FileSystem::GetLogicalDrives() };
    (0..26)
        .filter(|i| mask & (1 << i) != 0)
        .map(|i| (b'A' + i as u8) as char)
        .collect()
}

/// 候选目录：之前选过的 → 注册表 → 各盘符常见路径。返回时过滤掉不存在的。
pub fn scan(preferred: &Path) -> Vec<PathBuf> {
    let mut seen: HashSet<String> = HashSet::new();
    let mut out: Vec<PathBuf> = Vec::new();

    let push = |path: PathBuf, out: &mut Vec<PathBuf>, seen: &mut HashSet<String>| {
        let Some(normalized) = normalize(&path) else {
            return;
        };
        let key = normalized.to_string_lossy().to_lowercase();
        if seen.insert(key) {
            out.push(normalized);
        }
    };

    push(preferred.to_path_buf(), &mut out, &mut seen);
    for (root, subkey, value) in REGISTRY_CANDIDATES {
        if let Some(path) = registry_value(*root, subkey, value) {
            push(path, &mut out, &mut seen);
        }
    }
    for drive in logical_drives() {
        for sub in COMMON_SUBDIRS {
            push(
                PathBuf::from(format!("{drive}:\\{sub}")),
                &mut out,
                &mut seen,
            );
        }
    }
    out
}

/// 原生目录选择框，返回用户选的原始路径（是否合法由调用方 normalize 判断）。
pub fn pick_folder() -> Option<PathBuf> {
    rfd::FileDialog::new()
        .set_title("选择原神安装目录")
        .pick_folder()
}
