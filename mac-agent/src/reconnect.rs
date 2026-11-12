//! "保持显示"：Afar 让哪台 iPad 显示了，就一直保持，直到从 Afar（App 或菜单栏）断开。
//! 中途不管因为什么断了（iPad 锁屏、无线信号不好、开机第一次连接超时、在 iPad 侧边栏点了断开），都自动连回去：
//! 前 3 次每 10 秒一次，之后 30 秒、1 分钟、2 分钟；6 次都没连稳就暂停，iPad 不在附近时不试。
//! 纯逻辑（时间由调用方传入），可在任何电脑上测试。
//!
//! v0.4.9：原来每 15 秒一直试、连上一下就清零。无线不稳时随航"连上 20～40 秒又断"，每次失败 / 断开系统都弹一个
//! "无法连接…"的框（系统的随航程序弹的，Afar 关不掉），Mac 上越积越多。改为：连稳 2 分钟（`STABLE`）才清零，
//! 间隔逐步拉长，第 6 次还不稳就暂停（`paused`），等用户在 iPad 上点卡片 / 菜单栏"立即连接"（`want`）、
//! 或 iPad 离开附近又回来（`presence`）再恢复。
//!
//! 设计决定（v0.3.6 测试后）：Afar 分不出"锁屏断开"和"侧边栏断开"，按"不是从 Afar 断开的都连回"处理；
//! 想断开请用 App 里的"断开显示"（侧边栏已隐藏）。

use std::time::{Duration, Instant};

/// 断开后第 1～6 次自动连回前各等多久（秒）。
const GAPS: [u64; MAX_TRIES as usize] = [10, 10, 10, 30, 60, 120];
/// 连续这么多次都没连稳：暂停自动连回。
pub const MAX_TRIES: u32 = 6;
/// 连上这么久没断才算稳，清零计数。
pub const STABLE: Duration = Duration::from_secs(120);
/// 目标 iPad 不在附近这么久才算"真的离开"（无线不稳时它可能在附近列表里闪一下，不能每次都清零）。
const AWAY: Duration = Duration::from_secs(60);

#[derive(Debug, Default)]
pub struct Keep {
    /// 要保持显示的 iPad（随航名称）。None = 用户从 Afar 断开了，或从没连过。
    pub target: Option<String>,
    /// 这次断开后已经试了几次。
    pub tries: u32,
    last_try: Option<Instant>,
    /// 目标 iPad 从什么时候起不在附近。
    away_since: Option<Instant>,
}

impl Keep {
    /// Afar 发起了连接（App、菜单栏、自动连接），或者看到正在显示：之后就保持这台。
    pub fn want(&mut self, name: &str, now: Instant) {
        self.retarget(name);
        self.tries = 0;
        self.last_try = Some(now);
    }

    /// 正在显示（连稳了）：清零计数。
    pub fn shown(&mut self, name: &str) {
        self.retarget(name);
        self.tries = 0;
        self.last_try = None;
    }

    /// 连着但还没连稳（不到 `STABLE`）：不清零；下次间隔从断开那一刻算。
    pub fn unstable(&mut self, name: &str, now: Instant) {
        self.retarget(name);
        self.last_try = Some(now);
    }

    /// 换了一台 iPad：上一台的计数、"不在附近"都不带过来。
    fn retarget(&mut self, name: &str) {
        if self.target.as_deref() != Some(name) {
            self.target = Some(name.to_string());
            self.tries = 0;
            self.away_since = None;
        }
    }

    /// 用户从 Afar 断开：不再保持。
    pub fn stop(&mut self) {
        *self = Keep::default();
    }

    /// 连续 `MAX_TRIES` 次没连稳：暂停自动连回。
    pub fn paused(&self) -> bool {
        self.target.is_some() && self.tries >= MAX_TRIES
    }

    /// 没在显示时每轮调用：目标 iPad 离开附近至少 `AWAY` 又回来 = 换了环境（关机、关蓝牙后再开等），从头试。
    pub fn presence(&mut self, available: bool, now: Instant) {
        if !available {
            self.away_since.get_or_insert(now);
        } else if let Some(t) = self.away_since.take() {
            if now.saturating_duration_since(t) >= AWAY {
                self.tries = 0;
                self.last_try = None;
            }
        }
    }

    /// 没在显示时每轮调用：`available` = 目标 iPad 在附近。返回 true = 现在去连一次（调用方连完调用 `tried`）。
    pub fn due(&self, available: bool, now: Instant) -> bool {
        if self.target.is_none() || !available || self.paused() {
            return false;
        }
        let gap = Duration::from_secs(GAPS.get(self.tries as usize).copied().unwrap_or(GAPS[GAPS.len() - 1]));
        self.last_try.is_none_or(|t| now.saturating_duration_since(t) >= gap)
    }

    pub fn tried(&mut self, now: Instant) {
        self.tries += 1;
        self.last_try = Some(now);
    }
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
