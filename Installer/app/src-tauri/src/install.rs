use std::fs;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::gpu;
use crate::ini;
use crate::launch;
use crate::log;
use crate::paths;
use crate::pkg;
use crate::settings::Settings;

pub struct Progress {
    pub index: usize,
    pub total: usize,
    pub component: String,
    pub label: String,
}

pub struct Outcome {
    pub ok: bool,
    pub cancelled: bool,
    pub error: String,
    pub failed_component: String,
}

fn err(component: &str, message: impl Into<String>) -> Outcome {
    let message = message.into();
    log::write(&format!("[install] {component}: {message}"));
    Outcome {
        ok: false,
        cancelled: false,
        error: message,
        failed_component: component.to_string(),
    }
}

/// 逐组件写各自需要的那份配置，最后写 fps_config.json 的注入链。
/// 插件本体一律留在 payload，不往游戏目录复制任何 DLL。
pub fn apply(
    game_exe: &Path,
    fps: i32,
    selected: &[String],
    settings: &Settings,
    cancel: &AtomicBool,
    mut progress: impl FnMut(&Progress),
) -> Outcome {
    let game_dir = game_exe.parent().unwrap_or(Path::new("."));
    let total = selected.len().max(1);

    for (i, id) in selected.iter().enumerate() {
        if cancel.load(Ordering::SeqCst) {
            return Outcome {
                ok: false,
                cancelled: true,
                error: String::new(),
                failed_component: String::new(),
            };
        }

        let label = match id.as_str() {
            "fpsun" => "检查 FPS Unlocker",
            "bridge" => "写入 Dx11FsrBridge.ini",
            "opti" => "写入 OptiScaler.ini",
            "reshade" => "写入 ReShade.ini",
            _ => "检查组件文件",
        };
        progress(&Progress {
            index: i + 1,
            total,
            component: id.clone(),
            label: label.to_string(),
        });

        let step = match id.as_str() {
            "bridge" => prepare_bridge(settings),
            "opti" => prepare_opti(settings),
            "reshade" => prepare_reshade(game_dir),
            _ => Ok(()),
        };
        if let Err(message) = step {
            return err(id, message);
        }
    }

    if cancel.load(Ordering::SeqCst) {
        return Outcome {
            ok: false,
            cancelled: true,
            error: String::new(),
            failed_component: String::new(),
        };
    }

    let dlls = pkg::dll_list(selected, settings);
    if let Err(message) = launch::write_config(game_exe, fps, &dlls) {
        return err("fpsun", message);
    }

    Outcome {
        ok: true,
        cancelled: false,
        error: String::new(),
        failed_component: String::new(),
    }
}

/// 切版本时把那版的 ini 另存一份到 default_config，名字跟版本目录一致。
/// 已经存在就不动 —— 基线只取第一次见到该版本时的样子，避免把用户改过的内容当默认值。
pub fn snapshot_baseline(id: &str, settings: &Settings) -> Option<String> {
    let version = pkg::resolve(id, settings)?;
    let live = match id {
        "bridge" => version.path.join("Dx11FsrBridge.ini"),
        "opti" => version.path.join("OptiScaler.ini"),
        _ => return None,
    };
    if !live.is_file() {
        return None;
    }
    let backup = pkg::version_baseline(&version.dir);
    if backup.is_file() {
        return None;
    }
    if let Some(parent) = backup.parent() {
        let _ = fs::create_dir_all(parent);
    }
    match fs::copy(&live, &backup) {
        Ok(_) => {
            log::write(&format!(
                "[install] 已记录 {} 的出厂配置基线：{}",
                version.dir,
                backup.display()
            ));
            Some(backup.display().to_string())
        }
        Err(error) => {
            log::write(&format!("[install] 记录配置基线失败：{error}"));
            None
        }
    }
}

/// 「恢复默认设置」：用 default_config 里的基线覆盖各组件的 ini。
/// 游戏目录可选 —— 没有的话只还原 payload 内的那几份（ReShade 的 ini 在游戏目录，跳过）。
pub fn restore_defaults(
    game_exe: Option<&Path>,
    selected: &[String],
    settings: &Settings,
) -> Result<Vec<String>, String> {
    let payload = paths::payload_dir();
    let mut restored: Vec<String> = Vec::new();
    let copy_over = |source: &Path, target: &Path, restored: &mut Vec<String>| -> Result<(), String> {
        fs::copy(source, target).map_err(|e| format!("还原 {} 失败：{e}", target.display()))?;
        restored.push(target.display().to_string());
        Ok(())
    };
    for id in ["bridge", "opti"] {
        if !selected.iter().any(|chosen| chosen == id) {
            continue;
        }
        let (Some(source), Some(version)) = (pkg::restore_source(id, settings), pkg::resolve(id, settings)) else {
            continue;
        };
        let target = match id {
            "bridge" => version.path.join("Dx11FsrBridge.ini"),
            _ => version.path.join("OptiScaler.ini"),
        };
        copy_over(&source, &target, &mut restored)?;
    }
    if selected.iter().any(|chosen| chosen == "reshade") {
        if let Some(dir) = game_exe.and_then(|exe| exe.parent()) {
            for name in ["ReShade.ini", "ReShadePreset.ini"] {
                let source = payload.join("default_config").join(name);
                if source.is_file() {
                    copy_over(&source, &dir.join(name), &mut restored)?;
                }
            }
        }
    }
    if selected.iter().any(|chosen| chosen == "texture") {
        let source = payload.join("default_config\\TextureLoader.ini");
        let target = payload.join("TextureLoader\\TextureLoader.ini");
        if source.is_file() && target.exists() {
            copy_over(&source, &target, &mut restored)?;
        }
    }
    log::write(&format!(
        "[install] 恢复默认设置：还原 {} 个 ini 文件",
        restored.len()
    ));
    Ok(restored)
}

/// TextureLoader 的 Mods 目录：写进 `payload\TextureLoader\TextureLoader.ini` 的 `mods_dir`。
/// 传空串 = 回到默认（DLL 同目录的 Mods）。返回实际写入的值。
pub fn write_mods_dir(value: &str) -> Result<String, String> {
    let ini_path = paths::payload_dir().join("TextureLoader\\TextureLoader.ini");
    if !ini_path.is_file() {
        return Err("payload\\TextureLoader\\TextureLoader.ini 不存在".into());
    }
    let text = value.trim().to_string();
    ini::set(&ini_path, "", "mods_dir", &text)
        .map_err(|e| format!("写 TextureLoader.ini 失败：{e}"))?;
    log::write(&format!(
        "[install] TextureLoader mods_dir = {}",
        if text.is_empty() { "（默认 Mods）" } else { &text }
    ));
    Ok(text)
}

/// 切版本时只需要重写各组件自己的 ini（不需要游戏目录）。
pub fn prepare_ini(selected: &[String], settings: &Settings) -> Result<(), (String, String)> {
    for id in selected {
        let step = match id.as_str() {
            "bridge" => prepare_bridge(settings),
            "opti" => prepare_opti(settings),
            _ => Ok(()),
        };
        if let Err(message) = step {
            return Err((id.clone(), message));
        }
    }
    Ok(())
}

/// Dx11FsrBridge.ini 只有一项：RDNA2 关掉异步超分（与旧脚本一致）。
fn prepare_bridge(settings: &Settings) -> Result<(), String> {
    let Some(version) = pkg::resolve("bridge", settings) else {
        return Err("payload 里找不到 Dx11FsrBridge 的版本目录".into());
    };
    let amd = paths::payload_dir().join("AMD\\amd_fidelityfx_upscaler_dx12.dll");
    if !amd.is_file() {
        return Err(
            "FSR Bridge 依赖不完整：缺少 payload\\AMD\\amd_fidelityfx_upscaler_dx12.dll".into(),
        );
    }
    let path = version.path.join("Dx11FsrBridge.ini");
    if !path.is_file() {
        log::write("[install] 版本目录没有 Dx11FsrBridge.ini，跳过该键");
        return Ok(());
    }
    let value = if gpu::has_rdna2() { "0" } else { "1" };
    ini::set(&path, "Dx11FsrBridge", "Ffx12AsyncUpscale", value)
        .map_err(|e| format!("无法写入 Dx11FsrBridge.ini：{e}"))
}

/// OptiScaler.ini 的托管键：FSR4 策略按显卡判定，OptiDllPath 按所选版本的布局写。
fn prepare_opti(settings: &Settings) -> Result<(), String> {
    let Some(version) = pkg::resolve("opti", settings) else {
        return Err("payload 里找不到 OptiScaler 的版本目录".into());
    };
    let dir = version.path.clone();
    let path = dir.join("OptiScaler.ini");
    if !path.is_file() {
        // 优先包内官方模板，其次版本目录自带的 OptiScaler.default.ini
        let candidates = [
            paths::payload_dir().join("default_config").join("OptiScaler.ini"),
            dir.join("OptiScaler.default.ini"),
        ];
        let Some(template) = candidates.iter().find(|p| p.is_file()) else {
            return Err("payload 缺少 OptiScaler.ini 模板".into());
        };
        fs::copy(template, &path).map_err(|e| format!("无法创建 OptiScaler.ini：{e}"))?;
        log::write(&format!(
            "[install] 已从模板创建 {}",
            path.display()
        ));
    }

    let policy = gpu::policy();
    let write = |section: &str, key: &str, value: &str| -> Result<(), String> {
        ini::set(&path, section, key, value)
            .map_err(|e| format!("无法写入 OptiScaler.ini（{section}.{key}）：{e}"))
    };

    write("FSR", "Fsr4Update", if policy.mode.supported() { "true" } else { "false" })?;
    write(
        "FSR",
        "UpscalerIndex",
        if policy.mode.supported() { "0" } else { "auto" },
    )?;
    write(
        "FSR",
        "Fsr4ForceEnableInt8",
        if policy.mode == gpu::Mode::Int8 { "true" } else { "false" },
    )?;
    write("FSR", "FsrNonLinearColorSpace", "true")?;
    write("Spoofing", "Dxgi", "false")?;
    // 0.9.x 是平铺（组件与主 DLL 同级），0.10 起是嵌套（组件在 OptiScaler\ 子目录）
    let opti_dll_path = paths::to_display(&version.component_dir)
        .trim_end_matches('\\')
        .to_string();
    write("Libraries", "OptiDllPath", &opti_dll_path)?;
    write("Log", "LogToFile", "true")?;
    write("Log", "LogLevel", "2")?;
    write("Log", "SingleFile", "true")?;
    write("Log", "LogAsync", "false")?;
    write("Log", "LogAsyncThreads", "1")?;
    // 只有一种发布形态：2.4.0 起不再打"非帧生成版"（NonFrameGeneration.edition 已不再打包），
    // 帧生成交给 OptiScaler 按显卡自己探测，安装器不再写死禁用。
    write("FrameGen", "Enabled", "auto")?;
    write("FrameGen", "FGInput", "auto")?;
    write("FrameGen", "FGOutput", "auto")?;

    log::write(&format!(
        "OptiScaler 版本 {} · {} · OptiDllPath={}",
        version.dir,
        if version.nested { "嵌套布局" } else { "平铺布局" },
        opti_dll_path
    ));
    log::write(&format!(
        "FSR4 GPU policy: {:?} / {} / {}",
        policy.mode,
        policy.reason,
        if policy.name.is_empty() { "unknown" } else { &policy.name }
    ));
    Ok(())
}

/// ReShade.ini 落在游戏目录；三处 payload 路径每次刷新，游戏目录的两处只在为空时写。
fn prepare_reshade(game_dir: &Path) -> Result<(), String> {
    let shader_dir = paths::payload_dir().join("ReShade").join("reshade-shaders");
    let ini_path = game_dir.join("ReShade.ini");
    let preset_path = game_dir.join("ReShadePreset.ini");
    let screenshots = game_dir.join("Screenshots");
    fs::create_dir_all(&screenshots).map_err(|e| format!("无法创建 Screenshots 目录：{e}"))?;

    let template = paths::payload_dir().join("default_config").join("ReShade.ini");
    if !ini_has_effects(&ini_path, game_dir) {
        if template.is_file() {
            fs::copy(&template, &ini_path).map_err(|e| format!("无法写入 ReShade.ini：{e}"))?;
        } else if !ini_path.is_file() {
            let minimal = "[ADDON]\nAddonPath=\nDisabledAddons=\n\n[GENERAL]\nEffectSearchPaths=\nTextureSearchPaths=\nPresetPath=\n\n[SCREENSHOT]\nSavePath=\n";
            fs::write(&ini_path, minimal).map_err(|e| format!("无法写入 ReShade.ini：{e}"))?;
        }
    }

    let addons = paths::to_display(&shader_dir.join("Addons"));
    let shaders = paths::to_display(&shader_dir.join("Shaders"));
    let textures = paths::to_display(&shader_dir.join("Textures"));
    let preset = paths::to_display(&preset_path);
    let shots = paths::to_display(&screenshots);

    let write = |section: &str, key: &str, value: &str| -> Result<(), String> {
        ini::set(&ini_path, section, key, value)
            .map_err(|e| format!("无法写入 ReShade.ini（{section}.{key}）：{e}"))
    };
    write("ADDON", "AddonPath", &addons)?;
    write("GENERAL", "EffectSearchPaths", &shaders)?;
    write("GENERAL", "TextureSearchPaths", &textures)?;

    let preset_now = ini::get(&ini_path, "GENERAL", "PresetPath").unwrap_or_default();
    if preset_now.trim().is_empty() {
        write("GENERAL", "PresetPath", &preset)?;
    }
    let save_now = ini::get(&ini_path, "SCREENSHOT", "SavePath").unwrap_or_default();
    if save_now.trim().is_empty() {
        write("SCREENSHOT", "SavePath", &shots)?;
    }

    if !preset_path.is_file() {
        let preset_template = paths::payload_dir().join("default_config").join("ReShadePreset.ini");
        if preset_template.is_file() {
            fs::copy(&preset_template, &preset_path)
                .map_err(|e| format!("无法写入 ReShadePreset.ini：{e}"))?;
        } else {
            fs::write(&preset_path, "PreprocessorDefinitions=\nTechniques=\nTechniqueSorting=\n")
                .map_err(|e| format!("无法写入 ReShadePreset.ini：{e}"))?;
        }
    }

    // 旧布局会把 ini 放在 payload\ReShade 里，残留会被 ReShade 误读。
    for stale in ["ReShade.ini", "ReShadePreset.ini"] {
        let path = paths::payload_dir().join("ReShade").join(stale);
        if path.is_file() {
            let _ = fs::remove_file(&path);
            log::write(&format!("清理旧位置残留: {}", path.display()));
        }
    }
    Ok(())
}

/// 现有 ReShade.ini 是否已经指向真实的效果文件目录（是就不再覆写正文）。
fn ini_has_effects(ini_path: &Path, game_dir: &Path) -> bool {
    let Some(value) = ini::get(ini_path, "GENERAL", "EffectSearchPaths") else {
        return false;
    };
    for part in value.split(';') {
        let raw = part.trim().trim_matches('"');
        if raw.is_empty() {
            continue;
        }
        let expanded = crate::paths::expand_env(raw);
        let path = Path::new(&expanded);
        let path = if path.is_absolute() {
            path.to_path_buf()
        } else {
            game_dir.join(path)
        };
        if path.is_dir() && has_fx(&path, 0) {
            return true;
        }
    }
    false
}

fn has_fx(dir: &Path, depth: usize) -> bool {
    if depth > 6 {
        return false;
    }
    let Ok(entries) = fs::read_dir(dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(meta) = entry.metadata() else { continue };
        if meta.is_dir() {
            if has_fx(&path, depth + 1) {
                return true;
            }
        } else if path
            .extension()
            .map(|ext| ext.eq_ignore_ascii_case("fx"))
            .unwrap_or(false)
        {
            return true;
        }
    }
    false
}

