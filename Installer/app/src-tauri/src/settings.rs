use serde::{Deserialize, Serialize};
use std::fs;

use crate::log;
use crate::paths;

#[derive(Serialize, Deserialize, Clone)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    pub game_dir: String,
    pub fps: i32,
    /// 已勾选的组件 id；必装项由 pkg::with_mandatory 补齐。
    pub selected: Vec<String>,
    pub lang: String,
    /// 窗口背景效果：mica / acrylic / off
    pub backdrop: String,
    /// 外壳不透明度，百分比；卡片始终不透明。
    pub opacity: i32,
    /// 当前使用的 Dx11FsrBridge 版本目录名（payload 下的文件夹名）
    pub bridge_dir: String,
    /// 当前使用的 OptiScaler 版本目录名（payload 下的文件夹名）
    pub opti_dir: String,
    /// TextureLoader 的 Mods 目录；空 = 用 TextureLoader 同目录的 Mods
    pub mods_dir: String,
    /// 用户自己加进注入链的 DLL 绝对路径（追加在内置组件之后）
    pub custom_dlls: Vec<String>,
    /// 界面语言：system / zh / en
    pub ui_lang: String,
    /// 明暗主题：system / light / dark
    pub theme: String,
    /// 安装组件时在桌面放一个「启动游戏」快捷方式（指向解帧工具本体）
    pub desktop_shortcut: bool,
    /// 已经点过「不再提醒」的警告 id（nvidia-reshade-opti / nvidia-texture）
    pub suppress_warnings: Vec<String>,
    /// 自定义注入里当前勾选的路径（取消勾选就不再写进注入链，文件仍留着）。
    /// `None` 表示老配置文件没这个字段：那时自定义注入都是启用的，按全开处理。
    pub custom_on: Option<Vec<String>>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            game_dir: String::new(),
            fps: 240,
            selected: Vec::new(),
            lang: "zh".into(),
            backdrop: "mica".into(),
            opacity: 80,
            // 空 = 自动用 payload 里扫到的第一个版本目录
            bridge_dir: String::new(),
            opti_dir: String::new(),
            mods_dir: String::new(),
            custom_dlls: Vec::new(),
            ui_lang: "system".into(),
            theme: "system".into(),
            desktop_shortcut: true,
        suppress_warnings: Vec::new(),
            custom_on: None,
        }
    }
}

impl Settings {
    pub fn clamp(&mut self) {
        self.fps = self.fps.clamp(30, 1000);
        self.opacity = self.opacity.clamp(40, 100);
        if !matches!(self.backdrop.as_str(), "mica" | "acrylic" | "off") {
            self.backdrop = "mica".into();
        }
        if !matches!(self.ui_lang.as_str(), "system" | "zh" | "en") {
            self.ui_lang = "system".into();
        }
        if !matches!(self.theme.as_str(), "system" | "light" | "dark") {
            self.theme = "system".into();
        }
    }

    /// 界面实际使用的语言（system 时按系统语言判定）。
    pub fn resolved_lang(&self) -> &'static str {
        match self.ui_lang.as_str() {
            "zh" => "zh",
            "en" => "en",
            _ => {
                if crate::i18n::system_is_chinese() {
                    "zh"
                } else {
                    "en"
                }
            }
        }
    }

    /// 某个自定义注入现在是否勾选。老配置没有 customOn 字段时按全选处理。
    pub fn custom_enabled(&self, path: &str) -> bool {
        match &self.custom_on {
            None => true,
            Some(list) => list.iter().any(|p| p.eq_ignore_ascii_case(path)),
        }
    }

    /// 勾选/取消勾选一个自定义注入；第一次改时把 None 落成显式列表。
    pub fn set_custom_enabled(&mut self, path: &str, on: bool) {
        if self.custom_on.is_none() {
            self.custom_on = Some(self.custom_dlls.clone());
        }
        let list = self.custom_on.get_or_insert_with(Vec::new);
        if on {
            if !list.iter().any(|p| p.eq_ignore_ascii_case(path)) {
                list.push(path.to_string());
            }
        } else {
            list.retain(|p| !p.eq_ignore_ascii_case(path));
        }
    }
}

pub fn load() -> Settings {
    let file = paths::settings_file();
    let mut settings = match fs::read_to_string(&file) {
        Ok(text) => serde_json::from_str::<Settings>(&text).unwrap_or_else(|e| {
            log::write(&format!("settings.json 解析失败，使用默认值：{e}"));
            Settings::default()
        }),
        Err(_) => Settings::default(),
    };
    settings.clamp();
    settings
}

pub fn save(settings: &Settings) -> bool {
    let dir = paths::config_dir();
    if fs::create_dir_all(&dir).is_err() {
        log::write("无法创建配置目录");
        return false;
    }
    let text = match serde_json::to_string_pretty(settings) {
        Ok(t) => t,
        Err(_) => return false,
    };
    if fs::write(paths::settings_file(), text).is_err() {
        log::write("无法写入 settings.json");
        return false;
    }
    true
}
