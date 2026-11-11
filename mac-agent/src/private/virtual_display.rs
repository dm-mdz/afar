//! 虚拟显示器（私有类 CGVirtualDisplay）。程序活着它就在，程序退出系统自动移除。
//! 作用：没接显示器的 Mac mini 也有一块"屏"——后台程序截图有画面，随航有主屏可镜像。

use objc2::rc::{Allocated, Retained};
use objc2::runtime::{AnyClass, AnyObject, Bool, Sel};
use objc2::{msg_send, sel};
use objc2_foundation::{NSArray, NSSize, NSString};
use std::ffi::CStr;

extern "C" {
    /// dispatch_get_main_queue() 在 C 里是宏，实际就是这个全局对象的地址。
    static _dispatch_main_q: u8;
}

pub struct VirtualDisplay {
    _obj: Retained<AnyObject>,
    pub id: u32,
}

fn class(name: &CStr) -> Result<&'static AnyClass, String> {
    AnyClass::get(name).ok_or_else(|| format!("找不到私有类 {name:?}（系统版本变了？）"))
}

unsafe fn responds(obj: &AnyObject, s: Sel) -> bool {
    let r: Bool = msg_send![obj, respondsToSelector: s];
    r.as_bool()
}

/// 创建虚拟屏。`pixel_modes` 是模式列表（写法见 scale::vd_modes），`max` 是最大像素（宽、高）。
pub fn create(pixel_modes: &[(u32, u32)], max: (u32, u32), name: &str, serial: u32) -> Result<VirtualDisplay, String> {
    if pixel_modes.is_empty() {
        return Err("没有可用的分辨率".into());
    }
    let max_w = max.0.max(pixel_modes.iter().map(|m| m.0).max().unwrap_or(0));
    let max_h = max.1.max(pixel_modes.iter().map(|m| m.1).max().unwrap_or(0));

    unsafe {
        let desc: Retained<AnyObject> = msg_send![class(c"CGVirtualDisplayDescriptor")?, new];
        let queue = std::ptr::addr_of!(_dispatch_main_q) as *mut AnyObject;
        if responds(&desc, sel!(setDispatchQueue:)) {
            let _: () = msg_send![&*desc, setDispatchQueue: queue];
        } else {
            let _: () = msg_send![&*desc, setQueue: queue];
        }
        let ns_name = NSString::from_str(name);
        let _: () = msg_send![&*desc, setName: &*ns_name];
        let _: () = msg_send![&*desc, setMaxPixelsWide: max_w];
        let _: () = msg_send![&*desc, setMaxPixelsHigh: max_h];
        // 物理尺寸只影响系统估算的 DPI；按 iPad Air 4 屏幕约 247mm × 172mm 给。
        let _: () = msg_send![&*desc, setSizeInMillimeters: NSSize::new(247.0, 172.0)];
        let _: () = msg_send![&*desc, setVendorID: 0x4D50u32];
        let _: () = msg_send![&*desc, setProductID: 0x0001u32];
        let _: () = msg_send![&*desc, setSerialNum: serial];

        let alloc: Allocated<AnyObject> = msg_send![class(c"CGVirtualDisplay")?, alloc];
        let vd: Option<Retained<AnyObject>> = msg_send![alloc, initWithDescriptor: &*desc];
        let vd = vd.ok_or("系统拒绝创建虚拟屏")?;

        let settings: Retained<AnyObject> = msg_send![class(c"CGVirtualDisplaySettings")?, new];
        let _: () = msg_send![&*settings, setHiDPI: 1u32];
        let mode_cls = class(c"CGVirtualDisplayMode")?;
        let mut modes: Vec<Retained<AnyObject>> = Vec::new();
        for &(w, h) in pixel_modes {
            let a: Allocated<AnyObject> = msg_send![mode_cls, alloc];
            let m: Retained<AnyObject> = msg_send![a, initWithWidth: w, height: h, refreshRate: 60.0f64];
            modes.push(m);
        }
        let refs: Vec<&AnyObject> = modes.iter().map(|m| &**m).collect();
        let arr = NSArray::from_slice(&refs);
        let _: () = msg_send![&*settings, setModes: &*arr];
        let ok: Bool = msg_send![&*vd, applySettings: &*settings];
        if !ok.as_bool() {
            return Err("虚拟屏分辨率设置失败".into());
        }
        let id: u32 = msg_send![&*vd, displayID];
        Ok(VirtualDisplay { _obj: vd, id })
    }
}
