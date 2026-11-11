//! 系统通知（公开接口 UserNotifications）：有新的 iPad 配对时提醒，点通知打开"设置 Afar"里已配对的 iPad。
//! 只在 Afar.app 里用：命令行直接运行的程序没有 App 包，通知中心一调用就会让程序崩掉。

use block2::{DynBlock, RcBlock};
use objc2::rc::Retained;
use objc2::runtime::{Bool, NSObject, NSObjectProtocol, ProtocolObject};
use objc2::{define_class, msg_send, AllocAnyThread};
use objc2_foundation::{NSError, NSString};
use objc2_user_notifications::{
    UNAuthorizationOptions, UNMutableNotificationContent, UNNotification, UNNotificationPresentationOptions,
    UNNotificationRequest, UNNotificationResponse, UNUserNotificationCenter, UNUserNotificationCenterDelegate,
};

define_class!(
    // 通知中心可能在别的线程上回调，这里只改一个原子变量
    #[unsafe(super(NSObject))]
    #[name = "AfarNotifyDelegate"]
    struct Delegate;

    unsafe impl NSObjectProtocol for Delegate {}

    unsafe impl UNUserNotificationCenterDelegate for Delegate {
        /// "设置 Afar"开着（Afar 在前台）时也照样弹出来。
        #[unsafe(method(userNotificationCenter:willPresentNotification:withCompletionHandler:))]
        fn will_present(
            &self,
            _center: &UNUserNotificationCenter,
            _n: &UNNotification,
            done: &DynBlock<dyn Fn(UNNotificationPresentationOptions)>,
        ) {
            done.call((UNNotificationPresentationOptions::Banner | UNNotificationPresentationOptions::List,));
        }

        /// 点了通知：打开状态页，列出已配对的 iPad。
        #[unsafe(method(userNotificationCenter:didReceiveNotificationResponse:withCompletionHandler:))]
        fn did_receive(
            &self,
            _center: &UNUserNotificationCenter,
            _r: &UNNotificationResponse,
            done: &DynBlock<dyn Fn()>,
        ) {
            crate::menubar::post(crate::menubar::ACT_DEVICES);
            done.call(());
        }
    }
);

pub struct Notifier {
    center: Retained<UNUserNotificationCenter>,
    /// 通知中心只弱引用代理，这里持有。
    _delegate: Retained<Delegate>,
}

impl Notifier {
    /// 不在 Afar.app 里运行时返回 None（见模块说明）。第一次会弹"允许 Afar 发送通知吗"，不允许也不影响配对。
    pub fn new() -> Option<Notifier> {
        if !crate::app::in_bundle() {
            return None;
        }
        let center = UNUserNotificationCenter::currentNotificationCenter();
        let delegate: Retained<Delegate> = unsafe { msg_send![Delegate::alloc(), init] };
        center.setDelegate(Some(ProtocolObject::from_ref(&*delegate)));
        let asked = RcBlock::new(|ok: Bool, _e: *mut NSError| {
            if !ok.as_bool() {
                crate::log::emit("INFO", "没有允许 Afar 发通知：有新 iPad 配对时不会提醒");
            }
        });
        center.requestAuthorizationWithOptions_completionHandler(
            UNAuthorizationOptions::Alert | UNAuthorizationOptions::Sound,
            &asked,
        );
        Some(Notifier { center, _delegate: delegate })
    }

    /// 有新的 iPad 配对了这台 Mac。
    pub fn paired(&self, name: &str) {
        let c = UNMutableNotificationContent::new();
        c.setTitle(&NSString::from_str("新的 iPad 已配对"));
        c.setBody(&NSString::from_str(&format!(
            "\u{201c}{name}\u{201d}刚刚配对了这台 Mac。不是你本人操作的话，到 检查并配置… 里取消配对。"
        )));
        let id = format!(
            "afar-paired-{}",
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0)
        );
        let req = UNNotificationRequest::requestWithIdentifier_content_trigger(&NSString::from_str(&id), &c, None);
        self.center.addNotificationRequest_withCompletionHandler(&req, None);
    }
}
