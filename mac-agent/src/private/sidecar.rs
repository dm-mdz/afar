//! 随航（私有框架 SidecarCore）。只能从 Mac 端发起连接。

use block2::RcBlock;
use objc2::msg_send;
use objc2::rc::Retained;
use objc2::runtime::{AnyClass, AnyObject};
use objc2_foundation::NSString;
use std::sync::Once;

static LOAD: Once = Once::new();

fn load_framework() {
    LOAD.call_once(|| unsafe {
        let p = c"/System/Library/PrivateFrameworks/SidecarCore.framework/SidecarCore";
        if libc::dlopen(p.as_ptr(), libc::RTLD_LAZY).is_null() {
            warn!("加载 SidecarCore 失败");
        }
    });
}

fn manager() -> Result<Retained<AnyObject>, String> {
    load_framework();
    let cls =
        AnyClass::get(c"SidecarDisplayManager").ok_or("找不到随航接口 SidecarDisplayManager（系统版本变了？）")?;
    let m: Option<Retained<AnyObject>> = unsafe { msg_send![cls, sharedManager] };
    m.ok_or_else(|| "取不到随航管理器".to_string())
}

pub struct Device {
    pub name: String,
    obj: Retained<AnyObject>,
}

unsafe fn name_of(obj: &AnyObject) -> String {
    let n: Option<Retained<NSString>> = msg_send![obj, name];
    n.map(|s| s.to_string()).unwrap_or_default()
}

/// 调用返回 NSArray 的方法，逐个取出设备。
fn list(which: &str) -> Result<Vec<Device>, String> {
    let m = manager()?;
    unsafe {
        let arr: Option<Retained<AnyObject>> = match which {
            "connected" => msg_send![&*m, connectedDevices],
            _ => msg_send![&*m, devices],
        };
        let Some(arr) = arr else { return Ok(Vec::new()) };
        let n: usize = msg_send![&*arr, count];
        let mut out = Vec::with_capacity(n);
        for i in 0..n {
            let obj: Retained<AnyObject> = msg_send![&*arr, objectAtIndex: i];
            out.push(Device { name: name_of(&obj), obj });
        }
        Ok(out)
    }
}

/// 附近可用的 iPad。
pub fn devices() -> Result<Vec<Device>, String> {
    list("devices")
}

/// 正在当随航屏幕的 iPad。
pub fn connected() -> Result<Vec<Device>, String> {
    list("connected")
}

/// 连续失败了几次（成功清零）。iPad 锁一整夜时自动连回每 15 秒失败一次，日志只记前 3 次和之后每 20 次。
static FAILS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

fn completion(action: &'static str, name: String) -> RcBlock<dyn Fn(*mut AnyObject)> {
    use std::sync::atomic::Ordering::Relaxed;
    RcBlock::new(move |err: *mut AnyObject| {
        if err.is_null() {
            FAILS.store(0, Relaxed);
            info!("随航{action}成功：{name}");
        } else {
            let n = FAILS.fetch_add(1, Relaxed) + 1;
            if n <= 3 || n.is_multiple_of(20) {
                let desc: Option<Retained<NSString>> = unsafe { msg_send![err, localizedDescription] };
                warn!("随航{action}失败（连续第 {n} 次）：{name}：{}", desc.map(|d| d.to_string()).unwrap_or_default());
            }
        }
    })
}

/// 发起连接（异步，结果写日志；需要主 RunLoop 在转）。
pub fn connect(dev: &Device) -> Result<(), String> {
    let m = manager()?;
    let block = completion("连接", dev.name.clone());
    unsafe {
        let _: () = msg_send![&*m, connectToDevice: &*dev.obj, completion: &*block];
    }
    Ok(())
}

/// 断开（异步）。
pub fn disconnect(dev: &Device) -> Result<(), String> {
    let m = manager()?;
    let block = completion("断开", dev.name.clone());
    unsafe {
        let _: () = msg_send![&*m, disconnectFromDevice: &*dev.obj, completion: &*block];
    }
    Ok(())
}
