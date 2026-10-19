//! 界面文案的双语选择：调用处同时给出中文与英文，按当前设置挑一条。
//!
//! 前端自己有一份完整的词表；这里只覆盖后端会主动推给界面的文案
//! （进度、错误、解帧状态等），避免英文界面里混进中文提示。

use std::sync::OnceLock;
use std::sync::RwLock;

use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ};
use winreg::RegKey;

static LANG: OnceLock<RwLock<&'static str>> = OnceLock::new();

fn lang_slot() -> &'static RwLock<&'static str> {
    LANG.get_or_init(|| RwLock::new("zh"))
}

/// 记录当前界面语言（zh / en），供各处零散文案取用。
pub fn set_lang(lang: &str) {
    let slot = lang_slot();
    if let Ok(mut guard) = slot.write() {
        *guard = if lang == "en" { "en" } else { "zh" };
    }
}

pub fn current() -> &'static str {
    lang_slot().read().map(|g| *g).unwrap_or("zh")
}

/// 系统界面语言是不是中文（读安装语言，避免依赖 PowerShell / 环境变量）。
pub fn system_is_chinese() -> bool {
    let key = RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey_with_flags(r"SYSTEM\CurrentControlSet\Control\Nls\Language", KEY_READ);
    let Ok(key) = key else { return false };
    let install: String = key.get_value("InstallLanguage").unwrap_or_default();
    let trimmed = install.trim();
    // 0x0804 简体、0x0404 繁体、0x0C04 香港、0x1004 新加坡：低 8 位都是 0x04
    u32::from_str_radix(trimmed, 16)
        .map(|id| id & 0xff == 0x04)
        .unwrap_or(false)
}
