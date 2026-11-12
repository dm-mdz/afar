//! 菜单栏图标（公开接口 AppKit）。显示状态；可立即连接 / 断开 iPad、配对新 iPad、打开设置、退出。
//! 主循环用 `pump` 代替普通 RunLoop，这样菜单能响应点击，私有接口的回调也照常执行。

use objc2::rc::Retained;
use objc2::runtime::{AnyObject, NSObject, NSObjectProtocol, ProtocolObject};
use objc2::{define_class, msg_send, sel, MainThreadMarker, MainThreadOnly};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSApplicationDelegate, NSEventMask, NSMenu, NSMenuItem, NSStatusBar,
    NSStatusItem, NSVariableStatusItemLength,
};
use objc2_foundation::{NSDate, NSDefaultRunLoopMode, NSString};
use std::sync::atomic::{AtomicU8, Ordering};
use std::time::{Duration, Instant};

/// 菜单里点了什么（主循环读取后清零）。
pub const ACT_NONE: u8 = 0;
pub const ACT_TOGGLE: u8 = 1;
pub const ACT_SETUP: u8 = 2;
/// 已在运行时又从"应用程序"里双击 Afar.app。
pub const ACT_REOPEN: u8 = 3;
/// 点了"配对新 iPad…"。
pub const ACT_PAIR: u8 = 4;
/// 点了"新的 iPad 已配对"通知：打开状态页、列出已配对的 iPad。
pub const ACT_DEVICES: u8 = 5;
static ACTION: AtomicU8 = AtomicU8::new(ACT_NONE);

/// 菜单以外的地方（通知）也能发动作给主循环。
pub fn post(act: u8) {
    ACTION.store(act, Ordering::SeqCst);
}

pub fn take_action() -> u8 {
    ACTION.swap(ACT_NONE, Ordering::SeqCst)
}

define_class!(
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "AfarAppDelegate"]
    struct AppDelegate;

    unsafe impl NSObjectProtocol for AppDelegate {}

    unsafe impl NSApplicationDelegate for AppDelegate {
        /// 程序已在运行时又被双击打开（Finder 发"重新打开"）。
        #[unsafe(method(applicationShouldHandleReopen:hasVisibleWindows:))]
        fn should_handle_reopen(&self, _app: &NSApplication, _visible: bool) -> bool {
            ACTION.store(ACT_REOPEN, Ordering::SeqCst);
            false
        }
    }
);

define_class!(
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "AfarMenuTarget"]
    struct MenuTarget;

    impl MenuTarget {
        #[unsafe(method(toggleConnect:))]
        fn toggle_connect(&self, _sender: Option<&AnyObject>) {
            ACTION.store(ACT_TOGGLE, Ordering::SeqCst);
        }

        #[unsafe(method(openSetup:))]
        fn open_setup(&self, _sender: Option<&AnyObject>) {
            ACTION.store(ACT_SETUP, Ordering::SeqCst);
        }

        #[unsafe(method(pairNew:))]
        fn pair_new(&self, _sender: Option<&AnyObject>) {
            ACTION.store(ACT_PAIR, Ordering::SeqCst);
        }

        #[unsafe(method(quitAfar:))]
        fn quit_afar(&self, _sender: Option<&AnyObject>) {
            crate::log::emit("INFO", "从菜单栏退出");
            std::process::exit(0); // 正常退出：launchd 不会再拉起（见 launchd.rs 的 KeepAlive 设置）
        }
    }
);

/// 菜单上要显示的内容。
pub struct View {
    pub name: String,
    /// "showing" | "connecting" | "online"
    pub state: String,
    pub showing: Option<String>,
    /// "设置 Afar"里还差几项。
    pub todo: usize,
    /// 随航反复连不稳，已暂停自动连回（reconnect::Keep::paused）。
    pub paused: bool,
}

pub struct MenuBar {
    mtm: MainThreadMarker,
    app: Retained<NSApplication>,
    item: Retained<NSStatusItem>,
    target: Retained<MenuTarget>,
    /// 应用委托（系统只弱引用它，这里持有）。
    _delegate: Retained<AppDelegate>,
    last_key: String,
}

fn text_item(mtm: MainThreadMarker, title: &str) -> Retained<NSMenuItem> {
    let it = unsafe {
        NSMenuItem::initWithTitle_action_keyEquivalent(
            NSMenuItem::alloc(mtm),
            &NSString::from_str(title),
            None,
            &NSString::from_str(""),
        )
    };
    it.setEnabled(false);
    it
}

impl MenuBar {
    /// 不在主线程上时返回 None（此时不显示菜单栏图标，程序照常运行）。
    pub fn new() -> Option<MenuBar> {
        let mtm = MainThreadMarker::new()?;
        let app = NSApplication::sharedApplication(mtm);
        // 不在程序坞显示图标，只在菜单栏
        app.setActivationPolicy(NSApplicationActivationPolicy::Accessory);
        let delegate: Retained<AppDelegate> = unsafe { msg_send![AppDelegate::alloc(mtm), init] };
        app.setDelegate(Some(ProtocolObject::from_ref(&*delegate)));
        app.finishLaunching();
        let item = NSStatusBar::systemStatusBar().statusItemWithLength(NSVariableStatusItemLength);
        let target: Retained<MenuTarget> = unsafe { msg_send![MenuTarget::alloc(mtm), init] };
        Some(MenuBar { mtm, app, item, target, _delegate: delegate, last_key: String::new() })
    }

    /// 内容有变化才重建菜单。
    pub fn update(&mut self, v: &View) {
        let key = format!("{}|{}|{:?}|{}|{}", v.name, v.state, v.showing, v.todo, v.paused);
        if key == self.last_key {
            return;
        }
        self.last_key = key;
        let mtm = self.mtm;
        let (icon, line) = match v.state.as_str() {
            "showing" => ("▣", format!("● 正在显示到 {}", v.showing.clone().unwrap_or_default())),
            "connecting" => ("▣", "◌ 正在连接…".to_string()),
            _ if v.paused => ("⚠︎", "⚠︎ 随航不稳定，已暂停自动连接".to_string()),
            _ => ("▢", "○ 等待 iPad".to_string()),
        };
        let icon = if v.todo > 0 { "⚠︎" } else { icon };
        if let Some(b) = self.item.button(mtm) {
            b.setTitle(&NSString::from_str(&format!("{icon} Afar")));
        }

        let menu = NSMenu::new(mtm);
        menu.setAutoenablesItems(false);
        menu.addItem(&text_item(mtm, &v.name));
        menu.addItem(&text_item(mtm, &line));
        menu.addItem(&NSMenuItem::separatorItem(mtm));

        let toggle_title = if v.state == "showing" { "断开 iPad" } else { "立即连接 iPad" };
        let toggle = unsafe {
            NSMenuItem::initWithTitle_action_keyEquivalent(
                NSMenuItem::alloc(mtm),
                &NSString::from_str(toggle_title),
                Some(sel!(toggleConnect:)),
                &NSString::from_str(""),
            )
        };
        unsafe { toggle.setTarget(Some(&self.target)) };
        menu.addItem(&toggle);
        // v0.4.8：配对码不再一直显示，平时也不接受配对；点这里才打开配对 10 分钟
        let pair = unsafe {
            NSMenuItem::initWithTitle_action_keyEquivalent(
                NSMenuItem::alloc(mtm),
                &NSString::from_str("配对新 iPad…"),
                Some(sel!(pairNew:)),
                &NSString::from_str(""),
            )
        };
        unsafe { pair.setTarget(Some(&self.target)) };
        menu.addItem(&pair);
        let setup_title = if v.todo > 0 {
            format!("检查并配置…（还差 {} 项）", v.todo)
        } else {
            "检查并配置…".to_string()
        };
        let setup = unsafe {
            NSMenuItem::initWithTitle_action_keyEquivalent(
                NSMenuItem::alloc(mtm),
                &NSString::from_str(&setup_title),
                Some(sel!(openSetup:)),
                &NSString::from_str(""),
            )
        };
        unsafe { setup.setTarget(Some(&self.target)) };
        menu.addItem(&setup);
        menu.addItem(&text_item(mtm, &format!("Afar v{}", env!("CARGO_PKG_VERSION"))));
        menu.addItem(&NSMenuItem::separatorItem(mtm));

        let quit = unsafe {
            NSMenuItem::initWithTitle_action_keyEquivalent(
                NSMenuItem::alloc(mtm),
                &NSString::from_str("退出 Afar（会停止保护；换新版前先退出）"),
                Some(sel!(quitAfar:)),
                &NSString::from_str(""),
            )
        };
        unsafe { quit.setTarget(Some(&self.target)) };
        menu.addItem(&quit);
        self.item.setMenu(Some(&menu));
    }

    /// 处理界面事件，最多等 `seconds` 秒。（同时会执行主线程上排队的回调）
    pub fn pump(&self, seconds: f64) {
        let end = Instant::now() + Duration::from_secs_f64(seconds);
        loop {
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            let until = NSDate::dateWithTimeIntervalSinceNow(left.as_secs_f64());
            let ev = self.app.nextEventMatchingMask_untilDate_inMode_dequeue(
                NSEventMask::Any,
                Some(&until),
                unsafe { NSDefaultRunLoopMode },
                true,
            );
            if let Some(ev) = ev {
                self.app.sendEvent(&ev);
            }
        }
    }
}
