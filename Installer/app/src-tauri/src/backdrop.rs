use tauri::WebviewWindow;
use window_vibrancy::{apply_acrylic, apply_mica, clear_acrylic, clear_mica};

/// 应用窗口背景效果，返回实际生效的档位（不支持的平台回退成 off）。
pub fn apply(window: &WebviewWindow, mode: &str) -> String {
    let _ = clear_acrylic(window);
    let _ = clear_mica(window);

    match mode {
        "mica" => {
            if apply_mica(window, None).is_ok() {
                return "mica".into();
            }
            // Win11 21H2 之前没有 Mica，退到亚克力，再不行就纯色。
            if apply_acrylic(window, Some((245, 245, 247, 140))).is_ok() {
                return "acrylic".into();
            }
            "off".into()
        }
        "acrylic" => {
            if apply_acrylic(window, Some((245, 245, 247, 140))).is_ok() {
                return "acrylic".into();
            }
            "off".into()
        }
        _ => "off".into(),
    }
}

/// 标题栏 / 材质跟随明暗：深色时把窗口标成 dark，系统才会给深色标题与深色 mica 底。
pub fn set_dark(window: &WebviewWindow, dark: bool) {
    use windows_sys::Win32::Graphics::Dwm::{
        DwmSetWindowAttribute, DWMWA_USE_IMMERSIVE_DARK_MODE,
    };
    let Ok(handle) = window.hwnd() else { return };
    let hwnd = handle.0 as *mut core::ffi::c_void;
    let value: i32 = if dark { 1 } else { 0 };
    unsafe {
        DwmSetWindowAttribute(
            hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE as u32,
            &value as *const i32 as *const core::ffi::c_void,
            std::mem::size_of::<i32>() as u32,
        );
    }
}