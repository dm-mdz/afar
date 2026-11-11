//! 显示器操作：列显示器、镜像、切换缩放档位。
//! 基本只用公开的 CoreGraphics 接口；插回显示器恢复分辨率时，公开接口列不出的模式改用系统设置同款的 CGS 接口（见 `set_mode`）。

use core_foundation::base::TCFType;
use core_foundation::boolean::CFBoolean;
use core_foundation::dictionary::CFDictionary;
use core_foundation::string::{CFString, CFStringRef};
use core_foundation_sys::array::{CFArrayGetCount, CFArrayGetValueAtIndex, CFArrayRef};
use core_foundation_sys::base::CFRelease;
use core_foundation_sys::dictionary::CFDictionaryRef;
use std::ffi::c_void;

type ConfigRef = *mut c_void;
type ModeRef = *const c_void;

#[link(name = "CoreGraphics", kind = "framework")]
extern "C" {
    fn CGGetOnlineDisplayList(max: u32, displays: *mut u32, count: *mut u32) -> i32;
    fn CGBeginDisplayConfiguration(cfg: *mut ConfigRef) -> i32;
    fn CGConfigureDisplayMirrorOfDisplay(cfg: ConfigRef, display: u32, master: u32) -> i32;
    fn CGCompleteDisplayConfiguration(cfg: ConfigRef, option: u32) -> i32;
    fn CGCancelDisplayConfiguration(cfg: ConfigRef) -> i32;
    fn CGDisplayMirrorsDisplay(display: u32) -> u32;
    fn CGDisplayVendorNumber(display: u32) -> u32;
    fn CGDisplayModelNumber(display: u32) -> u32;
    fn CGDisplaySerialNumber(display: u32) -> u32;
    fn CGDisplayIsBuiltin(display: u32) -> u32;
    fn CGDisplayCopyAllDisplayModes(display: u32, options: CFDictionaryRef) -> CFArrayRef;
    fn CGDisplayCopyDisplayMode(display: u32) -> ModeRef;
    fn CGDisplayModeGetWidth(mode: ModeRef) -> usize;
    fn CGDisplayModeGetHeight(mode: ModeRef) -> usize;
    fn CGDisplayModeGetPixelWidth(mode: ModeRef) -> usize;
    fn CGDisplayModeGetPixelHeight(mode: ModeRef) -> usize;
    fn CGDisplayModeRelease(mode: ModeRef);
    fn CGDisplayModeGetRefreshRate(mode: ModeRef) -> f64;
    fn CGDisplayModeIsUsableForDesktopGUI(mode: ModeRef) -> bool;
    fn CGDisplaySetDisplayMode(display: u32, mode: ModeRef, options: CFDictionaryRef) -> i32;
    fn CGConfigureDisplayWithDisplayMode(cfg: ConfigRef, display: u32, mode: ModeRef, options: CFDictionaryRef) -> i32;
    static kCGDisplayShowDuplicateLowResolutionModes: CFStringRef;
    // 私有（SkyLight，经 CoreGraphics 导出）：系统设置 › 显示器用的模式表，含公开接口过滤掉的模式
    fn CGSGetNumberOfDisplayModes(display: u32, count: *mut i32) -> i32;
    fn CGSGetDisplayModeDescriptionOfLength(display: u32, index: i32, desc: *mut CgsMode, len: i32) -> i32;
    fn CGSConfigureDisplayMode(cfg: ConfigRef, display: u32, mode: i32) -> i32;
}

/// CGSGetDisplayModeDescriptionOfLength 返回的结构（0xD4 字节，布局同 displayplacer 等开源工具）。
#[repr(C)]
#[derive(Clone, Copy)]
struct CgsMode {
    mode: u32,
    flags: u32,
    width: u32,
    height: u32,
    depth: u32,
    _dc2: [u32; 42],
    _dc3: u16,
    freq: u16,
    _dc4: [u32; 4],
    scale: f32,
}

const CGS_MODE_LEN: usize = 0xD4;
const _: () = assert!(std::mem::size_of::<CgsMode>() == CGS_MODE_LEN);
/// IOKit kDisplayModeValidFlag：模式可用。
const CGS_VALID: u32 = 0x1;
/// 系统标成 unsupported 的模式（WindowServer 日志 "scaler, unsupported"）。
const CGS_UNSUPPORTED: u32 = 0x4000_0000;

/// 只在本次登录会话内生效（重启后由程序重新设置）。
const CONFIGURE_FOR_SESSION: u32 = 1;

#[derive(Debug, Clone, Copy)]
pub struct Mode {
    pub w: usize,
    pub h: usize,
    pub pw: usize,
    pub ph: usize,
    /// 刷新率（Hz；虚拟屏、部分显示器为 0）。
    pub hz: f64,
    /// 能当桌面用（有些模式只给全屏游戏 / 视频用）。
    pub usable: bool,
}

/// 用来比较、保存的模式：（宽、高、像素宽、像素高、刷新率×100）。
pub type ModeKey = (usize, usize, usize, usize, usize);

impl Mode {
    pub fn key(&self) -> ModeKey {
        (self.w, self.h, self.pw, self.ph, (self.hz * 100.0).round() as usize)
    }
}

/// 所有在线显示器（含被镜像的）。
pub fn online() -> Vec<u32> {
    let mut ids = [0u32; 16];
    let mut n = 0u32;
    let r = unsafe { CGGetOnlineDisplayList(16, ids.as_mut_ptr(), &mut n) };
    if r != 0 {
        return Vec::new();
    }
    ids[..n as usize].to_vec()
}

/// 系统给显示器的（厂商号, 型号, 序列号, 是否内建）。虚拟屏的厂商号是我们自己定的 0x4D50。
/// 用途：采集随航屏的这组编号，之后改成按编号确定地认随航屏（现在靠宽高比 + 出现时间推断）。
pub fn ident(display: u32) -> (u32, u32, u32, bool) {
    unsafe {
        (
            CGDisplayVendorNumber(display),
            CGDisplayModelNumber(display),
            CGDisplaySerialNumber(display),
            CGDisplayIsBuiltin(display) != 0,
        )
    }
}

/// display 正在镜像的主显示器 ID；没镜像返回 0。
pub fn mirrors(display: u32) -> u32 {
    unsafe { CGDisplayMirrorsDisplay(display) }
}

/// 让 display 镜像 master；master 为 0 = 解除镜像。
pub fn mirror(display: u32, master: u32) -> Result<(), String> {
    unsafe {
        let mut cfg: ConfigRef = std::ptr::null_mut();
        let r = CGBeginDisplayConfiguration(&mut cfg);
        if r != 0 {
            return Err(format!("开始显示配置失败 {r}"));
        }
        let r = CGConfigureDisplayMirrorOfDisplay(cfg, display, master);
        if r != 0 {
            CGCancelDisplayConfiguration(cfg);
            return Err(format!("设置镜像失败 {r}"));
        }
        let r = CGCompleteDisplayConfiguration(cfg, CONFIGURE_FOR_SESSION);
        if r != 0 {
            return Err(format!("应用镜像失败 {r}"));
        }
    }
    Ok(())
}

fn mode_of(m: ModeRef) -> Mode {
    unsafe {
        Mode {
            w: CGDisplayModeGetWidth(m),
            h: CGDisplayModeGetHeight(m),
            pw: CGDisplayModeGetPixelWidth(m),
            ph: CGDisplayModeGetPixelHeight(m),
            hz: CGDisplayModeGetRefreshRate(m),
            usable: CGDisplayModeIsUsableForDesktopGUI(m),
        }
    }
}

fn mode_options() -> CFDictionary<CFString, CFBoolean> {
    let key = unsafe { CFString::wrap_under_get_rule(kCGDisplayShowDuplicateLowResolutionModes) };
    CFDictionary::from_CFType_pairs(&[(key, CFBoolean::true_value())])
}

/// 列出显示器支持的全部模式（包括 HiDPI 模式）。
pub fn modes(display: u32) -> Vec<Mode> {
    let opts = mode_options();
    unsafe {
        let arr = CGDisplayCopyAllDisplayModes(display, opts.as_concrete_TypeRef());
        if arr.is_null() {
            return Vec::new();
        }
        let n = CFArrayGetCount(arr);
        let out = (0..n).map(|i| mode_of(CFArrayGetValueAtIndex(arr, i))).collect();
        CFRelease(arr as *const c_void);
        out
    }
}

pub fn current_mode(display: u32) -> Option<Mode> {
    unsafe {
        let m = CGDisplayCopyDisplayMode(display);
        if m.is_null() {
            return None;
        }
        let out = mode_of(m);
        CGDisplayModeRelease(m);
        Some(out)
    }
}

/// 在 display 的模式列表里找"看起来是 w×h、像素是 2 倍"的 HiDPI 模式，交给 `f` 使用（返回系统错误码）。
fn with_hidpi_mode(display: u32, w: u32, h: u32, f: impl FnOnce(ModeRef) -> i32) -> Result<(), String> {
    let opts = mode_options();
    unsafe {
        let arr = CGDisplayCopyAllDisplayModes(display, opts.as_concrete_TypeRef());
        if arr.is_null() {
            return Err("读不到显示模式".into());
        }
        let n = CFArrayGetCount(arr);
        let mut result = Err(format!("没有 {w}×{h} 的 HiDPI 模式"));
        for i in 0..n {
            let m = CFArrayGetValueAtIndex(arr, i);
            let md = mode_of(m);
            if md.w == w as usize && md.h == h as usize && md.pw == md.w * 2 && md.ph == md.h * 2 {
                let r = f(m);
                result = if r == 0 { Ok(()) } else { Err(format!("切换模式失败 {r}")) };
                break;
            }
        }
        CFRelease(arr as *const c_void);
        result
    }
}

/// 切到和 `want` 一样的模式（宽高、像素宽高一致、能当桌面用；刷新率一致的优先，没有就取最高的）。
/// 只在本次登录会话内生效：每次插回 Afar 都会改，不写进系统的显示设置（写进去可能连镜像排列一起记住）。
pub fn set_mode(display: u32, want: ModeKey) -> Result<(), String> {
    let (w, h, pw, ph, hz) = want;
    let opts = mode_options();
    unsafe {
        let arr = CGDisplayCopyAllDisplayModes(display, opts.as_concrete_TypeRef());
        if arr.is_null() {
            return Err("读不到显示模式".into());
        }
        let n = CFArrayGetCount(arr);
        let mut best: Option<(ModeRef, Mode)> = None;
        for i in 0..n {
            let m = CFArrayGetValueAtIndex(arr, i);
            let md = mode_of(m);
            if (md.w, md.h, md.pw, md.ph) != (w, h, pw, ph) || !md.usable {
                continue;
            }
            let better = match &best {
                None => true,
                Some((_, b)) => {
                    let exact = |x: &Mode| x.key().4 == hz;
                    (exact(&md) && !exact(b)) || (exact(&md) == exact(b) && md.hz > b.hz)
                }
            };
            if better {
                best = Some((m, md));
            }
        }
        let result = match best {
            // 公开接口只列显示器标了 safe 的模式。实测（v0.4.6，一块便携显示器）：EDID 只把 800×600 标成 default/safe，
            // 1024×768…1920×1080 的 IOFlags 是 0x1，CGDisplayCopyAllDisplayModes 一个都不给，当前就是 1920×1080 时也列不出；
            // 插回后系统套默认配置掉到 800×600，公开接口永远改不回去。系统设置用 CGS 模式表，能列出能切。
            None => set_mode_cgs(display, want).map_err(|e| format!("没有 {w}×{h}（像素 {pw}×{ph}）这个模式；{e}")),
            Some((m, _)) => {
                let mut cfg: ConfigRef = std::ptr::null_mut();
                let mut r = CGBeginDisplayConfiguration(&mut cfg);
                if r == 0 {
                    r = CGConfigureDisplayWithDisplayMode(cfg, display, m, std::ptr::null());
                    r = if r == 0 {
                        CGCompleteDisplayConfiguration(cfg, CONFIGURE_FOR_SESSION)
                    } else {
                        CGCancelDisplayConfiguration(cfg);
                        r
                    };
                }
                if r == 0 {
                    Ok(())
                } else {
                    Err(format!("切换模式失败 {r}"))
                }
            }
        };
        CFRelease(arr as *const c_void);
        result
    }
}

/// CGS 模式表（只要可用的）：（模式号, Mode）。
fn cgs_modes(display: u32) -> Vec<(i32, Mode)> {
    let mut n = 0i32;
    if unsafe { CGSGetNumberOfDisplayModes(display, &mut n) } != 0 {
        return Vec::new();
    }
    (0..n)
        .filter_map(|i| {
            let mut d: CgsMode = unsafe { std::mem::zeroed() };
            let r = unsafe { CGSGetDisplayModeDescriptionOfLength(display, i, &mut d, CGS_MODE_LEN as i32) };
            cgs_usable(r, &d).then(|| (d.mode as i32, cgs_to_mode(&d)))
        })
        .collect()
}

fn cgs_usable(r: i32, d: &CgsMode) -> bool {
    r == 0 && d.flags & CGS_VALID != 0 && d.flags & CGS_UNSUPPORTED == 0 && d.width > 0 && d.height > 0
}

fn cgs_to_mode(d: &CgsMode) -> Mode {
    let s = if d.scale >= 1.5 { 2 } else { 1 };
    Mode {
        w: d.width as usize,
        h: d.height as usize,
        pw: d.width as usize * s,
        ph: d.height as usize * s,
        hz: d.freq as f64,
        usable: true,
    }
}

/// 在 CGS 模式里挑和 want 一样的（宽高、像素宽高；刷新率一致的优先，没有就取最高的）。
fn pick_cgs(list: &[(i32, Mode)], want: ModeKey) -> Option<i32> {
    let (w, h, pw, ph, hz) = want;
    let exact = |m: &Mode| m.key().4 == hz;
    list.iter()
        .filter(|(_, m)| (m.w, m.h, m.pw, m.ph) == (w, h, pw, ph))
        .max_by(|(_, a), (_, b)| (exact(a), a.hz).partial_cmp(&(exact(b), b.hz)).unwrap_or(std::cmp::Ordering::Equal))
        .map(|(n, _)| *n)
}

/// 用 CGS 模式号切模式（同样只在本次登录会话内生效）。
fn set_mode_cgs(display: u32, want: ModeKey) -> Result<(), String> {
    let list = cgs_modes(display);
    let Some(n) = pick_cgs(&list, want) else {
        return Err(format!("系统模式表里也没有（{} 个可用）", list.len()));
    };
    unsafe {
        let mut cfg: ConfigRef = std::ptr::null_mut();
        let mut r = CGBeginDisplayConfiguration(&mut cfg);
        if r == 0 {
            r = CGSConfigureDisplayMode(cfg, display, n);
            r = if r == 0 {
                CGCompleteDisplayConfiguration(cfg, CONFIGURE_FOR_SESSION)
            } else {
                CGCancelDisplayConfiguration(cfg);
                r
            };
        }
        if r == 0 {
            Ok(())
        } else {
            Err(format!("按系统模式表切换失败 {r}（模式 {n}）"))
        }
    }
}

/// 系统模式表里有、公开接口列不出的模式（日志用）："1024×768 … 1920×1080"。
pub fn hidden_modes(display: u32) -> String {
    let public: Vec<ModeKey> = modes(display).iter().filter(|m| m.usable).map(|m| m.key()).collect();
    let mut v: Vec<String> = Vec::new();
    for (_, m) in cgs_modes(display) {
        let s = format!("{}×{}{}", m.w, m.h, if m.pw == m.w * 2 { "@2x" } else { "" });
        if !public.iter().any(|k| (k.0, k.1, k.2, k.3) == (m.w, m.h, m.pw, m.ph)) && !v.contains(&s) {
            v.push(s);
        }
    }
    v.join(" ")
}

/// 切到"看起来是 w×h、像素是 2 倍"的 HiDPI 模式。
pub fn set_hidpi_mode(display: u32, w: u32, h: u32) -> Result<(), String> {
    with_hidpi_mode(display, w, h, |m| unsafe { CGDisplaySetDisplayMode(display, m, std::ptr::null()) })
}

/// 同上，但在同一次显示配置里先解除"镜像 display 的显示器"（随航屏），再切模式。
/// 实测（AFAR-003）：随航屏镜像着虚拟屏时直接切模式返回 1001；解除镜像后由主循环重新镜像。
pub fn set_hidpi_mode_unmirrored(display: u32, w: u32, h: u32) -> Result<(), String> {
    let followers: Vec<u32> = online().into_iter().filter(|d| *d != display && mirrors(*d) == display).collect();
    with_hidpi_mode(display, w, h, |m| unsafe {
        let mut cfg: ConfigRef = std::ptr::null_mut();
        let r = CGBeginDisplayConfiguration(&mut cfg);
        if r != 0 {
            return r;
        }
        for d in &followers {
            // 镜像目标设成 0（kCGNullDirectDisplay）= 解除镜像
            let r = CGConfigureDisplayMirrorOfDisplay(cfg, *d, 0);
            if r != 0 {
                CGCancelDisplayConfiguration(cfg);
                return r;
            }
        }
        let r = CGConfigureDisplayWithDisplayMode(cfg, display, m, std::ptr::null());
        if r != 0 {
            CGCancelDisplayConfiguration(cfg);
            return r;
        }
        CGCompleteDisplayConfiguration(cfg, CONFIGURE_FOR_SESSION)
    })
}
