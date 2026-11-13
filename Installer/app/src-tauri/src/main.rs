#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod backdrop;
mod fetch;
mod framegen;
mod gamepath;
mod gpu;
mod gpu_tables;
mod i18n;
mod ini;
mod install;
mod launch;
mod log;
mod logs;
mod manifest;
mod paths;
mod pkg;
mod settings;
mod shortcut;
mod srinput;
mod update;
mod verify;

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};

pub struct AppState {
    settings: Mutex<settings::Settings>,
    busy: AtomicBool,
    cancel: AtomicBool,
    watch: AtomicBool,
}

fn send(app: &AppHandle, value: Value) {
    let _ = app.emit("msg", value.to_string());
}

fn snapshot(app: &AppHandle) -> settings::Settings {
    app.state::<AppState>()
        .settings
        .lock()
        .unwrap()
        .clone()
}

fn version_list(id: &str, settings: &settings::Settings) -> Vec<Value> {
    let current = pkg::chosen_dir(id, settings);
    pkg::versions_of(id)
        .iter()
        .map(|v| {
            json!({
                "dir": v.dir,
                "path": paths::to_display(&v.path),
                "componentDir": paths::to_display(&v.component_dir),
                "nested": v.nested,
                "current": v.dir.eq_ignore_ascii_case(&current),
            })
        })
        .collect()
}

fn post_state(app: &AppHandle) {
    let s = snapshot(app);
    let dlls = launch::dll_list();
    // 没选游戏目录就没有「已安装」可言：包内可能残留上次的 fps_config.json。
    let have_game = !s.game_dir.trim().is_empty()
        && gamepath::find_game_exe(std::path::Path::new(&s.game_dir)).is_some();
    let components: Vec<Value> = pkg::CATALOG
        .iter()
        .map(|c| {
            json!({
                "id": c.id,
                "name": c.name,
                "note": c.note,
                "bytes": pkg::component_bytes(c.id, &s),
                "installed": have_game && pkg::is_enabled(&dlls, c.id, &s),
                "present": pkg::present(c.id, &s),
                "mandatory": c.mandatory,
            })
        })
        .collect();

    send(
        app,
        json!({
            "type": "state",
            "dir": s.game_dir,
            "fps": s.fps,
            "selected": pkg::with_mandatory(&s.selected),
            "components": components,
            "payloadAvailable": pkg::payload_available(),
            "payloadDir": paths::to_display(&paths::payload_dir()),
            "exeDir": paths::to_display(&paths::exe_dir()),
            "logPath": paths::to_display(&paths::log_file()),
            "unlocking": launch::running(),
            "build": env!("GFSR_BUILD_DATE"),
            "version": update::local_version(),
            "backdrop": s.backdrop,
            "opacity": s.opacity,
            "packageVersion": paths::package_version(),
            "bridgeDir": pkg::chosen_dir("bridge", &s),
            "optiDir": pkg::chosen_dir("opti", &s),
            "versions": {
                "bridge": version_list("bridge", &s),
                "opti": version_list("opti", &s),
            },
            "unlocker": launch::unlocker_settings(),
            "srInput": {
                "value": srinput::effective(&s),
                "available": srinput::ini_path(&s).is_some(),
            },
        "frameGen": {
            // 三态：on / off / auto；没装 OptiScaler 时 available=false（UI 禁用开关）
            "value": framegen::snapshot(&s).unwrap_or("auto"),
            "available": framegen::ini_path(&s).is_some(),
        },
            "modsDir": s.mods_dir,
            "modsDefault": paths::to_display(&paths::payload_dir().join("TextureLoader\\Mods")),
            "textureIni": paths::to_display(&paths::payload_dir().join("TextureLoader\\TextureLoader.ini")),
            "custom": custom_entries(&s),
            "logs": logs::info(&s),
            "gpuVendor": gpu::vendor(),
            "suppressWarnings": s.suppress_warnings,
            "uiLang": s.ui_lang,
            "theme": s.theme,
            "desktopShortcut": s.desktop_shortcut,
            "unlockerDir": paths::to_display(&paths::unlocker_dir()),
            "shortcutName": shortcut::link_name(i18n::current() == "zh"),
        }),
    );
}

/// 自定义注入：只暴露路径、文件名、文件在不在与是否启用，注入链本身由 dll_list 负责。
fn custom_entries(s: &settings::Settings) -> Vec<Value> {
    s.custom_dlls
        .iter()
        .map(|raw| {
            let path = Path::new(raw);
            json!({
                "path": raw,
                "name": path.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_else(|| raw.clone()),
                "present": path.is_file(),
                "on": s.custom_enabled(raw),
            })
        })
        .collect()
}

/// 把当前设置重新落到 ini 与 fps_config.json 上（加/删自定义注入后调用）。
fn reapply(app: &AppHandle) -> String {
    let s = snapshot(app);
    let Some(game_exe) = gamepath::find_game_exe(Path::new(&s.game_dir)) else {
        return "先选择游戏目录".to_string();
    };
    let selected = pkg::with_mandatory(&s.selected);
    let cancel = AtomicBool::new(false);
    install::apply(&game_exe, s.fps, &selected, &s, &cancel, |_| {}).error
}

/// DLSS 运行时的落点：桥和 OptiScaler 各自按自己的规则找文件，所以不止一处。
/// - 桥：`NgxBackend::resolve_feature_dirs()` 在 `<payload>/Bridge/../NVIDIA/DLSS` 里找
///   `nvngx_dlss.dll` —— 只装桥（`Ffx12Backend=ngx`，或 RTX 上的 `auto`）时这条就是唯一来源；
///   它同时也是「包内自带」的位置，所以排第一：先补齐它，后面的目标就能走「payload 里已有」。
/// - OptiScaler：`NvngxDlssPath = auto` 相对**组件目录**解析（放版本根层在嵌套布局下找不到）。
fn dlss_targets(s: &settings::Settings) -> Vec<PathBuf> {
    let mut targets = vec![paths::payload_dir()
        .join("NVIDIA")
        .join("DLSS")
        .join("nvngx_dlss.dll")];
    if s.selected.iter().any(|id| id == "opti") {
        // 2026-11-12：这里以前用版本目录（`dll_path_of(...).parent()`），而
        // `prepare_opti` 写的 `Libraries.OptiDllPath` 是 `component_dir` —— 嵌套布局
        // （0.10 起的 `OptiScaler_v0.10.0\OptiScaler\`）下两者差一层，放版本根层等于没放。
        if let Some(version) = pkg::resolve("opti", s) {
            let path = version.component_dir.join("nvngx_dlss.dll");
            if !targets.iter().any(|t| t == &path) {
                targets.push(path);
            }
        }
    }
    targets
}

/// 需要这份运行时的两种情况：勾了 OptiScaler（它自己的 DLSS 扩展），或只装桥但桥会走 NGX。
/// GTX 16 / A 卡（没被判定成 RTX）不部署 DLSS。
fn dlss_applicable(s: &settings::Settings) -> bool {
    gpu::has_rtx()
        && (s.selected.iter().any(|id| id == "opti")
            || s.selected.iter().any(|id| id == "bridge") && srinput::uses_ngx(s))
}

/// 补齐失败时**弹窗**（不再只是一条 toast）：把目标路径一起给界面，用户能直接打开目录手放。
fn report_dlss_error(app: &AppHandle, target: &Path, reason: &str) {
    log::write(&format!("[dlss] 补齐失败: {reason}"));
    send(
        app,
        json!({
            "type": "dlssError",
            "message": reason,
            "target": paths::to_display(target),
        }),
    );
}

/// 已存在即跳过、payload 有就同步复制、都没有才联网补；多个落点逐个处理（下载只下一次）。
///
/// 五个时机共用这一个入口：**启动 / 保存 / 安装成功 / 切换 OptiScaler 版本 / 改超分输入类型**。
fn ensure_dlss(app: &AppHandle, s: &settings::Settings) {
    if !dlss_applicable(s) {
        return;
    }
    let pending: Vec<PathBuf> = dlss_targets(s)
        .into_iter()
        .filter(|target| !target.is_file())
        .collect();
    if pending.is_empty() {
        return;
    }
    let count = pending.len();

    if paths::payload_dir()
        .join("NVIDIA")
        .join("DLSS")
        .join("nvngx_dlss.dll")
        .is_file()
    {
        for target in pending {
            match fetch::install_bundled(&target) {
                Ok(()) => log::write(&format!(
                    "[dlss] 已从 payload 补齐 nvngx_dlss.dll -> {}",
                    paths::to_display(&target)
                )),
                Err(err) => {
                    report_dlss_error(app, &target, &format!("复制失败：{err}"));
                    return;
                }
            }
        }
        let message = if i18n::current() == "zh" {
            "已补齐 NVIDIA DLSS 组件（payload 里已有）".to_string()
        } else {
            "NVIDIA DLSS runtime installed (bundled)".to_string()
        };
        send(
            app,
            json!({"type": "fetch", "id": "dlss", "status": "done", "message": message}),
        );
        return;
    }

    // 包内没有（GitHub 发布包常态）：后台联网补齐，失败必须让用户看见。
    let app_handle = app.clone();
    std::thread::spawn(move || {
        let reporter = app_handle.clone();
        let report = move |text: &str| {
            send(
                &reporter,
                json!({"type": "fetch", "id": "dlss", "status": "working", "message": text}),
            );
        };
        let mut source = String::new();
        for target in pending {
            match fetch::fetch_dlss(&target, &report) {
                Ok(from) => source = from,
                Err(err) => {
                    report_dlss_error(&app_handle, &target, &format!("下载失败：{err}"));
                    return;
                }
            }
        }
        log::write(&format!(
            "[dlss] 已补齐 nvngx_dlss.dll（{source}）到 {count} 个位置"
        ));
        let message = if i18n::current() == "zh" {
            format!("已补齐 NVIDIA DLSS 组件（{source}）")
        } else {
            format!("NVIDIA DLSS runtime installed ({source})")
        };
        send(
            &app_handle,
            json!({"type": "fetch", "id": "dlss", "status": "done", "message": message}),
        );
    });
}

/// 补齐 OptiScaler 的 Streamline 运行时（DLSSG 帧生成依赖它）。
///
/// OptiScaler 从**自己组件目录下的 `streamline\`** 加载这几个文件（路径硬编码）：
/// `sl.interposer.dll` / `sl.common.dll` / `nvngx_dlssg.dll` /
/// `sl.dlss_g.dll` / `sl.reflex.dll` / `sl.pcl.dll`。
///
/// 缺了会报 "Streamline proxy can't find sl.interposer.dll!"，界面上是
/// "Can't init DLSSG Output / Are you missing the streamline folder?"
///
/// 只在**可能用到 DLSSG** 时补：勾了 OptiScaler + 帧生成开着（`FrameGen.FGOutput` 是
/// `DLSSG`，或 `auto` 且这张卡能跑 DLSS）。不做无条件下载 —— 那个 SDK 包有 263 MB，
/// 用不上的用户不该为它付流量。
fn ensure_streamline(app: &AppHandle, s: &settings::Settings) {
    if !s.selected.iter().any(|id| id == "opti") {
        return;
    }
    // 帧生成没开就不用 DLSSG，也就不需要 Streamline。
    if !framegen::wants_dlssg(s) {
        return;
    }
    let Some(version) = pkg::resolve("opti", s) else {
        return;
    };
    let dest = version.component_dir.join("streamline");
    if fetch::streamline_ready(&dest) {
        return;
    }

    let app_handle = app.clone();
    let dest_for_thread = dest.clone();
    let settings_for_thread = s.clone();
    std::thread::spawn(move || {
        let report = |text: &str| {
            send(
                &app_handle,
                json!({
                    "type": "fetch",
                    "id": "streamline",
                    "status": "progress",
                    "message": text,
                }),
            );
        };
        let result = fetch::fetch_streamline(&dest_for_thread, &report);
        match result {
            Ok(source) => {
                log::write(&format!(
                    "[streamline] 已补齐 -> {}（{source}）",
                    paths::to_display(&dest_for_thread)
                ));
                send(
                    &app_handle,
                    json!({
                        "type": "fetch",
                        "id": "streamline",
                        "status": "done",
                        "message": format!("Streamline 运行时已就绪（{source}）"),
                    }),
                );
            }
            Err(err) => {
                log::write(&format!("[streamline] 补齐失败: {err}"));
                // 失败必须让用户看见：帧生成会直接不可用。
                send(
                    &app_handle,
                    json!({
                        "type": "notice",
                        "level": "error",
                        "title": "Streamline 运行时缺失",
                        "message": format!(
                            "DLSS 帧生成需要 Streamline 运行时，补齐失败：{err}\n\n可以手动把\
                             sl.interposer.dll / sl.common.dll / nvngx_dlssg.dll / sl.dlss_g.dll / \
                             sl.reflex.dll / sl.pcl.dll 放进：\n{}",
                            paths::to_display(&dest_for_thread)
                        ),
                    }),
                );
            }
        }
        let _ = &settings_for_thread;
    });
}

/// 首次装 OptiScaler 且显卡属于「官方 FSR4 要新驱动」的两类平台（RDNA2 独显 / RDNA3.5 核显）：
/// 提示把 Adrenalin 更新到 26.10.1+。每台机器只弹一次，记在 settings.json 的
/// `fsr4DriverNoticeShown`，不跟 N 卡的「不再提醒」混在一起。
fn maybe_notice_fsr4_driver(app: &AppHandle, selected: &[String]) {
    if !selected.iter().any(|id| id == "opti") || !gpu::needs_fsr4_driver_notice() {
        return;
    }
    let st = app.state::<AppState>();
    let (settings_now, name, platform) = {
        let mut guard = st.settings.lock().unwrap();
        if guard.fsr4_driver_notice_shown {
            return;
        }
        guard.fsr4_driver_notice_shown = true;
        let info = gpu::info();
        (guard.clone(), info.name.clone(), info.platform.id())
    };
    settings::save(&settings_now);
    log::write(&format!(
        "[notice] 首次安装 OptiScaler：提示升级驱动（platform={platform}, gpu={name}）"
    ));
    send(
        app,
        json!({
            "type": "fsr4DriverNotice",
            "gpu": name,
            "platform": platform,
            "minDriver": "26.10.1",
        }),
    );
}

fn take_selection(msg: &Value) -> Vec<String> {    let ids: Vec<String> = msg["selected"]
        .as_array()
        .map(|list| {
            list.iter()
                .filter_map(|v| v.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default();
    pkg::with_mandatory(&ids)
}

fn on_save(app: &AppHandle, msg: &Value) {
    let dir = msg["dir"].as_str().unwrap_or_default().to_string();
    let fps = msg["fps"].as_i64().unwrap_or(240) as i32;
    let selected = take_selection(msg);

    let st = app.state::<AppState>();
    let (previous_dlls, settings_now) = {
        let mut guard = st.settings.lock().unwrap();
        let previous = launch::dll_list();
        if !dir.is_empty() {
            guard.game_dir = dir;
        }
        guard.fps = fps.clamp(10, 1000);
        guard.selected = selected.clone();
        (previous, guard.clone())
    };
    let ok = settings::save(&settings_now);

    // 保存 = 重写注入链。取消勾选只是把它从 DllList 里去掉，文件全部留在 payload。
    let mut removed: Vec<String> = Vec::new();
    let mut failed: Vec<Value> = Vec::new();
    let game_dir = PathBuf::from(&settings_now.game_dir);
    match gamepath::find_game_exe(&game_dir) {
        Some(game_exe) => {
            st.cancel.store(false, Ordering::SeqCst);
            let outcome =
                install::apply(&game_exe, settings_now.fps, &selected, &settings_now, &st.cancel, |_| {});
            if outcome.ok {
                let now_dlls = launch::dll_list();
                for component in pkg::CATALOG {
                    if component.id == "fpsun" {
                        continue;
                    }
                    if pkg::is_enabled(&previous_dlls, component.id, &settings_now)
                        && !pkg::is_enabled(&now_dlls, component.id, &settings_now)
                    {
                        log::write(&format!("已从注入链移除: {}", component.id));
                        removed.push(component.id.to_string());
                    }
                }
            } else if !outcome.error.is_empty() {
                failed.push(json!({ "id": outcome.failed_component, "errors": [outcome.error] }));
            }
        }
        None if !settings_now.game_dir.is_empty() => {
            failed.push(json!({ "id": "fpsun", "errors": ["游戏目录里找不到 YuanShen.exe"] }));
        }
        None => {}
    }


    // 勾了 OptiScaler + RTX 卡：把 nvngx_dlss.dll 补进组件目录（后台线程，不挡界面）。
    ensure_dlss(app, &settings_now);
     ensure_streamline(app, &settings_now);

    // 先刷新状态（行里的「已安装 / 未选择」跟着变），再发 saved，底栏提示才不会被覆盖。
    post_state(app);
    send(
        app,
        json!({ "type": "saved", "ok": ok, "removed": removed, "failed": failed }),
    );
}

/// 「插件恢复默认设置」：用 default_config 里的基线覆盖各组件的 ini，再把托管键与注入链写一遍。
fn on_restore_defaults(app: &AppHandle) {
    let st = app.state::<AppState>();
    if st.busy.load(Ordering::SeqCst) {
        send(
            app,
            json!({ "type": "restored", "ok": false, "error": "正在写入，稍后再试" }),
        );
        return;
    }
    let settings_now = {
        let guard = st.settings.lock().unwrap();
        guard.clone()
    };
    let selected = pkg::with_mandatory(&settings_now.selected);
    let game_dir = PathBuf::from(&settings_now.game_dir);
    let game_exe = gamepath::find_game_exe(&game_dir);

    let mut files: Vec<String> = Vec::new();
    let mut error = String::new();
    match install::restore_defaults(game_exe.as_deref(), &selected, &settings_now) {
        Ok(restored) => files = restored,
        Err(message) => error = message,
    }
    if error.is_empty() {
        // 基线里的 ini 不含本机路径，写完基线必须再补一遍托管键和注入链。
        if let Some(exe) = game_exe.as_deref() {
            st.cancel.store(false, Ordering::SeqCst);
            let outcome = install::apply(
                exe,
                settings_now.fps,
                &selected,
                &settings_now,
                &st.cancel,
                |_| {},
            );
            if !outcome.ok {
                error = outcome.error;
            }
        }
    }
    if error.is_empty() {
        let st = app.state::<AppState>();
        {
            let mut guard = st.settings.lock().unwrap();
            if !guard.mods_dir.is_empty() {
                guard.mods_dir.clear();
                let cleared = guard.clone();
                drop(guard);
                settings::save(&cleared);
            }
        }
    }

    post_state(app);
    send(
        app,
        json!({ "type": "restored", "ok": error.is_empty(), "files": files, "error": error }),
    );
}

fn on_install(app: &AppHandle, msg: &Value) {
    let st = app.state::<AppState>();
    if st.busy.swap(true, Ordering::SeqCst) {
        send(app, json!({ "type": "error", "message": "已有安装任务在进行" }));
        return;
    }

    let dir = msg["dir"].as_str().unwrap_or_default().to_string();
    let fps = msg["fps"].as_i64().unwrap_or(240) as i32;
    let selected = take_selection(msg);

    let settings_now = {
        let mut guard = st.settings.lock().unwrap();
        if !dir.is_empty() {
            guard.game_dir = dir;
        }
        guard.fps = fps.clamp(10, 1000);
        guard.selected = selected.clone();
        guard.clone()
    };
    settings::save(&settings_now);

    let game_dir = PathBuf::from(&settings_now.game_dir);
    let Some(game_exe) = gamepath::find_game_exe(&game_dir) else {
        st.busy.store(false, Ordering::SeqCst);
        send(
            app,
            json!({ "type": "installed", "ok": false, "error": "请先选择原神安装目录" }),
        );
        return;
    };
    if !pkg::payload_available() {
        st.busy.store(false, Ordering::SeqCst);
        send(
            app,
            json!({ "type": "installed", "ok": false, "error": "安装器缺少组件文件（payload 目录）" }),
        );
        return;
    }

    let missing = pkg::missing_files(&selected, &settings_now);
    if !missing.is_empty() {
        st.busy.store(false, Ordering::SeqCst);
        let mut ids: Vec<String> = Vec::new();
        for entry in &missing {
            let id = entry.split(':').next().unwrap_or(entry).trim().to_string();
            if !ids.contains(&id) {
                ids.push(id);
            }
        }
        send(
            app,
            json!({
                "type": "installed",
                "ok": false,
                "error": format!("缺少 {} 个组件文件，可在组件右侧点「下载」联网补齐", missing.len()),
                "missing": ids,
            }),
        );
        return;
    }

    let fps_now = settings_now.fps;
    let settings_for_thread = settings_now.clone();
    let app_handle = app.clone();
    std::thread::spawn(move || {
        let st = app_handle.state::<AppState>();
        st.cancel.store(false, Ordering::SeqCst);
        let mut last = Instant::now();
        let outcome = install::apply(
            &game_exe,
            fps_now,
            &selected,
            &settings_for_thread,
            &st.cancel,
            |p| {
            let is_final = p.index == p.total;
            if !is_final && last.elapsed() < Duration::from_millis(60) {
                return;
            }
            last = Instant::now();
            send(
                &app_handle,
                json!({
                    "type": "progress",
                    "index": p.index,
                    "total": p.total,
                    "component": p.component,
                    "label": p.label,
                }),
            );
        });

        send(
            &app_handle,
            json!({
                "type": "installed",
                "ok": outcome.ok,
                "cancelled": outcome.cancelled,
                "error": outcome.error,
                "failedComponent": outcome.failed_component,
                "installed": launch::enabled_flags(&launch::dll_list(), &settings_for_thread),
            }),
        );
        if outcome.ok && !outcome.cancelled {
            ensure_shortcut(&settings_for_thread);
            ensure_dlss(&app_handle, &settings_for_thread);
             ensure_streamline(&app_handle, &settings_for_thread);
            // 提示放最后：先把「已安装」推给界面，弹窗再来。
            maybe_notice_fsr4_driver(&app_handle, &selected);
        }
        st.busy.store(false, Ordering::SeqCst);
    });
}

fn on_fetch_component(app: &AppHandle, msg: &Value) {
    let id = msg["id"].as_str().unwrap_or_default().to_string();
    if pkg::find(&id).is_none() {
        return;
    }
    let st = app.state::<AppState>();
    if st.busy.swap(true, Ordering::SeqCst) {
        send(
            app,
            json!({ "type": "fetch", "id": id, "status": "error", "message": "正在写入组件，稍后再试" }),
        );
        return;
    }
    let settings_now = st
        .settings
        .lock()
        .map(|s| s.clone())
        .unwrap_or_else(|e| e.into_inner().clone());
    send(
        app,
        json!({ "type": "fetch", "id": id, "status": "start", "message": "准备下载…" }),
    );

    let app_handle = app.clone();
    let thread_id = id.clone();
    std::thread::spawn(move || {
        let report = |text: &str| {
            send(
                &app_handle,
                json!({ "type": "fetch", "id": thread_id, "status": "working", "message": text }),
            );
        };
        let result = fetch::fetch_component(&thread_id, &settings_now, &report);
        match result {
            Ok(copied) => {
                log::write(&format!("已补齐组件 {thread_id}: {} 个文件", copied.len()));
                post_state(&app_handle);
                send(
                    &app_handle,
                    json!({
                        "type": "fetch",
                        "id": thread_id,
                        "status": "done",
                        "count": copied.len(),
                    }),
                );
            }
            Err(error) => {
                log::write(&format!("[fetch] {thread_id} 失败: {error}"));
                send(
                    &app_handle,
                    json!({ "type": "fetch", "id": thread_id, "status": "error", "message": error }),
                );
            }
        }
        app_handle
            .state::<AppState>()
            .busy
            .store(false, Ordering::SeqCst);
    });
}

fn on_launch(app: &AppHandle, msg: &Value) {
    let dir = msg["dir"].as_str().unwrap_or_default().to_string();
    let fps = msg["fps"].as_i64().unwrap_or(240) as i32;
    let selected = take_selection(msg);

    let settings_now = {
        let st = app.state::<AppState>();
        let mut guard = st.settings.lock().unwrap();
        if !dir.is_empty() {
            guard.game_dir = dir;
        }
        guard.fps = fps.clamp(10, 1000);
        guard.selected = selected.clone();
        guard.clone()
    };
    settings::save(&settings_now);

    let game_dir = PathBuf::from(&settings_now.game_dir);
    let Some(game_exe) = gamepath::find_game_exe(&game_dir) else {
        send(
            app,
            json!({ "type": "unlock", "status": "error", "message": "请先选择原神安装目录" }),
        );
        return;
    };

    send(
        app,
        json!({ "type": "unlock", "status": "launching", "message": "正在启动解帧工具" }),
    );

    let dlls = pkg::dll_list(&selected, &settings_now);
    if let Err(error) =
        launch::ensure_tool().and_then(|_| launch::write_config(&game_exe, settings_now.fps, &dlls))
    {
        send(
            app,
            json!({ "type": "unlock", "status": "error", "message": error }),
        );
        return;
    }

    match launch::launch() {
        Ok(_) => send(
            app,
            json!({ "type": "unlock", "status": "started", "message": "解帧工具已启动 · 游戏马上打开" }),
        ),
        Err(error) => {
            send(
                app,
                json!({ "type": "unlock", "status": "error", "message": error }),
            );
            return;
        }
    }

    // 工具自己拉起游戏并随游戏退出；这里只把「退出」回报给界面。
    let st = app.state::<AppState>();
    st.watch.store(false, Ordering::SeqCst);
    let app_handle = app.clone();
    std::thread::spawn(move || {
        let st = app_handle.state::<AppState>();
        while !st.watch.load(Ordering::SeqCst) && launch::running() {
            std::thread::sleep(Duration::from_millis(400));
        }
        if st.watch.load(Ordering::SeqCst) {
            return;
        }
        launch::clear();
        send(
            &app_handle,
            json!({ "type": "unlock", "status": "exited", "message": "解帧工具已退出" }),
        );
        post_state(&app_handle);
    });
}

/// 切换 Bridge / OptiScaler 的版本目录：改设置 → 重写该版本的 ini →
/// 重写注入链（有游戏目录时）。
fn on_switch_version(app: &AppHandle, msg: &Value) {
    let component = msg["component"].as_str().unwrap_or_default().to_string();
    let dir = msg["dir"].as_str().unwrap_or_default().to_string();
    if !pkg::is_versioned(&component) || dir.is_empty() {
        send(
            app,
            json!({ "type": "switched", "ok": false, "error": "未知的组件" }),
        );
        return;
    }
    let available = pkg::versions_of(&component);
    let Some(version) = available.iter().find(|v| v.dir.eq_ignore_ascii_case(&dir)) else {
        send(
            app,
            json!({ "type": "switched", "ok": false, "error": format!("payload 里没有 {dir}") }),
        );
        return;
    };
    let chosen = version.dir.clone();

    let settings_now = {
        let st = app.state::<AppState>();
        let mut guard = st.settings.lock().unwrap();
        if component == "bridge" {
            guard.bridge_dir = chosen.clone();
        } else {
            guard.opti_dir = chosen.clone();
        }
        guard.clone()
    };
    settings::save(&settings_now);
    log::write(&format!("切换版本: {component} -> {chosen}"));

    // 第一次见到这版就把它的 ini 留一份当出厂基线（已存在则不动）。
    let _ = install::snapshot_baseline(&component, &settings_now);

    // 只写**被切换的那一个**组件：其他插件的 ini 一概不动。
    let selected = pkg::with_mandatory(&settings_now.selected);
    let mut error = String::new();
    if let Err(message) = install::prepare_component_ini(&component, &settings_now) {
        log::write(&format!("[switch] {component}: {message}"));
        error = message;
    }
    if error.is_empty() && pkg::payload_available() {
        // 注入链里的绝对路径跟着版本目录换了：只改 DllList（不动 FPSTarget / UseHDR）。
        let dlls = pkg::dll_list(&selected, &settings_now);
        if let Err(message) = launch::repair_paths(None, Some(&dlls)) {
            log::write(&format!("[switch] 注入链: {message}"));
            error = message;
        }
    }
    if error.is_empty() {
        // 换到新版本目录后，那个目录可能还没有 DLSS 运行时。
        ensure_dlss(app, &settings_now);
         ensure_streamline(app, &settings_now);
    }

    post_state(app);
    send(
        app,
        json!({
            "type": "switched",
            "component": component,
            "dir": chosen,
            "ok": error.is_empty(),
            "error": error,
        }),
    );
}

/// 主页「超分输入类型」：只改**已安装那份** `Dx11FsrBridge.ini` 的 `Ffx12Backend`。
/// 规则跟随桥（只认精确小写的 auto / native / ngx），安装器不替它纠错；写成功后再补一次
/// `nvngx_dlss.dll` —— 切到 ngx（或 RTX 上的 auto）时桥需要它，不能等下次安装。
fn on_sr_input(app: &AppHandle, msg: &Value) {
    let value = msg["value"].as_str().unwrap_or_default().to_string();
    let s = snapshot(app);
    match srinput::write(&s, &value) {
        Ok(()) => {
            log::write(&format!("[bridge] 超分输入类型 -> {value}"));
            post_state(app);
            ensure_dlss(app, &s);
             ensure_streamline(app, &s);
        }
        Err(err) => {
            log::write(&format!("[bridge] 写 Ffx12Backend 失败: {err}"));
            send(app, json!({ "type": "error", "message": err }));
        }
    }
}

/// 「帧生成」开关。三态：开（按显卡选 DLSSG / FSRFG）/ 关（NoFG）/ 自动（auto）。
///
/// 改 `OptiScaler.ini` 的 `[FrameGen] FGInput` + `FGOutput`，**并重新应用注入链** ——
/// 因为帧生成状态会改变 ReShade 的加载方式（见 `pkg::reshade_via_opti`）：
///   · 开 ⇒ ReShade 由 OptiScaler 加载（`LoadReshade=true`），不进 DllList；
///   · 关 ⇒ ReShade 走外部注入（进 DllList），`LoadReshade=false`。
/// 不重跑应用的话，用户拨了开关但注入链还是旧的 ⇒ 表现与开关不符。
fn on_frame_gen(app: &AppHandle, msg: &Value) {
    // value 只有 "on" / "off" —— 界面 2026-11-14 起不再有 auto 档。
    // 非 "off" 一律当 "on"（兼容老界面缓存 / 手改消息，且偏保守：开比关更容易被发现）。
    let value = msg["value"].as_str().unwrap_or("off");
    let on = !value.eq_ignore_ascii_case("off");
    let s = snapshot(app);
    let state = match framegen::write(&s, on) {
        Ok(state) => state,
        Err(err) => {
            log::write(&format!("[install] 写帧生成开关失败: {err}"));
            send(app, json!({ "type": "error", "message": err }));
            return;
        }
    };
    log::write(&format!("[install] 帧生成 -> {}", state.as_str()));

    // 帧生成状态变了 ⇒ ReShade 加载方式可能跟着变 ⇒ 必须重写注入链。
    // 游戏还没选目录时跳过（此时也没东西可应用），只更新界面。
    let game_dir = PathBuf::from(&s.game_dir);
    if !game_dir.as_os_str().is_empty() {
        if let Some(game_exe) = gamepath::find_game_exe(&game_dir) {
            let st = app.state::<AppState>();
            st.cancel.store(false, Ordering::SeqCst);
            let outcome =
                install::apply(&game_exe, s.fps, &s.selected, &s, &st.cancel, |_| {});
            if !outcome.ok {
                log::write(&format!(
                    "[install] 帧生成切换后重应用失败: {}",
                    outcome.error
                ));
                send(
                    app,
                    json!({ "type": "error", "message": outcome.error }),
                );
            }
        }
    }

    post_state(app);
}


/// 解帧工具选项（fps_config.json 里除 GamePath/FPSTarget/DllList/UseHDR 之外的键）。
fn on_unlocker_config(app: &AppHandle, msg: &Value) {
    let patch = msg.get("patch").cloned().unwrap_or_else(|| json!({}));
    match launch::patch_config(&patch) {
        Ok(config) => {
            send(app, json!({ "type": "unlocker", "ok": true, "config": config }));
        }
        Err(error) => {
            log::write(&format!("[unlocker] {error}"));
            send(
                app,
                json!({ "type": "unlocker", "ok": false, "error": error }),
            );
        }
    }
}

fn on_mods_dir(app: &AppHandle, dir: Option<String>) {
    let value = dir.unwrap_or_default().trim().to_string();
    if !value.is_empty() && !std::path::Path::new(&value).is_dir() {
        send(
            app,
            json!({ "type": "mods", "ok": false, "error": "这个目录不存在" }),
        );
        return;
    }
    match install::write_mods_dir(&value) {
        Ok(written) => {
            app.state::<AppState>().settings.lock().unwrap().mods_dir = written.clone();
            settings::save(&snapshot(app));
            // 先推状态再回消息：state 会重刷底栏，反过来的话提示会被立刻覆盖
            post_state(app);
            send(app, json!({ "type": "mods", "ok": true, "dir": written }));
        }
        Err(error) => {
            log::write(&format!("[mods] {error}"));
            send(app, json!({ "type": "mods", "ok": false, "error": error }));
        }
    }
}

fn on_pick_custom_dll(app: &AppHandle) {
    if snapshot(app).game_dir.trim().is_empty() {
        send(
            app,
            json!({ "type": "custom", "ok": false, "error": "先选择游戏目录" }),
        );
        return;
    }
    let picked = rfd::FileDialog::new()
        .set_title("选择要注入的 DLL")
        .add_filter("DLL", &["dll"])
        .pick_file();
    let Some(path) = picked else {
        send(app, json!({ "type": "custom", "ok": true, "cancelled": true }));
        return;
    };
    on_add_custom_dll(app, &paths::to_display(&path));
}

/// 把某个 DLL 加进注入链末尾（消息里带了 path 就跳过文件对话框）。
fn on_add_custom_dll(app: &AppHandle, raw: &str) {
    let text = raw.trim().to_string();
    if text.is_empty() || !Path::new(&text).is_file() {
        send(
            app,
            json!({ "type": "custom", "ok": false, "error": "这个文件不存在" }),
        );
        return;
    }
    let is_dll = Path::new(&text)
        .extension()
        .map(|e| e.eq_ignore_ascii_case("dll"))
        .unwrap_or(false);
    if !is_dll {
        send(
            app,
            json!({ "type": "custom", "ok": false, "error": "只能注入 .dll 文件" }),
        );
        return;
    }
    if snapshot(app).game_dir.trim().is_empty() {
        send(
            app,
            json!({ "type": "custom", "ok": false, "error": "先选择游戏目录" }),
        );
        return;
    }
    {
        let st = app.state::<AppState>();
        let mut guard = st.settings.lock().unwrap();
        if !guard
            .custom_dlls
            .iter()
            .any(|p| p.eq_ignore_ascii_case(&text))
        {
            guard.custom_dlls.push(text.clone());
        }
        guard.set_custom_enabled(&text, true);
    }
    settings::save(&snapshot(app));
    log::write(&format!("加入自定义注入: {text}"));
    let error = reapply(app);
    post_state(app);
    send(
        app,
        json!({ "type": "custom", "ok": error.is_empty(), "path": text, "error": error }),
    );
}

fn on_remove_custom_dll(app: &AppHandle, msg: &Value) {
    let path = msg["path"].as_str().unwrap_or_default().to_string();
    {
        let st = app.state::<AppState>();
        let mut guard = st.settings.lock().unwrap();
        guard
            .custom_dlls
            .retain(|p| !p.eq_ignore_ascii_case(&path));
        guard.set_custom_enabled(&path, false);
    }
    settings::save(&snapshot(app));
    log::write(&format!("移除自定义注入: {path}"));
    let error = reapply(app);
    post_state(app);
    send(
        app,
        json!({ "type": "custom", "ok": error.is_empty(), "removed": path, "error": error }),
    );
}

/// 勾选/取消勾选一个自定义注入：和自带插件一样改的是注入链，文件本身不动。
fn on_set_custom_on(app: &AppHandle, msg: &Value) {
    let path = msg["path"].as_str().unwrap_or_default().to_string();
    let on = msg["on"].as_bool().unwrap_or(true);
    if path.is_empty() {
        return;
    }
    {
        let st = app.state::<AppState>();
        let mut guard = st.settings.lock().unwrap();
        if !guard.custom_dlls.iter().any(|p| p.eq_ignore_ascii_case(&path)) {
            drop(guard);
            send(
                app,
                json!({ "type": "custom", "ok": false, "error": "这个自定义注入不在列表里" }),
            );
            return;
        }
        guard.set_custom_enabled(&path, on);
    }
    settings::save(&snapshot(app));
    log::write(&format!(
        "自定义注入 {}: {path}",
        if on { "启用" } else { "停用" }
    ));
    let error = reapply(app);
    post_state(app);
    send(
        app,
        json!({ "type": "custom", "ok": error.is_empty(), "path": path, "on": on, "error": error }),
    );
}

fn on_window(window: &WebviewWindow, action: &str) {
    match action {
        "close" => {
            let _ = window.close();
        }
        "minimize" => {
            let _ = window.minimize();
        }
        "maximize" => {
            if window.is_maximized().unwrap_or(false) {
                let _ = window.unmaximize();
            } else {
                let _ = window.maximize();
            }
        }
        "drag" => {
            let _ = window.start_dragging();
        }
        _ => {}
    }
}

fn open_in_explorer(path: &std::path::Path) {
    if path.as_os_str().is_empty() {
        return;
    }
    let _ = std::process::Command::new("explorer").arg(path).spawn();
}

/// 插件包发布页：整包更新入口（不做单插件在线更新）。
const RELEASES_URL: &str = "https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases";

fn on_open_path(app: &AppHandle, what: &str) {
    if what == "releases" {
        let _ = std::process::Command::new("rundll32")
            .args(["url.dll,FileProtocolHandler", RELEASES_URL])
            .spawn();
        return;
    }
    let s = snapshot(app);
    let target = match what {
        "folder" => PathBuf::from(&s.game_dir),
        "log" => paths::log_file(),
        "payload" => paths::payload_dir(),
        "opti" => pkg::dll_path_of("opti", &s)
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| paths::payload_dir()),
        "config" => paths::config_dir(),
        _ => return,
    };
    if let Some(parent) = target.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    open_in_explorer(&target);
}

/// 打开某个组件最新一份日志；还没有日志就退一步打开它所在的目录。
fn on_open_log(app: &AppHandle, id: &str) {
    let s = snapshot(app);
    if let Some(file) = logs::newest_file(id, &s) {
        let _ = std::process::Command::new("rundll32")
            .args(["url.dll,FileProtocolHandler", &paths::to_display(&file)])
            .spawn();
        return;
    }
    match logs::source_dir(id, &s) {
        Some(dir) if dir.is_dir() => open_in_explorer(&dir),
        _ => send(
            app,
            json!({ "type": "logs", "ok": false, "error": "这个组件还没有生成日志" }),
        ),
    }
}

/// 把四个来源的日志打成一个 zip；没给路径就弹保存对话框让用户选。
fn on_bundle_logs(app: &AppHandle, msg: &Value) {
    let s = snapshot(app);
    let mut dest = match msg["path"].as_str() {
        Some(p) if !p.trim().is_empty() => PathBuf::from(p),
        _ => {
            let home = std::env::var_os("USERPROFILE")
                .map(PathBuf::from)
                .unwrap_or_default();
            let desktop = home.join("Desktop");
            let start = if desktop.is_dir() {
                desktop
            } else {
                paths::config_dir()
            };
            match rfd::FileDialog::new()
                .set_title("保存日志压缩包")
                .set_file_name(logs::archive_name())
                .add_filter("ZIP 压缩包", &["zip"])
                .set_directory(start)
                .save_file()
            {
                Some(path) => path,
                None => {
                    send(app, json!({ "type": "logs", "cancelled": true }));
                    return;
                }
            }
        }
    };
    if dest.extension().is_none() {
        dest.set_extension("zip");
    }
    match logs::bundle(&dest, &s) {
        Ok((count, names)) => {
            log::write(&format!("日志已打包: {} ({} 个文件)", paths::to_display(&dest), count));
            send(
                app,
                json!({
                    "type": "logs",
                    "ok": true,
                    "path": paths::to_display(&dest),
                    "count": count,
                    "files": names,
                }),
            )
        }
        Err(error) => {
            log::write(&format!("日志打包失败: {error}"));
            send(app, json!({ "type": "logs", "ok": false, "error": error }));
        }
    }
}

fn on_appearance(window: &WebviewWindow, app: &AppHandle, msg: &Value) {
    let st = app.state::<AppState>();
    let (backdrop_want, opacity) = {
        let mut guard = st.settings.lock().unwrap();
        if let Some(mode) = msg["backdrop"].as_str() {
            guard.backdrop = mode.to_string();
        }
        if let Some(value) = msg["opacity"].as_i64() {
            guard.opacity = (value as i32).clamp(40, 100);
        }
        guard.clamp();
        let snapshot = guard.clone();
        (snapshot.backdrop.clone(), snapshot.opacity)
    };
    settings::save(&snapshot(app));
    let applied = backdrop::apply(window, &backdrop_want);
    send(
        app,
        json!({ "type": "appearance", "applied": applied, "opacity": opacity }),
    );
}

/// 查 GitHub 最新版本（走系统代理与加速线路，见 fetch 模块）。
fn on_check_update(app: &AppHandle) {
    send(
        app,
        json!({ "type": "update", "phase": "checking", "message": "正在查询 GitHub 最新版本…" }),
    );
    let handle = app.clone();
    std::thread::spawn(move || {
        let info = update::check();
        send(&handle, json!({ "type": "update", "phase": "checked", "info": info }));
    });
}

/// 下载整包并覆盖组件。
fn on_apply_update(app: &AppHandle) {
    let st = app.state::<AppState>();
    if st.busy.swap(true, Ordering::SeqCst) {
        send(
            app,
            json!({ "type": "update", "phase": "error", "message": "正在写入组件，稍后再试" }),
        );
        return;
    }
    send(
        app,
        json!({ "type": "update", "phase": "working", "message": "正在查询最新版本…" }),
    );

    let handle = app.clone();
    std::thread::spawn(move || {
        let report = |text: &str| {
            send(&handle, json!({ "type": "update", "phase": "working", "message": text }));
        };
        let st = handle.state::<AppState>();
        let mut settings_now = st
            .settings
            .lock()
            .map(|s| s.clone())
            .unwrap_or_else(|e| e.into_inner().clone());
        match update::apply(&mut settings_now, &report) {
            Ok(info) => {
                if let Ok(mut guard) = st.settings.lock() {
                    *guard = settings_now.clone();
                }
                settings::save(&settings_now);
                post_state(&handle);
                send(&handle, json!({ "type": "update", "phase": "done", "info": info }));
            }
            Err(message) => {
                log::write(&format!("[update] {message}"));
                send(
                    &handle,
                    json!({ "type": "update", "phase": "error", "message": message }),
                );
            }
        }
        st.busy.store(false, Ordering::SeqCst);
    });
}

/// 用默认浏览器打开 http(s) 链接（只放行 http/https）。
fn on_open_url(url: &str) {
    let text = url.trim();
    if !(text.starts_with("https://") || text.starts_with("http://")) {
        return;
    }
    let _ = std::process::Command::new("rundll32")
        .args(["url.dll,FileProtocolHandler", text])
        .spawn();
}

#[tauri::command]
fn bridge(window: WebviewWindow, app: AppHandle, msg: Value) {
    let kind = msg["type"].as_str().unwrap_or_default().to_string();
    match kind.as_str() {
        "init" => {
            if !launch::running() {
                launch::clear();
            }
            post_state(&app);
        }
        "scan" => {
            let s = snapshot(&app);
            let dirs: Vec<String> = gamepath::scan(&PathBuf::from(&s.game_dir))
                .iter()
                .map(|p| paths::to_display(p))
                .collect();
            send(
                &app,
                json!({ "type": "dirs", "dirs": dirs, "current": s.game_dir }),
            );
        }
        "pickFolder" => match gamepath::pick_folder() {
            None => {}
            Some(raw) => match gamepath::normalize(&raw) {
                Some(dir) => {
                    {
                        let st = app.state::<AppState>();
                        st.settings.lock().unwrap().game_dir = paths::to_display(&dir);
                    }
                    settings::save(&snapshot(&app));
                    send(
                        &app,
                        json!({ "type": "picked", "ok": true, "dir": paths::to_display(&dir) }),
                    );
                    post_state(&app);
                }
                None => send(
                    &app,
                    json!({
                        "type": "picked",
                        "ok": false,
                        "dir": paths::to_display(&raw),
                        "message": "这个目录里没有 YuanShen.exe",
                    }),
                ),
            },
        },
        "save" => on_save(&app, &msg),
        "install" => on_install(&app, &msg),
        "launch" => on_launch(&app, &msg),
        "stop" => {
            app.state::<AppState>().watch.store(true, Ordering::SeqCst);
            app.state::<AppState>().cancel.store(true, Ordering::SeqCst);
            send(
                &app,
                json!({ "type": "unlock", "status": "idle", "message": "" }),
            );
        }
        "window" => on_window(&window, msg["action"].as_str().unwrap_or_default()),
        "openPath" => on_open_path(&app, msg["what"].as_str().unwrap_or_default()),
        "switchVersion" => on_switch_version(&app, &msg),
        "unlockerConfig" => on_unlocker_config(&app, &msg),
        "srInput" => on_sr_input(&app, &msg),
        "frameGen" => on_frame_gen(&app, &msg),
        "modsDir" => on_mods_dir(&app, msg["dir"].as_str().map(str::to_string)),
        "pickMods" => {
            if let Some(dir) = gamepath::pick_folder() {
                on_mods_dir(&app, Some(paths::to_display(&dir)));
            }
        }
        "pickCustomDll" => match msg["path"].as_str() {
            Some(p) if !p.trim().is_empty() => on_add_custom_dll(&app, p),
            _ => on_pick_custom_dll(&app),
        },
        "removeCustomDll" => on_remove_custom_dll(&app, &msg),
        "restoreDefaults" => on_restore_defaults(&app),
        "openLog" => on_open_log(&app, msg["id"].as_str().unwrap_or_default()),
        "bundleLogs" => on_bundle_logs(&app, &msg),
        "fetchComponent" => on_fetch_component(&app, &msg),
        "checkUpdate" => on_check_update(&app),
        "applyUpdate" => on_apply_update(&app),
        "openUrl" => on_open_url(msg["url"].as_str().unwrap_or_default()),
        "prefs" => on_prefs(&window, &app, &msg),
        "suppressWarn" => on_suppress_warn(&app, &msg),
        "appearance" => on_appearance(&window, &app, &msg),
        "setShortcut" => on_set_shortcut(&app, &msg),
        "setCustomOn" => on_set_custom_on(&app, &msg),
        _ => {}
    }
}

/// 按设置创建桌面快捷方式；失败只写日志，不影响安装结果。
fn ensure_shortcut(s: &settings::Settings) {
    if !s.desktop_shortcut {
        return;
    }
    let unlocker = paths::unlocker_dir().join("unlockfps_nc.exe");
    let game_exe = gamepath::find_game_exe(Path::new(&s.game_dir));
    match shortcut::create(&unlocker, game_exe.as_deref(), i18n::current() == "zh") {
        Ok(p) => log::write(&format!("已创建桌面快捷方式: {}", p.display())),
        Err(e) => log::write(&format!("[shortcut] 创建失败: {e}")),
    }
}

/// 设置页的「桌面快捷方式」开关：开=立刻建一个，关=把两种语言名字的都删掉。
fn on_set_shortcut(app: &AppHandle, msg: &Value) {
    let enabled = msg["enabled"].as_bool().unwrap_or(true);
    let mut s = snapshot(app);
    s.desktop_shortcut = enabled;
    settings::save(&s);
    if let Ok(mut guard) = app.state::<AppState>().settings.lock() {
        *guard = s.clone();
    }

    let (ok, path, error) = if enabled {
        let unlocker = paths::unlocker_dir().join("unlockfps_nc.exe");
        let game_exe = gamepath::find_game_exe(Path::new(&s.game_dir));
        match shortcut::create(&unlocker, game_exe.as_deref(), i18n::current() == "zh") {
            Ok(p) => {
                log::write(&format!("已创建桌面快捷方式: {}", p.display()));
                (true, paths::to_display(&p), String::new())
            }
            Err(e) => {
                log::write(&format!("[shortcut] 创建失败: {e}"));
                (false, String::new(), e)
            }
        }
    } else {
        let unlocker = paths::unlocker_dir().join("unlockfps_nc.exe");
        match shortcut::remove(&unlocker) {
            Ok(n) => {
                log::write(&format!("已删除 {n} 个桌面快捷方式"));
                (true, String::new(), String::new())
            }
            Err(e) => (false, String::new(), e),
        }
    };

    post_state(app);
    send(
        app,
        json!({ "type": "shortcut", "ok": ok, "path": path, "error": error }),
    );
}

/// 语言与明暗：写进设置，同时让窗口标题栏/材质跟着变深。
fn on_suppress_warn(app: &AppHandle, msg: &Value) {
    let id = msg["id"].as_str().unwrap_or_default().to_string();
    if id.is_empty() {
        return;
    }
    {
        let st = app.state::<AppState>();
        let mut s = st.settings.lock().unwrap();
        if !s.suppress_warnings.iter().any(|x| x == &id) {
            s.suppress_warnings.push(id.clone());
        }
        settings::save(&s);
    }
    log::write(&format!("[warn] 不再提醒: {id}"));
    post_state(app);
}

fn on_prefs(window: &WebviewWindow, app: &AppHandle, msg: &Value) {
    let mut s = snapshot(app);
    if let Some(v) = msg["uiLang"].as_str() {
        s.ui_lang = v.to_string();
    }
    if let Some(v) = msg["theme"].as_str() {
        s.theme = v.to_string();
    }
    s.clamp();
    i18n::set_lang(s.resolved_lang());
    settings::save(&s);
    if let Ok(mut guard) = app.state::<AppState>().settings.lock() {
        *guard = s.clone();
    }
    let dark = msg["dark"].as_bool().unwrap_or(false);
    backdrop::set_dark(window, dark);
    log::write(&format!(
        "界面偏好: 语言={} 主题={} 深色={dark}",
        s.ui_lang, s.theme
    ));

    // 桌面快捷方式已存在时跟着语言改名（原神 ⇄ Genshin Impact）。
    // 只动我们自己建的那个：目标不是解帧工具的快捷方式一律不碰。
    let zh = i18n::current() == "zh";
    let keep = shortcut::link_path(zh);
    let other = shortcut::link_path(!zh);
    let unlocker = paths::unlocker_dir().join("unlockfps_nc.exe");
    // 同名快捷方式不是我们建的（用户自己放的）就不改名，免得覆盖掉
    let foreign_keep = keep.is_file() && !shortcut::is_ours(&keep, &unlocker);
    if !foreign_keep && (shortcut::is_ours(&keep, &unlocker) || shortcut::is_ours(&other, &unlocker)) {
        let game_exe = gamepath::find_game_exe(Path::new(&s.game_dir));
        match shortcut::create(&unlocker, game_exe.as_deref(), zh) {
            Ok(_) => {
                if shortcut::is_ours(&other, &unlocker) {
                    let _ = std::fs::remove_file(&other);
                }
            }
            Err(e) => log::write(&format!("[shortcut] 改名失败: {e}")),
        }
    }

    send(
        app,
        json!({ "type": "prefs", "ok": true, "uiLang": s.ui_lang, "theme": s.theme }),
    );
}
fn main() {
    tauri::Builder::default()
        .manage(AppState {
            settings: Mutex::new(settings::load()),
            busy: AtomicBool::new(false),
            cancel: AtomicBool::new(false),
            watch: AtomicBool::new(false),
        })
        .invoke_handler(tauri::generate_handler![bridge])
        .setup(|app| {
            log::write("安装器启动");
            let handle = app.handle().clone();
            if let Some(window) = app.get_webview_window("main") {
                let initial = snapshot(&handle);
                i18n::set_lang(initial.resolved_lang());
                let applied = backdrop::apply(&window, &initial.backdrop);
                log::write(&format!("背景效果: {applied}"));

                let handle_events = handle.clone();
                window.on_window_event(move |event| match event {
                    tauri::WindowEvent::Resized(_) => {
                        let maximized = handle_events
                            .get_webview_window("main")
                            .and_then(|w| w.is_maximized().ok())
                            .unwrap_or(false);
                        send(
                            &handle_events,
                            json!({ "type": "windowstate", "maximized": maximized }),
                        );
                    }
                    tauri::WindowEvent::CloseRequested { .. } => log::write("安装器退出"),
                    _ => {}
                });
            }

            // 启动时**只修路径**：OptiScaler 的 OptiDllPath、ReShade 的三个搜索路径、
            // fps_config.json 的 GamePath / DllList 按本机实际位置校正，
            // 顺手清掉已经不存在的条目。功能键（FSR4 策略、日志、帧生成、FPSTarget 等）
            // 一律不动 —— 那是「安装 / 保存」与「恢复默认设置」的事。
            let heal = handle.clone();
            std::thread::spawn(move || {
                let s = snapshot(&heal);
                let selected = pkg::with_mandatory(&s.selected);
                let game_exe = gamepath::find_game_exe(Path::new(&s.game_dir));
                match install::repair_paths(&selected, &s, game_exe.as_deref()) {
                    Ok(changed) if changed.is_empty() => {
                        log::write("[startup] 配置路径无需修正");
                    }
                    Ok(changed) => log::write(&format!(
                        "[startup] 已修正 {} 个配置文件里的路径: {}",
                        changed.len(),
                        changed.join(" / ")
                    )),
                    Err(err) => log::write(&format!("[startup] 路径修正失败: {err}")),
                }
                // 与路径修正同一批次：N 卡 + 勾了 OptiScaler 时把 DLSS 运行时校验/补齐一次
                // （缺且补不上会发 dlssError 弹窗）。
                ensure_dlss(&heal, &s);
                 ensure_streamline(&heal, &s);
                // 下载校验只认包内清单：启动时把"用的是哪份清单"记一行，便于排查。
                match manifest::manifest_path() {
                    Some(path) => log::write(&format!(
                        "[verify] 版本清单: {}",
                        paths::to_display(&path)
                    )),
                    None => log::write("[verify] 未找到 upstream-versions.json：联网下载将失败关闭"),
                }
                post_state(&heal);
            });

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("failed to run installer");
}
