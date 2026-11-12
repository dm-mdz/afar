//! 主循环：保活 → 建虚拟屏 → 设缩放 → 启动控制接口 → 每秒处理 iPad App 的指令，每 3 秒检查随航（自动连接、镜像）。

use crate::kbflow::{Act, KbFlow};
use crate::mac_backend::MacBackend;
use crate::menubar::{self, MenuBar};
use crate::persist::Saved;
use crate::private::sidecar;
use crate::private::virtual_display::{self, VirtualDisplay};
use crate::reconnect::{self, Front, FrontAct, Keep, FRONT_TIMEOUT};
use crate::screens::{self, Kind, VdAct, VdPlan};
use crate::server::{self, Command, Kb};
use crate::{bt, bt_pair, config, display, dnssd, keepalive, notify, runloop, scale, setup};
use afar_proto::{ScaleInfo, Status};
use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::os::fd::AsRawFd;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Receiver};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const TICK: Duration = Duration::from_secs(3);
/// 建虚拟屏 / 改镜像失败后多久再试。
const DISPLAY_RETRY: Duration = Duration::from_secs(10);
/// 刚断开随航后这段时间内 iPad 重新"出现"不算新出现（从 iPad 那头断开时不会被立刻连回去）。
const COOLDOWN: Duration = Duration::from_secs(30);
/// iPad App 点"连接"后，最多显示"连接中"多久。
const CONNECTING_FOR: Duration = Duration::from_secs(15);
/// 设置窗口关掉多久后释放网页（马上再打开时不用重建）。
const SETUP_RELEASE_AFTER: Duration = Duration::from_secs(30);
/// 从 Afar 断开后最多等这么久让断开生效（期间看到的"已连接"不算数）。
const DROP_GRACE: Duration = Duration::from_secs(10);

/// 随航连上后，等用户在 iPad 上断开键盘、Mac 接管的最长时间。
const KEYBOARD_TAKE_FOR: Duration = Duration::from_secs(60);

/// 防止开两个（会出现两块虚拟屏）。
fn single_instance() -> Result<File, String> {
    std::fs::create_dir_all(config::dir()).map_err(|e| e.to_string())?;
    let f = File::create(config::dir().join("agent.lock")).map_err(|e| e.to_string())?;
    let r = unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
    if r != 0 {
        return Err("已经有一个在运行了".into());
    }
    Ok(f)
}

struct State {
    /// 虚拟屏（换缩放档位失败时会按新档位重建，所以放在这里）。
    vd: Option<VirtualDisplay>,
    vid: u32,
    /// 实体显示器（按显示器编号认，见 screens.rs）。
    physical: HashSet<u32>,
    /// 每块在线显示器认成了什么（只在变化时记日志）。
    kinds: HashMap<u32, Kind>,
    /// 随航屏镜像虚拟屏（配置 mirror）。
    mirror: bool,
    /// 什么时候建、关虚拟屏。
    vd_plan: VdPlan,
    /// 上一轮正在显示的 iPad（随航里的名称，只用来记"断开了"的日志）。
    prev_showing: Option<String>,
    /// 保持显示：不是从 Afar 断开的都自动连回（见 reconnect.rs）。
    keep: Keep,
    /// "回到 Mac 桌面"进行中（断开 → 连回 → 稳定），见 reconnect::Front。
    front: Option<Front>,
    /// 刚从 Afar 断开（菜单栏、App、命令行）：断开是异步的，这几秒里还看到"已连接"不能当成又连上了
    /// （v0.4.3 测试：菜单栏断开后 3 秒被当成意外断开自动连回，2/2 复现）。
    dropping: Option<Instant>,
    /// 这次随航从什么时候开始连着（tick 里看，稳定 10 秒才算真连上）。
    connected_since: Option<Instant>,
    /// 随航屏从什么时候开始在线（每秒看；接着实体显示器时，稳定 2 秒才为它建虚拟屏）。
    sidecar_since: Option<Instant>,
    /// 重建虚拟屏后读不到它的模式时，已经多等了几轮。
    scale_wait: u8,
    /// 虚拟屏用的序列号（重建时在 1、2 之间换，新旧两块能同时存在）。
    vd_serial: u32,
    /// 实体显示器这次出现以来的情况（见 restore_physical_modes）。
    appeared: HashMap<u32, Appear>,
    /// 实体显示器刚插上：到这个时间前 Afar 不做任何显示配置（见 PLUG_HOLD）。
    plug_hold: Option<Instant>,
    /// 建虚拟屏 / 改镜像失败后，到这个时间再试（不每秒做一次显示配置，免得闪屏）。
    display_retry_at: Option<Instant>,
    /// 上一轮目标 iPad 是否可用（只在"从不可用变可用"时自动连，手动断开后不会被立刻连回去）。
    was_available: bool,
    /// 想要的缩放档位（启动时太早切不成功的话，主循环里会再试）。
    want: scale::Preset,
    scale_ok: bool,
    /// 最近一次切换缩放失败的原因（报给 iPad App；成功后清空）。
    scale_error: String,
    /// 最近一次重启失败的原因（后台线程写，报给 iPad App）。
    restart_error: Arc<Mutex<String>>,
    /// 这次换档已经重建过几次虚拟屏（写法 0、写法 1 各最多一次，见 scale::vd_modes），避免反复闪屏。
    rebuilds: u8,
    /// 这次换档已经替随航屏切过模式（只切一次）。
    sidecar_switched: bool,
    /// 切换接口返回成功、但下一轮核对还不对的次数（到 2 就当失败，免得每 3 秒切一次、一直闪）。
    set_tries: u8,
    /// 只核对随航屏尺寸（随航重连、系统自己恢复镜像后发现对不上）：替它切一次，不行只报告，不重建、不回默认、不改保存的档位。
    follow_only: bool,
    /// 这块随航屏已经对过、对不上，不再反复替它切（换一次连接显示器编号会变，再试一次）。
    follow_gave_up: Option<u32>,
    /// 三档尺寸已按这块随航屏支持的尺寸取过（显示器编号）。
    presets_from: Option<u32>,
    /// 按 iPad 比例算出的三档（随航屏没有对应尺寸时用）。
    base_presets: Vec<scale::Preset>,
    /// 配置里的 iPad 屏幕像素（iPad 没报过时用）。
    cfg_ipad_pixels: [u32; 2],
    /// 这次换档是 Afar 自己发起的（换了 iPad、按随航屏校正尺寸）：可以重建虚拟屏，但切不成只报告，
    /// 不回"默认"、不改保存的档位（那是用户自己选的）。
    auto_resize: bool,
    /// 这次换档用不了、自动回到了"默认"：成功切到默认后也保留提示。
    fell_back: bool,
    /// 换档是 iPad App 请求的（启动时只重试直接切换，不重建、不回退保存的档位）。
    requested: bool,
    /// 最近一次看到"已连接"的时间。
    last_connected: Option<Instant>,
    /// 同一类警告只记一次，避免日志无限变大。
    scale_warned: bool,
    mirror_warned: bool,
    /// 创建虚拟屏失败的警告只记一次。
    vd_warned: bool,
    /// iPad App 点了"连接"，等待随航连上。
    connecting_until: Option<Instant>,
    saved: Saved,
    dir: PathBuf,
    presets: Vec<scale::Preset>,
    status: Arc<Mutex<Status>>,
    bonjour: Option<dnssd::Registration>,
    port: u16,
    /// 系统信息（后台线程每分钟刷新，主线程只读，不会卡菜单）。
    sys: Arc<Mutex<SysInfo>>,
    /// 蓝牙键盘：后台线程的指令通道和实时状态。
    bt_tx: std::sync::mpsc::Sender<bt::Req>,
    bt_live: Arc<bt::LiveCell>,
    /// 键盘什么时候接管 / 松开的决策（纯逻辑，见 kbflow.rs，有序列测试）。
    kb: KbFlow,
    /// "设置 Afar"窗口（第一次打开时才建）。
    setup: Option<setup::SetupWindow>,
    /// 窗口开着时上次要求刷新系统信息的时间。
    setup_refresh: Option<Instant>,
    /// 设置窗口从什么时候开始是关着的（关满 SETUP_RELEASE_AFTER 就释放）。
    setup_hidden_since: Option<Instant>,
    /// 启动时程序文件的修改时间（再次双击 Afar.app 时发现程序被换了新版，就重启自己）。
    exe_mtime: Option<std::time::SystemTime>,
    /// 控制接口的句柄：开关配对、管理已配对的 iPad。
    pair: Option<server::Handle>,
    /// 配对是从"设置 Afar"窗口打开的（窗口关掉就停止配对；命令行打开的不跟窗口走）。
    pair_via_window: bool,
    /// 这次配对页上刚配对上的 iPad（页面显示"已配对"）。
    pair_done: String,
    /// 要"设置 Afar"页面跳到哪（见 setup::View::go），页面回 went 后清掉。
    setup_go: String,
    /// 系统通知（只在 Afar.app 里有）。
    notifier: Option<notify::Notifier>,
}

/// 慢的系统检查放后台线程：FileVault、远程登录、SSH 主机公钥、电脑名称。
#[derive(Clone, Default)]
struct SysInfo {
    filevault: bool,
    remote_login: bool,
    auto_login: bool,
    auto_restart: bool,
    ssh_host_keys: Vec<String>,
    computer_name: String,
    /// 防火墙没拦 Afar。
    firewall_ok: bool,
    /// 装了重启助手。
    helper: bool,
}

impl SysInfo {
    fn read() -> SysInfo {
        SysInfo {
            filevault: filevault_on(),
            remote_login: remote_login_on(),
            auto_login: auto_login_on(),
            auto_restart: auto_restart_on(),
            ssh_host_keys: ssh_host_keys(),
            computer_name: computer_name(),
            firewall_ok: setup::firewall_ok(
                &std::env::current_exe().and_then(|p| p.canonicalize()).unwrap_or_default(),
            ),
            helper: setup::helper_installed(),
        }
    }
}

fn start_sys_thread() -> Arc<Mutex<SysInfo>> {
    let sys = Arc::new(Mutex::new(SysInfo::read()));
    let s2 = sys.clone();
    std::thread::spawn(move || loop {
        // 每分钟刷新一次；"设置 Afar"里做完一步后要求马上刷新（SYS_REFRESH）
        for _ in 0..60 {
            std::thread::sleep(Duration::from_secs(1));
            if SYS_REFRESH.swap(false, Ordering::SeqCst) {
                break;
            }
        }
        let new = SysInfo::read();
        let old = s2.lock().unwrap().clone();
        if new.filevault && !new.remote_login && (old.filevault, old.remote_login) != (true, false) {
            warn!("FileVault 已开启但「远程登录」没开：断电重启后 iPad 无法远程解锁。请到 系统设置 > 通用 > 共享 打开「远程登录」");
        }
        *s2.lock().unwrap() = new;
    });
    sys
}

/// FileVault 是否开着。
fn filevault_on() -> bool {
    std::process::Command::new("/usr/bin/fdesetup")
        .arg("isactive")
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim() == "true")
        .unwrap_or(false)
}

/// "远程登录"（SSH）是否开着：本机 22 端口能连上就算开着。
fn remote_login_on() -> bool {
    let addr = std::net::SocketAddr::from(([127, 0, 0, 1], 22));
    std::net::TcpStream::connect_timeout(&addr, Duration::from_millis(300)).is_ok()
}

/// 是否开着"自动登录"：loginwindow 偏好里有 autoLoginUser（这个文件所有人可读）。
fn auto_login_on() -> bool {
    std::process::Command::new("/usr/bin/defaults")
        .args(["read", "/Library/Preferences/com.apple.loginwindow", "autoLoginUser"])
        .output()
        .map(|o| o.status.success() && !String::from_utf8_lossy(&o.stdout).trim().is_empty())
        .unwrap_or(false)
}

/// 是否开着"断电后自动开机"：`pmset -g` 里 autorestart 为 1。
fn auto_restart_on() -> bool {
    std::process::Command::new("/usr/bin/pmset")
        .arg("-g")
        .output()
        .map(|o| parse_autorestart(&String::from_utf8_lossy(&o.stdout)))
        .unwrap_or(false)
}

fn parse_autorestart(pmset: &str) -> bool {
    pmset.lines().any(|l| {
        let mut it = l.split_whitespace();
        it.next() == Some("autorestart") && it.next() == Some("1")
    })
}

/// 本机 SSH 主机公钥（这些文件所有人可读）。iPad 在已配对的加密通道里拿到，远程解锁时只认它们。
fn ssh_host_keys() -> Vec<String> {
    ["ed25519", "ecdsa", "rsa"]
        .iter()
        .filter_map(|k| std::fs::read_to_string(format!("/etc/ssh/ssh_host_{k}_key.pub")).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .collect()
}

/// 苹果菜单重启框里"登录时重新打开窗口"那个勾记在 com.apple.loginwindow 的 TALLogoutSavesState：
/// 按 iPad 上的选择写进去（和原生一样，下次在 Mac 上手动重启时这个勾也是这次的选择）。
fn set_reopen_windows(reopen: bool) {
    let r = std::process::Command::new("/usr/bin/defaults")
        .args(["write", "com.apple.loginwindow", "TALLogoutSavesState", "-bool", if reopen { "true" } else { "false" }])
        .status();
    match r {
        Ok(s) if s.success() => {}
        other => warn!("没能设置\"登录时重新打开窗口\"：{other:?}"),
    }
}

/// System Events 重启，并按用户的"登录时重新打开窗口"设置（TALLogoutSavesState）决定恢不恢复窗口。
/// 等 60 秒：第一次会弹"允许 Afar 控制 System Events"，给用户点的时间。
fn restart_with_state_pref() -> Result<(), String> {
    run_osascript("tell application \"System Events\" to restart with state saving preference", 120)
}

/// 跑一段 osascript，最多等 `half_secs` 个半秒；超时就结束它。
fn run_osascript(script: &str, half_secs: u32) -> Result<(), String> {
    let mut child = std::process::Command::new("/usr/bin/osascript")
        .args(["-e", script])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| e.to_string())?;
    for _ in 0..half_secs {
        if let Some(st) = child.try_wait().map_err(|e| e.to_string())? {
            if st.success() {
                return Ok(());
            }
            let mut msg = String::new();
            if let Some(mut e) = child.stderr.take() {
                let _ = std::io::Read::read_to_string(&mut e, &mut msg);
            }
            return Err(msg.trim().to_string());
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    let _ = child.kill();
    Err(format!("{} 秒没有回应", half_secs / 2))
}

/// 直接让 loginwindow 重启（kAERestart，无界面）：各程序正常退出，但不看"登录时重新打开窗口"的设置，总是恢复窗口。只作退路。
fn restart_via_loginwindow() -> Result<(), String> {
    run_osascript("tell application \"loginwindow\" to «event aevtrest»", 30)
}

/// 要系统信息线程马上刷新一次（"设置 Afar"里做完一步后）。
static SYS_REFRESH: AtomicBool = AtomicBool::new(false);

/// 从"应用程序"里打开 Afar.app 时留的请求文件：正在运行的 Afar 看到就打开"设置 Afar"窗口。
pub const OPEN_WINDOW_REQUEST: &str = "open-window.request";
/// 命令行 `afar-agent pair-code` 留的请求文件：正在运行的 Afar 看到就打开配对。
pub const PAIR_REQUEST: &str = "pair-open.request";

/// "设置 Afar"窗口要显示的状态。
fn setup_view(st: &State) -> setup::View {
    let sys = st.sys.lock().unwrap().clone();
    let status = st.status.lock().unwrap().clone();
    let (busy, error) = setup::action();
    let exe = std::env::current_exe().and_then(|p| p.canonicalize()).unwrap_or_default();
    let pairing = st.pair.as_ref().and_then(|h| h.pairing());
    let devices: Vec<setup::DeviceView> = st
        .pair
        .as_ref()
        .map(|h| h.devices())
        .unwrap_or_default()
        .into_iter()
        .map(|d| setup::DeviceView {
            showing: d.sidecar_name.is_some() && d.sidecar_name == status.showing,
            date: setup::day_label(d.paired_at),
            id: d.id,
            // iPadOS 不让 App 读设备名，配对时报上来的多半就是"iPad"；连过一次后有随航里的名称（如"某某的 iPad"），更好认
            name: d.sidecar_name.unwrap_or(d.name),
        })
        .collect();
    setup::View {
        version: env!("CARGO_PKG_VERSION").into(),
        name: status.name,
        code: pairing.as_ref().map(|p| p.code.clone()).unwrap_or_default(),
        pair_left: pairing.as_ref().map(|p| p.left.as_secs()).unwrap_or(0),
        pair_lock: pairing.as_ref().map(|p| p.locked.as_secs()).unwrap_or(0),
        pair_done: st.pair_done.clone(),
        go: st.setup_go.clone(),
        done: st.saved.setup_done,
        agent: crate::launchd::installed_for(&exe),
        fw: sys.firewall_ok,
        power: sys.auto_restart,
        helper: sys.helper,
        // 正在显示时那台 iPad 不一定还在"可用"列表里
        ipad: status.nearby.first().cloned().or(status.showing.clone()).unwrap_or_default(),
        showing: status.showing.unwrap_or_default(),
        paired: devices.len(),
        devices,
        filevault: sys.filevault,
        auto_login: sys.auto_login,
        remote_login: sys.remote_login,
        plan: st.saved.plan.clone().unwrap_or_else(|| if sys.filevault { "fv".into() } else { "auto".into() }),
        busy,
        error,
    }
}

fn exe_mtime() -> Option<std::time::SystemTime> {
    std::env::current_exe().and_then(std::fs::metadata).and_then(|m| m.modified()).ok()
}

fn open_setup(st: &mut State) {
    if st.setup.is_none() {
        st.setup = setup::SetupWindow::new();
    }
    if let Some(w) = &st.setup {
        w.show();
    }
}

/// 处理"设置 Afar"窗口发来的指令，并把最新状态推给它。每秒调用。
fn handle_setup(st: &mut State) {
    for m in setup::take_messages() {
        match m {
            setup::Msg::Ready => {
                if let Some(w) = st.setup.as_mut() {
                    w.reset();
                }
            }
            setup::Msg::One => setup::run_one(&st.dir, || SYS_REFRESH.store(true, Ordering::SeqCst)),
            setup::Msg::Ssh => setup::run_ssh(|| SYS_REFRESH.store(true, Ordering::SeqCst)),
            setup::Msg::Open(what) => {
                setup::open_settings(&what);
                SYS_REFRESH.store(true, Ordering::SeqCst);
            }
            setup::Msg::Plan(p) if p == "auto" || p == "fv" => {
                st.saved.plan = Some(p);
                st.saved.save(&st.dir);
            }
            setup::Msg::Done => {
                info!("设置 Afar：向导走完了");
                st.saved.setup_done = true;
                st.saved.save(&st.dir);
            }
            setup::Msg::Close => {
                if let Some(w) = &st.setup {
                    w.close();
                }
            }
            setup::Msg::PairOpen => {
                if let Some(h) = &st.pair {
                    h.open_pairing();
                }
                st.pair_done.clear();
                st.pair_via_window = true;
            }
            setup::Msg::PairClose => {
                if let Some(h) = &st.pair {
                    h.close_pairing();
                }
                st.pair_via_window = false;
            }
            setup::Msg::Went => st.setup_go.clear(),
            setup::Msg::Unpair(id) => {
                let gone = st.pair.as_ref().and_then(|h| h.unpair(&id)).and_then(|d| d.sidecar_name);
                if let Some(name) = gone {
                    // 正显示到这台：顺便断开（它的令牌已经失效，App 里也没法再断开）
                    if st.status.lock().unwrap().showing.as_deref() == Some(name.as_str()) {
                        run_command(Command::Disconnect, st);
                    }
                    // 也不再把它当成"上次用的 iPad"自动连（v0.4.8 审查）
                    if st.saved.last_ipad.as_deref() == Some(name.as_str()) {
                        st.saved.last_ipad = None;
                        st.saved.save(&st.dir);
                    }
                }
            }
            _ => {}
        }
    }
    // 关掉 30 秒后彻底释放（网页那部分很占内存）；再打开时重建，页面会回到向导第 1 屏或状态页
    match st.setup.as_ref().map(|w| w.visible() || w.minimized()) {
        Some(false) => {
            if let Some(w) = &st.setup {
                w.background();
            }
            // 窗口关了：从窗口打开的配对跟着停止
            if std::mem::take(&mut st.pair_via_window) {
                if let Some(h) = &st.pair {
                    h.close_pairing();
                }
            }
            let since = *st.setup_hidden_since.get_or_insert_with(Instant::now);
            if since.elapsed() >= SETUP_RELEASE_AFTER {
                info!("设置 Afar 窗口关掉 {} 秒了，释放", SETUP_RELEASE_AFTER.as_secs());
                if let Some(w) = st.setup.take() {
                    w.destroy();
                }
                st.setup_hidden_since = None;
            }
        }
        _ => st.setup_hidden_since = None,
    }
    if st.setup.as_ref().is_some_and(|w| w.visible()) {
        // 窗口开着时每 5 秒刷新一次系统信息：用户在系统设置里改完（文件保险箱、自动登录）很快就能看到
        if st.setup_refresh.is_none_or(|t| t.elapsed() >= Duration::from_secs(5)) {
            st.setup_refresh = Some(Instant::now());
            SYS_REFRESH.store(true, Ordering::SeqCst);
        }
        let v = setup_view(st);
        if let Some(w) = st.setup.as_mut() {
            w.push(&v);
        }
    }
}

/// 电脑名称（系统设置里的那个）。
fn computer_name() -> String {
    std::process::Command::new("/usr/sbin/scutil")
        .args(["--get", "ComputerName"])
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "Mac".into())
}

pub fn run() -> i32 {
    let _lock = match single_instance() {
        Ok(f) => f,
        Err(e) => {
            error!("{e}");
            return 1;
        }
    };
    let cfg = config::load();
    info!("启动：{:?}", cfg);

    let _keep = keepalive::KeepAlive::start();

    let dir = config::dir();
    let saved = Saved::load(&dir);
    // 上次用的 iPad 报过屏幕尺寸就按它算（不同型号屏幕不一样大），按随航屏校正过的尺寸也直接用；
    // 没报过用配置里的（默认 iPad Air 4 的 2360×1640）
    let (base_presets, presets) = ipad_presets(&saved, saved.last_ipad.as_deref(), cfg.ipad_pixels);
    let want = scale::pick(&presets, saved.scale.as_deref().unwrap_or(&cfg.scale));

    // 启动时先认显示器：要不要虚拟屏见 screens::want_vd（没有实体显示器，或 iPad 正在显示）。
    let already_connected = sidecar::connected().map(|c| !c.is_empty()).unwrap_or(false);
    let mut kinds = HashMap::new();
    let (physical, showing) = classify(&mut kinds, 0);
    let vd = if screens::want_vd(!physical.is_empty(), showing, cfg.mirror) {
        match virtual_display::create(&scale::vd_modes(&presets, want, 0), scale::vd_max(&presets), VD_NAME, 1) {
            Ok(v) => {
                info!("虚拟屏已创建，显示器 ID {}（{}）", v.id, vd_reason(!physical.is_empty()));
                // 等系统把虚拟屏挂上来再切缩放档位。
                runloop::run_for(2.0);
                Some(v)
            }
            // 没有实体显示器时没法继续（Mac 一块屏都没有）：退出让 launchd 重来；有显示器时主循环里再试
            Err(e) if physical.is_empty() => {
                error!("创建虚拟屏失败：{e}");
                return 1;
            }
            Err(e) => {
                warn!("创建虚拟屏失败（会重试）：{e}");
                None
            }
        }
    } else {
        info!("接着实体显示器 {physical:?}，iPad 没在显示，先不开虚拟屏");
        None
    };
    let bt_live = Arc::new(bt::LiveCell::default());
    let bt_tx = bt::spawn(bt_live.clone());
    if let Some(k) = &saved.keyboard {
        let _ = bt_tx.send(bt::Req::Watch(Some(k.address.clone())));
    }
    let mut st = State {
        bt_tx,
        bt_live,
        // 接管期间程序重启过（taken 存在 state.json 里）：记得断开随航时还要松开
        kb: KbFlow::new(already_connected, saved.keyboard_taken),
        vid: vd.as_ref().map(|v| v.id).unwrap_or(0),
        vd,
        physical,
        kinds,
        mirror: cfg.mirror,
        vd_plan: VdPlan::default(),
        prev_showing: None,
        keep: Keep::default(),
        front: None,
        dropping: None,
        connected_since: None,
        sidecar_since: None,
        scale_wait: 0,
        vd_serial: 1,
        appeared: HashMap::new(),
        plug_hold: None,
        display_retry_at: None,
        was_available: false,
        want,
        scale_ok: false,
        scale_error: String::new(),
        rebuilds: 0,
        sidecar_switched: false,
        set_tries: 0,
        follow_only: false,
        follow_gave_up: None,
        presets_from: None,
        base_presets,
        cfg_ipad_pixels: cfg.ipad_pixels,
        auto_resize: false,
        fell_back: false,
        requested: false,
        restart_error: Arc::new(Mutex::new(String::new())),
        last_connected: None,
        scale_warned: false,
        mirror_warned: false,
        vd_warned: false,
        connecting_until: None,
        saved,
        dir: dir.clone(),
        presets,
        status: Arc::new(Mutex::new(Status::default())),
        bonjour: None,
        port: cfg.port,
        sys: start_sys_thread(),
        setup: None,
        setup_refresh: None,
        setup_hidden_since: None,
        exe_mtime: exe_mtime(),
        pair: None,
        pair_via_window: false,
        pair_done: String::new(),
        setup_go: String::new(),
        notifier: None,
    };
    // 启动时已经接着的显示器不"恢复分辨率"（Afar 没运行时用户可能改过），直接当成早就在了
    let old = Instant::now().checked_sub(RESTORE_WITHIN).unwrap_or_else(Instant::now);
    // （开机后 40 秒内 checked_sub 拿不到更早的时间，所以同时把次数记满：不会去改它）
    st.appeared = st
        .physical
        .iter()
        .map(|d| (*d, Appear { since: old, tries: RESTORE_TRIES, last: None, stuck: None, listed: true }))
        .collect();
    ensure_scale(&mut st);

    // 控制接口：iPad App 通过它配对、看状态、连接、重启、改设置。
    let (tx, rx) = channel::<Command>();
    let backend = Arc::new(MacBackend { status: st.status.clone(), tx, bt: st.bt_tx.clone() });
    update_status(&mut st, &[], &[]);
    match server::start(dir, cfg.port, backend) {
        Ok(h) => {
            info!("控制接口已启动：{}", h.addr());
            st.pair = Some(h);
        }
        Err(e) => {
            error!("控制接口启动失败：{e}");
            return 1;
        }
    }
    register_bonjour(&mut st);

    // 菜单栏图标（设置环境变量 AFAR_NO_MENUBAR=1 可关掉，例如在 SSH 里手动运行时）。
    let mut bar = if std::env::var_os("AFAR_NO_MENUBAR").is_some() { None } else { MenuBar::new() };
    if bar.is_some() {
        st.notifier = notify::Notifier::new();
    }
    // 向导还没走完：有菜单栏（图形登录会话）时自动打开"设置 Afar"
    if bar.is_some() && !st.saved.setup_done {
        open_setup(&mut st);
    }

    // 上次运行留下的命令行断开请求作废（不然开机就把随航断了）
    let _ = std::fs::remove_file(st.dir.join(DISCONNECT_REQUEST));
    let _ = std::fs::remove_file(setup::restart_request(&st.dir));
    let _ = std::fs::remove_file(st.dir.join(PAIR_REQUEST));
    let mut last_tick = Instant::now().checked_sub(TICK).unwrap_or_else(Instant::now);
    let mut vd_created = Instant::now();
    loop {
        match &bar {
            Some(b) => b.pump(1.0),
            None => runloop::run_for(1.0),
        }
        let act = menubar::take_action();
        if act == menubar::ACT_SETUP {
            // 回到状态页（窗口可能还停在上次的配对页）
            st.setup_go = "status".into();
            open_setup(&mut st);
        }
        if act == menubar::ACT_PAIR {
            if let Some(h) = &st.pair {
                h.open_pairing();
            }
            st.pair_done.clear();
            st.pair_via_window = true;
            st.setup_go = "pair".into();
            open_setup(&mut st);
        }
        if act == menubar::ACT_DEVICES {
            st.setup_go = "devices".into();
            open_setup(&mut st);
        }
        // 命令行 `afar-agent pair-code`：打开配对（不跟窗口走，到时间自己停）
        let req = st.dir.join(PAIR_REQUEST);
        if req.exists() {
            let _ = std::fs::remove_file(&req);
            if let Some(h) = &st.pair {
                info!("命令行打开了配对");
                h.open_pairing();
            }
            st.pair_via_window = false; // 不跟窗口走：关掉窗口也不停
        }
        // 刚配对上的 iPad：配对页显示"已配对"，弹通知
        for name in st.pair.as_ref().map(|h| h.take_new_pairings()).unwrap_or_default() {
            if let Some(n) = &st.notifier {
                n.paired(&name);
            }
            st.pair_done = name;
            st.pair_via_window = false;
        }
        // Afar 在运行时再双击 Afar.app：系统不再启动新进程，而是给这里发"重新打开"
        if act == menubar::ACT_REOPEN {
            // （从磁盘映像里打开新版时系统启动的是新进程，升级提示在 app::upgrade 里，不走这里。）
            if exe_mtime() != st.exe_mtime {
                // 拖进来的是新版：退出（非 0），launchd 用新程序重新拉起，新的那份打开窗口
                info!("Afar.app 换了新版本，重新启动");
                let _ = std::fs::write(st.dir.join(OPEN_WINDOW_REQUEST), b"");
                return 1;
            }
            open_setup(&mut st);
        }
        // 再次从"应用程序"里打开 Afar.app
        let req = st.dir.join(OPEN_WINDOW_REQUEST);
        if req.exists() {
            let _ = std::fs::remove_file(&req);
            if bar.is_some() {
                open_setup(&mut st);
            }
        }
        if bar.is_some() {
            objc2::rc::autoreleasepool(|_| handle_setup(&mut st));
        }
        if act == menubar::ACT_TOGGLE {
            let showing = st.status.lock().unwrap().state == "showing";
            let cmd = if showing {
                Some(Command::Disconnect)
            } else {
                let devs = sidecar::devices().unwrap_or_default();
                let wanted = if !cfg.ipad_name.is_empty() {
                    cfg.ipad_name.clone()
                } else {
                    st.saved.last_ipad.clone().unwrap_or_default()
                };
                let target = pick(&devs, &wanted).or(devs.first());
                if target.is_none() {
                    // 只记日志，不发任何指令（发"断开"会触发 30 秒冷却）
                    warn!("附近没有可用的 iPad（iPad 要解锁、和 Mac 用同一个 Apple 账户、打开蓝牙和 Wi-Fi）");
                }
                target.map(|d| Command::Connect { sidecar_name: d.name.clone(), keyboard: false })
            };
            if let Some(cmd) = cmd {
                run_command(cmd, &mut st);
            }
        }
        // 命令行 `afar-agent disconnect` 留的请求：当成从 Afar 断开（不再自动连回）
        let req = st.dir.join(DISCONNECT_REQUEST);
        if req.exists() {
            let _ = std::fs::remove_file(&req);
            run_command(Command::Disconnect, &mut st);
        }
        objc2::rc::autoreleasepool(|_| handle_commands(&rx, &mut st));
        // 在 App 里配对成功的外设：直接选定为蓝牙键盘
        if let Some(k) = bt_pair::take_paired() {
            keyboard_command(Kb::Set(Some(k)), &mut st);
        }
        if let Some(b) = bar.as_mut() {
            let todo = setup_view(&st).todo();
            let s = st.status.lock().unwrap().clone();
            objc2::rc::autoreleasepool(|_| {
                b.update(&menubar::View {
                    name: s.name,
                    state: s.state,
                    showing: s.showing,
                    todo,
                    // 只在真的会自动连回时显示"已暂停"（关了自动连接、刚插显示器时不显示）
                    paused: st.keep.paused() && cfg.auto_connect && !plug_held(&st),
                })
            });
        }
        // 每秒：认显示器、按需建 / 关虚拟屏、纠正镜像（插回显示器时系统可能套用旧的镜像排列）
        if manage_virtual_display(&mut st) {
            vd_created = Instant::now();
        } else if vd_created.elapsed() >= Duration::from_secs(1) {
            fix_mirrors(&mut st);
        }
        if last_tick.elapsed() < TICK {
            continue;
        }
        last_tick = Instant::now();
        // 虚拟屏被系统移除了：退出，让 launchd 重新拉起并重建。（刚建好的几秒内系统可能还没挂上来，不查）
        if vd_created.elapsed() > TICK && st.vd.is_some() && !display::online().contains(&st.vid) {
            error!("虚拟屏 {} 不见了，退出重启", st.vid);
            return 1;
        }
        // 每轮的临时对象及时释放，常驻不涨内存。
        objc2::rc::autoreleasepool(|_| tick(&cfg, &mut st));
    }
}

const VD_NAME: &str = "Afar 虚拟屏";
/// 命令行断开时在配置目录留的请求文件（命令行是另一个进程，直接断开的话 Afar 会当成意外断开、自动连回）。
pub const DISCONNECT_REQUEST: &str = "disconnect.request";

/// 虚拟屏现在实际是哪一档（按当前模式的"点"尺寸对照）。
fn actual_preset(st: &State) -> Option<scale::Preset> {
    let m = display::current_mode(st.vid)?;
    if m.pw != m.w * 2 || m.ph != m.h * 2 {
        return None; // 不是 HiDPI 模式，不算任何一档
    }
    st.presets.iter().copied().find(|p| p.w as usize == m.w && p.h as usize == m.h)
}

fn pick<'a>(devs: &'a [sidecar::Device], name: &str) -> Option<&'a sidecar::Device> {
    if name.is_empty() {
        devs.first()
    } else {
        devs.iter().find(|d| d.name == name)
    }
}

fn display_name(st: &State) -> String {
    st.saved.name.clone().unwrap_or_else(|| st.sys.lock().unwrap().computer_name.clone())
}

fn register_bonjour(st: &mut State) {
    st.bonjour = None; // 先注销旧的
    match dnssd::register(&display_name(st), st.port) {
        Ok(r) => st.bonjour = Some(r),
        Err(e) => warn!("{e}（iPad App 将无法自动发现这台 Mac，可手动输入地址）"),
    }
}

/// 把当前情况写进共享状态，iPad App 查询时直接返回。
fn update_status(st: &mut State, available: &[sidecar::Device], connected: &[sidecar::Device]) {
    if st.connecting_until.is_some_and(|t| Instant::now() >= t) || !connected.is_empty() {
        st.connecting_until = None;
    }
    let state = if !connected.is_empty() {
        "showing"
    } else if st.connecting_until.is_some() || st.front.is_some() {
        // 回到桌面断开再连回的那几秒：报"连接中"，不让别的 iPad 以为可以连
        "connecting"
    } else {
        "online"
    };
    // 每把锁各取一次快照，再构造 Status。不要在下面的结构体里直接 lock（AFAR-011：同一语句里重复加锁会卡死）
    let sys = st.sys.lock().unwrap().clone();
    let kb = st.bt_live.view(Instant::now());
    let restart_error = st.restart_error.lock().unwrap().clone();
    let s = Status {
        name: display_name(st),
        version: env!("CARGO_PKG_VERSION").into(),
        state: state.into(),
        showing: connected.first().map(|d| d.name.clone()),
        nearby: available.iter().map(|d| d.name.clone()).collect(),
        // 报实际生效的档位，而不是请求的档位（AFAR-003）
        // 没有虚拟屏（接着显示器、iPad 没在显示）时报想要的档位：下次建虚拟屏就按它建
        scale: if st.vd.is_none() {
            st.want.key.to_string()
        } else {
            actual_preset(st).map(|p| p.key.to_string()).unwrap_or_default()
        },
        scale_error: st.scale_error.clone(),
        scale_busy: !st.scale_ok && st.requested && st.vd.is_some(),
        restart_error,
        keyboard: st.saved.keyboard.clone().map(|k| afar_proto::KeyboardInfo { connected: kb.connected, ..k }),
        keyboard_auto: st.saved.keyboard_auto,
        keyboard_pair: bt_pair::info(),
        keyboard_taking: kb.taking,
        keyboard_error: kb.error,
        keyboard_released: kb.released,
        // 没开虚拟屏（接着实体显示器）时没有缩放档位可选
        scales: if st.vd.is_none() {
            Vec::new()
        } else {
            st.presets.iter().map(|p| ScaleInfo { key: p.key.into(), label: p.label.into(), w: p.w, h: p.h }).collect()
        },
        password_saved: false, // 由控制接口查询钥匙串后填写
        user: crate::mac_backend::current_user(),
        filevault: sys.filevault,
        remote_login: sys.remote_login,
        ssh_host_keys: sys.ssh_host_keys,
        auto_login: sys.auto_login,
        auto_restart: sys.auto_restart,
        showing_me: false,    // 由控制接口按请求的 iPad 填写
        showing_other: false, // 同上
    };
    *st.status.lock().unwrap() = s;
}

/// 执行 iPad App 发来的指令（在主线程上，随航回调需要）。
fn handle_commands(rx: &Receiver<Command>, st: &mut State) {
    while let Ok(cmd) = rx.try_recv() {
        run_command(cmd, st);
    }
    let (av, con) = (sidecar::devices().unwrap_or_default(), sidecar::connected().unwrap_or_default());
    // 回到桌面：每秒看一次（tick 3 秒一次太慢，iPad 那边只等几秒）
    if st.front.is_some() {
        front_step(st, &av, &con);
    }
    update_status(st, &av, &con);
}

fn run_command(cmd: Command, st: &mut State) {
    info!("收到指令：{cmd:?}");
    match cmd {
        Command::Connect { sidecar_name, keyboard } => {
            st.front = None; // 用户换了操作：回到桌面作废（否则会把这次连接挤掉）
            st.dropping = None;
            use_ipad(st, &sidecar_name);
            let devs = sidecar::devices().unwrap_or_default();
            match devs.iter().find(|d| d.name == sidecar_name) {
                Some(d) => {
                    if let Err(e) = sidecar::connect(d) {
                        warn!("{e}");
                    }
                    st.connecting_until = Some(Instant::now() + CONNECTING_FOR);
                    st.keep.want(&sidecar_name, Instant::now());
                    st.was_available = true; // 避免自动连接再连一次
                                             // 只有 App 明确要求（用户确认过交键盘）才接管；菜单栏、自动连接都不接管。
                                             // 等这台 iPad 真的连上（主循环里看到）才动手，见 kbflow.rs
                    st.kb.on_connect(&sidecar_name, keyboard && st.saved.keyboard.is_some(), Instant::now());
                    if st.saved.last_ipad.as_deref() != Some(sidecar_name.as_str()) {
                        st.saved.last_ipad = Some(sidecar_name);
                        st.saved.save(&st.dir);
                    }
                }
                None => warn!("附近找不到 iPad「{sidecar_name}」"),
            }
        }
        Command::Disconnect => {
            st.front = None; // 回到桌面作废（否则断开后又被连回去）
            let connected = sidecar::connected().unwrap_or_default();
            for d in &connected {
                if let Err(e) = sidecar::disconnect(d) {
                    warn!("断开「{}」失败：{e}", d.name);
                }
            }
            st.connecting_until = None;
            st.kb.on_disconnect(!connected.is_empty(), Instant::now());
            st.last_connected = Some(Instant::now()); // 冷却期内不会被自动连回去
            st.keep.stop(); // 用户从 Afar 断开：不再自动连回
            st.dropping = (!connected.is_empty()).then(Instant::now);
        }
        Command::Front { sidecar_name } => {
            let now = Instant::now();
            st.dropping = None;
            let connected = sidecar::connected().unwrap_or_default();
            match connected.iter().find(|d| d.name == sidecar_name) {
                // 正显示在这台 iPad 上：断开，稍后连回（iPad 收到 Mac 发起的连接会切到随航画面）
                Some(d) => {
                    info!("回到 Mac 桌面：先断开「{sidecar_name}」再连回");
                    if let Err(e) = sidecar::disconnect(d) {
                        warn!("{e}");
                    }
                    st.kb.hold(now + FRONT_TIMEOUT);
                    st.front = Some(Front::new(&sidecar_name, now));
                }
                // 正显示在别的 iPad 上（App 那边状态过期了）：不抢
                None if !connected.is_empty() => {
                    warn!("回到 Mac 桌面：正显示在别的 iPad 上，不处理「{sidecar_name}」的请求");
                    return;
                }
                // 没在显示（刚好断了）：直接连
                None => {
                    if let Some(d) = sidecar::devices().unwrap_or_default().iter().find(|d| d.name == sidecar_name) {
                        if let Err(e) = sidecar::connect(d) {
                            warn!("{e}");
                        }
                    }
                }
            }
            st.connecting_until = Some(now + CONNECTING_FOR);
            st.keep.want(&sidecar_name, now);
        }
        Command::Restart { reopen } => {
            info!("重启 Mac（登录时{}重新打开窗口）", if reopen { "" } else { "不" });
            st.restart_error.lock().unwrap().clear();
            let err = st.restart_error.clone();
            // v0.4.7：要和苹果菜单"重新启动"一样（各程序正常退出、按勾选决定登录时重不重新打开窗口）。
            // 实测：重启助手的 shutdown -r now 直接杀进程，登录后程序自己把窗口恢复；直接给 loginwindow 发 kAERestart
            // 也不行——它是"无界面重启"，日志写 TAL Option set directly to: TALRestore，不看用户的勾选，照样恢复窗口。
            // System Events 的 restart 有 state saving preference 参数（sdef：Is the user defined state saving preference
            // followed?）：先把勾选写进 TALLogoutSavesState，再让它按这个设置重启。第一次会弹"允许控制 System Events"。
            // System Events 不通时（常见：没允许控制 System Events）：退回 loginwindow 直接重启 / 重启助手。
            // 这两条登录后都会恢复窗口；没勾"重新打开窗口"也照样退回——从 iPad 重启多半是远程救急，能重启比按勾选更重要
            // （v0.4.7 审查：原来没勾就直接报错，没允许 System Events 的 Mac 用默认选项从 iPad 重启不了，重启助手也不再兜底）。
            set_reopen_windows(reopen);
            let helper = setup::helper_installed();
            let dir = st.dir.clone();
            std::thread::spawn(move || {
                info!("交给 System Events 重启（按\"登录时{}重新打开窗口\"）", if reopen { "" } else { "不" });
                let e = match restart_with_state_pref() {
                    Ok(()) => return,
                    Err(e) => e,
                };
                if reopen {
                    warn!("System Events 重启没成功（{e}），改用 loginwindow 直接重启");
                } else {
                    warn!("System Events 重启没成功（{e}），改用 loginwindow 直接重启（登录后会重新打开窗口，和选择不同）");
                }
                if restart_via_loginwindow().is_ok() {
                    return;
                }
                if helper {
                    let req = setup::restart_request(&dir);
                    let ok = req
                        .parent()
                        .map(std::fs::create_dir_all)
                        .transpose()
                        .and_then(|_| std::fs::write(&req, b""))
                        .is_ok();
                    if ok {
                        warn!("改用重启助手");
                        std::thread::sleep(Duration::from_secs(20));
                        if !req.exists() {
                            return;
                        }
                        let _ = std::fs::remove_file(&req);
                    }
                }
                *err.lock().unwrap() = if e.contains("-1743") {
                    "没有允许 Afar 控制 System Events".into()
                } else {
                    format!("重启没成功：{e}")
                };
            });
        }
        Command::SetScale(key) => {
            st.want = scale::pick(&st.presets, &key);
            st.scale_ok = false;
            st.scale_warned = false;
            st.scale_error.clear();
            reset_scale_attempt(st);
            st.fell_back = false;
            st.auto_resize = false;
            st.follow_gave_up = None;
            st.requested = true;
            ensure_scale(st);
        }
        Command::IpadScreen { name, px } => {
            if st.saved.ipad_screens.get(&name) != Some(&px) {
                info!("iPad「{name}」屏幕 {}×{} 像素", px[0], px[1]);
                st.saved.ipad_screens.insert(name.clone(), px);
                st.saved.ipad_sizes.remove(&name); // 尺寸变了，以前校正过的作废
                st.saved.save(&st.dir);
            }
        }
        Command::Keyboard(kb) => keyboard_command(kb, st),
        Command::Rename(name) => {
            st.saved.name = Some(name);
            st.saved.save(&st.dir);
            register_bonjour(st);
        }
    }
}

/// 一次换档的尝试计数清零。
fn reset_scale_attempt(st: &mut State) {
    st.rebuilds = 0;
    st.sidecar_switched = false;
    st.set_tries = 0;
    st.follow_only = false;
}

/// 虚拟屏不是想要的档位就切过去；成功（或确定失败）后不再反复检查。
/// 依次尝试：直接切 → 先解除随航镜像再切 → 按新档位重建虚拟屏（写法 0，再不行换写法 1，见 scale::vd_modes）。
/// 虚拟屏对了以后，镜像它的随航屏也要是同一个尺寸（不对就替它切一次）。
/// iPad 换的不是"默认"、又切不成：自动回到"默认"并告诉 iPad（v0.4.0 测试：不接显示器时"更大文字"停在 1 倍低清、
/// "更多空间"随航屏和虚拟屏尺寸对不上，都一直停在那里）。启动时、随航重连后的核对只报告，不回默认、不改保存的档位。
/// 状态里永远是实际生效的档位（AFAR-003）。
fn ensure_scale(st: &mut State) {
    if st.scale_ok || st.vd.is_none() || plug_held(st) {
        return;
    }
    let w = st.want;
    if actual_preset(st).is_some_and(|p| p.key == w.key) {
        match sidecar_follow(st) {
            scale::Follow::Ok => scale_done(st),
            scale::Follow::Switch => {} // 刚替随航屏切了，下一轮核对
            scale::Follow::Missing if st.follow_only || st.auto_resize || !st.requested => {
                // 只报告：iPad 上看到原因，档位不动
                if !st.fell_back {
                    st.scale_error = format!("随航屏和虚拟屏尺寸对不上（「{}」）", w.label);
                }
                st.follow_only = false;
                st.auto_resize = false;
                st.scale_ok = true;
            }
            scale::Follow::Missing => scale_fallback(st, "随航屏没有这个尺寸的清晰模式"),
        }
        return;
    }
    // 刚重建：系统可能还读不到新屏的模式，多等几轮再下结论
    if st.rebuilds > 0 && display::current_mode(st.vid).is_none() && st.scale_wait < 3 {
        st.scale_wait += 1;
        return;
    }
    // 切换接口说成功、下一轮核对却还不对：最多给两次机会，之后当失败（否则每 3 秒切一次，一直闪）
    let e1 = if st.set_tries >= 2 {
        "切过两次又被系统改回去了".to_string()
    } else {
        match display::set_hidpi_mode(st.vid, w.w, w.h) {
            Ok(()) => {
                st.set_tries += 1;
                return; // 下一轮核对（连同随航屏）
            }
            Err(e) => e,
        }
    };
    if !st.requested && !st.auto_resize {
        // 启动时：系统可能还没准备好，下一轮再试（不重建、不改保存的档位）
        if !st.scale_warned {
            warn!("切换缩放暂未成功（会自动重试）：{e1}；当前 {:?}", display::current_mode(st.vid));
            st.scale_warned = true;
        }
        return;
    }
    // 随航屏镜像着虚拟屏时，"解除镜像再切"能切过去，但重新镜像后系统会把虚拟屏改回它创建时的档位
    // （v0.3.6 测试 15e），所以直接按新档位重建
    let mirrored = st.mirror && st.kinds.values().any(|k| *k == Kind::Sidecar);
    let e2 = match if mirrored || st.set_tries >= 2 {
        Err("随航正镜像着虚拟屏，直接重建".to_string())
    } else {
        display::set_hidpi_mode_unmirrored(st.vid, w.w, w.h)
    } {
        Ok(()) => {
            info!("缩放：直接切换失败（{e1}），解除镜像后切换成功，随航镜像会自动恢复");
            st.set_tries += 1;
            return;
        }
        Err(e) => e,
    };
    if st.rebuilds < 2 {
        let style = st.rebuilds;
        st.rebuilds += 1;
        st.scale_wait = 0;
        st.set_tries = 0;
        warn!("切换缩放失败（{e1}；解除镜像后：{e2}），按新档位重建虚拟屏（写法 {style}）");
        if style > 0 {
            log_modes(st);
        }
        match recreate_display(st, style) {
            // 下一轮核对实际档位
            Ok(()) => return,
            Err(e) => warn!("重建虚拟屏失败：{e}"),
        }
    }
    if !st.scale_warned {
        warn!("切换到「{}」没成功：{e1}；解除镜像后：{e2}；当前 {:?}", w.label, display::current_mode(st.vid));
        st.scale_warned = true;
    }
    scale_fallback(st, &e2);
}

/// 镜像虚拟屏的随航屏是不是和虚拟屏一样大；不一样就替它切一次。
fn sidecar_follow(st: &mut State) -> scale::Follow {
    let w = st.want;
    let followers: Vec<u32> = st
        .kinds
        .iter()
        .filter(|(d, k)| **k == Kind::Sidecar && display::mirrors(**d) == st.vid)
        .map(|(d, _)| *d)
        .collect();
    for d in followers {
        let cur = display::current_mode(d).map(|m| (m.w, m.h, m.pw, m.ph));
        let target = display::modes(d)
            .into_iter()
            .find(|m| m.w == w.w as usize && m.h == w.h as usize && m.pw == m.w * 2 && m.ph == m.h * 2 && m.usable);
        match scale::follow(cur, w, target.is_some()) {
            scale::Follow::Ok => continue,
            scale::Follow::Switch if !st.sidecar_switched => {
                st.sidecar_switched = true;
                info!("随航屏 {d} 是 {cur:?}，和虚拟屏「{}」不一样大，切过去", w.label);
                // 用显示配置的方式切（随航屏在镜像组里，直接切可能返回 1001）；不行再直接切
                let r = target
                    .map(|m| display::set_mode(d, m.key()))
                    .unwrap_or_else(|| Err("没有这个模式".into()))
                    .or_else(|e| {
                        warn!("随航屏 {d} 按显示配置切换失败：{e}，改用直接切换");
                        display::set_hidpi_mode(d, w.w, w.h)
                    });
                return match r {
                    Ok(()) => scale::Follow::Switch,
                    Err(e) => {
                        warn!("随航屏 {d} 切到 {}×{} 失败：{e}", w.w, w.h);
                        log_modes(st);
                        st.follow_gave_up = Some(d);
                        scale::Follow::Missing
                    }
                };
            }
            _ => {
                warn!("随航屏 {d} 还是 {cur:?}，和虚拟屏「{}」不一样大", w.label);
                log_modes(st);
                st.follow_gave_up = Some(d);
                return scale::Follow::Missing;
            }
        }
    }
    scale::Follow::Ok
}

/// 记一次虚拟屏和随航屏能用的清晰模式（换档失败时才记，给下一轮排查用）。
fn log_modes(st: &State) {
    let ids: Vec<u32> =
        std::iter::once(st.vid).chain(st.kinds.iter().filter(|(_, k)| **k == Kind::Sidecar).map(|(d, _)| *d)).collect();
    for d in ids {
        let list: Vec<String> = display::modes(d)
            .iter()
            .filter(|m| m.pw == m.w * 2)
            .map(|m| format!("{}×{}{}", m.w, m.h, if m.usable { "" } else { "(不可用)" }))
            .collect();
        info!("显示器 {d} 当前 {:?}，清晰模式：{}", display::current_mode(d), list.join(" "));
    }
}

/// iPad 请求的换档确定失败。换的不是"默认"：回到"默认"（实测一直可用）并告诉 iPad；
/// 换的就是"默认"（或回到默认后还不行）：退回实际档位并报原因。
fn scale_fallback(st: &mut State, why: &str) {
    let w = st.want;
    if st.auto_resize {
        // Afar 自己发起的：只报告，档位和保存的都不动
        warn!("按这台 iPad 调成「{}」（{}×{}）没成功：{why}", w.label, w.w, w.h);
        if !st.fell_back {
            st.scale_error = format!("没能按这台 iPad 调成「{}」（{why}）", w.label);
        }
        st.auto_resize = false;
        st.scale_ok = true;
        return;
    }
    let default = scale::pick(&st.presets, "default");
    if w.key != default.key && !st.fell_back {
        warn!("「{}」用不了（{why}），回到「默认」", w.label);
        st.scale_error = format!("这台 iPad 用不了「{}」，已回到「默认」", w.label);
        st.fell_back = true;
        st.want = default;
        st.saved.scale = Some(default.key.into());
        st.saved.save(&st.dir);
        reset_scale_attempt(st);
        st.scale_warned = false;
        return; // 下一轮按"默认"走一遍（必要时重建）
    }
    let actual = actual_preset(st);
    if !st.fell_back {
        st.scale_error = match actual {
            Some(a) if a.key == w.key => format!("随航屏和虚拟屏尺寸对不上（「{}」，{why}）", w.label),
            _ => {
                format!("没能切到「{}」（{why}），现在是「{}」", w.label, actual.map(|a| a.label).unwrap_or("未知档位"))
            }
        };
    }
    if let Some(a) = actual {
        st.want = a;
        st.saved.scale = Some(a.key.into());
        st.saved.save(&st.dir);
    }
    st.scale_ok = true;
}

fn scale_done(st: &mut State) {
    let w = st.want;
    info!("缩放：{}（{}×{}）", w.label, w.w, w.h);
    st.scale_ok = true;
    st.follow_only = false;
    st.auto_resize = false;
    if !st.fell_back {
        st.scale_error.clear(); // 回到"默认"的提示留着，直到下次换档
    }
    if st.saved.scale.as_deref() != Some(w.key) {
        st.saved.scale = Some(w.key.into());
        st.saved.save(&st.dir);
    }
}

/// 按 `st.want` 重建虚拟屏（想要的档位排第一，镜像时系统会用这个档位）。
/// 先建新的、让随航屏镜像过去，再关旧的：中间不会只剩随航屏（只剩随航屏时系统会把随航断开，v0.3.6 测试）。
/// 新旧两块用不同的序列号，系统才当成两块屏。创建失败时旧屏保留。
fn recreate_display(st: &mut State, style: u8) -> Result<(), String> {
    st.saved.scale = Some(st.want.key.into());
    st.saved.save(&st.dir);
    let serial = if st.vd_serial == 1 { 2 } else { 1 };
    let vd = virtual_display::create(
        &scale::vd_modes(&st.presets, st.want, style),
        scale::vd_max(&st.presets),
        VD_NAME,
        serial,
    )?;
    let id = vd.id;
    // 等系统把新屏挂上来（最多 3 秒）
    for _ in 0..6 {
        if display::online().contains(&id) {
            break;
        }
        runloop::run_for(0.5);
    }
    if st.mirror {
        let sidecars: Vec<u32> = st.kinds.iter().filter(|(_, k)| **k == Kind::Sidecar).map(|(d, _)| *d).collect();
        for d in sidecars {
            if let Err(e) = display::mirror(d, id) {
                warn!("随航屏 {d} 镜像新虚拟屏失败（会自动重试）：{e}");
            }
        }
    }
    info!("虚拟屏已重建，显示器 ID {}（旧的 {} 关掉）", id, st.vid);
    st.vid = id;
    st.vd = Some(vd); // 旧的在这里释放、移除
    st.vd_serial = serial;
    runloop::run_for(1.0);
    Ok(())
}

/// iPad App 发来的蓝牙键盘指令。
fn keyboard_command(kb: Kb, st: &mut State) {
    let addr = st.saved.keyboard.as_ref().map(|k| k.address.clone());
    match kb {
        Kb::Set(k) => {
            // 换键盘 / 不用了：先把 Afar 接管着的那块还回去
            let act = st.kb.release();
            kb_apply(st, act, addr.clone());
            if k.is_none() {
                st.saved.keyboard_auto = false;
            }
            let _ = st.bt_tx.send(bt::Req::Watch(k.as_ref().map(|k| k.address.clone())));
            st.saved.keyboard = k;
            st.saved.save(&st.dir);
        }
        Kb::Auto(v) => {
            st.saved.keyboard_auto = v;
            st.saved.save(&st.dir);
            if !v {
                let act = st.kb.release();
                kb_apply(st, act, addr);
            }
        }
        Kb::Take => {
            if addr.is_none() {
                warn!("还没选蓝牙键盘");
                return;
            }
            let on_mac = st.bt_live.view(Instant::now()).connected;
            let act = st.kb.take(on_mac);
            kb_apply(st, act, addr);
        }
        Kb::Release => {
            let act = st.kb.release();
            kb_apply(st, act, addr);
        }
        Kb::ReleaseNow => {
            let act = st.kb.release_now();
            kb_apply(st, Some(act), addr);
        }
        Kb::OpenSettings => {
            let _ = std::process::Command::new("/usr/bin/open")
                .arg("x-apple.systempreferences:com.apple.BluetoothSettings")
                .spawn();
        }
        Kb::PairSearch => {
            if bt::denied() {
                bt_pair::fail("Afar 没有蓝牙权限：请在 Mac 的 系统设置 › 隐私与安全 › 蓝牙 里允许 afar-agent");
                return;
            }
            // 配对时停掉正在进行的接管（同时操作蓝牙会互相干扰）
            if st.bt_live.view(Instant::now()).taking {
                let _ = st.kb.release();
                kb_apply(st, Some(Act::ClearReleased), addr);
            }
            if let Err(e) = bt_pair::search() {
                warn!("{e}");
            }
        }
        Kb::Pair(a) => {
            if let Err(e) = bt_pair::pair(&a) {
                warn!("{e}");
            }
        }
        Kb::PairCancel => bt_pair::cancel(),
    }
}

/// 执行 kbflow 的决定，并把"是不是 Afar 接管的"存盘（接管期间程序重启后，断开随航时照样松开）。
fn kb_apply(st: &mut State, act: Option<Act>, addr: Option<String>) {
    if let (Some(act), Some(a)) = (act, addr) {
        let req = match act {
            Act::Take => bt::Req::Take(a, KEYBOARD_TAKE_FOR),
            Act::Release => bt::Req::Release(a),
            // 重新只看状态：停掉进行中的接管，清掉"已松开"的记录
            Act::ClearReleased => bt::Req::Watch(Some(a)),
        };
        let _ = st.bt_tx.send(req);
    }
    if st.saved.keyboard_taken != st.kb.taken {
        st.saved.keyboard_taken = st.kb.taken;
        st.saved.save(&st.dir);
    }
}

/// 每轮：按随航连着谁、键盘在不在 Mac 上，决定接管 / 松开（规则和测试在 kbflow.rs）。
fn keyboard_follow_sidecar(st: &mut State, connected: &[sidecar::Device]) {
    let names: Vec<String> = connected.iter().map(|d| d.name.clone()).collect();
    let on_mac = st.bt_live.view(Instant::now()).connected;
    let act = st.kb.on_tick(&names, on_mac, Instant::now());
    let addr = st.saved.keyboard.as_ref().map(|k| k.address.clone());
    kb_apply(st, act, addr);
}

fn tick(cfg: &config::Config, st: &mut State) {
    if !plug_held(st) {
        adopt_sidecar_sizes(st);
        follow_check(st);
        ensure_scale(st);
    }
    let available = match sidecar::devices() {
        Ok(d) => d,
        Err(e) => {
            warn!("{e}");
            return;
        }
    };
    let connected = sidecar::connected().unwrap_or_default();
    update_status(st, &available, &connected);
    keyboard_follow_sidecar(st, &connected);
    note_drop(st, &connected, cfg.auto_connect);

    // 刚从 Afar 断开、断开还没生效：不算"又连上了"（否则会重新开始保持显示，断开生效后又自动连回去）
    if let Some(t) = st.dropping {
        if connected.is_empty() || t.elapsed() >= DROP_GRACE {
            st.dropping = None;
        } else {
            return;
        }
    }
    if let Some(d) = connected.first() {
        st.was_available = true;
        st.last_connected = Some(Instant::now());
        // 连稳 2 分钟才清零重试计数：无线不稳时会"连上 20～40 秒又断"，每次失败 / 断开系统都弹框（v0.4.9）
        let since = *st.connected_since.get_or_insert_with(Instant::now);
        if since.elapsed() >= reconnect::STABLE {
            st.keep.shown(&d.name);
        } else {
            st.keep.unstable(&d.name, Instant::now());
        }
        return;
    }
    st.connected_since = None;
    // 刚插上实体显示器：自动连回随航也等系统读完它再做（用户自己点连接不拦）
    if !cfg.auto_connect || plug_held(st) {
        return;
    }
    // 保持显示：不是从 Afar 断开的（锁屏、信号不好、开机超时、侧边栏断开）都连回去
    if let Some(name) = st.keep.target.clone() {
        let dev = available.iter().find(|d| d.name == name);
        let was_paused = st.keep.paused();
        st.keep.presence(dev.is_some(), Instant::now());
        if was_paused && !st.keep.paused() {
            info!("iPad「{name}」重新出现在附近，恢复自动连回");
        }
        if st.keep.due(dev.is_some(), Instant::now()) {
            st.keep.tried(Instant::now());
            info!("随航没在显示，自动连回「{name}」（第 {}/{} 次）", st.keep.tries, reconnect::MAX_TRIES);
            if let Some(dev) = dev {
                if let Err(e) = sidecar::connect(dev) {
                    warn!("{e}");
                }
            }
            if st.keep.paused() {
                warn!(
                    "这是第 {} 次自动连回：这次再连不上、或 2 分钟内又断，就暂停自动连回，\
                     到时在 iPad 上点卡片或菜单栏点「立即连接 iPad」恢复（无线信号不好？）",
                    reconnect::MAX_TRIES
                );
            }
        }
        st.was_available = true;
        return;
    }

    // 从没连过、或用户从 Afar 断开后：iPad 新出现时自动连一次（之后由"保持显示"负责重试）
    // 只连：配置里指定的 iPad → 上次用 iPad App 连过的 → 都没有就第一台
    let wanted =
        if !cfg.ipad_name.is_empty() { cfg.ipad_name.clone() } else { st.saved.last_ipad.clone().unwrap_or_default() };
    let target = pick(&available, &wanted);
    let now_available = target.is_some();
    // 刚断开不久：即使 iPad 刚"出现"，也当作一直在（不自动连回去）。
    if st.last_connected.is_some_and(|t| t.elapsed() < COOLDOWN) {
        st.was_available = true;
    }
    if let Some(dev) = target.filter(|_| !st.was_available) {
        info!("iPad「{}」可用，自动连接", dev.name);
        if let Err(e) = sidecar::connect(dev) {
            warn!("{e}");
        }
        st.keep.want(&dev.name, Instant::now());
    }
    st.was_available = now_available;
}

/// 重新镜像后系统可能把虚拟屏的模式改掉：如实报告，并以实际档位为准（不反复解除镜像重试）。
fn check_scale_after_mirror(st: &mut State) {
    if !st.scale_ok {
        return;
    }
    let Some(a) = actual_preset(st) else { return };
    if a.key != st.want.key {
        warn!("重新镜像后虚拟屏变成了「{}」（想要「{}」）", a.label, st.want.label);
        st.scale_error = format!("重新镜像后系统把显示大小改回了「{}」", a.label);
        st.want = a;
        st.saved.scale = Some(a.key.into());
        st.saved.save(&st.dir);
    }
    // 随航屏的尺寸由 follow_check 每轮核对（刚镜像完可能还是过渡状态，这里不读）
}

/// 每轮看一次：镜像虚拟屏的随航屏是不是和虚拟屏一样大（随航重连、系统自己恢复镜像后可能不一样）。
/// 不一样就只核对随航屏（follow_only）：替它切一次，不行只报告。同一块随航屏对不上过就不再试。
fn follow_check(st: &mut State) {
    if !st.scale_ok || st.vd.is_none() {
        return;
    }
    let w = st.want;
    let bad = st.kinds.iter().find(|(d, k)| {
        **k == Kind::Sidecar
            && display::mirrors(**d) == st.vid
            && display::current_mode(**d).is_some_and(|m| m.w != w.w as usize || m.h != w.h as usize || m.pw != m.w * 2)
    });
    if let Some((&d, _)) = bad {
        if st.follow_gave_up == Some(d) {
            return;
        }
        info!("随航屏 {d} 和虚拟屏「{}」不一样大，核对一次", w.label);
        st.scale_ok = false;
        reset_scale_attempt(st);
        st.follow_only = true;
    }
}

/// 某台 iPad 的三档：（按屏幕算的，开机 / 连接时用的）。屏幕没报过用配置里的；按随航屏校正过的直接用。
fn ipad_presets(saved: &Saved, name: Option<&str>, cfg_px: [u32; 2]) -> (Vec<scale::Preset>, Vec<scale::Preset>) {
    let px = name
        .and_then(|n| saved.ipad_screens.get(n))
        .copied()
        .and_then(afar_proto::normalize_screen) // state.json 可能被改坏：坏值会让没接显示器的 Mac 建不出虚拟屏
        .unwrap_or(cfg_px);
    let base = scale::presets(px);
    let presets = match name.and_then(|n| saved.ipad_sizes.get(n)) {
        Some(sizes) => scale::with_sizes(&base, sizes),
        None => base.clone(),
    };
    (base, presets)
}

/// 要连这台 iPad 了（App、菜单栏、自动连接都会走到这里）：三档换成它的。
fn use_ipad(st: &mut State, name: &str) {
    let (base, presets) = ipad_presets(&st.saved, Some(name), st.cfg_ipad_pixels);
    if base == st.base_presets && presets == st.presets {
        return;
    }
    info!("按 iPad「{name}」的屏幕算三档：{}", show_presets(&presets));
    st.base_presets = base;
    st.presets_from = None;
    apply_presets(st, presets);
}

/// 换三档；用着的档位尺寸变了就重新切一次（auto_resize：可以重建虚拟屏，切不成只报告）。
fn apply_presets(st: &mut State, presets: Vec<scale::Preset>) {
    let old = st.want;
    st.presets = presets;
    st.want = scale::pick(&st.presets, old.key);
    if (st.want.w, st.want.h) != (old.w, old.h) && st.vd.is_some() {
        st.scale_ok = false;
        reset_scale_attempt(st);
        st.auto_resize = true;
    }
}

fn show_presets(v: &[scale::Preset]) -> String {
    v.iter().map(|p| format!("{} {}×{}", p.label, p.w, p.h)).collect::<Vec<_>>().join("、")
}

/// 随航屏出现时，三档改用它自己支持的清晰尺寸（scale::from_sidecar），并把它的清晰模式记一次日志。
fn adopt_sidecar_sizes(st: &mut State) {
    let Some(sid) = st.kinds.iter().find(|(_, k)| **k == Kind::Sidecar).map(|(d, _)| *d) else { return };
    if st.presets_from == Some(sid) {
        return;
    }
    let modes: Vec<(usize, usize, usize, usize, bool)> =
        display::modes(sid).iter().map(|m| (m.w, m.h, m.pw, m.ph, m.usable)).collect();
    if modes.is_empty() {
        return; // 刚出现，系统还读不到模式：下一轮再看
    }
    st.presets_from = Some(sid);
    let hidpi: Vec<String> =
        modes.iter().filter(|m| m.2 == m.0 * 2 && m.4).map(|m| format!("{}×{}", m.0, m.1)).collect();
    info!("随航屏 {sid} 的清晰模式：{}", hidpi.join(" "));
    let new = scale::from_sidecar(&st.base_presets, &modes);
    if new == st.presets {
        return;
    }
    info!("三档改用随航屏支持的尺寸：{}（原来 {}）", show_presets(&new), show_presets(&st.presets));
    // 按 iPad 记下来：下次开机直接用，不用再校正一次
    if let Some(name) = st.keep.target.clone().or_else(|| st.saved.last_ipad.clone()) {
        st.saved.ipad_sizes.insert(name, scale::sizes_of(&new));
        st.saved.save(&st.dir);
    }
    apply_presets(st, new);
}

/// 认一遍在线显示器（按厂商号 / 型号，见 screens.rs），认法变了才记日志。返回（实体显示器，有没有随航屏）。
fn classify(kinds: &mut HashMap<u32, Kind>, vid: u32) -> (HashSet<u32>, bool) {
    let online = display::online();
    kinds.retain(|d, _| online.contains(d));
    let (mut physical, mut showing) = (HashSet::new(), false);
    for d in online {
        let (vendor, model, _, _) = display::ident(d);
        let k = screens::kind(d == vid && vid != 0, vendor, model);
        if kinds.insert(d, k) != Some(k) {
            info!("显示器 {d}：{}（厂商 {vendor:#x} 型号 {model:#x}）", k.label());
        }
        match k {
            Kind::Physical => {
                physical.insert(d);
            }
            Kind::Sidecar => showing = true,
            _ => {}
        }
    }
    (physical, showing)
}

fn vd_reason(has_physical: bool) -> &'static str {
    if has_physical {
        "iPad 正在显示，先备好虚拟屏，拔显示器时 iPad 不断"
    } else {
        "没有实体显示器"
    }
}

/// 按需建 / 关虚拟屏（规则见 screens::want_vd）。每秒调用。
/// 返回 true = 这一轮刚建了虚拟屏（系统可能还没挂上来，这一轮别检查它、别镜像）。
fn manage_virtual_display(st: &mut State) -> bool {
    let (physical, showing) = classify(&mut st.kinds, st.vid);
    let mut plugged: Vec<u32> = physical.difference(&st.physical).copied().collect();
    if !plugged.is_empty() {
        plugged.sort();
        info!("实体显示器 {plugged:?} 刚插上：{} 秒内先不调整任何显示器，等系统把它读完", PLUG_HOLD.as_secs());
        st.plug_hold = Some(Instant::now() + PLUG_HOLD);
    }
    restore_physical_modes(st, &physical);
    st.physical = physical;
    let has_physical = !st.physical.is_empty();
    // 接着实体显示器时，随航屏稳定 2 秒才建虚拟屏：锁屏期间自动连回可能"连上一下又断"，
    // 每次都建、关虚拟屏会让实体显示器跟着重配、闪（没有实体显示器时随航屏一出现就要虚拟屏）
    let since = if showing { *st.sidecar_since.get_or_insert_with(Instant::now) } else { Instant::now() };
    if !showing {
        st.sidecar_since = None;
    }
    let stable = showing && since.elapsed() >= Duration::from_secs(2);
    // 刚插上的显示器又没了（接触不良、待机断开）：不再等，没有实体显示器时虚拟屏要马上有
    if !has_physical {
        st.plug_hold = None;
    }
    if plug_held(st) {
        return false; // 建 / 关虚拟屏也等一等（随航屏是否稳定照常在上面计时）
    }
    // "回到 Mac 桌面"断开再连回的那几秒也算在显示：不关虚拟屏（关了窗口会搬家）
    let want =
        screens::want_vd(has_physical, (if has_physical { stable } else { showing }) || st.front.is_some(), st.mirror);
    match st.vd_plan.step(want, st.vd.is_some(), Instant::now()) {
        Some(VdAct::Destroy) => {
            // 先让实体显示器离开镜像，关屏后它直接回到自己的分辨率
            fix_mirrors(st);
            info!("接着实体显示器 {:?}、iPad 没在显示，关掉虚拟屏", st.physical);
            st.vd = None; // 释放即移除
            st.vid = 0;
        }
        Some(VdAct::Create) if st.display_retry_at.is_some_and(|t| Instant::now() < t) => {}
        Some(VdAct::Create) => {
            match virtual_display::create(
                &scale::vd_modes(&st.presets, st.want, 0),
                scale::vd_max(&st.presets),
                VD_NAME,
                st.vd_serial,
            ) {
                Ok(v) => {
                    info!("虚拟屏已创建，显示器 ID {}（{}）", v.id, vd_reason(has_physical));
                    st.vid = v.id;
                    st.vd = Some(v);
                    st.scale_ok = false;
                    // 新屏按想要的档位建的：不是 App 在换档，系统没挂好时只重试，不重建、不报"没能切到"
                    st.requested = false;
                    st.auto_resize = false;
                    reset_scale_attempt(st);
                    st.scale_warned = false;
                    st.vd_warned = false;
                    st.display_retry_at = None;
                    return true;
                }
                Err(e) => {
                    if !st.vd_warned {
                        error!("创建虚拟屏失败（10 秒后重试）：{e}");
                        st.vd_warned = true;
                    }
                    st.display_retry_at = Some(Instant::now() + DISPLAY_RETRY);
                }
            }
        }
        None => {}
    }
    false
}

/// 纠正镜像，只有一种正确排列：随航屏镜像虚拟屏；实体显示器不镜像虚拟屏 / 随航屏；iPad 在显示时虚拟屏本身不镜像别人。
/// 插回显示器时系统会套用以前记住的排列（实测把实体显示器并进镜像、分辨率变成 1180×820），这里马上拆开。
/// 改失败后 10 秒内不再试（每次都是一次显示配置，频繁做会闪屏），同类警告只记一次。
fn fix_mirrors(st: &mut State) {
    let vid = st.vid;
    if vid == 0 || st.display_retry_at.is_some_and(|t| Instant::now() < t) || plug_held(st) {
        return;
    }
    let showing = st.kinds.values().any(|k| *k == Kind::Sidecar);
    let mut todo: Vec<(u32, u32, String)> = Vec::new(); // （显示器，镜像目标，成功后的日志）
                                                        // iPad 没在显示时虚拟屏镜像着实体显示器无害（马上就要关了），拆开反而多出一块看不见的屏
    if showing && display::mirrors(vid) != 0 {
        todo.push((vid, 0, "虚拟屏被系统并进了别的显示器的镜像，已解除".into()));
    }
    let mut ids: Vec<(u32, Kind)> = st.kinds.iter().map(|(d, k)| (*d, *k)).collect();
    ids.sort_by_key(|(d, _)| *d);
    for (d, k) in ids {
        match k {
            // 两块实体显示器之间的镜像不管：可能是用户自己设的
            Kind::Physical if mirrors_ours_or_sidecar(&st.kinds, display::mirrors(d)) => {
                todo.push((d, 0, format!("显示器 {d} 是实体显示器，已解除镜像")))
            }
            Kind::Sidecar if st.mirror && display::mirrors(d) != vid => {
                todo.push((d, vid, format!("随航屏 {d} 已镜像虚拟屏")))
            }
            _ => {}
        }
    }
    for (d, master, ok) in todo {
        match display::mirror(d, master) {
            Ok(()) => {
                info!("{ok}");
                st.mirror_warned = false;
                if master != 0 {
                    check_scale_after_mirror(st);
                }
            }
            Err(e) => {
                if !st.mirror_warned {
                    warn!("调整镜像失败（10 秒后重试）：显示器 {d}：{e}");
                    st.mirror_warned = true;
                }
                st.display_retry_at = Some(Instant::now() + DISPLAY_RETRY);
                return;
            }
        }
    }
}

fn mirrors_ours_or_sidecar(kinds: &HashMap<u32, Kind>, master: u32) -> bool {
    master != 0 && matches!(kinds.get(&master), Some(Kind::Ours | Kind::Sidecar))
}

/// 实体显示器刚插上的这几秒，Afar 不做任何显示配置（换档、核对随航屏、改镜像、建关虚拟屏、恢复分辨率）。
/// v0.4.4 测试：Afar 运行时插回显示器 2/2 掉到 800×600，系统给它的模式只有 800×600 和 960×540 HiDPI，
/// 重试 8 次都一样；Afar 没运行时 2/2 正常。插回的同一秒 Afar 正在替随航屏切模式（系统把随航屏改成了 800×556），
/// 怀疑是这次显示配置打断了系统读新显示器 → 先等系统读完。
/// v0.4.6 测试复查：WindowServer 日志里插上约 0.3 秒就读完模式表、定下分辨率（800×600 是系统核对记住的配置失败后套的默认配置）；
/// 改不回来是公开接口列不出 1920×1080（见 display::set_mode），和等多久无关 → 等待缩短到 1 秒。
const PLUG_HOLD: Duration = Duration::from_secs(1);

fn plug_held(st: &State) -> bool {
    st.plug_hold.is_some_and(|t| Instant::now() < t)
}

/// 实体显示器出现后这段时间内，分辨率和上次不一样就改回上次的；过了这段时间还不一样，算用户自己改的，记下来。
/// v0.4.3 测试：插回后头几秒系统给的模式列表不全（只有 800×600 和 960×540 HiDPI，没有 1920×1080），
/// 原来 2 秒一次试 3 次就放弃了 → 插回后先等 PLUG_HOLD（1 秒）改第 1 次，之后每 2 秒一次，40 秒内最多试 5 次。
/// v0.4.6 复查：真正原因是公开接口看不到 1920×1080，set_mode 已改为找不到时用系统模式表。
const RESTORE_WITHIN: Duration = Duration::from_secs(40);
const RESTORE_AFTER: Duration = PLUG_HOLD;
const RESTORE_EVERY: Duration = Duration::from_secs(2);
const RESTORE_TRIES: u32 = 5;

/// 插回实体显示器时系统有时选错分辨率（v0.3.6 测试：插回后变成 800×600，45 秒没自己恢复）。
/// 按显示器编号记住每块实体显示器上次用的模式（含刷新率），插回后改回去（插回 1 秒后改第 1 次，之后每 2 秒一次，最多 5 次）。
/// - 镜像着别的屏时分辨率由镜像决定：不看，计时从拆开后重新算；
/// - 改了 5 次还是不对：这个错的模式不当成"上次用的"存下来，等用户自己再改了才存；
/// - 两块显示器编号一样（便宜显示器序列号常是 0）：分不清谁是谁，都不恢复、不存。
fn restore_physical_modes(st: &mut State, physical: &HashSet<u32>) {
    let now = Instant::now();
    let held = plug_held(st); // 又插上了一块：已有显示器的恢复也等它读完
    st.appeared.retain(|d, _| physical.contains(d));
    let mut ids: Vec<(u32, String)> = physical
        .iter()
        .map(|d| {
            let (vendor, model, serial, _) = display::ident(*d);
            (*d, format!("{vendor:x}-{model:x}-{serial:x}"))
        })
        .collect();
    ids.sort();
    for (d, key) in &ids {
        if ids.iter().filter(|(_, k)| k == key).count() > 1 {
            continue;
        }
        let a =
            st.appeared.entry(*d).or_insert(Appear { since: now, tries: 0, last: None, stuck: None, listed: false });
        if display::mirrors(*d) != 0 {
            a.since = now;
            continue;
        }
        let Some(cur) = display::current_mode(*d).map(|m| m.key()) else { continue };
        let saved = st.saved.display_modes.get(key).copied();
        // 插上等完之后记一次系统给了哪些模式（对照插回后掉到 800×600 的问题用）
        if !a.listed && now.saturating_duration_since(a.since) >= RESTORE_AFTER {
            a.listed = true;
            info!(
                "显示器 {d} 插上 {} 秒后是 {}×{}；现有模式 {}；公开接口列不出的 {}",
                RESTORE_AFTER.as_secs(),
                cur.0,
                cur.1,
                mode_list(*d),
                display::hidden_modes(*d)
            );
        }
        if now.saturating_duration_since(a.since) < RESTORE_WITHIN {
            let Some(m) = saved.filter(|m| *m != cur) else { continue };
            if held
                || now.saturating_duration_since(a.since) < RESTORE_AFTER
                || a.tries >= RESTORE_TRIES
                || a.last.is_some_and(|t| now.saturating_duration_since(t) < RESTORE_EVERY)
            {
                continue;
            }
            a.tries += 1;
            a.last = Some(now);
            a.stuck = Some(cur);
            match display::set_mode(*d, m) {
                Ok(()) => info!("显示器 {d} 插回后是 {}×{}，已改回上次的 {}×{}", cur.0, cur.1, m.0, m.1),
                Err(e) => {
                    // 记下系统这时给了哪些模式（排查插回后掉到 800×600 用）
                    warn!(
                        "显示器 {d} 改回 {}×{} 失败（第 {} 次，镜像 {}）：{e}；现有模式 {}",
                        m.0,
                        m.1,
                        a.tries,
                        display::mirrors(*d),
                        mode_list(*d)
                    );
                }
            }
        } else if saved != Some(cur) && a.stuck != Some(cur) {
            a.stuck = None;
            st.saved.display_modes.insert(key.clone(), cur);
            st.saved.save(&st.dir);
        }
    }
}

/// 显示器现在能选的模式（日志用）："800×600 960×540@2x"。
fn mode_list(d: u32) -> String {
    let list: Vec<String> = display::modes(d)
        .iter()
        .filter(|x| x.usable)
        .map(|x| format!("{}×{}{}", x.w, x.h, if x.pw == x.w * 2 { "@2x" } else { "" }))
        .collect();
    list.join(" ")
}

/// 实体显示器这次出现以来的情况（见 restore_physical_modes）。
struct Appear {
    since: Instant,
    tries: u32,
    last: Option<Instant>,
    /// 恢复前（或恢复不掉）的那个模式：不当成用户选的存下来。
    stuck: Option<display::ModeKey>,
    /// 插上等完后的模式列表已经记过日志。
    listed: bool,
}

/// "回到 Mac 桌面"每秒一步（规则见 reconnect::Front）。
fn front_step(st: &mut State, available: &[sidecar::Device], connected: &[sidecar::Device]) {
    let Some(f) = st.front.as_mut() else { return };
    let now = Instant::now();
    let name = f.name.clone();
    let dev = available.iter().find(|d| d.name == name);
    match f.step(now, connected.iter().any(|d| d.name == name), dev.is_some()) {
        FrontAct::Wait => {}
        FrontAct::Connect => {
            info!("回到 Mac 桌面：连回「{name}」");
            if let Some(d) = dev {
                if let Err(e) = sidecar::connect(d) {
                    warn!("{e}");
                }
            }
            st.connecting_until = Some(now + CONNECTING_FOR);
            st.keep.want(&name, now);
        }
        FrontAct::Done => {
            info!("回到 Mac 桌面：「{name}」已连回");
            st.front = None;
        }
        FrontAct::GiveUp => {
            warn!("回到 Mac 桌面：「{name}」没能断开再连回，交给保持显示");
            st.front = None;
        }
    }
}

fn note_drop(st: &mut State, connected: &[sidecar::Device], auto: bool) {
    let now_name = connected.first().map(|d| d.name.clone());
    if let (None, Some(prev)) = (&now_name, &st.prev_showing) {
        if st.front.is_some() {
            info!("回到 Mac 桌面：「{prev}」已断开，稍后连回");
        } else if auto && st.keep.target.is_some() {
            info!("随航断开了（不是从 Afar 断开的）：{prev}，会自动连回");
        }
    }
    st.prev_showing = now_name;
}
