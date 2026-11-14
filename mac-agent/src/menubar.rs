//! 菜单栏图标（公开接口 AppKit）。显示状态；可立即连接 / 断开 iPad、配对新 iPad、打开设置、退出。
//! 主循环用 `pump` 代替普通 RunLoop，这样菜单能响应点击，私有接口的回调也照常执行。

use objc2::rc::Retained;
use objc2::runtime::{AnyObject, NSObject, NSObjectProtocol, ProtocolObject};
use objc2::{define_class, msg_send, sel, DefinedClass, MainThreadMarker, MainThreadOnly};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSApplicationDelegate, NSAutoresizingMaskOptions, NSBezierPath,
    NSColor, NSEvent, NSEventMask, NSFont, NSMenu, NSMenuItem, NSStatusBar, NSStatusItem, NSTextField,
    NSVariableStatusItemLength, NSView,
};
use objc2_foundation::{NSDate, NSDefaultRunLoopMode, NSPoint, NSRect, NSSize, NSString};
use std::cell::Cell;
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
/// 把"开机自动连接 iPad"开关拨到开 / 关（按开关当时的状态，不是"反一下"：菜单开着时连拨几下也不会错位）。
pub const ACT_BOOT_ON: u8 = 6;
pub const ACT_BOOT_OFF: u8 = 7;
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

define_class!(
    /// 菜单里的一行"文字 + 右边开关"，整行点哪里都切换。开关自己画：菜单栏程序的菜单窗口不是当前窗口，
    /// 系统的 NSSwitch 在里面一律画成灰色（开着也像关着）。
    #[unsafe(super(NSView))]
    #[thread_kind = MainThreadOnly]
    #[name = "AfarSwitchRow"]
    #[ivars = Cell<bool>]
    struct SwitchRow;

    impl SwitchRow {
        #[unsafe(method(drawRect:))]
        fn draw_rect(&self, _dirty: NSRect) {
            let b = self.bounds();
            let on = self.ivars().get();
            let (tw, th) = (32.0, 18.0);
            let track = NSRect::new(NSPoint::new(b.size.width - tw - 14.0, (b.size.height - th) / 2.0), NSSize::new(tw, th));
            if on { NSColor::controlAccentColor() } else { NSColor::tertiaryLabelColor() }.setFill();
            NSBezierPath::bezierPathWithRoundedRect_xRadius_yRadius(track, th / 2.0, th / 2.0).fill();
            let k = th - 4.0;
            let kx = if on { track.origin.x + tw - k - 2.0 } else { track.origin.x + 2.0 };
            NSColor::whiteColor().setFill();
            NSBezierPath::bezierPathWithOvalInRect(NSRect::new(NSPoint::new(kx, track.origin.y + 2.0), NSSize::new(k, k))).fill();
        }

        #[unsafe(method(mouseUp:))]
        fn mouse_up(&self, _e: &NSEvent) {
            let on = !self.ivars().get();
            self.ivars().set(on);
            self.setNeedsDisplay(true);
            ACTION.store(if on { ACT_BOOT_ON } else { ACT_BOOT_OFF }, Ordering::SeqCst);
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
    /// "开机自动连接 iPad"开着。
    pub boot_connect: bool,
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
        let key = format!("{}|{}|{:?}|{}|{}", v.name, v.state, v.showing, v.todo, v.boot_connect);
        if key == self.last_key {
            return;
        }
        self.last_key = key;
        let mtm = self.mtm;
        let (icon, line) = match v.state.as_str() {
            "showing" => ("▣", format!("● 正在显示到 {}", v.showing.clone().unwrap_or_default())),
            "connecting" => ("▣", "◌ 正在连接…".to_string()),
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
        menu.addItem(&self.switch_item(
            "开机自动连接 iPad",
            "Mac 开机或重启后自动连一次随航。重启前从 Afar 断开过的不连，在 iPad 上点卡片连。",
            v.boot_connect,
        ));
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
                &NSString::from_str("退出 Afar"),
                Some(sel!(quitAfar:)),
                &NSString::from_str(""),
            )
        };
        unsafe { quit.setTarget(Some(&self.target)) };
        quit.setToolTip(Some(&NSString::from_str("退出后随航不再受保护；换新版前先退出")));
        menu.addItem(&quit);
        self.item.setMenu(Some(&menu));
    }

    /// 一行文字 + 右边一个开关（悬停显示说明）。切换发 `ACT_BOOT_ON` / `ACT_BOOT_OFF`。菜单变宽时开关贴着右边。
    fn switch_item(&self, title: &str, tip: &str, on: bool) -> Retained<NSMenuItem> {
        let mtm = self.mtm;
        let (w, h) = (250.0, 26.0);
        let this = SwitchRow::alloc(mtm).set_ivars(Cell::new(on));
        let row: Retained<SwitchRow> =
            unsafe { msg_send![super(this), initWithFrame: NSRect::new(NSPoint::new(0.0, 0.0), NSSize::new(w, h))] };
        row.setAutoresizingMask(NSAutoresizingMaskOptions::ViewWidthSizable);
        let label = NSTextField::labelWithString(&NSString::from_str(title), mtm);
        label.setFont(Some(&NSFont::menuFontOfSize(0.0)));
        label.setFrame(NSRect::new(NSPoint::new(14.0, 4.0), NSSize::new(w - 70.0, 18.0)));
        let tip = NSString::from_str(tip);
        row.setToolTip(Some(&tip));
        label.setToolTip(Some(&tip));
        row.addSubview(&label);
        let item = NSMenuItem::new(mtm);
        item.setView(Some(&row));
        item
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
