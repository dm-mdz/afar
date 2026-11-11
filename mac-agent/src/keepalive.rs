//! 常驻保活：显示器不休眠、系统不自动睡眠、屏保关闭 → 不会自动锁屏，后台程序截图不黑。
//! 只用公开接口（IOKit 电源断言 + defaults）。手动点"睡眠"仍然有效。

use core_foundation::base::TCFType;
use core_foundation::string::{CFString, CFStringRef};
use std::process::Command;

#[link(name = "IOKit", kind = "framework")]
extern "C" {
    fn IOPMAssertionCreateWithName(assertion_type: CFStringRef, level: u32, name: CFStringRef, id: *mut u32) -> i32;
    fn IOPMAssertionRelease(id: u32) -> i32;
}

const ASSERTION_ON: u32 = 255;

pub struct KeepAlive {
    ids: Vec<u32>,
}

impl KeepAlive {
    pub fn start() -> KeepAlive {
        let mut ids = Vec::new();
        let reason = CFString::new("Afar：保持显示常亮，后台截图不黑屏");
        for kind in ["PreventUserIdleDisplaySleep", "PreventUserIdleSystemSleep"] {
            let ty = CFString::new(kind);
            let mut id = 0u32;
            let r = unsafe {
                IOPMAssertionCreateWithName(
                    ty.as_concrete_TypeRef(),
                    ASSERTION_ON,
                    reason.as_concrete_TypeRef(),
                    &mut id,
                )
            };
            if r == 0 {
                info!("已开启 {kind}");
                ids.push(id);
            } else {
                warn!("开启 {kind} 失败，错误码 {r}");
            }
        }
        disable_screensaver();
        KeepAlive { ids }
    }
}

impl Drop for KeepAlive {
    fn drop(&mut self) {
        for id in &self.ids {
            unsafe {
                IOPMAssertionRelease(*id);
            }
        }
    }
}

/// 屏保会触发锁屏，所以把"闲置多久开屏保"设为永不。
fn disable_screensaver() {
    let r = Command::new("/usr/bin/defaults")
        .args(["-currentHost", "write", "com.apple.screensaver", "idleTime", "-int", "0"])
        .status();
    match r {
        Ok(s) if s.success() => info!("已关闭屏保（避免自动锁屏）"),
        Ok(s) => warn!("关闭屏保失败：{s}"),
        Err(e) => warn!("关闭屏保失败：{e}"),
    }
}
