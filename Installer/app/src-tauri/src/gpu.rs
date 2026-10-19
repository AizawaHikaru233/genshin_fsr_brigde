use std::sync::OnceLock;

use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ};
use winreg::RegKey;

use crate::gpu_tables::{AMD_FP8, AMD_INT8, AMD_RDNA2, NVIDIA_INT8};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// 不支持：FSR4 托管键按「关闭」写
    Auto,
    Int8,
    Fp8,
}

impl Mode {
    pub fn supported(self) -> bool {
        !matches!(self, Mode::Auto)
    }
}

pub struct Policy {
    pub mode: Mode,
    pub reason: &'static str,
    pub name: String,
}

struct Controller {
    name: String,
    vendor: u16,
    device: u16,
}

fn vendev(matching: &str) -> (u16, u16) {
    let upper = matching.to_uppercase();
    let vendor = upper
        .find("VEN_")
        .and_then(|i| u16::from_str_radix(upper.get(i + 4..i + 8)?, 16).ok())
        .unwrap_or(0);
    let device = upper
        .find("DEV_")
        .and_then(|i| u16::from_str_radix(upper.get(i + 4..i + 8)?, 16).ok())
        .unwrap_or(0);
    (vendor, device)
}

/// 与旧脚本一致：从显卡类注册表项读名字与硬件 ID（等价于 WMI 的 Win32_VideoController）。
fn controllers() -> Vec<Controller> {
    let mut result = Vec::new();
    let hklm = RegKey::predef(HKEY_LOCAL_MACHINE);
    let Ok(class) = hklm.open_subkey_with_flags(
        r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}",
        KEY_READ,
    ) else {
        return result;
    };
    for key_name in class.enum_keys().flatten() {
        let Ok(key) = class.open_subkey_with_flags(&key_name, KEY_READ) else {
            continue;
        };
        let name: String = key.get_value("DriverDesc").unwrap_or_default();
        if name.trim().is_empty() {
            continue;
        }
        let matching: String = key.get_value("MatchingDeviceId").unwrap_or_default();
        let (vendor, device) = vendev(&matching);
        result.push(Controller {
            name,
            vendor,
            device,
        });
    }
    result
}

fn model_number(name: &str, token: &str) -> Option<u32> {
    let upper = name.to_uppercase();
    let start = upper.find(token)? + token.len();
    let digits: String = upper[start..]
        .trim_start()
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect();
    if digits.is_empty() {
        None
    } else {
        digits.parse().ok()
    }
}

fn has_token(name: &str, tokens: &[&str]) -> bool {
    let upper = name.to_uppercase();
    tokens.iter().any(|token| upper.contains(token))
}


fn classify(controller: &Controller) -> (Mode, &'static str) {
    let (mode, reason) = match controller.vendor {
        0x1002 => amd(controller),
        0x10DE => nvidia(controller),
        0x8086 => intel(controller),
        _ => {
            // 硬件 ID 缺失时按名字兜底，与旧脚本的 vendor 判断等价。
            let upper = controller.name.to_uppercase();
            if upper.contains("AMD") || upper.contains("RADEON") {
                amd(controller)
            } else if upper.contains("NVIDIA") || upper.contains("GEFORCE") {
                nvidia(controller)
            } else if upper.contains("INTEL") {
                intel(controller)
            } else {
                (Mode::Auto, "unsupported")
            }
        }
    };
    (mode, reason)
}

fn amd(controller: &Controller) -> (Mode, &'static str) {
    if AMD_FP8.contains(&controller.device) {
        return (Mode::Fp8, "AMD RDNA4");
    }
    if AMD_INT8.contains(&controller.device) {
        return (Mode::Int8, "AMD RDNA3/3.5");
    }
    if let Some(n) = model_number(&controller.name, "RX") {
        if (9000..=9999).contains(&n) {
            return (Mode::Fp8, "AMD RDNA4");
        }
        if (7000..=7999).contains(&n) {
            return (Mode::Int8, "AMD RDNA3/3.5");
        }
    }
    if let Some(n) = model_number(&controller.name, "W") {
        if (9000..=9999).contains(&n) {
            return (Mode::Fp8, "AMD RDNA4");
        }
        if (7000..=7999).contains(&n) {
            return (Mode::Int8, "AMD RDNA3/3.5");
        }
    }
    if has_token(
        &controller.name,
        &[
            "740M", "760M", "780M", "840M", "860M", "880M", "890M", "8040S", "8050S", "8060S",
        ],
    ) {
        return (Mode::Int8, "AMD RDNA3/3.5");
    }
    (Mode::Auto, "unsupported")
}

fn nvidia(controller: &Controller) -> (Mode, &'static str) {
    if NVIDIA_INT8.contains(&controller.device) {
        return (Mode::Int8, "NVIDIA 16-50 系");
    }
    if let Some(n) = model_number(&controller.name, "RTX") {
        if (2000..=5999).contains(&n) {
            return (Mode::Int8, "NVIDIA RTX 20+");
        }
    }
    if let Some(n) = model_number(&controller.name, "GTX") {
        if (1600..=1699).contains(&n) {
            return (Mode::Int8, "NVIDIA GTX 16 Turing");
        }
    }
    (Mode::Auto, "unsupported")
}

fn intel(controller: &Controller) -> (Mode, &'static str) {
    let device = controller.device;
    if (0x5600..=0x56FF).contains(&device) || (0xE200..=0xE2FF).contains(&device) {
        return (Mode::Int8, "Intel Arc");
    }
    if controller.name.to_uppercase().contains("ARC") {
        return (Mode::Int8, "Intel Arc");
    }
    (Mode::Auto, "unsupported")
}

/// 一次算完的显卡信息：OptiScaler 托管键与界面提示共用同一份结果。
pub struct Info {
    /// nvidia | amd | intel | other
    pub vendor: &'static str,
    /// FSR4 策略（Auto = 不支持）
    pub mode: Mode,
    pub reason: &'static str,
    pub name: String,
}

static INFO: OnceLock<Info> = OnceLock::new();

fn vendor_of(controller: &Controller) -> &'static str {
    match controller.vendor {
        0x1002 => return "amd",
        0x10DE => return "nvidia",
        0x8086 => return "intel",
        _ => {}
    }
    let upper = controller.name.to_uppercase();
    if upper.contains("AMD") || upper.contains("RADEON") {
        "amd"
    } else if upper.contains("NVIDIA") || upper.contains("GEFORCE") {
        "nvidia"
    } else if upper.contains("INTEL") {
        "intel"
    } else {
        "other"
    }
}

fn compute_info() -> Info {
    let mut best: Option<Info> = None;
    for controller in controllers() {
        let (mode, reason) = classify(&controller);
        if mode == Mode::Auto {
            continue;
        }
        let better = match &best {
            None => true,
            Some(current) => mode == Mode::Fp8 && current.mode != Mode::Fp8,
        };
        if better {
            best = Some(Info {
                vendor: vendor_of(&controller),
                mode,
                reason,
                name: controller.name.clone(),
            });
        }
    }
    best.unwrap_or_else(|| Info {
        vendor: controllers()
            .first()
            .map(vendor_of)
            .unwrap_or("other"),
        mode: Mode::Auto,
        reason: "no whitelist match",
        name: String::new(),
    })
}

pub fn info() -> &'static Info {
    INFO.get_or_init(compute_info)
}

/// 遍历全部显卡取最优策略（fp8 优先于 int8）。
pub fn policy() -> Policy {
    let i = info();
    Policy {
        mode: i.mode,
        reason: i.reason,
        name: i.name.clone(),
    }
}

/// 主显卡厂商，界面上用来提示「N 卡 + 某些组件的已知问题」。
pub fn vendor() -> &'static str {
    info().vendor
}
/// RDNA2 需要同步模式（`Dx11FsrBridge.ini` 的 Ffx12AsyncUpscale=0）。
pub fn has_rdna2() -> bool {
    for controller in controllers() {
        let amd = controller.vendor == 0x1002
            || controller.name.to_uppercase().contains("AMD")
            || controller.name.to_uppercase().contains("RADEON");
        if !amd {
            continue;
        }
        if AMD_RDNA2.contains(&controller.device) {
            return true;
        }
        if let Some(n) = model_number(&controller.name, "RX") {
            if (6000..=6999).contains(&n) {
                return true;
            }
        }
    }
    false
}

/// 是否 RTX 架构的 N 卡：只有它才自动补 NVIDIA DLSS 运行时
/// （GTX 16 系列也能吃 FSR4 INT8，但不给它部署 DLSS）。
pub fn has_rtx() -> bool {
    for controller in controllers() {
        let nvidia = controller.vendor == 0x10DE
            || controller.name.to_uppercase().contains("NVIDIA")
            || controller.name.to_uppercase().contains("GEFORCE");
        if !nvidia {
            continue;
        }
        let gtx16 = model_number(&controller.name, "GTX")
            .map(|n| (1600..=1699).contains(&n))
            .unwrap_or(false)
            || controller.name.to_uppercase().contains("GTX 16");
        if NVIDIA_INT8.contains(&controller.device) && !gtx16 {
            return true;
        }
        if let Some(n) = model_number(&controller.name, "RTX") {
            if (2000..=5999).contains(&n) {
                return true;
            }
        }
    }
    false
}
