//! 局域网发现：用系统自带的 Bonjour（dns_sd，公开接口）广播 `_afar._tcp`，iPad App 打开就能看到这台 Mac。

use std::ffi::{c_char, c_void, CString};

type ServiceRef = *mut c_void;
type RegisterReply = extern "C" fn(ServiceRef, u32, i32, *const c_char, *const c_char, *const c_char, *mut c_void);

extern "C" {
    fn DNSServiceRegister(
        sd_ref: *mut ServiceRef,
        flags: u32,
        interface_index: u32,
        name: *const c_char,
        regtype: *const c_char,
        domain: *const c_char,
        host: *const c_char,
        port_be: u16,
        txt_len: u16,
        txt: *const c_void,
        callback: Option<RegisterReply>,
        context: *mut c_void,
    ) -> i32;
    fn DNSServiceRefDeallocate(sd_ref: ServiceRef);
}

extern "C" fn on_reply(
    _: ServiceRef,
    _: u32,
    _: i32,
    _: *const c_char,
    _: *const c_char,
    _: *const c_char,
    _: *mut c_void,
) {
}

/// 注册着的服务；丢弃即注销。
pub struct Registration(ServiceRef);

// 句柄只在主线程使用和释放。
unsafe impl Send for Registration {}

impl Drop for Registration {
    fn drop(&mut self) {
        unsafe { DNSServiceRefDeallocate(self.0) }
    }
}

/// Bonjour 实例名最多 63 字节（中文一个字 3 字节），按字截断、不切坏字。
fn fit_63_bytes(name: &str) -> String {
    let mut out = String::new();
    for ch in name.chars().filter(|c| *c != '\0') {
        if out.len() + ch.len_utf8() > 63 {
            break;
        }
        out.push(ch);
    }
    out
}

pub fn register(name: &str, port: u16) -> Result<Registration, String> {
    let name = CString::new(fit_63_bytes(name)).map_err(|e| e.to_string())?;
    let ty = CString::new(afar_proto::SERVICE_TYPE).expect("常量合法");
    let mut r: ServiceRef = std::ptr::null_mut();
    let err = unsafe {
        DNSServiceRegister(
            &mut r,
            0,
            0,
            name.as_ptr(),
            ty.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            port.to_be(),
            0,
            std::ptr::null(),
            Some(on_reply),
            std::ptr::null_mut(),
        )
    };
    if err != 0 || r.is_null() {
        return Err(format!("Bonjour 注册失败（错误码 {err}）"));
    }
    Ok(Registration(r))
}
