use std::fs;
use std::path::{Path, PathBuf};

use crate::paths;
use crate::settings::Settings;

/// 一个组件：插件本体留在 payload 里，安装 = 把它的绝对路径写进
/// `payload\FpsUnlocker\fps_config.json` 的 DllList（注入链）。
pub struct Component {
    pub id: &'static str,
    pub name: &'static str,
    pub note: &'static str,
    /// 必装：界面里锁定、不可取消
    pub mandatory: bool,
    /// 相对 payload 的注入文件（dll；fpsun 是宿主 exe，不进 DllList）。
    /// bridge / opti 有多个版本目录，实际路径由 dll_path_of 解析。
    pub dll: &'static str,
    /// payload 里必须存在的文件/目录（相对 payload），缺一项算包不完整
    pub files: &'static [&'static str],
}

/// 顺序即界面显示顺序；注入顺序由 `dll_list` 单独固定，改这里不影响注入链。
pub const CATALOG: &[Component] = &[
    Component {
        id: "fpsun",
        name: "FPS Unlocker",
        note: "解帧和插件注入（必装）",
        mandatory: true,
        dll: "FpsUnlocker\\unlockfps_nc.exe",
        files: &["FpsUnlocker\\unlockfps_nc.exe"],
    },
    Component {
        id: "bridge",
        name: "Dx11FsrBridge",
        note: "FSR4 超分桥接插件（必装）",
        mandatory: true,
        dll: "Bridge\\Dx11FsrBridge.dll",
        files: &["Bridge\\Dx11FsrBridge.dll", "AMD\\amd_fidelityfx_upscaler_dx12.dll"],
    },
    Component {
        id: "opti",
        name: "OptiScaler",
        note: "DLSS/XESS/FSR4 INT8 超分扩展（选装）",
        mandatory: false,
        dll: "OptiScaler\\OptiScaler.dll",
        files: &[
            "OptiScaler\\OptiScaler.dll",
            "OptiScaler\\amd_fidelityfx_upscaler_dx12.dll",
        ],
    },
    Component {
        id: "reshade",
        name: "ReShade + RenoDX",
        note: "HDR 增强（选装）",
        mandatory: false,
        dll: "ReShade\\ReShade64.dll",
        files: &["ReShade\\ReShade64.dll", "ReShade\\reshade-shaders"],
    },
    Component {
        id: "texture",
        name: "TextureLoader",
        note: "纹理 Mod 加载（选装）",
        mandatory: false,
        dll: "TextureLoader\\TextureLoader.dll",
        files: &[
            "TextureLoader\\TextureLoader.dll",
            "TextureLoader\\dstorage.dll",
            "TextureLoader\\dstoragecore.dll",
            "TextureLoader\\TextureLoader.ini",
            "TextureLoader\\licenses",
        ],
    },
    Component {
        id: "mosaic",
        name: "AntiPlayerMosaic",
        note: "反虚化 · 隐藏 UID（选装）",
        mandatory: false,
        dll: "AntiPlayerMosaic\\AntiPlayerMosaic.dll",
        files: &["AntiPlayerMosaic\\AntiPlayerMosaic.dll"],
    },
];

/// 可以在 payload 里放多份、由「插件配置」切换的组件。
pub const VERSIONED: &[&str] = &["bridge", "opti"];

/// 一个可切换的版本目录，例如 `payload\OptiScaler_v0.10x`。
pub struct VersionInfo {
    /// 目录名，同时作为界面上的版本标签
    pub dir: String,
    pub path: PathBuf,
    pub main_dll: PathBuf,
    /// OptiScaler 的超分组件目录：平铺 = 自身，嵌套 = 自身\OptiScaler
    pub component_dir: PathBuf,
    pub nested: bool,
}

pub fn find(id: &str) -> Option<&'static Component> {
    CATALOG.iter().find(|c| c.id == id)
}

pub fn is_versioned(id: &str) -> bool {
    VERSIONED.contains(&id)
}

/// 版本目录名前缀（`Bridge` / `OptiScaler`）。
pub fn prefix_of(id: &str) -> &'static str {
    match id {
        "bridge" => "Bridge",
        "opti" => "OptiScaler",
        _ => "",
    }
}

pub fn main_file(id: &str) -> &'static str {
    match id {
        "bridge" => "Dx11FsrBridge.dll",
        "opti" => "OptiScaler.dll",
        "reshade" => "ReShade64.dll",
        "texture" => "TextureLoader.dll",
        "mosaic" => "AntiPlayerMosaic.dll",
        "fpsun" => "unlockfps_nc.exe",
        _ => "",
    }
}

/// OptiScaler 两种发行布局：平铺（0.9.x）与嵌套（0.10 起）。
/// 判定依据是 `amd_fidelityfx_upscaler_dx12.dll` 所在目录——`[Libraries] OptiDllPath`
/// 指错目录会出现「注入成功但找不到超分后端」，退出时还会卡死。
fn layout(dir: &Path, id: &str) -> (bool, PathBuf) {
    if id != "opti" {
        return (false, dir.to_path_buf());
    }
    const MARKER: &str = "amd_fidelityfx_upscaler_dx12.dll";
    if dir.join(MARKER).is_file() {
        return (false, dir.to_path_buf());
    }
    let nested = dir.join("OptiScaler");
    if nested.join(MARKER).is_file() {
        return (true, nested);
    }
    (false, dir.to_path_buf())
}

/// 扫 payload 里所有 `<前缀>*` 且含主 DLL 的目录。
pub fn versions_of(id: &str) -> Vec<VersionInfo> {
    let prefix = prefix_of(id);
    let mut out = Vec::new();
    if prefix.is_empty() {
        return out;
    }
    let main = main_file(id);
    let Ok(entries) = fs::read_dir(paths::payload_dir()) else {
        return out;
    };
    for entry in entries.flatten() {
        let dir = entry.file_name().to_string_lossy().into_owned();
        if !dir.to_lowercase().starts_with(&prefix.to_lowercase()) {
            continue;
        }
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        // 主 DLL 可能在版本目录根（0.9.x 平铺）或 OptiScaler\ 子目录（0.10 起嵌套）
        let main_dll = if path.join(main).is_file() {
            path.join(main)
        } else if path.join("OptiScaler").join(main).is_file() {
            path.join("OptiScaler").join(main)
        } else {
            continue;
        };
        let (nested, component_dir) = layout(&path, id);
        out.push(VersionInfo {
            dir,
            path,
            main_dll,
            component_dir,
            nested,
        });
    }
    out.sort_by(|a, b| a.dir.to_lowercase().cmp(&b.dir.to_lowercase()));
    out
}

/// 当前选中的版本目录名：设置里记着就用它，否则用扫到的第一个。
pub fn chosen_dir(id: &str, settings: &Settings) -> String {
    let want = match id {
        "bridge" => &settings.bridge_dir,
        "opti" => &settings.opti_dir,
        _ => return String::new(),
    };
    let available = versions_of(id);
    if !want.is_empty() {
        if let Some(hit) = available
            .iter()
            .find(|v| v.dir.eq_ignore_ascii_case(want))
        {
            return hit.dir.clone();
        }
    }
    available.first().map(|v| v.dir.clone()).unwrap_or_default()
}

pub fn resolve(id: &str, settings: &Settings) -> Option<VersionInfo> {
    let dir = chosen_dir(id, settings);
    versions_of(id)
        .into_iter()
        .find(|v| v.dir.eq_ignore_ascii_case(&dir))
}

/// 组件的注入文件绝对路径（版本化组件按当前选中的版本目录解析）。
pub fn dll_path_of(id: &str, settings: &Settings) -> PathBuf {
    if is_versioned(id) {
        if let Some(v) = resolve(id, settings) {
            return v.main_dll;
        }
        let dir = chosen_dir(id, settings);
        return paths::payload_dir().join(dir).join(main_file(id));
    }
    match find(id) {
        Some(component) => paths::payload_dir().join(component.dll),
        None => PathBuf::new(),
    }
}

pub fn payload_available() -> bool {
    !versions_of("bridge").is_empty()
}

/// 随包的出厂 ini 模板（相对 payload）。Bridge 的模板名跟目录名不同，所以单独列。
pub fn ini_template(id: &str) -> Option<&'static str> {
    Some(match id {
        "bridge" => "default_config\\Dx11FsrBridge.ini",
        "opti" => "default_config\\OptiScaler.ini",
        "reshade" => "default_config\\ReShade.ini",
        "texture" => "default_config\\TextureLoader.ini",
        _ => return None,
    })
}

/// 版本目录对应的基线备份：`payload\default_config\<目录名>.ini`。
pub fn version_baseline(dir_name: &str) -> PathBuf {
    paths::payload_dir()
        .join("default_config")
        .join(format!("{dir_name}.ini"))
}

/// 「恢复默认设置」用的基线：先看当前版本的备份，再退回随包模板。
pub fn restore_source(id: &str, settings: &Settings) -> Option<PathBuf> {
    if is_versioned(id) {
        if let Some(version) = resolve(id, settings) {
            let backup = version_baseline(&version.dir);
            if backup.is_file() {
                return Some(backup);
            }
        }
    }
    ini_template(id)
        .map(|rel| paths::payload_dir().join(rel))
        .filter(|path| path.is_file())
}

/// 必装项无条件进列表、未知 id 丢掉、输出顺序归一为目录顺序。
pub fn with_mandatory(ids: &[String]) -> Vec<String> {
    CATALOG
        .iter()
        .filter(|c| c.mandatory || ids.iter().any(|id| id == c.id))
        .map(|c| c.id.to_string())
        .collect()
}

/// payload 里缺失的文件（只查已选组件），格式 `id: 相对路径`。
pub fn missing_files(selected: &[String], settings: &Settings) -> Vec<String> {
    let payload = paths::payload_dir();
    let mut missing = Vec::new();
    for component in CATALOG {
        if !selected.iter().any(|id| id == component.id) {
            continue;
        }
        if is_versioned(component.id) {
            match resolve(component.id, settings) {
                None => missing.push(format!("{}: payload\\{}*", component.id, prefix_of(component.id))),
                Some(version) => {
                    if !version.main_dll.is_file() {
                        missing.push(format!("{}: {}\\{}", component.id, version.dir, main_file(component.id)));
                    }
                    // Bridge 的 AMD 超分库放在 payload\AMD；OptiScaler 的跟着版本目录走
                    if component.id == "bridge" {
                        if !payload.join("AMD\\amd_fidelityfx_upscaler_dx12.dll").is_file() {
                            missing.push("bridge: AMD\\amd_fidelityfx_upscaler_dx12.dll".to_string());
                        }
                    } else if !version.component_dir.join("amd_fidelityfx_upscaler_dx12.dll").is_file() {
                        missing.push(format!(
                            "{}: {}\\amd_fidelityfx_upscaler_dx12.dll",
                            component.id, version.dir
                        ));
                    }
                }
            }
            continue;
        }
        for rel in component.files {
            if !payload.join(rel).exists() {
                missing.push(format!("{}: {}", component.id, rel));
            }
        }
    }
    missing
}

/// 组件在 payload 里是否齐全（界面用它区分「缺少文件」和「未启用」）。
pub fn present(id: &str, settings: &Settings) -> bool {
    let Some(component) = find(id) else {
        return false;
    };
    let payload = paths::payload_dir();
    if is_versioned(id) {
        return match resolve(id, settings) {
            Some(version) => {
                let opti_ok = id == "bridge"
                    || version
                        .component_dir
                        .join("amd_fidelityfx_upscaler_dx12.dll")
                        .is_file();
                version.main_dll.is_file()
                    && opti_ok
                    && (id != "bridge"
                        || payload.join("AMD\\amd_fidelityfx_upscaler_dx12.dll").is_file())
            }
            None => false,
        };
    }
    component.files.iter().all(|rel| payload.join(rel).exists())
}

fn dir_bytes(path: &Path) -> u64 {
    let mut total = 0;
    if let Ok(entries) = fs::read_dir(path) {
        for entry in entries.flatten() {
            match entry.metadata() {
                Ok(meta) if meta.is_dir() => total += dir_bytes(&entry.path()),
                Ok(meta) => total += meta.len(),
                Err(_) => {}
            }
        }
    }
    total
}

pub fn component_bytes(id: &str, settings: &Settings) -> u64 {
    let payload = paths::payload_dir();
    let Some(component) = find(id) else {
        return 0;
    };
    if is_versioned(id) {
        // 版本化组件：当前选中版本的目录体积（Bridge 另加 payload\AMD）
        let mut total = match resolve(id, settings) {
            Some(version) => dir_bytes(&version.path),
            None => 0,
        };
        if id == "bridge" {
            total += fs::metadata(payload.join("AMD\\amd_fidelityfx_upscaler_dx12.dll"))
                .map(|meta| meta.len())
                .unwrap_or(0);
        }
        return total;
    }
    component
        .files
        .iter()
        .map(|rel| {
            let path = payload.join(rel);
            match fs::metadata(&path) {
                Ok(meta) if meta.is_dir() => dir_bytes(&path),
                Ok(meta) => meta.len(),
                Err(_) => 0,
            }
        })
        .sum()
}

fn normalize(path: &Path) -> String {
    path.to_string_lossy().to_lowercase().replace('/', "\\")
}

/// DllList 里的绝对路径是否包含该组件。
pub fn is_enabled(dlls: &[String], id: &str, settings: &Settings) -> bool {
    if id == "fpsun" {
        return dll_path_of(id, settings).is_file();
    }
    let target = normalize(&dll_path_of(id, settings));
    !target.is_empty()
        && dlls
            .iter()
            .any(|entry| normalize(Path::new(entry)) == target)
}

/// 注入顺序固定：ReShade64 → Dx11FsrBridge → OptiScaler → AntiPlayerMosaic → TextureLoader
/// 用户自定义注入的 DLL 一律追加在这些之后。
pub fn dll_list(selected: &[String], settings: &Settings) -> Vec<PathBuf> {
    const ORDER: &[&str] = &["reshade", "bridge", "opti", "mosaic", "texture"];
    let mut list: Vec<PathBuf> = ORDER
        .iter()
        .filter(|id| selected.iter().any(|s| s == *id))
        .map(|id| dll_path_of(id, settings))
        .filter(|path| path.is_file())
        .collect();
    for raw in &settings.custom_dlls {
        if !settings.custom_enabled(raw) {
            continue;
        }
        let path = PathBuf::from(raw);
        if !path.is_file() {
            continue;
        }
        let key = normalize(&path);
        if list.iter().any(|p| normalize(p) == key) {
            continue;
        }
        list.push(path);
    }
    list
}

