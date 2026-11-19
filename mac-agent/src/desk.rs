//! iPad 没在显示时，虚拟屏用"上次远程用的清晰尺寸"。
//!
//! 场景：Mac 不接显示器，iPad 随航和远程桌面软件（如 UU 远程）轮流用。iPad 在显示时虚拟屏是 iPad 的档位（iPad 上清楚）；
//! iPad 断开后虚拟屏停在 iPad 的尺寸。新建的虚拟屏从没用过远程的尺寸，系统没有可以恢复的，远程软件只能拿到 iPad 的尺寸
//! （实测拔掉显示器、Afar 重建虚拟屏后，远程软件里才出现想要的尺寸）。有的远程软件连上时还会把虚拟屏切成 1 倍低清。
//!
//! 规则（纯逻辑，时间由调用方传入，可在任何电脑上测试）：
//! - "安静"时（虚拟屏在、iPad 没在显示、没接实体显示器、Afar 没在切档）：虚拟屏换成了**清晰模式** = 用户换的，记住；
//!   换成 iPad 当前的档位 = 不要了，清掉。
//! - 换成了**低清模式**（像素不是点的 2 倍）：不记，切回记住的清晰尺寸（没记过就切成同样大小的清晰模式）。
//!   `LOW_WINDOW` 内第 `LOW_MAX + 1` 次被换成低清就不再管（不和远程软件来回抢），等下次 iPad 断开 / 虚拟屏重建 / Afar 重启。
//! - 刚变安静（iPad 断开、开机 / 拔显示器后新建虚拟屏、Afar 切过档）：记住的尺寸和当前不一样就切过去；
//!   切了 `TRIES` 次还不对就不再试（不清掉记住的尺寸：多半是别的程序又改了）。切换接口报错（尺寸用不了）由调用方清掉。

use std::time::{Duration, Instant};

/// 比较、保存用的尺寸：（宽、高、像素宽、像素高），不管刷新率。
pub type Size = [usize; 4];

pub const TRIES: u8 = 2;
pub const LOW_WINDOW: Duration = Duration::from_secs(60);
pub const LOW_MAX: usize = 2;

/// 清晰模式（HiDPI）：像素是点的 2 倍。
pub fn hidpi(s: Size) -> bool {
    s[2] == s[0] * 2 && s[3] == s[1] * 2
}

#[derive(Debug, PartialEq, Eq)]
pub enum Act {
    None,
    /// 用户换了清晰尺寸：存下（None = 换回了 iPad 的档位，清掉）。
    Remember(Option<Size>),
    /// 切到这个尺寸（`.1` = 是记住的尺寸）。
    Restore(Size, bool),
    /// 切回记住的尺寸切了几次还不对：先不试了（记住的尺寸保留）。
    Yield(Size),
    /// 一分钟里被换成低清太多次：先不管了。
    YieldLow,
}

#[derive(Debug, Default)]
pub struct Desk {
    /// 安静时上一轮看到的尺寸。None = 刚变安静，先看要不要切回记住的尺寸。
    seen: Option<Size>,
    vid: u32,
    tries: u8,
    lows: Vec<Instant>,
    /// 切回记住的尺寸试够了次数：先不试（用户重新选了尺寸、或 reset 后再试）。
    stop_retry: bool,
    /// 一分钟里被换成低清太多次：先不管低清（reset 后再管）。
    stop_low: bool,
}

impl Desk {
    /// 暂时不安静（Afar 在切档、回到 Mac 桌面、读不到模式）：下次安静时重新判断。
    pub fn busy(&mut self) {
        self.seen = None;
    }

    /// iPad 在显示 / 接着实体显示器 / 没有虚拟屏：之后重新开始（试的次数、"先不管"都清零）。
    pub fn reset(&mut self) {
        self.busy();
        self.tries = 0;
        self.lows.clear();
        self.stop_retry = false;
        self.stop_low = false;
    }

    /// 安静时每轮调用。`vid` 虚拟屏 ID，`cur` 当前尺寸，`ipad` iPad 当前档位的尺寸，`saved` 记住的尺寸。
    pub fn step(&mut self, vid: u32, cur: Size, ipad: Size, saved: Option<Size>, now: Instant) -> Act {
        if vid != self.vid {
            self.vid = vid; // 虚拟屏重建了
            self.reset();
        }
        let Some(prev) = self.seen else {
            self.seen = Some(cur);
            if let Some(m) = saved.filter(|m| *m != cur && !self.stop_retry) {
                if self.tries >= TRIES {
                    self.stop_retry = true;
                    return Act::Yield(m);
                }
                self.tries += 1;
                self.seen = None; // 下一轮核对切过去没有
                return Act::Restore(m, true);
            }
            if saved == Some(cur) {
                self.tries = 0;
            }
            // 刚变安静时就已经是低清（比如远程软件在 iPad 显示期间切的）：一样切回清晰
            return if hidpi(cur) { Act::None } else { self.low(cur, saved, now) };
        };
        if prev == cur {
            return Act::None;
        }
        self.seen = Some(cur);
        if !hidpi(cur) {
            return self.low(cur, saved, now);
        }
        let keep = if cur == ipad { None } else { Some(cur) };
        if keep == saved {
            return Act::None;
        }
        self.tries = 0;
        self.stop_retry = false;
        Act::Remember(keep)
    }

    /// 被换成了低清：切回记住的清晰尺寸（没记过就切成同样大小的清晰模式），一分钟里太多次就先不管。
    fn low(&mut self, cur: Size, saved: Option<Size>, now: Instant) -> Act {
        if self.stop_low {
            return Act::None;
        }
        self.lows.retain(|t| now.duration_since(*t) < LOW_WINDOW);
        self.lows.push(now);
        if self.lows.len() > LOW_MAX {
            self.stop_low = true;
            return Act::YieldLow;
        }
        match saved {
            Some(m) => Act::Restore(m, true),
            None => Act::Restore([cur[0], cur[1], cur[0] * 2, cur[1] * 2], false),
        }
    }
}
