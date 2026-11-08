//! 下载物校验：SHA-256（纯 Rust，不引新依赖）+ Authenticode（WinVerifyTrust）。
//!
//! 口径：**固定版本清单里的 SHA-256 是主校验**，Authenticode 只做第二道（挡住
//! "清单写错但文件本身是别人替换的"这一种）。清单缺失时不猜、直接失败。

use std::ffi::c_void;
use std::fs;
use std::path::Path;

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

fn compress(state: &mut [u32; 8], block: &[u8]) {
    let mut w = [0u32; 64];
    for (i, chunk) in block.chunks_exact(4).enumerate() {
        w[i] = u32::from_be_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
    }
    for i in 16..64 {
        let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
        let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
    }
    let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h) =
        (state[0], state[1], state[2], state[3], state[4], state[5], state[6], state[7]);
    for i in 0..64 {
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let ch = (e & f) ^ ((!e) & g);
        let t1 = h.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let maj = (a & b) ^ (a & c) ^ (b & c);
        let t2 = s0.wrapping_add(maj);
        h = g; g = f; f = e; e = d.wrapping_add(t1);
        d = c; c = b; b = a; a = t1.wrapping_add(t2);
    }
    for (slot, value) in state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
        *slot = slot.wrapping_add(value);
    }
}

/// 对一段内存算 SHA-256，返回大写十六进制。
pub fn sha256_hex(bytes: &[u8]) -> String {
    let mut state: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let mut offset = 0usize;
    while offset + 64 <= bytes.len() {
        compress(&mut state, &bytes[offset..offset + 64]);
        offset += 64;
    }
    let mut tail = Vec::from(&bytes[offset..]);
    tail.push(0x80);
    while tail.len() % 64 != 56 {
        tail.push(0);
    }
    tail.extend_from_slice(&((bytes.len() as u64) * 8).to_be_bytes());
    for block in tail.chunks_exact(64) {
        compress(&mut state, block);
    }
    state.iter().map(|word| format!("{word:08X}")).collect()
}

/// 文件 SHA-256（大写十六进制）。
pub fn sha256_file(path: &Path) -> Result<String, String> {
    let bytes = fs::read(path).map_err(|e| format!("读取文件失败（{}）：{e}", path.display()))?;
    Ok(sha256_hex(&bytes))
}

/// 校验文件 SHA-256；不匹配直接报错（调用方据此中止安装）。
pub fn check_sha256(path: &Path, expected: &str, label: &str) -> Result<(), String> {
    let expected = expected.trim().to_uppercase();
    if expected.len() != 64 {
        return Err(format!("{label}：清单里的 SHA-256 不合法（{}）", expected.len()));
    }
    let actual = sha256_file(path)?;
    if actual != expected {
        return Err(format!(
            "{label} 校验失败：期望 {expected}，实际 {actual}（文件可能被替换或下载不完整）"
        ));
    }
    Ok(())
}

#[repr(C)]
struct Guid {
    data1: u32,
    data2: u16,
    data3: u16,
    data4: [u8; 8],
}

#[repr(C)]
struct WinTrustFileInfo {
    cb_struct: u32,
    file_path: *const u16,
    file_handle: *mut c_void,
    known_subject: *const Guid,
}

#[repr(C)]
struct WinTrustData {
    cb_struct: u32,
    policy_callback: *mut c_void,
    sip_client: *mut c_void,
    ui_choice: u32,
    revocation_checks: u32,
    union_choice: u32,
    file: *const WinTrustFileInfo,
    state_action: u32,
    state_data: *mut c_void,
    url_reference: *const u16,
    prov_flags: u32,
    ui_context: u32,
    signature_settings: *mut c_void,
}

type WinVerifyTrustFn =
    unsafe extern "system" fn(hwnd: *mut c_void, action: *const Guid, data: *mut c_void) -> i32;

#[link(name = "kernel32")]
extern "system" {
    fn LoadLibraryW(path: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
}

/// 动态取 wintrust.dll!WinVerifyTrust：不引 import lib，构建机不需要额外的 SDK lib 路径。
fn win_verify_trust() -> Option<WinVerifyTrustFn> {
    use std::os::windows::ffi::OsStrExt;
    let name: Vec<u16> = std::ffi::OsStr::new("wintrust.dll")
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    unsafe {
        let module = LoadLibraryW(name.as_ptr());
        if module.is_null() {
            return None;
        }
        let proc = GetProcAddress(module, b"WinVerifyTrust\0".as_ptr());
        if proc.is_null() {
            return None;
        }
        Some(std::mem::transmute::<*mut c_void, WinVerifyTrustFn>(proc))
    }
}

/// Authenticode 信任链是否有效（0 = 有效）。
pub fn authenticode_trusted(path: &Path) -> Result<bool, String> {
    use std::os::windows::ffi::OsStrExt;
    let wide: Vec<u16> = path
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    // WINTRUST_ACTION_GENERIC_VERIFY_V2 {00AAC56B-CD44-11D0-8CC2-00C04FC295EE}
    let action = Guid {
        data1: 0x00AA_C56B,
        data2: 0xCD44,
        data3: 0x11D0,
        data4: [0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE],
    };
    let mut file = WinTrustFileInfo {
        cb_struct: std::mem::size_of::<WinTrustFileInfo>() as u32,
        file_path: wide.as_ptr(),
        file_handle: std::ptr::null_mut(),
        known_subject: std::ptr::null(),
    };
    let mut data = WinTrustData {
        cb_struct: std::mem::size_of::<WinTrustData>() as u32,
        policy_callback: std::ptr::null_mut(),
        sip_client: std::ptr::null_mut(),
        ui_choice: 2,          // WTD_UI_NONE
        revocation_checks: 0,  // WTD_REVOKE_NONE
        union_choice: 1,       // WTD_CHOICE_FILE
        file: &mut file,
        state_action: 0,       // WTD_STATEACTION_IGNORE
        state_data: std::ptr::null_mut(),
        url_reference: std::ptr::null(),
        prov_flags: 0x10 | 0x20, // WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL
        ui_context: 0,
        signature_settings: std::ptr::null_mut(),
    };
    let Some(verify) = win_verify_trust() else {
        return Err("系统里找不到 wintrust.dll，无法校验数字签名".to_string());
    };
    let status = unsafe { verify(std::ptr::null_mut(), &action, &mut data as *mut _ as *mut c_void) };
    Ok(status == 0)
}

/// 要求文件有有效的 Authenticode 签名（信任链有效即可，不校验具体签名者）。
pub fn require_authenticode(path: &Path, label: &str) -> Result<(), String> {
    match authenticode_trusted(path)? {
        true => Ok(()),
        false => Err(format!("{label} 没有有效的数字签名（可能被篡改）")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_known_vectors() {
        assert_eq!(
            sha256_hex(b""),
            "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"
        );
        assert_eq!(
            sha256_hex(b"abc"),
            "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
        );
        let long = vec![b'a'; 1000];
        assert_eq!(
            sha256_hex(&long),
            "41EDECE42D63E8D9BF515A9BA6932E1C20CBC9F5A5D134645ADB5DB1B9737EA3".to_string()
        );
    }

    #[test]
    fn authenticode_accepts_system_file_and_rejects_unsigned() {
        // 系统 DLL 必然带有效签名：用它验证 FFI 结构与 WTD_* 取值没写错。
        let signed = Path::new("C:\\Windows\\System32\\kernel32.dll");
        if signed.is_file() {
            assert_eq!(authenticode_trusted(signed).unwrap(), true, "kernel32.dll 应通过签名校验");
        }
        let dir = std::env::temp_dir();
        let unsigned = dir.join(format!("gfsr-unsigned-{}.bin", std::process::id()));
        fs::write(&unsigned, b"not signed").unwrap();
        assert_eq!(authenticode_trusted(&unsigned).unwrap(), false, "无签名文件必须被拒");
        assert!(require_authenticode(&unsigned, "x").is_err());
        let _ = fs::remove_file(&unsigned);
    }

    #[test]
    fn reject_bad_hash() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("gfsr-verify-{}.bin", std::process::id()));
        fs::write(&path, b"abc").unwrap();
        assert!(check_sha256(&path, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "x").is_ok());
        assert!(check_sha256(&path, "00", "x").is_err());
        assert!(check_sha256(&path, &"0".repeat(64), "x").is_err());
        let _ = fs::remove_file(&path);
    }
}
