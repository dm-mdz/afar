//! Afar 通信协议（Mac 端和 iPad App 共用）。
//!
//! - 传输：局域网 TCP + TLS（Mac 端自签证书，iPad 配对时记住证书指纹，之后只认这一张证书）。
//! - 格式：极简 HTTP/1.1 + JSON，每次请求一个连接。
//! - 配对：6 位配对码 + SPAKE2（配对码不会在网上传，也无法被离线暴力破解），
//!   双方再用协商出的密钥对"各自看到的证书指纹"做确认，防中间人。
//! - 之后的请求带 `Authorization: Bearer <token>`。

pub mod client;
pub mod http;
pub mod pair;

use serde::{Deserialize, Serialize};

/// 默认端口。
pub const PORT: u16 = 47911;
/// 局域网发现用的 Bonjour 服务类型。
pub const SERVICE_TYPE: &str = "_afar._tcp";
/// 协议版本（不兼容的改动才加 1）。
pub const PROTO_VERSION: u32 = 1;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Info {
    pub name: String,
    pub version: String,
    pub proto: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PairStartReq {
    /// iPad 的显示名（只用于 Mac 端列表）。
    pub device_name: String,
    /// SPAKE2 消息（base64）。
    pub msg: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PairStartResp {
    pub pair_id: String,
    pub msg: String,
    /// Mac 端证明自己知道配对码、并绑定自己的证书指纹。
    pub confirm: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PairFinishReq {
    pub pair_id: String,
    pub confirm: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PairFinishResp {
    pub device_id: String,
    pub token: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PasswordReq {
    pub password: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ConnectReq {
    /// 随航里看到的 iPad 名称。第一次连接时如果附近有多台 iPad，需要 iPad App 让用户选一次；之后 Mac 端会记住。
    pub sidecar_name: Option<String>,
    /// 这次连上后把选定的蓝牙键盘接到 Mac（用户在 App 里确认过才为 true）。
    /// 放在连接请求里：随航一全屏 iPad App 就被系统挂起，连上之后 App 发不了指令。
    #[serde(default)]
    pub keyboard: bool,
    /// iPad 屏幕的物理像素（横屏：长边、短边），Mac 按它算三档显示大小。v0.4.2 起；旧版 iPad App 不带。
    #[serde(default)]
    pub screen: Option<[u32; 2]>,
}

/// POST /restart 的请求体。旧版 iPad App 发 `{}`：按不重新打开窗口处理。
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct RestartReq {
    /// 和苹果菜单"重新启动…"里的"登录时重新打开窗口"一样：true = 重启登录后把现在开着的程序和窗口重新打开。
    #[serde(default)]
    pub reopen: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct SettingsReq {
    /// 改 Mac 显示名。
    pub name: Option<String>,
    /// 缩放档位："larger" | "default" | "more"。
    pub scale: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ScaleInfo {
    pub key: String,
    pub label: String,
    pub w: u32,
    pub h: u32,
}

/// Mac 当前状态。
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct Status {
    pub name: String,
    pub version: String,
    /// "online"（在线、没在显示）| "connecting"（连接中）| "showing"（正在显示到某台 iPad）
    pub state: String,
    /// 正在显示到哪台 iPad（随航里的名称）。
    pub showing: Option<String>,
    /// 附近可用的 iPad（随航里的名称）。
    pub nearby: Vec<String>,
    /// 当前实际生效的缩放档位（按虚拟屏现在的模式算；对不上任何档位时为空）。
    pub scale: String,
    pub scales: Vec<ScaleInfo>,
    /// Mac 端是否已保存登录密码。
    pub password_saved: bool,
    /// Mac 登录用户名（断电后 iPad 远程解锁磁盘加密时要用）。
    #[serde(default)]
    pub user: String,
    /// 是否开着 FileVault（磁盘加密）。开着时断电重启后需要 iPad 远程解锁。
    #[serde(default)]
    pub filevault: bool,
    /// 是否开着"远程登录"（SSH）。FileVault 开着时必须开，否则断电后 iPad 无法远程解锁。
    #[serde(default)]
    pub remote_login: bool,
    /// Mac 的 SSH 主机公钥（OpenSSH 格式）。iPad 在配对过的加密通道里拿到并记住，远程解锁时只认它们（防冒充）。
    #[serde(default)]
    pub ssh_host_keys: Vec<String>,
    /// 正在显示的就是"发请求的这台 iPad"（按这台 iPad 选过的随航名称判断）。
    #[serde(default)]
    pub showing_me: bool,
    /// 正在显示到的是**另一台已配对的 iPad**（按那台选过的随航名称判断）。
    /// 两个都是 false = 不确定（这台 iPad 还没从 App 连过，Mac 不知道它在随航里叫什么）。
    #[serde(default)]
    pub showing_other: bool,
    /// 是否开着"自动登录"（系统设置 > 用户与群组）。FileVault 开着时系统不允许自动登录。
    #[serde(default)]
    pub auto_login: bool,
    /// 是否开着"断电后自动开机"（pmset autorestart）。
    #[serde(default)]
    pub auto_restart: bool,
    /// 最近一次切换缩放失败的原因（`scale` 始终是实际生效的档位；成功后清空）。
    #[serde(default)]
    pub scale_error: String,
    /// 正在换显示大小（还没核对完，`scale` 可能还会变）。v0.4.2 起。
    #[serde(default)]
    pub scale_busy: bool,
    /// 最近一次"重启 Mac"失败的原因（例如没允许控制 System Events）；成功或没重启过为空。
    #[serde(default)]
    pub restart_error: String,
    /// 选定的蓝牙键盘（`connected` = 现在连在 Mac 上）。没选为空。
    #[serde(default)]
    pub keyboard: Option<KeyboardInfo>,
    /// 用户打开了"用随航时把键盘交给 Mac"。
    #[serde(default)]
    pub keyboard_auto: bool,
    /// Mac 正在尝试接管键盘（等用户在 iPad 上断开它）。
    #[serde(default)]
    pub keyboard_taking: bool,
    /// 最近一次接管 / 松开键盘失败的原因；成功后清空。
    #[serde(default)]
    pub keyboard_error: String,
    /// Mac 刚把键盘松开（10 分钟内）：App 提示用户在控制中心连回 iPad。
    #[serde(default)]
    pub keyboard_released: bool,
    /// 在 App 里配对新键盘的进度（没在配对为空）。
    #[serde(default)]
    pub keyboard_pair: Option<PairInfo>,
}

/// 配对新蓝牙键盘的进度。`stage`：searching（搜索中）| searched（搜完了）| pairing（配对中）| done | failed。
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct PairInfo {
    pub stage: String,
    /// 搜到的、处在配对模式的外设。
    #[serde(default)]
    pub found: Vec<KeyboardInfo>,
    /// 正在配对的设备地址。
    #[serde(default)]
    pub address: String,
    /// 要用户在键盘上输入（然后回车）的数字；不需要时为空。
    #[serde(default)]
    pub code: String,
    #[serde(default)]
    pub message: String,
}

/// Mac 上已配对的蓝牙键盘 / 鼠标 / 触控板。
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct KeyboardInfo {
    /// 蓝牙地址，如 "a1-b2-c3-d4-e5-f6"。
    pub address: String,
    pub name: String,
    /// 现在连在 Mac 上。
    #[serde(default)]
    pub connected: bool,
    /// 低功耗蓝牙（BLE）设备：系统列得出来，但 Afar 松不开它（v0.3.6 实测一款低功耗蓝牙键盘），只显示、不能选。
    #[serde(default)]
    pub ble: bool,
}

/// POST /keyboard。`action`：set（选定键盘；address 为空 = 不用）| auto（开关自动切换）|
/// take（现在接管）| release（松开 Afar 接管的）| release_now（不管是谁接的，现在就让 Mac 松开；配对后验证用）| open_settings（在 Mac 上打开蓝牙设置）|
/// pair_search（搜索处在配对模式的新外设）| pair（和搜到的 address 配对）| pair_cancel（取消）。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct KeyboardReq {
    pub action: String,
    #[serde(default)]
    pub address: Option<String>,
    #[serde(default)]
    pub name: Option<String>,
    #[serde(default)]
    pub auto: Option<bool>,
}

/// 出错时的返回体。`code` 给程序判断，`error` 给人看。
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ErrorResp {
    pub code: String,
    pub error: String,
}

/// 错误码。
/// iPad 报上来的屏幕物理像素规整成"长边在前"，不合理的值返回 None（Mac 端会拿它建虚拟屏，坏值可能让没接显示器的 Mac
/// 开机反复重启，所以两端都要过一遍）。iPadOS 27 的 iPad 从 mini（2266×1488）到 Pro 13 寸（2752×2064），短长比 0.65～0.75。
pub fn normalize_screen(px: [u32; 2]) -> Option<[u32; 2]> {
    let (a, b) = (px[0].max(px[1]), px[0].min(px[1]));
    let ratio = b as f64 / a.max(1) as f64;
    ((1500..=4000).contains(&a) && (1000..=3000).contains(&b) && (0.6..=0.8).contains(&ratio)).then_some([a, b])
}

pub mod codes {
    pub const BAD_REQUEST: &str = "bad_request";
    pub const UNAUTHORIZED: &str = "unauthorized";
    pub const WRONG_CODE: &str = "wrong_code";
    pub const LOCKED: &str = "locked";
    /// Mac 上没有打开配对（v0.4.8 起，要在 Mac 上点"配对新 iPad…"）。
    pub const PAIRING_CLOSED: &str = "pairing_closed";
    pub const WRONG_PASSWORD: &str = "wrong_password";
    pub const CHOOSE_IPAD: &str = "choose_ipad";
    pub const NO_IPAD: &str = "no_ipad";
    pub const NOT_FOUND: &str = "not_found";
    /// Mac 端没有蓝牙权限。
    pub const BT_DENIED: &str = "bt_denied";
    pub const INTERNAL: &str = "internal";
}
