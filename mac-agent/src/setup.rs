//! Mac 上的"设置 Afar"窗口（AFAR-008，界面稿 v3）：第一次用的 4 屏向导 + 平时的状态页。
//! 界面是一页 HTML（`ui/setup.html`，编进程序里），放在 WKWebView 里显示：
//! 页面用 `window.webkit.messageHandlers.afar.postMessage(JSON 字符串)` 发指令，这里收进队列由主循环处理；
//! 主循环每秒把状态（JSON）推给页面的 `afar.update(...)`，有变化才推。
//!
//! 需要管理员权限的事（防火墙放行、断电后自动开机、重启助手、远程登录）合成一个脚本，用 osascript 的
//! `with administrator privileges` 执行：系统弹自己的密码框，Afar 拿不到密码。在后台线程里跑，不卡主循环。

pub use crate::setupcore::*;
use objc2::rc::Retained;
use objc2::runtime::{NSObject, NSObjectProtocol, ProtocolObject};
use objc2::{define_class, msg_send, MainThreadMarker, MainThreadOnly};
use objc2_app_kit::{NSApplication, NSApplicationActivationPolicy, NSBackingStoreType, NSWindow, NSWindowStyleMask};
use objc2_foundation::{NSPoint, NSRect, NSSize, NSString};
use objc2_web_kit::{
    WKScriptMessage, WKScriptMessageHandler, WKUserContentController, WKWebView, WKWebViewConfiguration,
};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

const PAGE: &str = include_str!("../ui/setup.html");

/// 页面发来的指令（JSON 字符串），主循环取走处理。
static INBOX: Mutex<Vec<String>> = Mutex::new(Vec::new());
/// 正在执行的管理员操作（"one" / "ssh"，空 = 没有）和上次失败的原因。后台线程写，主循环读。
static ACTION: Mutex<(String, String)> = Mutex::new((String::new(), String::new()));

define_class!(
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "AfarSetupHandler"]
    struct Handler;

    unsafe impl NSObjectProtocol for Handler {}

    unsafe impl WKScriptMessageHandler for Handler {
        #[unsafe(method(userContentController:didReceiveScriptMessage:))]
        fn did_receive(&self, _c: &WKUserContentController, msg: &WKScriptMessage) {
            let body = unsafe { msg.body() };
            if let Some(s) = body.downcast_ref::<NSString>() {
                INBOX.lock().unwrap().push(s.to_string());
            }
        }
    }
);

pub struct SetupWindow {
    window: Retained<NSWindow>,
    web: Retained<WKWebView>,
    _handler: Retained<Handler>,
    last: String,
}

impl SetupWindow {
    pub fn new() -> Option<SetupWindow> {
        let mtm = MainThreadMarker::new()?;
        let rect = NSRect::new(NSPoint::new(0.0, 0.0), NSSize::new(520.0, 720.0));
        let style = NSWindowStyleMask::Titled | NSWindowStyleMask::Closable | NSWindowStyleMask::Miniaturizable;
        let window = unsafe {
            NSWindow::initWithContentRect_styleMask_backing_defer(
                NSWindow::alloc(mtm),
                rect,
                style,
                NSBackingStoreType::Buffered,
                false,
            )
        };
        // 关掉先只是隐藏（马上再打开还是这一屏）；隐藏 30 秒后由主循环 destroy() 释放
        unsafe { window.setReleasedWhenClosed(false) };
        window.setTitle(&NSString::from_str("设置 Afar"));
        let handler: Retained<Handler> = unsafe { msg_send![Handler::alloc(mtm), init] };
        let web = unsafe {
            let cfg = WKWebViewConfiguration::new(mtm);
            cfg.userContentController()
                .addScriptMessageHandler_name(ProtocolObject::from_ref(&*handler), &NSString::from_str("afar"));
            let web = WKWebView::initWithFrame_configuration(WKWebView::alloc(mtm), rect, &cfg);
            let _ = web.loadHTMLString_baseURL(&NSString::from_str(PAGE), None);
            web
        };
        window.setContentView(Some(&web));
        window.center();
        Some(SetupWindow { window, web, _handler: handler, last: String::new() })
    }

    pub fn show(&self) {
        let mtm = MainThreadMarker::from(&*self.window);
        let app = NSApplication::sharedApplication(mtm);
        // macOS 14 起 activateIgnoringOtherApps 不一定把只在菜单栏的程序提到前面：再加 orderFrontRegardless。
        // v0.4.7 测试（macOS 27）：这样也不够——第一次安装、换新版后自动打开的向导 / 状态页都被压在当时最前面的程序后面，
        // 用户看不到。和 app::confirm 一样：窗口开着时临时切成普通 App（程序坞里有图标），关掉后由 `background` 切回只在菜单栏。
        app.setActivationPolicy(NSApplicationActivationPolicy::Regular);
        #[allow(deprecated)]
        app.activateIgnoringOtherApps(true);
        app.activate();
        self.window.makeKeyAndOrderFront(None);
        self.window.orderFrontRegardless();
    }

    /// 窗口关掉（隐藏）后切回只在菜单栏（见 `show`）。每秒调用也没关系：已经是就不动。
    pub fn background(&self) {
        let app = NSApplication::sharedApplication(MainThreadMarker::from(&*self.window));
        if app.activationPolicy() != NSApplicationActivationPolicy::Accessory {
            app.setActivationPolicy(NSApplicationActivationPolicy::Accessory);
        }
    }

    pub fn visible(&self) -> bool {
        self.window.isVisible()
    }

    /// 最小化到程序坞了（不在屏幕上，但不算关掉，不释放）。
    pub fn minimized(&self) -> bool {
        self.window.isMiniaturized()
    }

    pub fn close(&self) {
        self.window.orderOut(None);
    }

    /// 彻底拆掉（关窗口一段时间后调用）：网页和它的 WebContent 进程一起释放，下次打开重新建。
    /// v0.4.0 测试 P1：只隐藏的话，关窗口后 Afar 多占约 35MB、WebContent 进程约 50MB 一直不回落。
    /// v0.4.4 测试：换空白页、摘下视图、关窗口都做了，WebKit 日志里仍没有 WebPageProxy::close，每开一次多留一个
    /// WebContent 进程（约 39MB）→ 网页对象还被别处引用着、等不到释放。改成直接调用 WKWebView 的 `_close`
    /// （WebKit 的私有接口，声明在 WKWebViewPrivate.h）：不管还有谁引用，网页立刻关掉，没有别的网页的 WebContent 进程随之退出。
    pub fn destroy(self) {
        objc2::rc::autoreleasepool(|_| {
            unsafe {
                self.web
                    .configuration()
                    .userContentController()
                    .removeScriptMessageHandlerForName(&NSString::from_str("afar"));
                self.web.stopLoading();
                let close = objc2::sel!(_close);
                if self.web.respondsToSelector(close) {
                    let _: () = msg_send![&*self.web, _close];
                } else {
                    // 系统去掉了这个私有接口：退回到以前的做法（至少网页换成空白、不再占着页面内容）
                    crate::log::emit("WARN", "设置窗口：WKWebView 没有 _close，网页进程可能不会退出");
                    let _ = self.web.loadHTMLString_baseURL(&NSString::from_str(""), None);
                }
                self.web.removeFromSuperview();
            }
            self.window.setContentView(None);
            self.window.orderOut(None);
            self.window.close();
            drop(self);
        });
    }

    /// 页面刚加载好：下次 `push` 一定重推。
    pub fn reset(&mut self) {
        self.last.clear();
    }

    /// 状态有变化才推给页面。
    pub fn push(&mut self, v: &View) {
        let json = serde_json::to_string(v).unwrap_or_default();
        if json == self.last {
            return;
        }
        self.last = json;
        let js = format!("window.afar && afar.update({})", self.last);
        unsafe { self.web.evaluateJavaScript_completionHandler(&NSString::from_str(&js), None) };
    }
}

pub fn take_messages() -> Vec<Msg> {
    std::mem::take(&mut *INBOX.lock().unwrap()).iter().filter_map(|s| parse(s)).collect()
}

pub fn action() -> (String, String) {
    ACTION.lock().unwrap().clone()
}

/// 打开系统设置的某一页（链接都在 macOS 27 上核对过）。
pub fn open_settings(what: &str) {
    let url = match what {
        "bluetooth" => "x-apple.systempreferences:com.apple.BluetoothSettings",
        "wifi" => "x-apple.systempreferences:com.apple.wifi-settings-extension",
        "handoff" => "x-apple.systempreferences:com.apple.AirDrop-Handoff-Settings.extension",
        "filevault" => "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?FileVault",
        "users" => "x-apple.systempreferences:com.apple.Users-Groups-Settings.extension",
        "sharing" => "x-apple.systempreferences:com.apple.Sharing-Settings.extension",
        "energy" => "x-apple.systempreferences:com.apple.Battery-Settings.extension",
        // 防火墙页没有能直达的链接：打开网络设置，页面上写着再点"防火墙"
        "firewall" => "x-apple.systempreferences:com.apple.Network-Settings.extension",
        _ => return,
    };
    let _ = std::process::Command::new("/usr/bin/open").arg(url).status();
}

/// 重启助手的请求文件（在单独的子目录里）。
pub fn restart_request(dir: &Path) -> PathBuf {
    dir.join(RESTART_DIR).join(RESTART_FILE)
}

/// 重启助手装好了而且 launchd 加载了它。普通用户读不了 system 域时，退回"plist 在不在"。
pub fn helper_installed() -> bool {
    if !Path::new(HELPER_PLIST).exists() {
        return false;
    }
    match std::process::Command::new("/bin/launchctl").args(["print", HELPER_LABEL]).output() {
        Ok(o) if o.status.success() => true,
        Ok(o) => {
            let err =
                String::from_utf8_lossy(&o.stderr).to_lowercase() + &String::from_utf8_lossy(&o.stdout).to_lowercase();
            !(err.contains("could not find") || err.contains("not found"))
        }
        Err(_) => true,
    }
}

/// 以管理员身份跑一段脚本（后台线程）：系统弹密码框，`prompt` 是框里的说明。`after` 在主循环之外调用。
/// 脚本整个放在 osascript 的参数里（不经过 shell，不落文件）。
fn run_admin(kind: &'static str, script: String, prompt: &str, after: impl FnOnce() + Send + 'static) {
    {
        let mut a = ACTION.lock().unwrap();
        if !a.0.is_empty() {
            return; // 上一个还没完
        }
        *a = (kind.into(), String::new());
    }
    let apple = format!(
        "do shell script \"{}\" with prompt \"{}\" with administrator privileges",
        applescript_str(&script),
        applescript_str(prompt)
    );
    std::thread::spawn(move || {
        let r = std::process::Command::new("/usr/bin/osascript").args(["-e", &apple]).output();
        let err = match r {
            Ok(o) if o.status.success() => match parse_fail(&String::from_utf8_lossy(&o.stdout)) {
                Some(f) if f.is_empty() => String::new(),
                Some(f) => format!("这几项没做成：{f}"),
                None => "没做成：脚本没有返回结果".into(),
            },
            // -128 = 用户在密码框点了取消
            Ok(o) if String::from_utf8_lossy(&o.stderr).contains("-128") => "取消了".into(),
            Ok(o) => format!("没做成：{}", String::from_utf8_lossy(&o.stderr).trim()),
            Err(e) => format!("没做成：{e}"),
        };
        if err.is_empty() {
            info!("设置 Afar：{kind} 完成");
        } else {
            warn!("设置 Afar：{kind}：{err}");
        }
        after();
        *ACTION.lock().unwrap() = (String::new(), err);
    });
}

/// 第 1 屏"一键配置"：防火墙放行、断电后自动开机、装重启助手（开机自启由打开 Afar.app 时装好，不用管理员）。
pub fn run_one(dir: &Path, after: impl FnOnce() + Send + 'static) {
    let exe = std::env::current_exe().and_then(|p| p.canonicalize()).unwrap_or_default();
    let req = restart_request(dir);
    // 请求文件的目录由用户自己建（root 建的话用户写不进去）
    if let Some(d) = req.parent() {
        let _ = std::fs::create_dir_all(d);
    }
    run_admin(
        "one",
        one_script(&exe, &req),
        "Afar 要配置这台 Mac：防火墙放行 Afar、断电后自动开机、安装重启助手。",
        after,
    );
}

pub fn run_ssh(after: impl FnOnce() + Send + 'static) {
    run_admin("ssh", ssh_script(), "Afar 要打开\"远程登录\"（断电后 iPad 远程解锁磁盘要用）。", after);
}

/// 防火墙有没有拦这个程序：防火墙关着算没拦；开着时看 `--getappblocked` 的结果。
pub fn firewall_ok(exe: &Path) -> bool {
    let fw = "/usr/libexec/ApplicationFirewall/socketfilterfw";
    let out = |args: &[&str]| {
        std::process::Command::new(fw)
            .args(args)
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_lowercase())
            .unwrap_or_default()
    };
    let state = out(&["--getglobalstate"]);
    if state.contains("disabled") || state.contains("state = 0") {
        return true;
    }
    let blocked = out(&["--getappblocked", &exe.to_string_lossy()]);
    blocked.contains("permitted") || blocked.contains("allow")
}
