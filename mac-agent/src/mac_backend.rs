//! 控制接口在 Mac 上的实现：校验 / 保存登录密码（公开接口 OpenDirectory、钥匙串），
//! 其它动作交给主循环执行（随航等私有接口回调在主线程上）。

use crate::server::{Backend, Command};
use afar_proto::Status;
use objc2::msg_send;
use objc2::rc::Retained;
use objc2::runtime::{AnyClass, AnyObject, Bool};
use objc2_foundation::NSString;
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex};

#[link(name = "OpenDirectory", kind = "framework")]
extern "C" {}

/// 钥匙串条目（当前用户的登录钥匙串）。
const KEYCHAIN_SERVICE: &str = "com.afar.agent.login";
/// kODNodeTypeLocalNodes
const OD_LOCAL_NODES: u32 = 0x2200;

/// 当前用户名（取自系统账户表，不依赖环境变量）。
pub fn current_user() -> String {
    unsafe {
        let pw = libc::getpwuid(libc::getuid());
        if pw.is_null() || (*pw).pw_name.is_null() {
            return std::env::var("USER").unwrap_or_default();
        }
        std::ffi::CStr::from_ptr((*pw).pw_name).to_string_lossy().into_owned()
    }
}

/// 用 OpenDirectory 校验当前用户的登录密码。
fn verify_with_od(user: &str, password: &str) -> bool {
    objc2::rc::autoreleasepool(|_| unsafe {
        let (Some(sess_cls), Some(node_cls)) = (AnyClass::get(c"ODSession"), AnyClass::get(c"ODNode")) else {
            warn!("找不到 OpenDirectory");
            return false;
        };
        let session: Option<Retained<AnyObject>> = msg_send![sess_cls, defaultSession];
        let Some(session) = session else { return false };
        let no_err: *mut *mut AnyObject = std::ptr::null_mut();
        let node: Option<Retained<AnyObject>> =
            msg_send![node_cls, nodeWithSession: &*session, type: OD_LOCAL_NODES, error: no_err];
        let Some(node) = node else { return false };
        let rec_type = NSString::from_str("dsRecTypeStandard:Users");
        let name = NSString::from_str(user);
        let no_attrs: Option<&AnyObject> = None;
        let rec: Option<Retained<AnyObject>> = msg_send![
            &*node,
            recordWithRecordType: &*rec_type,
            name: &*name,
            attributes: no_attrs,
            error: no_err
        ];
        let Some(rec) = rec else { return false };
        let pw = NSString::from_str(password);
        let ok: Bool = msg_send![&*rec, verifyPassword: &*pw, error: no_err];
        ok.as_bool()
    })
}

pub struct MacBackend {
    pub status: Arc<Mutex<Status>>,
    pub tx: Sender<Command>,
    /// 蓝牙线程（连接 / 断开 / 列表在那一个线程里做；搜索和配对在主线程）。
    pub bt: Sender<crate::bt::Req>,
}

impl Backend for MacBackend {
    fn verify_password(&self, password: &str) -> bool {
        let user = current_user();
        !user.is_empty() && verify_with_od(&user, password)
    }

    fn save_password(&self, password: &str) -> Result<(), String> {
        security_framework::passwords::set_generic_password(KEYCHAIN_SERVICE, &current_user(), password.as_bytes())
            .map_err(|e| e.to_string())
    }

    /// 只查条目在不在、不读密码内容（读内容在程序重新编译后会弹钥匙串授权框，无头 Mac 上没人点会卡住）。
    fn password_saved(&self) -> bool {
        use security_framework::item::{ItemClass, ItemSearchOptions, Limit};
        ItemSearchOptions::new()
            .class(ItemClass::generic_password())
            .service(KEYCHAIN_SERVICE)
            .account(&current_user())
            .load_attributes(true)
            .limit(Limit::Max(1))
            .search()
            .map(|r| !r.is_empty())
            .unwrap_or(false)
    }

    fn status(&self) -> Status {
        self.status.lock().unwrap().clone()
    }

    fn keyboards(&self) -> Result<Vec<afar_proto::KeyboardInfo>, String> {
        crate::bt::list(&self.bt)
    }

    fn command(&self, cmd: Command) -> Result<(), String> {
        self.tx.send(cmd).map_err(|_| "主循环已退出".to_string())
    }
}
