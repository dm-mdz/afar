//! "保持显示"：Afar 让哪台 iPad 显示了，就一直保持，直到从 Afar（App 或菜单栏）断开。
//! 中途不管因为什么断了（iPad 锁屏、无线信号不好、开机第一次连接超时、在 iPad 侧边栏点了断开），都自动连回去：
//! 前 3 次每 10 秒一次，之后每 15 秒一次，iPad 不在附近时不试。纯逻辑（时间由调用方传入），可在任何电脑上测试。
//!
//! 设计决定（v0.3.6 测试后）：Afar 分不出"锁屏断开"和"侧边栏断开"，按"不是从 Afar 断开的都连回"处理；
//! 想断开请用 App 里的"断开显示"（侧边栏已隐藏）。

use std::time::{Duration, Instant};

const FAST: Duration = Duration::from_secs(10);
/// 之后 15 秒一次：iPad 锁屏几分钟后解锁，最多等 15 秒左右就连回（v0.3.6 测试解锁后等了 25 秒以上没连回算不通过）。
const SLOW: Duration = Duration::from_secs(15);
const FAST_TRIES: u32 = 3;

#[derive(Debug, Default)]
pub struct Keep {
    /// 要保持显示的 iPad（随航名称）。None = 用户从 Afar 断开了，或从没连过。
    pub target: Option<String>,
    /// 这次断开后已经试了几次。
    pub tries: u32,
    last_try: Option<Instant>,
}

impl Keep {
    /// Afar 发起了连接（App、菜单栏、自动连接），或者看到正在显示：之后就保持这台。
    pub fn want(&mut self, name: &str, now: Instant) {
        if self.target.as_deref() != Some(name) {
            self.target = Some(name.to_string());
        }
        self.tries = 0;
        self.last_try = Some(now);
    }

    /// 正在显示：清零计数。
    pub fn shown(&mut self, name: &str) {
        self.target = Some(name.to_string());
        self.tries = 0;
        self.last_try = None;
    }

    /// 用户从 Afar 断开：不再保持。
    pub fn stop(&mut self) {
        *self = Keep::default();
    }

    /// 没在显示时每轮调用：`available` = 目标 iPad 在附近。返回 true = 现在去连一次（调用方连完调用 `tried`）。
    pub fn due(&self, available: bool, now: Instant) -> bool {
        if self.target.is_none() || !available {
            return false;
        }
        let gap = if self.tries < FAST_TRIES { FAST } else { SLOW };
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
