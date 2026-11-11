//! 蓝牙键盘 / 鼠标 / 触控板：Mac 端接管和松开（公开框架 IOBluetooth，开源工具 blueutil 用的是同一套接口）。
//!
//! iPad 上的 App 没有断开系统蓝牙设备的接口，所以流程是：用户在 iPad 控制中心断开键盘 →
//! Mac 在一段时间内反复尝试连上它。连接是阻塞调用（每次最多等几秒），放在单独的线程里，不卡主循环。
//! 前提（待真机确认）：键盘能同时记住 Mac 和 iPad 两台设备；键盘走经典蓝牙（低功耗蓝牙键盘这里看不到）。

use afar_proto::KeyboardInfo;
use objc2::msg_send;
use objc2::rc::Retained;
use objc2::runtime::{AnyClass, AnyObject, Bool};
use objc2_foundation::NSString;
use std::sync::mpsc::{channel, RecvTimeoutError, Sender};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[link(name = "IOBluetooth", kind = "framework")]
extern "C" {}
#[link(name = "CoreBluetooth", kind = "framework")]
extern "C" {}

/// 蓝牙权限被拒（被拒时设备列表是空的，不能当成"没配对"）。
pub fn denied() -> bool {
    matches!(authorization(), 1 | 2)
}

/// CBManagerAuthorization：0 未决定 1 受限 2 拒绝 3 允许（拿不到类时当作允许）。
pub fn authorization() -> isize {
    let Some(cls) = AnyClass::get(c"CBManager") else { return 3 };
    unsafe { msg_send![cls, authorization] }
}

/// 设备大类"外设"（键盘、鼠标、触控板都在这一类）。
const MAJOR_PERIPHERAL: u32 = 0x05;
/// 多久检查 / 重试一次。
const POLL: Duration = Duration::from_secs(2);

fn class() -> Option<&'static AnyClass> {
    AnyClass::get(c"IOBluetoothDevice")
}

unsafe fn text(obj: &AnyObject, sel: &str) -> String {
    let s: Option<Retained<NSString>> = match sel {
        "addressString" => msg_send![obj, addressString],
        _ => msg_send![obj, nameOrAddress],
    };
    s.map(|s| s.to_string()).unwrap_or_default()
}

unsafe fn connected_of(obj: &AnyObject) -> bool {
    let c: Bool = msg_send![obj, isConnected];
    c.as_bool()
}

/// Mac 上已配对的外设（键盘、鼠标、触控板）。只在蓝牙线程里调用。
fn paired() -> Vec<KeyboardInfo> {
    objc2::rc::autoreleasepool(|_| unsafe {
        let Some(cls) = class() else { return Vec::new() };
        let arr: Option<Retained<AnyObject>> = msg_send![cls, pairedDevices];
        let Some(arr) = arr else { return Vec::new() };
        let n: usize = msg_send![&*arr, count];
        let mut out = Vec::new();
        for i in 0..n {
            let d: Retained<AnyObject> = msg_send![&*arr, objectAtIndex: i];
            let major: u32 = msg_send![&*d, deviceClassMajor];
            // 设备大类 0：低功耗蓝牙设备（v0.3.6 实测，一款低功耗蓝牙键盘是 0x00/0x00）。列出来但标明不能交接：
            // 系统能让它"断开"（返回成功），实际还连着
            if major != MAJOR_PERIPHERAL && major != 0 {
                continue;
            }
            let name = text(&d, "nameOrAddress");
            // 低功耗蓝牙设备分不出类别：iPad、iPhone、耳机这些按名字排除
            if major == 0 && crate::kbflow::is_apple_device(&name) {
                continue;
            }
            out.push(KeyboardInfo {
                address: text(&d, "addressString"),
                name,
                connected: connected_of(&d),
                ble: major == 0,
            });
        }
        out
    })
}

fn with_device<T>(addr: &str, f: impl FnOnce(&AnyObject) -> T) -> Option<T> {
    objc2::rc::autoreleasepool(|_| unsafe {
        let cls = class()?;
        let a = NSString::from_str(addr);
        let d: Option<Retained<AnyObject>> = msg_send![cls, deviceWithAddressString: &*a];
        d.map(|d| f(&d))
    })
}

pub fn is_connected(addr: &str) -> bool {
    with_device(addr, |d| unsafe { connected_of(d) }).unwrap_or(false)
}

/// 连上（阻塞，最多等系统的寻呼超时，几秒）。返回系统错误码，0 = 成功。
fn open(addr: &str) -> i32 {
    with_device(addr, |d| unsafe { msg_send![d, openConnection] }).unwrap_or(-1)
}

fn close(addr: &str) -> i32 {
    with_device(addr, |d| unsafe { msg_send![d, closeConnection] }).unwrap_or(-1)
}

/// 报给 iPad App 的实时情况（定义在 kbflow.rs，那里能跨平台测试；锁不对外暴露）。
pub use crate::kbflow::LiveCell;

pub enum Req {
    /// 在 `for` 时间内反复尝试连上。
    Take(String, Duration),
    Release(String),
    /// 只看这个设备的连接状态（选定 / 换键盘时）；None = 不再看。
    Watch(Option<String>),
    /// 列出已配对的外设（连接 / 断开 / 列表都在这一个线程里做；搜索和配对在主线程，见 bt_pair.rs）。
    List(Sender<Result<Vec<KeyboardInfo>, String>>),
}

/// 通过蓝牙线程列出已配对的外设。
pub fn list(tx: &Sender<Req>) -> Result<Vec<KeyboardInfo>, String> {
    let (rtx, rrx) = channel();
    tx.send(Req::List(rtx)).map_err(|_| "蓝牙线程已退出".to_string())?;
    // 蓝牙线程可能正卡在一次连接尝试上（最多几秒）
    rrx.recv_timeout(Duration::from_secs(12)).map_err(|_| "读取蓝牙设备超时，请稍后再试".to_string())?
}

/// 启动后台线程，返回发指令的通道。
pub fn spawn(live: Arc<LiveCell>) -> Sender<Req> {
    let (tx, rx) = channel::<Req>();
    std::thread::spawn(move || {
        let mut watch: Option<String> = None;
        let mut target: Option<(String, Instant)> = None;
        let mut last_code = 0;
        let mut secs = 0;
        loop {
            match rx.recv_timeout(POLL) {
                Ok(Req::Take(a, d)) => {
                    info!("等待接管蓝牙键盘 {a}（{} 秒内）", d.as_secs());
                    watch = Some(a.clone());
                    target = Some((a, Instant::now() + d));
                    secs = d.as_secs();
                    last_code = 0;
                    live.update(|l| {
                        l.error.clear();
                        l.released_at = None;
                    });
                }
                Ok(Req::Release(a)) => {
                    target = None;
                    watch = Some(a.clone());
                    live.update(|l| l.error.clear());
                    if is_connected(&a) {
                        let r = close(&a);
                        if r == 0 {
                            info!("已松开蓝牙键盘 {a}");
                            live.update(|l| l.released_at = Some(Instant::now()));
                        } else {
                            warn!("松开蓝牙键盘失败：{r:#x}");
                            let msg = format!("Mac 没能松开键盘（系统返回 {r:#x}），可在 Mac 的蓝牙设置里断开");
                            live.update(|l| l.error = msg);
                        }
                    }
                }
                Ok(Req::Watch(a)) => {
                    target = None;
                    watch = a;
                    live.update(|l| l.released_at = None);
                }
                Ok(Req::List(reply)) => {
                    let r = if denied() {
                        Err("Afar 没有蓝牙权限：请在 Mac 的 系统设置 › 隐私与安全 › 蓝牙 里允许 afar-agent".to_string())
                    } else {
                        Ok(paired())
                    };
                    let _ = reply.send(r);
                }
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => return,
            }
            if let Some((a, until)) = target.clone() {
                if is_connected(&a) {
                    info!("蓝牙键盘已接到 Mac");
                    target = None;
                } else if Instant::now() > until {
                    warn!("没能接管蓝牙键盘（最后一次系统返回 {last_code:#x}）");
                    let msg: String = if denied() {
                        "Afar 没有蓝牙权限：请在 Mac 的 系统设置 › 隐私与安全 › 蓝牙 里允许 afar-agent".into()
                    } else {
                        format!("{secs} 秒内没接上键盘。请确认已在 iPad 控制中心断开它；如果键盘只能记住一台设备，这个功能用不了")
                    };
                    live.update(|l| l.error = msg);
                    target = None;
                } else {
                    last_code = open(&a);
                }
            }
            let connected = watch.as_deref().map(is_connected).unwrap_or(false);
            let taking = target.is_some();
            live.update(|l| {
                l.connected = connected;
                l.taking = taking;
            });
        }
    });
    tx
}

/// 诊断用（`afar-agent bt-dump`）：系统经典蓝牙接口看到的全部已配对设备，不按设备大类过滤。
pub fn dump_paired() -> Vec<String> {
    objc2::rc::autoreleasepool(|_| unsafe {
        let Some(cls) = class() else { return vec!["（没有 IOBluetoothDevice）".into()] };
        let arr: Option<Retained<AnyObject>> = msg_send![cls, pairedDevices];
        let Some(arr) = arr else { return Vec::new() };
        let n: usize = msg_send![&*arr, count];
        (0..n)
            .map(|i| {
                let d: Retained<AnyObject> = msg_send![&*arr, objectAtIndex: i];
                let major: u32 = msg_send![&*d, deviceClassMajor];
                let minor: u32 = msg_send![&*d, deviceClassMinor];
                format!(
                    "{}  地址 {}  大类 {major:#04x} 小类 {minor:#04x}  {}{}",
                    text(&d, "nameOrAddress"),
                    text(&d, "addressString"),
                    if connected_of(&d) { "已连接" } else { "未连接" },
                    match major {
                        MAJOR_PERIPHERAL => "",
                        0 => "（低功耗蓝牙：Afar 列表里显示为不支持）",
                        _ => "（Afar 列表会过滤掉）",
                    }
                )
            })
            .collect()
    })
}

/// 诊断用：低功耗蓝牙（BLE）里、此刻连着 Mac 的键盘鼠标（HID 服务 0x1812）和带设备信息服务（0x180A）的设备。
/// 要在主线程调用：需要转 RunLoop 等蓝牙就绪。
pub fn dump_ble() -> Vec<String> {
    objc2::rc::autoreleasepool(|_| unsafe {
        let (Some(mgr_cls), Some(uuid_cls), Some(arr_cls)) =
            (AnyClass::get(c"CBCentralManager"), AnyClass::get(c"CBUUID"), AnyClass::get(c"NSArray"))
        else {
            return vec!["（没有 CoreBluetooth）".into()];
        };
        let mgr: Retained<AnyObject> = msg_send![mgr_cls, new];
        // CBManagerState：5 = 已打开
        let mut state: isize = 0;
        for _ in 0..50 {
            state = msg_send![&*mgr, state];
            if state == 5 {
                break;
            }
            crate::runloop::run_for(0.1);
        }
        if state != 5 {
            return vec![format!("蓝牙没就绪（状态 {state}；3 = 没有权限，4 = 蓝牙关着）")];
        }
        let mut out = Vec::new();
        for (svc, label) in [("1812", "HID 键盘鼠标"), ("180A", "设备信息")] {
            let s = NSString::from_str(svc);
            let u: Retained<AnyObject> = msg_send![uuid_cls, UUIDWithString: &*s];
            let list: Retained<AnyObject> = msg_send![arr_cls, arrayWithObject: &*u];
            let ps: Option<Retained<AnyObject>> = msg_send![&*mgr, retrieveConnectedPeripheralsWithServices: &*list];
            let n: usize = ps.as_ref().map(|a| msg_send![&**a, count]).unwrap_or(0);
            out.push(format!("[{svc} {label}] {n} 个"));
            for i in 0..n {
                let p: Retained<AnyObject> = msg_send![ps.as_deref().unwrap(), objectAtIndex: i];
                let name: Option<Retained<NSString>> = msg_send![&*p, name];
                let id: Retained<AnyObject> = msg_send![&*p, identifier];
                let ids: Retained<NSString> = msg_send![&*id, UUIDString];
                out.push(format!(
                    "  {}  标识 {ids}",
                    name.map(|n| n.to_string()).unwrap_or_else(|| "（无名称）".into())
                ));
            }
        }
        out
    })
}

/// 诊断用：对某个地址试一次断开 / 连上（经典蓝牙接口），报告系统返回值和 3 秒后的连接状态。
pub fn try_action(action: &str, addr: &str) -> String {
    let before = is_connected(addr);
    let r = if action == "close" { close(addr) } else { open(addr) };
    std::thread::sleep(Duration::from_secs(3));
    format!(
        "{action} {addr}：系统返回 {r}（{r:#x}；0 = 成功，-1 = 找不到这个地址）；之前{}，3 秒后{}",
        if before { "已连接" } else { "未连接" },
        if is_connected(addr) { "已连接" } else { "未连接" }
    )
}
