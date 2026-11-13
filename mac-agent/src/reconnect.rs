//! "连接请求"：用户发起了连接（iPad 上点卡片 / App 回到前台自动连、菜单栏、回到 Mac 桌面），没连上就再试 2 次；
//! Mac 自己发起的（开机 / iPad 出现时自动连接）只连一次；用户从 Afar 断开过就不自己发起，等用户点连接（或 Afar 重启）。
//! 连上以后中途断了（iPad 锁屏 / 自己休眠、信号不好、在 iPad 侧边栏点了断开）Mac **不**自己连回：
//! 由 iPad App 回到前台时（解锁后回到 Afar、打开 Afar）自动发起连接。纯逻辑（时间由调用方传入），可在任何电脑上测试。
//!
//! v0.4.10 改动的根源：原来（v0.3.6 起）"不是从 Afar 断开的都一直连回"。iPad 锁屏 / 休眠后 Mac 照样去连，
//! 连上会把 iPad 叫醒一下，iPad 20～40 秒后又睡、随航断开（系统日志 RTCP Timeout），Mac 再连……
//! 每次失败 / 断开系统的随航程序都弹一个"无法连接…"的框，没完没了（v0.4.9 只是 6 次后暂停，治标）。
//! Mac 分不出"iPad 睡了"和"信号断了"，iPad App 却知道自己什么时候醒着，所以连回改由 iPad 发起。

use std::time::{Duration, Instant};

/// 连接请求没连上时，再试前等多久；试几次。超时后的下一次常被 iPad 秒拒（-101），所以至少要再试 2 次。
/// 15 秒：一次连接超时要 10 秒左右才报失败（结果是异步回来的），间隔太短会和还没结束的那次撞上。
const GAP: Duration = Duration::from_secs(15);
pub const RETRIES: u32 = 2;

#[derive(Debug, Default)]
pub struct Keep {
    /// 最近一次要显示到哪台 iPad（随航名称）。None = 用户从 Afar 断开了，或从没连过。
    pub target: Option<String>,
    /// 这次连接请求已经补试了几次。
    pub tries: u32,
    last_try: Option<Instant>,
    /// 有连接请求还没连上。
    pending: bool,
}

impl Keep {
    /// Afar 发起了连接（App、菜单栏、开机自动连接、回到 Mac 桌面）：没连上就补试。
    pub fn want(&mut self, name: &str, now: Instant) {
        self.target = Some(name.to_string());
        self.tries = 0;
        self.last_try = Some(now);
        self.pending = true;
    }

    /// Mac 自己发起的连接（开机 / iPad 出现时自动连接）：只连这一次，不补试。
    pub fn once(&mut self, name: &str) {
        self.shown(name);
    }

    /// 正在显示：请求完成；之后断了不自己连回。
    pub fn shown(&mut self, name: &str) {
        self.target = Some(name.to_string());
        self.tries = 0;
        self.last_try = None;
        self.pending = false;
    }

    /// 用户从 Afar 断开：清空。
    pub fn stop(&mut self) {
        *self = Keep::default();
    }

    /// 没在显示时每轮调用：`available` = 目标 iPad 在附近。返回 true = 现在去补试一次（调用方连完调用 `tried`）。
    pub fn due(&self, available: bool, now: Instant) -> bool {
        self.pending
            && self.target.is_some()
            && available
            && self.tries < RETRIES
            && self.last_try.is_none_or(|t| now.saturating_duration_since(t) >= GAP)
    }

    pub fn tried(&mut self, now: Instant) {
        self.tries += 1;
        self.last_try = Some(now);
    }
}

/// Mac 自己发起的自动连接（iPad 新出现：之前不在附近、现在在了）这一轮要不要连。
/// 用户从 Afar 断开过（`user_off`）就不连：iPad 睡一觉醒来会被当成"新出现"，连了等于断开不算数。
pub fn auto_on_appear(was_available: bool, user_off: bool) -> bool {
    !was_available && !user_off
}

/// "回到 Mac 桌面"（POST /front）：随航已连在这台 iPad 上时，先断开、再连回，iPad 收到 Mac 发起的连接会切到随航画面。
/// 断开是异步的，所以分两段：等断开生效后连回；连回后看到稳定在显示才算结束。期间调用方保留虚拟屏、状态报"连接中"。
#[derive(Debug, Clone)]
pub struct Front {
    pub name: String,
    reconnect_at: Instant,
    sent: Option<Instant>,
    seen: Option<Instant>,
    until: Instant,
}

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum FrontAct {
    Wait,
    /// 现在去连这台 iPad。
    Connect,
    /// 连回来了，结束。
    Done,
    /// 断开一直没生效 / 一直没连回：放弃（交给"保持显示"）。
    GiveUp,
}

/// 断开后隔多久连回（太早可能连到还没断干净的连接）。
pub const FRONT_GAP: Duration = Duration::from_millis(1500);
/// 断开这么久还没生效就放弃。
const FRONT_STUCK: Duration = Duration::from_secs(5);
/// 连回后稳定这么久才算结束（接着实体显示器时，随航屏稳定 2 秒才算在显示）。
const FRONT_STABLE: Duration = Duration::from_secs(3);
/// 整个过程最长这么久。
pub const FRONT_TIMEOUT: Duration = Duration::from_secs(20);

impl Front {
    pub fn new(name: &str, now: Instant) -> Front {
        Front {
            name: name.to_string(),
            reconnect_at: now + FRONT_GAP,
            sent: None,
            seen: None,
            until: now + FRONT_TIMEOUT,
        }
    }

    /// 每秒调用。`connected` = 这台 iPad 此刻在"已连接"里；`available` = 它在附近。
    pub fn step(&mut self, now: Instant, connected: bool, available: bool) -> FrontAct {
        if now >= self.until {
            return FrontAct::GiveUp;
        }
        if self.sent.is_none() {
            if now < self.reconnect_at {
                return FrontAct::Wait;
            }
            if connected {
                return if now >= self.reconnect_at + FRONT_STUCK { FrontAct::GiveUp } else { FrontAct::Wait };
            }
            if !available {
                return FrontAct::Wait;
            }
            self.sent = Some(now);
            return FrontAct::Connect;
        }
        if !connected {
            self.seen = None;
            return FrontAct::Wait;
        }
        let seen = *self.seen.get_or_insert(now);
        if now.saturating_duration_since(seen) >= FRONT_STABLE {
            FrontAct::Done
        } else {
            FrontAct::Wait
        }
    }
}
