use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::Mutex;

use serde_json::{json, Map, Value};

use crate::log;
use crate::paths;
use crate::pkg;
use crate::settings::Settings;

static CHILD: Mutex<Option<Child>> = Mutex::new(None);

/// 解帧工具直接跑在 payload 里，不再复制到 %LOCALAPPDATA%。
pub fn tool_exe() -> PathBuf {
    paths::unlocker_dir().join("unlockfps_nc.exe")
}

pub fn config_path() -> PathBuf {
    paths::unlocker_dir().join("fps_config.json")
}

/// 上游 ConfigService 的默认值；界面没暴露的项保持默认，已有的用户设置一律保留。
fn defaults() -> Map<String, Value> {
    let mut m = Map::new();
    m.insert("GamePath".into(), json!(""));
    m.insert("AutoStart".into(), json!(true));
    m.insert("AutoClose".into(), json!(true));
    m.insert("PopupWindow".into(), json!(true));
    m.insert("Fullscreen".into(), json!(false));
    m.insert("UseCustomRes".into(), json!(false));
    m.insert("IsExclusiveFullscreen".into(), json!(false));
    m.insert("StartMinimized".into(), json!(true));
    m.insert("UsePowerSave".into(), json!(false));
    m.insert("SuspendLoad".into(), json!(false));
    m.insert("UseMobileUI".into(), json!(false));
    m.insert("UseHDR".into(), json!(false));
    m.insert("FPSTarget".into(), json!(240));
    m.insert("CustomResX".into(), json!(1920));
    m.insert("CustomResY".into(), json!(1080));
    m.insert("MonitorNum".into(), json!(1));
    m.insert("Priority".into(), json!(3));
    m.insert("AdditionalCommandLine".into(), json!(""));
    m.insert("LastVersionNotify".into(), json!(0));
    m.insert("DllList".into(), json!([]));
    m
}

pub fn ensure_tool() -> Result<(), String> {
    if !tool_exe().is_file() {
        return Err("payload 缺少 FpsUnlocker\\unlockfps_nc.exe".into());
    }
    Ok(())
}

/// 读 fps_config.json 并补齐缺失键；文件不存在时返回默认值。
fn config_map() -> Map<String, Value> {
    let mut config = defaults();
    if let Ok(bytes) = fs::read(config_path()) {
        let text = String::from_utf8_lossy(&bytes);
        let text = text.trim_start_matches('\u{feff}');
        match serde_json::from_str::<Value>(text) {
            Ok(Value::Object(existing)) => {
                for (k, v) in existing {
                    config.insert(k, v);
                }
            }
            Ok(_) => log::write("fps_config.json 不是对象，按默认值处理"),
            Err(e) => log::write(&format!("fps_config.json 解析失败，按默认值处理: {e}")),
        }
    }
    config
}

fn save_config(config: &Map<String, Value>) -> Result<(), String> {
    let path = config_path();
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("无法创建解帧工具目录：{e}"))?;
    }
    let text = serde_json::to_string_pretty(&Value::Object(config.clone()))
        .map_err(|e| format!("无法序列化 fps_config.json：{e}"))?;
    fs::write(&path, text).map_err(|_| "无法写入 fps_config.json".to_string())?;
    Ok(())
}

/// 界面可改的开关：JSON 键名 → unlockfps_nc 的配置键。
const SWITCHES: &[(&str, &str)] = &[
    ("autoStart", "AutoStart"),
    ("autoClose", "AutoClose"),
    ("startMinimized", "StartMinimized"),
    ("powerSave", "UsePowerSave"),
    ("borderless", "PopupWindow"),
    ("fullscreen", "Fullscreen"),
    ("exclusive", "IsExclusiveFullscreen"),
    ("customRes", "UseCustomRes"),
    ("mobileUI", "UseMobileUI"),
    ("suspendLoad", "SuspendLoad"),
];

fn flag(config: &Map<String, Value>, key: &str) -> bool {
    config.get(key).and_then(Value::as_bool).unwrap_or(false)
}

fn number(config: &Map<String, Value>, key: &str, fallback: i64) -> i64 {
    config.get(key).and_then(Value::as_i64).unwrap_or(fallback)
}

/// 解帧工具选项的界面视图（只读投影，供 state 消息使用）。
pub fn unlocker_settings() -> Value {
    let c = config_map();
    json!({
        "ready": config_path().is_file(),
        "autoStart": flag(&c, "AutoStart"),
        "autoClose": flag(&c, "AutoClose"),
        "startMinimized": flag(&c, "StartMinimized"),
        "powerSave": flag(&c, "UsePowerSave"),
        "borderless": flag(&c, "PopupWindow"),
        "fullscreen": flag(&c, "Fullscreen"),
        "exclusive": flag(&c, "IsExclusiveFullscreen"),
        "customRes": flag(&c, "UseCustomRes"),
        "mobileUI": flag(&c, "UseMobileUI"),
        "suspendLoad": flag(&c, "SuspendLoad"),
        "hdr": flag(&c, "UseHDR"),
        "resX": number(&c, "CustomResX", 1920),
        "resY": number(&c, "CustomResY", 1080),
        "monitor": number(&c, "MonitorNum", 1),
        "priority": number(&c, "Priority", 3),
    })
}

/// 应用界面改动。只碰白名单键，GamePath / FPSTarget / DllList / UseHDR 一律不动。
pub fn patch_config(patch: &Value) -> Result<Value, String> {
    let patch = patch.as_object().ok_or("无效的修改内容")?;
    if !config_path().is_file() {
        return Err("还没有写入注入链，先完成一次安装".into());
    }
    let mut config = config_map();

    let mut borderless_set = false;
    let mut fullscreen_set = false;
    for (ui_key, cfg_key) in SWITCHES {
        if let Some(v) = patch.get(*ui_key).and_then(Value::as_bool) {
            config.insert((*cfg_key).to_string(), json!(v));
            match *ui_key {
                "borderless" => borderless_set = true,
                "fullscreen" => fullscreen_set = true,
                _ => {}
            }
        }
    }

    // 无边框窗口与全屏在解锁器里互斥：后改的那个赢。
    if flag(&config, "PopupWindow") && flag(&config, "Fullscreen") {
        if borderless_set && !fullscreen_set {
            config.insert("Fullscreen".into(), json!(false));
        } else {
            config.insert("PopupWindow".into(), json!(false));
        }
    }

    if let Some(v) = patch.get("resX").and_then(Value::as_i64) {
        config.insert("CustomResX".into(), json!(v.clamp(200, 7680)));
    }
    if let Some(v) = patch.get("resY").and_then(Value::as_i64) {
        config.insert("CustomResY".into(), json!(v.clamp(200, 4320)));
    }
    if let Some(v) = patch.get("monitor").and_then(Value::as_i64) {
        config.insert("MonitorNum".into(), json!(v.clamp(1, 16)));
    }
    if let Some(v) = patch.get("priority").and_then(Value::as_i64) {
        config.insert("Priority".into(), json!(v.clamp(0, 5)));
    }

    save_config(&config)?;
    log::write(&format!(
        "已更新解帧工具选项: {}",
        serde_json::to_string(&Value::Object(config.clone())).unwrap_or_default()
    ));
    Ok(unlocker_settings())
}

/// 写 fps_config.json：文件不存在时写整套默认值，之后只强制四个键
/// （GamePath / FPSTarget / DllList / UseHDR），其余键保留用户现状。
pub fn write_config(game_exe: &Path, fps: i32, dlls: &[PathBuf]) -> Result<(), String> {
    let path = config_path();
    let existed = path.is_file();
    let mut config = config_map();

    let dll_entries: Vec<String> = dlls
        .iter()
        .map(|d| d.to_string_lossy().into_owned())
        .collect();
    let use_hdr = dlls.iter().any(|d| {
        d.file_name()
            .map(|n| n.to_string_lossy().eq_ignore_ascii_case("ReShade64.dll"))
            .unwrap_or(false)
    });

    config.insert(
        "GamePath".into(),
        json!(game_exe.to_string_lossy().into_owned()),
    );
    config.insert("FPSTarget".into(), json!(fps));
    config.insert("DllList".into(), json!(dll_entries));
    config.insert("UseHDR".into(), json!(use_hdr));

    if !existed {
        log::write("fps_config.json 不存在，写入整套默认值");
    }

    save_config(&config)?;
    log::write(&format!("已写入 fps_config.json: {}", path.display()));
    Ok(())
}

/// 当前注入链（来自 fps_config.json）。文件缺失时为空。
pub fn dll_list() -> Vec<String> {
    let Ok(bytes) = fs::read(config_path()) else {
        return Vec::new();
    };
    let text = String::from_utf8_lossy(&bytes);
    let text = text.trim_start_matches('\u{feff}');
    let Ok(value) = serde_json::from_str::<Value>(text) else {
        return Vec::new();
    };
    value["DllList"]
        .as_array()
        .map(|list| {
            list.iter()
                .filter_map(|v| v.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

pub fn running() -> bool {
    let mut guard = CHILD.lock().unwrap();
    match guard.as_mut() {
        Some(child) => matches!(child.try_wait(), Ok(None)),
        None => false,
    }
}

pub fn clear() {
    let mut guard = CHILD.lock().unwrap();
    *guard = None;
}

pub fn launch() -> Result<u32, String> {
    if running() {
        return Err("解帧工具已经在运行".into());
    }
    clear();

    let exe = tool_exe();
    if !exe.is_file() {
        return Err("payload 里找不到解帧工具".into());
    }
    let dir = paths::unlocker_dir();

    let child = Command::new(&exe)
        .current_dir(&dir)
        .spawn()
        .map_err(|e| format!("无法启动解帧工具：{e}"))?;
    let pid = child.id();
    *CHILD.lock().unwrap() = Some(child);
    log::write(&format!("已启动解帧工具，pid={pid}"));
    Ok(pid)
}

/// 按注入链回填每个组件的启用状态（界面用它区分「已安装 / 未写入」）。
pub fn enabled_flags(dlls: &[String], settings: &Settings) -> Vec<String> {
    pkg::CATALOG
        .iter()
        .map(|c| {
            if pkg::is_enabled(dlls, c.id, settings) {
                "1".to_string()
            } else {
                "0".to_string()
            }
        })
        .collect()
}
