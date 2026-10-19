use std::fs::{self, OpenOptions};
use std::io::Write;
use std::sync::Mutex;

use crate::paths;

static LOCK: Mutex<()> = Mutex::new(());

fn timestamp() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
        + 8 * 3600;
    let t = secs.rem_euclid(86_400);
    format!(
        "{:02}:{:02}:{:02}",
        t / 3600,
        (t % 3600) / 60,
        t % 60
    )
}

/// 追加一行日志；日志文件超过 2 MB 就先轮转成 .old.log。
pub fn write(line: &str) {
    let _guard = LOCK.lock();
    let dir = paths::config_dir();
    if fs::create_dir_all(&dir).is_err() {
        return;
    }
    let file = paths::log_file();
    if let Ok(meta) = fs::metadata(&file) {
        if meta.len() > 2 * 1024 * 1024 {
            let old = file.with_extension("old.log");
            let _ = fs::remove_file(&old);
            let _ = fs::rename(&file, &old);
        }
    }
    if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(&file) {
        let _ = writeln!(f, "{}\t{}", timestamp(), line);
    }
}
