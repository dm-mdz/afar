//! 在 iPad App 里配对新的蓝牙键盘 / 触控板（不用去 Mac 的蓝牙设置）：
//! 搜索附近处在配对模式的外设 → 用户在 App 里点一下 → 配对；键盘要求输数字时，把数字报给 App 显示。
//! 用公开框架 IOBluetooth 的 IOBluetoothDeviceInquiry / IOBluetoothDevicePair（blueutil 的 --inquiry / --pair 同一套）。
//! 回调走主线程的 RunLoop（主循环每秒都在转），所以这里的函数都在主线程上调用。

use afar_proto::{KeyboardInfo, PairInfo};
use objc2::encode::{Encode, Encoding, RefEncode};
use objc2::rc::Retained;
use objc2::runtime::{AnyClass, AnyObject, Bool, NSObject};
use objc2::{define_class, msg_send, MainThreadMarker, MainThreadOnly};
use objc2_foundation::NSString;
use std::cell::RefCell;
use std::sync::Mutex;

/// 老式配对用的数字（系统类型 BluetoothPINCode：16 字节）。
#[repr(C)]
struct PinCode {
    data: [u8; 16],
}
unsafe impl Encode for PinCode {
    const ENCODING: Encoding = Encoding::Struct("BluetoothPINCode", &[Encoding::Array(16, &Encoding::UChar)]);
}
unsafe impl RefEncode for PinCode {
    const ENCODING_REF: Encoding = Encoding::Pointer(&Self::ENCODING);
}

/// 设备大类"外设"（键盘、鼠标、触控板）。
const MAJOR_PERIPHERAL: u32 = 0x05;

/// 报给 App 的配对进度。stage：searching | searched | pairing | done | failed。
static INFO: Mutex<Option<PairInfo>> = Mutex::new(None);
/// 配对成功、等主循环选定为"蓝牙键盘"的设备。
static PAIRED: Mutex<Option<KeyboardInfo>> = Mutex::new(None);

thread_local! {
    /// 进行中的搜索 / 配对对象和回调对象（要一直持有，否则系统会提前释放）。
    static CUR: RefCell<Cur> = RefCell::new(Cur::default());
}

#[derive(Default)]
struct Cur {
    delegate: Option<Retained<PairDelegate>>,
    inquiry: Option<Retained<AnyObject>>,
    pair: Option<Retained<AnyObject>>,
    /// 搜到的设备对象（配对时直接用，按地址找）。
    found: Vec<(String, Retained<AnyObject>)>,
}

fn set_info(f: impl FnOnce(&mut PairInfo)) {
    let mut g = INFO.lock().unwrap();
    let info = g.get_or_insert_with(PairInfo::default);
    f(info);
}

unsafe fn text(obj: &AnyObject, addr: bool) -> String {
    let s: Option<Retained<NSString>> =
        if addr { msg_send![obj, addressString] } else { msg_send![obj, nameOrAddress] };
    s.map(|s| s.to_string()).unwrap_or_default()
}

define_class!(
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "AfarPairDelegate"]
    struct PairDelegate;

    impl PairDelegate {
        // ── 搜索 ──
        #[unsafe(method(deviceInquiryDeviceFound:device:))]
        fn device_found(&self, _sender: *mut AnyObject, device: *mut AnyObject) {
            let Some(d) = (unsafe { Retained::retain(device) }) else { return };
            let major: u32 = unsafe { msg_send![&*d, deviceClassMajor] };
            let paired: Bool = unsafe { msg_send![&*d, isPaired] };
            if major != MAJOR_PERIPHERAL || paired.as_bool() {
                return;
            }
            let (addr, name) = unsafe { (text(&d, true), text(&d, false)) };
            info!("搜到蓝牙外设：{name}（{addr}）");
            CUR.with(|c| {
                let mut c = c.borrow_mut();
                if !c.found.iter().any(|(a, _)| *a == addr) {
                    c.found.push((addr.clone(), d));
                }
            });
            set_info(|i| {
                if !i.found.iter().any(|k| k.address == addr) {
                    i.found.push(KeyboardInfo { address: addr, name, ..Default::default() });
                }
            });
        }

        #[unsafe(method(deviceInquiryDeviceNameUpdated:device:devicesRemaining:))]
        fn name_updated(&self, _sender: *mut AnyObject, device: *mut AnyObject, _remaining: u32) {
            let Some(d) = (unsafe { Retained::retain(device) }) else { return };
            let (addr, name) = unsafe { (text(&d, true), text(&d, false)) };
            set_info(|i| {
                if let Some(k) = i.found.iter_mut().find(|k| k.address == addr) {
                    k.name = name;
                }
            });
        }

        #[unsafe(method(deviceInquiryComplete:error:aborted:))]
        fn inquiry_done(&self, _sender: *mut AnyObject, error: i32, _aborted: Bool) {
            set_info(|i| {
                if i.stage == "searching" {
                    i.stage = "searched".into();
                    if error != 0 {
                        i.message = format!("搜索出错（系统返回 {error:#x}）");
                    }
                }
            });
        }

        // ── 配对 ──
        #[unsafe(method(devicePairingStarted:))]
        fn pairing_started(&self, _sender: *mut AnyObject) {}

        #[unsafe(method(devicePairingConnecting:))]
        fn pairing_connecting(&self, _sender: *mut AnyObject) {}

        /// 老式配对：Mac 定一个数字，用户在键盘上输入后回车。
        #[unsafe(method(devicePairingPINCodeRequest:))]
        fn pin_request(&self, sender: *mut AnyObject) {
            let pin: String = format!("{:04}", rand::random::<u16>() % 10000);
            let mut code = PinCode { data: [0u8; 16] };
            code.data[..4].copy_from_slice(pin.as_bytes());
            unsafe {
                let _: () = msg_send![sender, replyPINCode: 4usize, PINCode: &mut code as *mut PinCode];
            }
            set_info(|i| i.code = pin);
        }

        /// 数字比对：键盘没有屏幕，直接确认。
        #[unsafe(method(devicePairingUserConfirmationRequest:numericValue:))]
        fn confirm_request(&self, sender: *mut AnyObject, _value: u32) {
            unsafe {
                let _: () = msg_send![sender, replyUserConfirmation: Bool::YES];
            }
        }

        /// 键盘输入式配对：系统给出 6 位数字，用户在键盘上输入后回车。
        #[unsafe(method(devicePairingUserPasskeyNotification:passkey:))]
        fn passkey(&self, _sender: *mut AnyObject, passkey: u32) {
            set_info(|i| i.code = format!("{passkey:06}"));
        }

        #[unsafe(method(devicePairingFinished:error:))]
        fn pairing_finished(&self, _sender: *mut AnyObject, error: i32) {
            let mut done = None;
            set_info(|i| {
                if error == 0 {
                    i.stage = "done".into();
                    i.code.clear();
                    done = i.found.iter().find(|k| k.address == i.address).cloned();
                } else {
                    i.stage = "failed".into();
                    i.message = format!("配对没成功（系统返回 {error:#x}）。请让键盘重新进入配对模式后再试");
                }
            });
            match done {
                Some(k) => {
                    info!("蓝牙外设已配对：{}", k.name);
                    *PAIRED.lock().unwrap() = Some(k);
                }
                None => warn!("蓝牙配对失败：{error:#x}"),
            }
            // 不在这里释放配对对象（正在它的回调里），下次搜索 / 取消时再释放
        }
    }
);

fn delegate(mtm: MainThreadMarker) -> Retained<PairDelegate> {
    CUR.with(|c| {
        c.borrow_mut().delegate.get_or_insert_with(|| unsafe { msg_send![PairDelegate::alloc(mtm), init] }).clone()
    })
}

/// 开始搜索附近处在配对模式的外设。
pub fn search() -> Result<(), String> {
    let mtm = MainThreadMarker::new().ok_or("不在主线程上")?;
    cancel();
    let cls = AnyClass::get(c"IOBluetoothDeviceInquiry").ok_or("找不到 IOBluetoothDeviceInquiry")?;
    let d = delegate(mtm);
    let r: i32 = objc2::rc::autoreleasepool(|_| unsafe {
        let inq: Option<Retained<AnyObject>> = msg_send![cls, inquiryWithDelegate: &*d];
        let Some(inq) = inq else { return -1 };
        // 不调 setInquiryLength：旧版头文件里它返回 uint8_t、新版是属性，返回类型拿不准；
        // 类型对不上 debug 版会直接崩。用系统默认的搜索时长（约 10 秒），App 里可以"再搜一次"
        let _: () = msg_send![&*inq, setUpdateNewDeviceNames: Bool::YES];
        // 不设搜索条件（传 0 是"未分类"而不是"不限"，会什么都搜不到），在回调里按"外设"过滤
        let r: i32 = msg_send![&*inq, start];
        CUR.with(|c| c.borrow_mut().inquiry = Some(inq));
        r
    });
    if r != 0 {
        *INFO.lock().unwrap() = Some(PairInfo {
            stage: "failed".into(),
            message: format!("没能开始搜索（系统返回 {r:#x}），请确认 Mac 的蓝牙已打开"),
            ..Default::default()
        });
        return Err(format!("搜索失败 {r:#x}"));
    }
    info!("开始搜索蓝牙外设");
    *INFO.lock().unwrap() = Some(PairInfo { stage: "searching".into(), ..Default::default() });
    Ok(())
}

/// 和搜到的某台设备配对。
pub fn pair(address: &str) -> Result<(), String> {
    let mtm = MainThreadMarker::new().ok_or("不在主线程上")?;
    let dev = CUR.with(|c| c.borrow().found.iter().find(|(a, _)| a == address).map(|(_, d)| d.clone()));
    let dev = dev.ok_or("没有搜到这个设备，请重新搜索")?;
    if INFO.lock().unwrap().as_ref().is_some_and(|i| i.stage == "pairing") {
        return Err("正在配对，请稍候".into());
    }
    // 配对前先停止搜索（同时进行会互相干扰）。先取出再调用，不在借用期间回调
    let inq = CUR.with(|c| c.borrow_mut().inquiry.take());
    if let Some(inq) = inq {
        let _: i32 = unsafe { msg_send![&*inq, stop] };
    }
    let old = CUR.with(|c| c.borrow_mut().pair.take());
    if let Some(p) = old {
        let _: () = unsafe { msg_send![&*p, stop] };
    }
    let cls = AnyClass::get(c"IOBluetoothDevicePair").ok_or("找不到 IOBluetoothDevicePair")?;
    let d = delegate(mtm);
    let r: i32 = objc2::rc::autoreleasepool(|_| unsafe {
        let p: Option<Retained<AnyObject>> = msg_send![cls, pairWithDevice: &*dev];
        let Some(p) = p else { return -1 };
        let _: () = msg_send![&*p, setDelegate: &*d];
        let r: i32 = msg_send![&*p, start];
        CUR.with(|c| c.borrow_mut().pair = Some(p));
        r
    });
    set_info(|i| {
        i.address = address.into();
        i.code.clear();
        i.message.clear();
        if r == 0 {
            i.stage = "pairing".into();
        } else {
            i.stage = "failed".into();
            i.message = format!("没能开始配对（系统返回 {r:#x}）");
        }
    });
    if r == 0 {
        info!("开始配对 {address}");
        Ok(())
    } else {
        Err(format!("配对失败 {r:#x}"))
    }
}

/// 取消搜索 / 配对，清掉进度。
pub fn cancel() {
    // 先从 RefCell 里取出来再调 stop（stop 可能同步回调，回调里还要用 CUR）
    let (inq, pair) = CUR.with(|c| {
        let mut c = c.borrow_mut();
        c.found.clear();
        (c.inquiry.take(), c.pair.take())
    });
    if let Some(inq) = inq {
        let _: i32 = unsafe { msg_send![&*inq, stop] };
    }
    if let Some(p) = pair {
        let _: () = unsafe { msg_send![&*p, stop] };
    }
    *INFO.lock().unwrap() = None;
}

/// 直接报失败（比如没有蓝牙权限），App 不会一直转圈。
pub fn fail(message: &str) {
    cancel();
    *INFO.lock().unwrap() = Some(PairInfo { stage: "failed".into(), message: message.into(), ..Default::default() });
}

/// 当前进度（给状态接口）。
pub fn info() -> Option<PairInfo> {
    INFO.lock().unwrap().clone()
}

/// 刚配对成功的设备（取一次就清空）。
pub fn take_paired() -> Option<KeyboardInfo> {
    PAIRED.lock().unwrap().take()
}
