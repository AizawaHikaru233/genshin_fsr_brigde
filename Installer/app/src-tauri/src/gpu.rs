use std::sync::OnceLock;

use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ};
use winreg::RegKey;

use crate::gpu_tables::{AMD_FP8, AMD_INT8, AMD_RDNA2, AMD_RDNA2_DGPU, AMD_RDNA35_IGPU, NVIDIA_INT8};

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

/// 判定命中的显卡平台。FSR4 托管键与界面提示共用同一份结果，不会各判一套。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Platform {
    Rdna4,
    Rdna3,
    /// RDNA3.5 核显（890M/880M/860M/840M/8060S/8050S/8040S）
    Rdna35Igpu,
    /// RDNA2 独显（RX 6000 系）
    Rdna2Dgpu,
    /// RDNA2 核显：FSR4 仍不启用，只参与同步交叠判定
    Rdna2Igpu,
    Nvidia,
    Intel,
    Other,
}

impl Platform {
    /// 这两类平台要 Adrenalin 26.10.1+ 才开官方 FSR4：首次装 OptiScaler 时提示一次驱动。
    pub fn needs_fsr4_driver_notice(self) -> bool {
        matches!(self, Platform::Rdna2Dgpu | Platform::Rdna35Igpu)
    }

    /// 发给界面的稳定标识（界面自己出双语名字）。
    pub fn id(self) -> &'static str {
        match self {
            Platform::Rdna4 => "rdna4",
            Platform::Rdna3 => "rdna3",
            Platform::Rdna35Igpu => "rdna35_igpu",
            Platform::Rdna2Dgpu => "rdna2_dgpu",
            Platform::Rdna2Igpu => "rdna2_igpu",
            Platform::Nvidia => "nvidia",
            Platform::Intel => "intel",
            Platform::Other => "other",
        }
    }
}

pub struct Policy {
    pub mode: Mode,
    pub reason: &'static str,
    pub platform: Platform,
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

/// 硬件 ID 缺失时按名字兜底厂商（与旧脚本的 vendor 判断等价）。
fn vendor_from_name(name: &str) -> u16 {
    let upper = name.to_uppercase();
    if upper.contains("AMD") || upper.contains("RADEON") {
        0x1002
    } else if upper.contains("NVIDIA") || upper.contains("GEFORCE") {
        0x10DE
    } else if upper.contains("INTEL") {
        0x8086
    } else {
        0
    }
}

/// 纯函数分类：vendor / device / 名字 → (策略, 理由, 平台)。
///
/// 真机路径（注册表读到的显卡）与单测喂进来的假显卡走同一条判定，
/// 避免"界面说支持、写 ini 时又说不支持"这种两边不一致。
pub fn classify_device(vendor: u16, device: u16, name: &str) -> (Mode, &'static str, Platform) {
    let vendor = if vendor == 0 { vendor_from_name(name) } else { vendor };
    match vendor {
        0x1002 => amd(device, name),
        0x10DE => nvidia(device, name),
        0x8086 => intel(device, name),
        _ => (Mode::Auto, "unsupported", Platform::Other),
    }
}

fn amd(device: u16, name: &str) -> (Mode, &'static str, Platform) {
    if AMD_FP8.contains(&device) {
        return (Mode::Fp8, "AMD RDNA4", Platform::Rdna4);
    }
    // 2026-10：RDNA2 独显不再封锁，走 FSR4 INT8
    if AMD_RDNA2_DGPU.contains(&device) {
        return (Mode::Int8, "AMD RDNA2", Platform::Rdna2Dgpu);
    }
    // 2026-10：RDNA3.5 核显不再封锁，走 FSR4 INT8（此前被漂移的表误判成 fp8）
    if AMD_RDNA35_IGPU.contains(&device) {
        return (Mode::Int8, "AMD RDNA3.5 核显", Platform::Rdna35Igpu);
    }
    if AMD_INT8.contains(&device) {
        return (Mode::Int8, "AMD RDNA3/3.5", Platform::Rdna3);
    }
    // RDNA2 核显：只用来判 Ffx12AsyncUpscale，不启用 FSR4
    if AMD_RDNA2.contains(&device) {
        return (Mode::Auto, "AMD RDNA2 核显", Platform::Rdna2Igpu);
    }
    if let Some(n) = model_number(name, "RX") {
        if (9000..=9999).contains(&n) {
            return (Mode::Fp8, "AMD RDNA4", Platform::Rdna4);
        }
        if (7000..=7999).contains(&n) {
            return (Mode::Int8, "AMD RDNA3/3.5", Platform::Rdna3);
        }
        if (6000..=6999).contains(&n) {
            return (Mode::Int8, "AMD RDNA2", Platform::Rdna2Dgpu);
        }
    }
    if let Some(n) = model_number(name, "W") {
        if (9000..=9999).contains(&n) {
            return (Mode::Fp8, "AMD RDNA4", Platform::Rdna4);
        }
        if (7000..=7999).contains(&n) {
            return (Mode::Int8, "AMD RDNA3/3.5", Platform::Rdna3);
        }
    }
    if has_token(name, &["8060S", "8050S", "8040S", "890M", "880M", "860M", "840M"]) {
        return (Mode::Int8, "AMD RDNA3.5 核显", Platform::Rdna35Igpu);
    }
    if has_token(name, &["780M", "760M", "740M"]) {
        return (Mode::Int8, "AMD RDNA3/3.5", Platform::Rdna3);
    }
    (Mode::Auto, "unsupported", Platform::Other)
}

fn nvidia(device: u16, name: &str) -> (Mode, &'static str, Platform) {
    if NVIDIA_INT8.contains(&device) {
        return (Mode::Int8, "NVIDIA 16-50 系", Platform::Nvidia);
    }
    if let Some(n) = model_number(name, "RTX") {
        if (2000..=5999).contains(&n) {
            return (Mode::Int8, "NVIDIA RTX 20+", Platform::Nvidia);
        }
    }
    if let Some(n) = model_number(name, "GTX") {
        if (1600..=1699).contains(&n) {
            return (Mode::Int8, "NVIDIA GTX 16 Turing", Platform::Nvidia);
        }
    }
    (Mode::Auto, "unsupported", Platform::Other)
}

fn intel(device: u16, name: &str) -> (Mode, &'static str, Platform) {
    if (0x5600..=0x56FF).contains(&device) || (0xE200..=0xE2FF).contains(&device) {
        return (Mode::Int8, "Intel Arc", Platform::Intel);
    }
    if name.to_uppercase().contains("ARC") {
        return (Mode::Int8, "Intel Arc", Platform::Intel);
    }
    (Mode::Auto, "unsupported", Platform::Other)
}

/// 一次算完的显卡信息：OptiScaler 托管键与界面提示共用同一份结果。
pub struct Info {
    /// nvidia | amd | intel | other
    pub vendor: &'static str,
    /// FSR4 策略（Auto = 不支持）
    pub mode: Mode,
    pub reason: &'static str,
    /// 命中的平台（Auto 时是 Other，核显等不启用 FSR4 的平台按实际填）
    pub platform: Platform,
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
    match vendor_from_name(&controller.name) {
        0x1002 => "amd",
        0x10DE => "nvidia",
        0x8086 => "intel",
        _ => "other",
    }
}

fn compute_info() -> Info {
    let mut best: Option<Info> = None;
    let all = controllers();
    for controller in &all {
        let (mode, reason, platform) =
            classify_device(controller.vendor, controller.device, &controller.name);
        if mode == Mode::Auto {
            continue;
        }
        let better = match &best {
            None => true,
            Some(current) => mode == Mode::Fp8 && current.mode != Mode::Fp8,
        };
        if better {
            best = Some(Info {
                vendor: vendor_of(controller),
                mode,
                reason,
                platform,
                name: controller.name.clone(),
            });
        }
    }
    best.unwrap_or_else(|| Info {
        vendor: all.first().map(vendor_of).unwrap_or("other"),
        mode: Mode::Auto,
        reason: "no whitelist match",
        platform: Platform::Other,
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
        platform: i.platform,
        name: i.name.clone(),
    }
}

/// 主显卡厂商，界面上用来提示「N 卡 + 某些组件的已知问题」。
pub fn vendor() -> &'static str {
    info().vendor
}

/// 首次装 OptiScaler 时是否要提示升级驱动（RDNA2 独显 / RDNA3.5 核显）。
pub fn needs_fsr4_driver_notice() -> bool {
    info().platform.needs_fsr4_driver_notice()
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

#[cfg(test)]
mod tests {
    use super::*;

    fn classify(vendor: u16, device: u16, name: &str) -> (Mode, &'static str, Platform) {
        classify_device(vendor, device, name)
    }

    #[test]
    fn rdna4_dgpu_is_fp8() {
        let (mode, reason, platform) = classify(0x1002, 0x7550, "AMD Radeon RX 9070 XT");
        assert_eq!(mode, Mode::Fp8);
        assert_eq!(reason, "AMD RDNA4");
        assert_eq!(platform, Platform::Rdna4);
        assert!(!platform.needs_fsr4_driver_notice());
    }

    #[test]
    fn rdna2_dgpu_is_int8_and_wants_driver_notice() {
        // RX 6800 XT / RX 6600 XT / RX 6500 XT
        for device in [0x73BFu16, 0x73FF, 0x743F] {
            let (mode, reason, platform) = classify(0x1002, device, "AMD Radeon RX 6800 XT");
            assert_eq!(mode, Mode::Int8, "device 0x{device:04X}");
            assert_eq!(reason, "AMD RDNA2");
            assert_eq!(platform, Platform::Rdna2Dgpu);
            assert!(platform.needs_fsr4_driver_notice());
        }
    }

    #[test]
    fn rdna35_igpu_is_int8_and_wants_driver_notice() {
        // Strix Point 890M / Strix Halo 8060S / Krackan 860M
        for (device, name) in [
            (0x150Eu16, "AMD Radeon(TM) 890M Graphics"),
            (0x1586, "AMD Radeon(TM) 8060S Graphics"),
            (0x1114, "AMD Radeon(TM) 860M"),
        ] {
            let (mode, reason, platform) = classify(0x1002, device, name);
            assert_eq!(mode, Mode::Int8, "device 0x{device:04X}");
            assert_eq!(reason, "AMD RDNA3.5 核显");
            assert_eq!(platform, Platform::Rdna35Igpu);
            assert!(platform.needs_fsr4_driver_notice());
        }
    }

    #[test]
    fn rdna3_dgpu_stays_int8_without_driver_notice() {
        // RX 7900 XTX / RX 7800 XT / RX 7600 —— 这类平台早就支持，不该弹驱动提示
        for device in [0x744Cu16, 0x747E, 0x7480] {
            let (mode, _, platform) = classify(0x1002, device, "AMD Radeon RX 7900 XTX");
            assert_eq!(mode, Mode::Int8, "device 0x{device:04X}");
            assert!(!platform.needs_fsr4_driver_notice());
        }
    }

    #[test]
    fn rdna2_igpu_stays_unsupported() {
        // Rembrandt 680M：仍需同步交叠，但 FSR4 不启用
        let (mode, _, platform) = classify(0x1002, 0x1681, "AMD Radeon(TM) 680M");
        assert_eq!(mode, Mode::Auto);
        assert_eq!(platform, Platform::Rdna2Igpu);
        assert!(!platform.needs_fsr4_driver_notice());
    }

    #[test]
    fn amd_name_fallbacks() {
        let (mode, _, platform) = classify(0x1002, 0, "AMD Radeon RX 6800 XT");
        assert_eq!(mode, Mode::Int8);
        assert_eq!(platform, Platform::Rdna2Dgpu);

        let (mode, _, platform) = classify(0x1002, 0, "AMD Radeon RX 9070 XT");
        assert_eq!(mode, Mode::Fp8);
        assert_eq!(platform, Platform::Rdna4);

        // 核显名字通常只有 "AMD Radeon(TM) Graphics"，靠 ID 命中
        let (mode, _, platform) = classify(0x1002, 0x1586, "AMD Radeon(TM) Graphics");
        assert_eq!(mode, Mode::Int8);
        assert_eq!(platform, Platform::Rdna35Igpu);
    }

    #[test]
    fn nvidia_and_intel_are_unchanged() {
        let (mode, _, platform) = classify(0x10DE, 0x2704, "NVIDIA GeForce RTX 4070");
        assert_eq!(mode, Mode::Int8);
        assert_eq!(platform, Platform::Nvidia);

        let (mode, _, platform) = classify(0x8086, 0x56A0, "Intel(R) Arc(TM) A770 Graphics");
        assert_eq!(mode, Mode::Int8);
        assert_eq!(platform, Platform::Intel);
    }

    #[test]
    fn unknown_device_is_auto() {
        let (mode, _, platform) = classify(0x1002, 0x1234, "AMD Radeon(TM) Graphics");
        assert_eq!(mode, Mode::Auto);
        assert_eq!(platform, Platform::Other);
    }

    #[test]
    fn vendor_falls_back_to_name_when_id_missing() {
        let (mode, _, platform) = classify(0, 0, "NVIDIA GeForce RTX 4070");
        assert_eq!(mode, Mode::Int8);
        assert_eq!(platform, Platform::Nvidia);
    }
}
