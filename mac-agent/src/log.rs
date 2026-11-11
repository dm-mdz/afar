//! 极简日志：带本地时间，输出到 stderr（LaunchAgent 会写进 ~/Library/Logs/Afar/agent.log）。

pub fn emit(level: &str, msg: &str) {
    eprintln!("{} [{}] {}", now(), level, msg);
}

#[cfg(unix)]
fn now() -> String {
    unsafe {
        let t = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&t, &mut tm);
        format!(
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec
        )
    }
}

/// 非 Mac 系统只用于跑测试：给 Unix 秒数即可。
#[cfg(not(unix))]
fn now() -> String {
    let s = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    format!("t={s}")
}

#[macro_export]
macro_rules! info {
    ($($t:tt)*) => { $crate::log::emit("INFO", &format!($($t)*)) };
}

#[macro_export]
macro_rules! warn {
    ($($t:tt)*) => { $crate::log::emit("WARN", &format!($($t)*)) };
}

#[macro_export]
macro_rules! error {
    ($($t:tt)*) => { $crate::log::emit("ERROR", &format!($($t)*)) };
}
