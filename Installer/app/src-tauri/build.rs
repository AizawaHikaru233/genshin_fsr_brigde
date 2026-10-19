use std::time::{SystemTime, UNIX_EPOCH};

/// 把 Unix 天数换算成 (年, 月, 日)，避免为了一个构建日期引入 chrono。
fn ymd_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn build_date() -> String {
    // 构建机在东八区；这里只是给「关于」页一个可读日期。
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
        + 8 * 3600;
    let (y, m, d) = ymd_from_days(secs / 86_400);
    format!("{y:04}-{m:02}-{d:02}")
}

fn main() {
    println!("cargo:rustc-env=GFSR_BUILD_DATE={}", build_date());
    tauri_build::build();
}
