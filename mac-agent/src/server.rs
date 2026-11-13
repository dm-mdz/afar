//! 给 iPad App 的控制接口（HTTPS + JSON）。本文件与平台无关，具体动作由 `Backend` 实现。
//!
//! 接口一览（除 /info 和 /pair/* 外都要带令牌）：
//!   GET  /info           名称、版本
//!   POST /pair/start     配对第 1 步（SPAKE2）
//!   POST /pair/finish    配对第 2 步，返回令牌
//!   POST /password       保存 Mac 登录密码（先校验）
//!   GET  /status         当前状态
//!   POST /connect        连接随航（iPad 当屏幕）
//!   POST /disconnect     断开随航
//!   POST /restart        重启 Mac
//!   POST /settings       改名称 / 缩放档位
//!   GET  /keyboards      Mac 上已配对的蓝牙键盘 / 鼠标 / 触控板
//!   POST /keyboard       选定键盘、开关"随航时交给 Mac"、立即接管 / 松开、打开蓝牙设置
//!   POST /unpair         取消配对（删除这台 iPad）

use afar_proto::http::{read_message, write_response, Message};
use afar_proto::{
    codes, pair, ConnectReq, ErrorResp, Info, KeyboardInfo, KeyboardReq, PairFinishReq, PairFinishResp, PairStartReq,
    PairStartResp, PasswordReq, SettingsReq, Status, PROTO_VERSION,
};
use rand::Rng;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use rustls::{ServerConfig, ServerConnection, StreamOwned};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// 每个配对码最多试几次（每次发起配对都算一次，配对成功清零）；密码连续输错几次后锁定。
const MAX_FAILURES: u32 = 5;
/// 同时处理的连接数上限、每个连接最长占用时间（防止被慢连接拖垮）。
const MAX_CONNS: usize = 32;
const CONN_DEADLINE: Duration = Duration::from_secs(15);
/// 第一次锁定的时长；之后每锁一次翻倍（1、2、4…分钟），最长 LOCK_MAX。
const LOCK_FOR: Duration = Duration::from_secs(60);
const LOCK_MAX: Duration = Duration::from_secs(60 * 60);
/// 配对开着多久：平时不接受配对，只有在 Mac 上点"配对新 iPad…"（或向导走到配对那一屏）后的这段时间里接受。
/// v0.4.8：原来配对一直开着、每错 5 次只锁 1 分钟，没人看着的 Mac 可以被慢慢猜（一年约 93% 能猜中）。
pub const PAIR_WINDOW: Duration = Duration::from_secs(10 * 60);
/// 配对第 1 步到第 2 步之间最多等多久。
const PENDING_TTL: Duration = Duration::from_secs(60);

/// 交给主循环执行的动作。
#[derive(Debug, Clone, PartialEq)]
pub enum Command {
    /// `keyboard`：这次连上后接管蓝牙键盘。
    Connect {
        sidecar_name: String,
        keyboard: bool,
        /// iPad App 回到前台时自动发起的（见 ConnectReq::auto）。
        auto: bool,
    },
    Disconnect,
    /// "回到 Mac 桌面"：随航已经连在这台 iPad 上、但 iPad 上 Afar 在前台时，让 Mac 把随航断开再连回，
    /// iPad 会自动切到随航画面（不是从 Afar 断开：保持显示、键盘交接都不停）。
    Front {
        sidecar_name: String,
    },
    /// `reopen`：登录后是否重新打开窗口（同苹果菜单重启框里的勾选）。
    Restart {
        reopen: bool,
    },
    /// 这台 iPad（随航名称）屏幕的物理像素（长边、短边，已规整），连接前由 iPad App 报上来：Mac 按它算三档显示大小。
    IpadScreen {
        name: String,
        px: [u32; 2],
    },
    SetScale(String),
    Rename(String),
    Keyboard(Kb),
}

/// 蓝牙键盘相关的动作。
#[derive(Debug, Clone, PartialEq)]
pub enum Kb {
    /// 选定键盘（None = 不用）。
    Set(Option<KeyboardInfo>),
    /// 开关"用随航时把键盘交给 Mac"。
    Auto(bool),
    Take,
    Release,
    /// 不管是谁接的，现在就让 Mac 松开选定的键盘（配对后验证"能不能连回 iPad"前要先松开）。
    ReleaseNow,
    OpenSettings,
    /// 在 App 里配对新外设：搜索 / 和搜到的某台配对 / 取消。
    PairSearch,
    Pair(String),
    PairCancel,
}

/// 平台相关的部分（Mac 上是真实实现，测试里是假的）。
pub trait Backend: Send + Sync + 'static {
    fn verify_password(&self, password: &str) -> bool;
    fn save_password(&self, password: &str) -> Result<(), String>;
    fn password_saved(&self) -> bool;
    /// 当前状态（不含 password_saved，由服务填）。
    fn status(&self) -> Status;
    fn command(&self, cmd: Command) -> Result<(), String>;
    /// Mac 上已配对的蓝牙键盘 / 鼠标 / 触控板。Err = 读不了（比如没有蓝牙权限）。
    fn keyboards(&self) -> Result<Vec<KeyboardInfo>, String> {
        Ok(Vec::new())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct Device {
    id: String,
    name: String,
    token_hash: String,
    /// 这台 iPad 在随航里的名称（第一次连接时确定）。
    sidecar_name: Option<String>,
    paired_at: u64,
}

struct Pending {
    key: Vec<u8>,
    device_name: String,
    created: Instant,
}

struct PairState {
    /// 当前配对码；配对没打开时为空。
    code: String,
    /// 配对开到什么时候（None = 没打开，不接受配对）。
    open_until: Option<Instant>,
    /// 当前配对码已被尝试的次数。
    attempts: u32,
    locked_until: Option<Instant>,
    /// 这次打开配对以来锁过几次（锁定时间按它翻倍）。
    lock_rounds: u32,
    pending: HashMap<String, Pending>,
}

struct Shared {
    dir: PathBuf,
    fp: [u8; 32],
    backend: Arc<dyn Backend>,
    pair: Mutex<PairState>,
    devices: Mutex<Vec<Device>>,
    pw_failures: Mutex<(u32, Option<Instant>)>,
    /// 刚配对上的 iPad 名称（Mac 端取走后弹通知）。
    new_pairs: Mutex<Vec<String>>,
}

/// 已配对的 iPad（给"设置 Afar"窗口显示）。
#[derive(Debug, Clone, PartialEq)]
pub struct Paired {
    pub id: String,
    pub name: String,
    pub paired_at: u64,
    /// 在随航里的名称（第一次连接后才有）。
    pub sidecar_name: Option<String>,
}

/// 配对现在的情况（配对开着时才有）。
#[derive(Debug, Clone)]
pub struct Pairing {
    pub code: String,
    /// 还开多久。
    pub left: Duration,
    /// 试错太多、暂停中：还要等多久（0 = 没暂停）。
    pub locked: Duration,
}

/// 控制接口的句柄：Mac 端界面用它开关配对、管理已配对的 iPad。
#[derive(Clone)]
pub struct Handle {
    sh: Arc<Shared>,
    addr: SocketAddr,
}

impl Handle {
    /// 实际监听的地址。
    pub fn addr(&self) -> SocketAddr {
        self.addr
    }

    /// 打开配对 PAIR_WINDOW 这么久，换一个新配对码（已开着就重新计时、换码）；之前的试错锁定清零。
    pub fn open_pairing(&self) {
        let mut st = self.sh.pair.lock().unwrap();
        st.open_until = Some(Instant::now() + PAIR_WINDOW);
        st.locked_until = None;
        st.lock_rounds = 0;
        self.sh.rotate_code(&mut st);
        info!("已打开配对（{} 分钟内有效）", PAIR_WINDOW.as_secs() / 60);
    }

    /// 停止配对（配对码随之作废）。
    pub fn close_pairing(&self) {
        let mut st = self.sh.pair.lock().unwrap();
        if st.open_until.take().is_some() {
            self.sh.clear_code(&mut st);
            info!("已停止配对");
        }
    }

    /// 配对开着时：配对码、还开多久、是否暂停中。
    pub fn pairing(&self) -> Option<Pairing> {
        let st = self.sh.pair.lock().unwrap();
        let now = Instant::now();
        let left = st.open_until?.checked_duration_since(now).filter(|d| !d.is_zero())?;
        let locked = st.locked_until.and_then(|t| t.checked_duration_since(now)).unwrap_or_default();
        Some(Pairing { code: st.code.clone(), left, locked })
    }

    pub fn devices(&self) -> Vec<Paired> {
        self.sh.devices.lock().unwrap().iter().map(Device::paired).collect()
    }

    /// 在 Mac 上取消配对：这台 iPad 的令牌立即失效。返回被取消的那台。
    pub fn unpair(&self, id: &str) -> Option<Paired> {
        let mut devs = self.sh.devices.lock().unwrap();
        let i = devs.iter().position(|d| d.id == id)?;
        let d = devs.remove(i);
        self.sh.save_devices(&devs);
        info!("在 Mac 上取消配对：{}（{}）", d.name, d.id);
        Some(d.paired())
    }

    /// 取走上次调用以来新配对上的 iPad 名称。
    pub fn take_new_pairings(&self) -> Vec<String> {
        std::mem::take(&mut *self.sh.new_pairs.lock().unwrap())
    }
}

impl Device {
    fn paired(&self) -> Paired {
        Paired {
            id: self.id.clone(),
            name: self.name.clone(),
            paired_at: self.paired_at,
            sidecar_name: self.sidecar_name.clone(),
        }
    }
}

fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn new_code() -> String {
    format!("{:06}", rand::thread_rng().gen_range(0..1_000_000))
}

/// 只有当前用户能读写。
fn write_private(path: &Path, data: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    let tmp = path.with_extension("tmp");
    {
        let mut o = std::fs::OpenOptions::new();
        o.write(true).create(true).truncate(true);
        // Mac 上限定 0600；其它系统（只用于跑测试）没有这个权限位
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            o.mode(0o600);
        }
        let mut f = o.open(&tmp)?;
        f.write_all(data)?;
    }
    std::fs::rename(tmp, path)
}

/// 读取或生成自签证书。返回 (证书, 私钥)。
fn load_or_create_cert(dir: &Path) -> Result<(Vec<u8>, Vec<u8>), String> {
    let (cp, kp) = (dir.join("tls-cert.der"), dir.join("tls-key.der"));
    if let (Ok(c), Ok(k)) = (std::fs::read(&cp), std::fs::read(&kp)) {
        return Ok((c, k));
    }
    let ck = rcgen::generate_simple_self_signed(vec!["afar.local".to_string()]).map_err(|e| e.to_string())?;
    let (c, k) = (ck.cert.der().to_vec(), ck.key_pair.serialize_der());
    std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    write_private(&cp, &c).map_err(|e| e.to_string())?;
    write_private(&kp, &k).map_err(|e| e.to_string())?;
    Ok((c, k))
}

/// 当前配对码（配对开着时才有这个文件，给 `pair-code` 命令读）。
pub fn code_path(dir: &Path) -> PathBuf {
    dir.join("pair-code")
}

impl Shared {
    fn save_devices(&self, devs: &[Device]) {
        if let Ok(j) = serde_json::to_vec_pretty(devs) {
            if let Err(e) = write_private(&self.dir.join("devices.json"), &j) {
                warn!("保存配对设备失败：{e}");
            }
        }
    }

    fn rotate_code(&self, st: &mut PairState) {
        st.code = new_code();
        st.attempts = 0;
        st.pending.clear();
        if let Err(e) = write_private(&code_path(&self.dir), st.code.as_bytes()) {
            warn!("写配对码失败：{e}");
        }
    }

    /// 配对关了：配对码作废、文件删掉。
    fn clear_code(&self, st: &mut PairState) {
        st.code.clear();
        st.attempts = 0;
        st.pending.clear();
        let _ = std::fs::remove_file(code_path(&self.dir));
    }

    fn device_by_token(&self, msg: &Message) -> Option<Device> {
        let h = pair::hash_token(msg.bearer()?);
        self.devices.lock().unwrap().iter().find(|d| pair::ct_eq(&d.token_hash, &h)).cloned()
    }
}

/// 启动服务（后台线程）。返回句柄（含实际监听地址）。配对默认关着，要 `Handle::open_pairing` 打开。
pub fn start(dir: PathBuf, port: u16, backend: Arc<dyn Backend>) -> Result<Handle, String> {
    start_on(dir, SocketAddr::from(([0, 0, 0, 0], port)), backend)
}

/// 同 `start`，但指定监听地址（测试工具只监听本机 127.0.0.1，不对局域网开放）。
pub fn start_on(dir: PathBuf, bind: SocketAddr, backend: Arc<dyn Backend>) -> Result<Handle, String> {
    let port = bind.port();
    let (cert, key) = load_or_create_cert(&dir)?;
    let fp = pair::fingerprint(&cert);
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let cfg = ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .with_no_client_auth()
        .with_single_cert(vec![CertificateDer::from(cert)], PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key)))
        .map_err(|e| e.to_string())?;
    let cfg = Arc::new(cfg);

    let devices: Vec<Device> =
        std::fs::read(dir.join("devices.json")).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
    let shared = Arc::new(Shared {
        dir,
        fp,
        backend,
        pair: Mutex::new(PairState {
            code: String::new(),
            open_until: None,
            attempts: 0,
            locked_until: None,
            lock_rounds: 0,
            pending: HashMap::new(),
        }),
        devices: Mutex::new(devices),
        pw_failures: Mutex::new((0, None)),
        new_pairs: Mutex::new(Vec::new()),
    });
    {
        // 上次运行留下的配对码作废：启动时配对是关着的
        let mut st = shared.pair.lock().unwrap();
        shared.clear_code(&mut st);
    }

    // 配对开着的时间到了就关掉（配对码随之作废）
    {
        let shared = shared.clone();
        std::thread::spawn(move || loop {
            std::thread::sleep(Duration::from_secs(5));
            let mut st = shared.pair.lock().unwrap();
            if st.open_until.is_some_and(|t| Instant::now() >= t) {
                st.open_until = None;
                shared.clear_code(&mut st);
                info!("配对开了 {} 分钟，没有 iPad 配上，已停止", PAIR_WINDOW.as_secs() / 60);
            }
        });
    }

    let listener = TcpListener::bind(bind).map_err(|e| format!("监听端口 {port} 失败：{e}"))?;
    let addr = listener.local_addr().map_err(|e| e.to_string())?;
    let handle = Handle { sh: shared.clone(), addr };
    let active = Arc::new(AtomicUsize::new(0));
    std::thread::spawn(move || {
        for conn in listener.incoming() {
            let tcp = match conn {
                Ok(t) => t,
                Err(_) => {
                    std::thread::sleep(Duration::from_millis(100)); // 例如句柄用完，别空转
                    continue;
                }
            };
            if active.load(Ordering::SeqCst) >= MAX_CONNS {
                continue; // 直接丢弃（关闭）
            }
            active.fetch_add(1, Ordering::SeqCst);
            let (cfg, shared, active) = (cfg.clone(), shared.clone(), active.clone());
            std::thread::spawn(move || {
                // 连接中断、TLS 握手失败等很常见，不刷屏
                let _ = serve(tcp, cfg, &shared);
                active.fetch_sub(1, Ordering::SeqCst);
            });
        }
    });
    Ok(handle)
}

/// 第 n 次锁定锁多久：1、2、4、8…分钟，最长 LOCK_MAX。
fn lock_for(round: u32) -> Duration {
    LOCK_FOR.saturating_mul(1 << round.saturating_sub(1).min(10)).min(LOCK_MAX)
}

/// 向上取整的分钟数（至少 1）。
fn minutes(d: Duration) -> u64 {
    d.as_secs().div_ceil(60).max(1)
}

/// 给连接加"总时长"上限：每次读写前按剩余时间设超时，到点就失败。
struct Deadline {
    tcp: TcpStream,
    until: Instant,
}

impl Deadline {
    fn left(&self) -> std::io::Result<Duration> {
        let left = self.until.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "连接超时"));
        }
        Ok(left)
    }
}

impl Read for Deadline {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let t = self.left()?;
        self.tcp.set_read_timeout(Some(t))?;
        self.tcp.read(buf)
    }
}

impl Write for Deadline {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        let t = self.left()?;
        self.tcp.set_write_timeout(Some(t))?;
        self.tcp.write(buf)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.tcp.flush()
    }
}

fn serve(tcp: TcpStream, cfg: Arc<ServerConfig>, sh: &Shared) -> std::io::Result<()> {
    let conn = ServerConnection::new(cfg).map_err(std::io::Error::other)?;
    let mut tls = StreamOwned::new(conn, Deadline { tcp, until: Instant::now() + CONN_DEADLINE });
    let msg = read_message(&mut tls)?;
    let (status, body) = route(sh, &msg);
    write_response(&mut tls, status, &body)?;
    tls.conn.send_close_notify();
    let _ = tls.conn.complete_io(&mut tls.sock);
    Ok(())
}

type Reply = (u16, Vec<u8>);

fn ok<T: Serialize>(v: &T) -> Reply {
    (200, serde_json::to_vec(v).unwrap_or_default())
}

fn err(status: u16, code: &str, msg: &str) -> Reply {
    (status, serde_json::to_vec(&ErrorResp { code: code.into(), error: msg.into() }).unwrap_or_default())
}

fn parse<T: for<'de> Deserialize<'de>>(m: &Message) -> Result<T, Reply> {
    serde_json::from_slice(&m.body).map_err(|_| err(400, codes::BAD_REQUEST, "请求格式错误"))
}

#[derive(Serialize)]
struct Done {
    ok: bool,
}

fn route(sh: &Shared, m: &Message) -> Reply {
    let (method, path) = m.method_path();
    match (method, path) {
        ("GET", "/info") => {
            let s = sh.backend.status();
            ok(&Info { name: s.name, version: env!("CARGO_PKG_VERSION").into(), proto: PROTO_VERSION })
        }
        ("POST", "/pair/start") => pair_start(sh, m),
        ("POST", "/pair/finish") => pair_finish(sh, m),
        _ => {
            let Some(dev) = sh.device_by_token(m) else {
                return err(401, codes::UNAUTHORIZED, "未配对或令牌无效");
            };
            authed(sh, m, method, path, dev)
        }
    }
}

fn pair_start(sh: &Shared, m: &Message) -> Reply {
    let req: PairStartReq = match parse(m) {
        Ok(r) => r,
        Err(e) => return e,
    };
    let mut st = sh.pair.lock().unwrap();
    let now = Instant::now();
    if !st.open_until.is_some_and(|t| now < t) {
        return err(
            403,
            codes::PAIRING_CLOSED,
            "Mac 上没有打开配对。请在 Mac 菜单栏点 Afar › 配对新 iPad…，再输入出现的数字",
        );
    }
    if let Some(t) = st.locked_until {
        if now < t {
            let m = minutes(t - now);
            return err(
                429,
                codes::LOCKED,
                &format!("试错次数太多，已暂停配对 {m} 分钟，到时间后用 Mac 上显示的新配对码再试"),
            );
        }
        st.locked_until = None;
    }
    // 每次发起配对都算一次尝试（猜码的人每试一个码都要来这里），用完就锁定并换码；锁的时间一次比一次长
    if st.attempts >= MAX_FAILURES {
        st.lock_rounds += 1;
        let lock = lock_for(st.lock_rounds);
        st.locked_until = Some(now + lock);
        sh.rotate_code(&mut st);
        let m = minutes(lock);
        warn!("配对码已被尝试 {MAX_FAILURES} 次，暂停配对 {m} 分钟并更换配对码");
        return err(
            429,
            codes::LOCKED,
            &format!("试错次数太多，已暂停配对 {m} 分钟，到时间后用 Mac 上显示的新配对码再试"),
        );
    }
    st.attempts += 1;
    st.pending.retain(|_, p| p.created.elapsed() < PENDING_TTL);
    let (half, msg) = pair::start_mac(&st.code);
    let key = match half.finish(&req.msg) {
        Ok(k) => k,
        Err(e) => return err(400, codes::BAD_REQUEST, &e),
    };
    let confirm = pair::confirm(&key, pair::ROLE_MAC, &sh.fp);
    let pair_id = pair::new_id();
    let name: String = req.device_name.chars().take(64).collect();
    st.pending.insert(pair_id.clone(), Pending { key, device_name: name, created: Instant::now() });
    ok(&PairStartResp { pair_id, msg, confirm })
}

fn pair_finish(sh: &Shared, m: &Message) -> Reply {
    let req: PairFinishReq = match parse(m) {
        Ok(r) => r,
        Err(e) => return e,
    };
    let mut st = sh.pair.lock().unwrap();
    let Some(p) = st.pending.remove(&req.pair_id) else {
        return err(400, codes::BAD_REQUEST, "配对已过期，请重新输入配对码");
    };
    if !pair::verify_confirm(&p.key, pair::ROLE_IPAD, &sh.fp, &req.confirm) {
        return err(403, codes::WRONG_CODE, "配对码不对"); // 已在第 1 步计过次数
    }
    // 配对成功，这次配对就结束了（配对码作废；要再配一台，在 Mac 上再点一次"配对新 iPad…"）
    st.open_until = None;
    st.lock_rounds = 0;
    sh.clear_code(&mut st);
    drop(st);

    let token = pair::new_token();
    let dev = Device {
        id: pair::new_id(),
        name: p.device_name,
        token_hash: pair::hash_token(&token),
        sidecar_name: None,
        paired_at: now_secs(),
    };
    info!("已配对：{}（{}）", dev.name, dev.id);
    sh.new_pairs.lock().unwrap().push(dev.name.clone());
    let device_id = dev.id.clone();
    let mut devs = sh.devices.lock().unwrap();
    devs.push(dev);
    sh.save_devices(&devs);
    ok(&PairFinishResp { device_id, token })
}

fn authed(sh: &Shared, m: &Message, method: &str, path: &str, dev: Device) -> Reply {
    match (method, path) {
        ("GET", "/status") => {
            let mut s = sh.backend.status();
            s.password_saved = sh.backend.password_saved();
            s.showing_me = s.showing.is_some() && s.showing == dev.sidecar_name;
            // 确定是别的 iPad：另一台已配对设备选过这个随航名称。都不是 = 不确定，界面两种可能都给出
            s.showing_other = s.showing.is_some()
                && !s.showing_me
                && sh.devices.lock().unwrap().iter().any(|d| d.id != dev.id && d.sidecar_name == s.showing);
            ok(&s)
        }
        ("POST", "/password") => {
            let req: PasswordReq = match parse(m) {
                Ok(r) => r,
                Err(e) => return e,
            };
            // 校验前先计数（同一把锁里）：同时发很多请求也只能试 MAX_FAILURES 次（v0.4.8 审查：原来校验完才计数，一轮能试约 32 次）
            {
                let mut f = sh.pw_failures.lock().unwrap();
                if let Some(t) = f.1 {
                    if Instant::now() < t {
                        return err(429, codes::LOCKED, "密码输错次数太多，请 1 分钟后重试");
                    }
                    *f = (0, None);
                }
                if f.0 >= MAX_FAILURES {
                    *f = (0, Some(Instant::now() + LOCK_FOR));
                    return err(429, codes::LOCKED, "密码输错次数太多，请 1 分钟后重试");
                }
                f.0 += 1;
            }
            if !sh.backend.verify_password(&req.password) {
                let mut f = sh.pw_failures.lock().unwrap();
                if f.0 >= MAX_FAILURES {
                    *f = (0, Some(Instant::now() + LOCK_FOR));
                }
                return err(400, codes::WRONG_PASSWORD, "密码不对");
            }
            *sh.pw_failures.lock().unwrap() = (0, None);
            match sh.backend.save_password(&req.password) {
                Ok(()) => ok(&Done { ok: true }),
                Err(e) => err(500, codes::INTERNAL, &format!("保存密码失败：{e}")),
            }
        }
        ("POST", "/connect") => {
            let req: ConnectReq = match parse(m) {
                Ok(r) => r,
                Err(e) => return e,
            };
            let nearby = sh.backend.status().nearby;
            let target = req.sidecar_name.or(dev.sidecar_name.clone()).or_else(|| {
                if nearby.len() == 1 {
                    nearby.first().cloned()
                } else {
                    None
                }
            });
            let Some(target) = target else {
                return if nearby.is_empty() {
                    err(
                        409,
                        codes::NO_IPAD,
                        "附近没有可用的 iPad（确认 iPad 已解锁、同一 Apple 账号、蓝牙/Wi-Fi/接力已开）",
                    )
                } else {
                    err(409, codes::CHOOSE_IPAD, &format!("附近有多台 iPad，请选一台：{}", nearby.join("、")))
                };
            };
            if dev.sidecar_name.as_deref() != Some(target.as_str()) {
                let mut devs = sh.devices.lock().unwrap();
                if let Some(d) = devs.iter_mut().find(|d| d.id == dev.id) {
                    d.sidecar_name = Some(target.clone());
                }
                sh.save_devices(&devs);
            }
            // 先告诉主循环这台 iPad 的屏幕多大（同一个队列，连接前处理）
            if let Some(px) = req.screen.and_then(afar_proto::normalize_screen) {
                let _ = sh.backend.command(Command::IpadScreen { name: target.clone(), px });
            }
            run(sh, Command::Connect { sidecar_name: target, keyboard: req.keyboard, auto: req.auto })
        }
        ("POST", "/disconnect") => run(sh, Command::Disconnect),
        ("POST", "/front") => match dev.sidecar_name.clone() {
            Some(name) => run(sh, Command::Front { sidecar_name: name }),
            // 这台 iPad 还没从 App 连过：Mac 不知道它在随航里叫什么
            None => err(409, codes::NO_IPAD, "这台 iPad 还没从 Afar 连过，请先点卡片连接"),
        },
        ("POST", "/restart") => {
            // 旧版 iPad App 发的是 {}（甚至没有请求体）：都按不重新打开窗口
            let req: afar_proto::RestartReq = parse(m).unwrap_or_default();
            run(sh, Command::Restart { reopen: req.reopen })
        }
        ("POST", "/settings") => {
            let req: SettingsReq = match parse(m) {
                Ok(r) => r,
                Err(e) => return e,
            };
            if let Some(s) = req.scale {
                if !sh.backend.status().scales.iter().any(|x| x.key == s) {
                    return err(400, codes::BAD_REQUEST, "没有这个缩放档位");
                }
                if let Err(e) = sh.backend.command(Command::SetScale(s)) {
                    return err(500, codes::INTERNAL, &e);
                }
            }
            if let Some(n) = req.name {
                let n: String = n.trim().chars().take(64).collect();
                if n.is_empty() {
                    return err(400, codes::BAD_REQUEST, "名称不能为空");
                }
                if let Err(e) = sh.backend.command(Command::Rename(n)) {
                    return err(500, codes::INTERNAL, &e);
                }
            }
            ok(&Done { ok: true })
        }
        ("GET", "/keyboards") => match sh.backend.keyboards() {
            Ok(v) => ok(&v),
            Err(e) => err(403, codes::BT_DENIED, &e),
        },
        ("POST", "/keyboard") => {
            let req: KeyboardReq = match parse(m) {
                Ok(r) => r,
                Err(e) => return e,
            };
            let kb = match req.action.as_str() {
                "set" => match req.address.filter(|a| !a.trim().is_empty()) {
                    None => Kb::Set(None),
                    Some(a) => {
                        // 只能选 Mac 上已配对的设备
                        let Some(k) = sh.backend.keyboards().unwrap_or_default().into_iter().find(|k| k.address == a)
                        else {
                            return err(400, codes::BAD_REQUEST, "Mac 上没有配对这个设备");
                        };
                        if k.ble {
                            return err(
                                400,
                                codes::BAD_REQUEST,
                                "这是低功耗蓝牙设备，Afar 没法在 Mac 和 iPad 之间交接它",
                            );
                        }
                        Kb::Set(Some(KeyboardInfo { connected: false, ..k }))
                    }
                },
                "auto" => match req.auto {
                    Some(v) => Kb::Auto(v),
                    None => return err(400, codes::BAD_REQUEST, "缺少 auto"),
                },
                "take" => Kb::Take,
                "release" => Kb::Release,
                "release_now" => Kb::ReleaseNow,
                "open_settings" => Kb::OpenSettings,
                "pair_search" => Kb::PairSearch,
                "pair_cancel" => Kb::PairCancel,
                "pair" => {
                    let a = req.address.unwrap_or_default();
                    let progress = sh.backend.status().keyboard_pair;
                    if progress.as_ref().is_some_and(|p| p.stage == "pairing") {
                        return err(409, codes::BAD_REQUEST, "正在配对，请稍候");
                    }
                    let found = progress.map(|p| p.found).unwrap_or_default();
                    if !found.iter().any(|k| k.address == a) {
                        return err(400, codes::BAD_REQUEST, "没有搜到这个设备，请重新搜索");
                    }
                    Kb::Pair(a)
                }
                _ => return err(400, codes::BAD_REQUEST, "没有这个动作"),
            };
            run(sh, Command::Keyboard(kb))
        }
        ("POST", "/unpair") => {
            let mut devs = sh.devices.lock().unwrap();
            devs.retain(|d| d.id != dev.id);
            sh.save_devices(&devs);
            info!("已取消配对：{}（{}）", dev.name, dev.id);
            ok(&Done { ok: true })
        }
        _ => err(404, codes::NOT_FOUND, "没有这个接口"),
    }
}

fn run(sh: &Shared, cmd: Command) -> Reply {
    match sh.backend.command(cmd) {
        Ok(()) => ok(&Done { ok: true }),
        Err(e) => err(500, codes::INTERNAL, &e),
    }
}
