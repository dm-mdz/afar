//! 蓝牙键盘"跟着随航交接"的决策：什么时候让 Mac 接管、什么时候松开。
//! 纯逻辑，不碰系统（时间、随航状态、键盘状态都由调用方传进来），所以能在任何电脑上用"连、断、切换"的序列测试。
//!
//! 规则：
//! - 只有 iPad App 发来的连接请求里带了"交键盘"（用户确认过）才接管；菜单栏、自动连接不接管。
//! - 等到**目标那台 iPad**出现在"已连接"里才接管（断开是异步的，旧连接可能还会多留一两轮）。
//! - 随航断开时只松开这次自己接管的；平时就连在 Mac 上的键盘不动。

use std::sync::Mutex;
use std::time::{Duration, Instant};

/// 蓝牙线程写、主线程读的键盘实时情况。
#[derive(Debug, Clone, Default)]
pub struct Live {
    /// 选定的键盘现在连在 Mac 上。
    pub connected: bool,
    /// 正在尝试接管。
    pub taking: bool,
    pub error: String,
    /// 最近一次松开键盘的时间（App 据此提示"连回 iPad"）。
    pub released_at: Option<Instant>,
}

/// 给状态接口用的一份快照。
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LiveView {
    pub connected: bool,
    pub taking: bool,
    pub error: String,
    /// 刚松开不久（App 提示连回 iPad 的有效期内）。
    pub released: bool,
}

/// "刚松开"的有效期。
const RELEASED_FOR: Duration = Duration::from_secs(600);

/// 键盘实时情况的存放处。**不把锁暴露出去**：只能整份取快照（`view`）或在一个闭包里改（`update`），
/// 每次调用只加一次锁、调用结束就放。
/// AFAR-011：以前在同一条语句（构造 Status）里多次 `live.lock()`，前一次的锁要到语句结束才放，
/// 第二次就把自己卡死，Mac 端启动不了。现在外面拿不到锁，写不出那种代码。
#[derive(Debug, Default)]
pub struct LiveCell(Mutex<Live>);

impl LiveCell {
    pub fn view(&self, now: Instant) -> LiveView {
        let l = self.0.lock().unwrap();
        LiveView {
            connected: l.connected,
            taking: l.taking,
            error: l.error.clone(),
            released: l.released_at.is_some_and(|t| now.saturating_duration_since(t) < RELEASED_FOR),
        }
    }

    /// 改里面的值。闭包里不要做慢的事（比如蓝牙调用），也不要再碰这个 LiveCell。
    pub fn update(&self, f: impl FnOnce(&mut Live)) {
        f(&mut self.0.lock().unwrap());
    }
}

/// App 点连接后，多久内随航连上才接管。
const ARM_FOR: Duration = Duration::from_secs(30);
/// 收到断开指令后这段时间内看到的"已连接"可能还是旧连接，先不接管（看到断开过或超时后才接管）。
const GAP: Duration = Duration::from_secs(6);

/// 要调用方去做的事。
#[derive(Debug, PartialEq, Clone, Copy)]
pub enum Act {
    /// 开始接管键盘。
    Take,
    /// 松开键盘。
    Release,
    /// 这次不接管：清掉上一次"已松开"的记录（免得 App 又提示连回 iPad）。
    ClearReleased,
}

#[derive(Debug, Default)]
pub struct KbFlow {
    was_showing: bool,
    /// 等这台 iPad 连上就接管（名称、截止时间）。
    take_for: Option<(String, Instant)>,
    gap_until: Option<Instant>,
    /// "回到 Mac 桌面"会把随航断开再连回：这段时间里的断开不松开键盘。
    hold_until: Option<Instant>,
    /// hold 期间真的压下过一次断开（还欠一次"松开"：没连回来、hold 到期或被取消时补上）。
    held_drop: bool,
    /// 这次是 Afar 接管的键盘（调用方负责存盘，程序重启后还要记得松开）。
    pub taken: bool,
}

impl KbFlow {
    pub fn new(showing: bool, taken: bool) -> KbFlow {
        KbFlow { was_showing: showing, taken, ..Default::default() }
    }

    /// 收到连接指令。`keyboard`：这次要交键盘。
    pub fn on_connect(&mut self, target: &str, keyboard: bool, now: Instant) {
        self.take_for = keyboard.then(|| (target.to_string(), now + ARM_FOR));
        self.hold_until = None; // 用户换了操作：不再替"回到桌面"压着断开
    }

    /// "回到 Mac 桌面"：接下来这段时间里随航会断开再连回，期间不松开键盘。
    pub fn hold(&mut self, until: Instant) {
        self.hold_until = Some(until);
        self.held_drop = false;
    }

    /// 收到断开指令。`showing_now`：此刻随航还连着。
    pub fn on_disconnect(&mut self, showing_now: bool, now: Instant) {
        self.take_for = None;
        self.hold_until = None; // 压下过的断开在下一轮补松开
        if showing_now {
            self.gap_until = Some(now + GAP);
        }
    }

    /// 每轮调用。`connected`：正连着的 iPad 名称；`kb_on_mac`：键盘此刻连在 Mac 上。
    pub fn on_tick(&mut self, connected: &[String], kb_on_mac: bool, now: Instant) -> Option<Act> {
        let showing = !connected.is_empty();
        if !showing {
            self.gap_until = None;
        }
        let was = std::mem::replace(&mut self.was_showing, showing);
        let holding = self.hold_until.is_some_and(|t| now < t);
        if was && !showing {
            if holding {
                self.held_drop = true;
                return None; // 回到桌面的短暂断开：键盘留在 Mac
            }
            // 待接管的不清（可能正在换一台 iPad）
            return std::mem::take(&mut self.taken).then_some(Act::Release);
        }
        if self.held_drop {
            if showing {
                // 断开过、又连回来了：这次回到桌面结束（断开还没生效的那几轮不算）
                self.held_drop = false;
                self.hold_until = None;
            } else if !holding {
                // 没连回来（iPad 走远、锁屏）或被用户断开取消：补上欠的松开
                self.held_drop = false;
                self.hold_until = None;
                return std::mem::take(&mut self.taken).then_some(Act::Release);
            }
        }
        if let Some((name, until)) = self.take_for.clone() {
            if now > until {
                self.take_for = None;
            } else if self.gap_until.is_none_or(|t| now >= t) && connected.contains(&name) {
                self.take_for = None;
                return self.take(kb_on_mac);
            } else {
                return None; // 还在等目标 iPad 连上
            }
        }
        (!was && showing).then_some(Act::ClearReleased)
    }

    /// 现在接管（用户在 App 里手动点的，或上面条件满足）。已经连在 Mac 上的不算 Afar 接管。
    pub fn take(&mut self, kb_on_mac: bool) -> Option<Act> {
        if kb_on_mac {
            return None;
        }
        self.taken = true;
        Some(Act::Take)
    }

    /// 松开 Afar 接管的（没接管就什么都不做）。
    pub fn release(&mut self) -> Option<Act> {
        self.take_for = None;
        std::mem::take(&mut self.taken).then_some(Act::Release)
    }

    /// 不管是谁接的，现在就松开。
    pub fn release_now(&mut self) -> Act {
        self.take_for = None;
        self.taken = false;
        Act::Release
    }
}

/// 蓝牙已配对列表里明显不是键盘 / 鼠标的苹果设备（低功耗蓝牙设备分不出类别，v0.3.7 测试里 iPad 也被列成了"不支持的键盘"）。
pub fn is_apple_device(name: &str) -> bool {
    let n = name.to_lowercase();
    // "Keys-To-Go for iPad" 这类名字里带 iPad 的键盘不能排除
    if ["keyboard", "keys", "mouse", "trackpad", "键盘", "鼠标", "触控板"].iter().any(|k| n.contains(k)) {
        return false;
    }
    [
        "ipad",
        "iphone",
        "apple watch",
        "airpods",
        "macbook",
        "imac",
        "mac mini",
        "mac studio",
        "mac pro",
        "apple tv",
        "vision pro",
    ]
    .iter()
    .any(|k| n.contains(k))
}
