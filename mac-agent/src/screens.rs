//! 认显示器、决定要不要虚拟屏。纯逻辑（显示器编号和时间由调用方传入），可在任何电脑上测试。
//!
//! 实测编号（v0.3.5 真机，`afar-agent displays`）：
//! - 随航屏：厂商 0x6161706c（"aapl"）型号 0x69506164（"iPad"）；
//! - 一块屏幕都没有时系统临时挂的占位屏：厂商 0x756e6b6e（"unkn"）型号 0x76697274（"virt"）；
//! - Afar 虚拟屏：厂商 0x4d50；实体显示器是 EDID 里的厂商号（Apple 自家显示器是 0x610，不会和随航屏混）。
//!
//! 以前靠"宽高比像 iPad + 随航连上后不久出现"推断随航屏，随航屏用别的显示模式（如 1074×789）时会认错（v0.3.5 测试第八、九节）。

use std::time::{Duration, Instant};

const AAPL: u32 = 0x6161_706c;
const IPAD: u32 = 0x6950_6164;
const UNKN: u32 = 0x756e_6b6e;
const VIRT: u32 = 0x7669_7274;
/// Afar 虚拟屏的厂商号（private/virtual_display 里创建时用的）。
const OURS: u32 = 0x4d50;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// 这次运行建的虚拟屏。
    Ours,
    Sidecar,
    Physical,
    /// 占位屏、其它 Apple 虚拟屏、别的进程留下的虚拟屏、读不到编号（刚消失）的：既不算实体显示器，也不镜像。
    Other,
}

impl Kind {
    pub fn label(self) -> &'static str {
        match self {
            Kind::Ours => "Afar 虚拟屏",
            Kind::Sidecar => "随航屏",
            Kind::Physical => "实体显示器",
            Kind::Other => "系统虚拟屏（不处理）",
        }
    }
}

pub fn kind(is_ours: bool, vendor: u32, model: u32) -> Kind {
    if is_ours {
        Kind::Ours
    } else if vendor == AAPL && model == IPAD {
        Kind::Sidecar
    } else if vendor == AAPL || vendor == OURS || (vendor == UNKN && model == VIRT) || (vendor == 0 && model == 0) {
        Kind::Other
    } else {
        Kind::Physical
    }
}

/// 要不要虚拟屏：
/// - 没有实体显示器：要（Mac 不能一块屏幕都没有）；
/// - iPad 正在显示（且随航屏镜像虚拟屏）：也要。实测拔掉最后一块实体显示器的瞬间系统就把随航断了，
///   等拔掉再建虚拟屏来不及（v0.3.5 测试第八节）；事先有虚拟屏、随航屏镜像它，拔显示器时随航不受影响；
/// - 接着实体显示器、iPad 没在显示：不要。
pub fn want_vd(has_physical: bool, showing: bool, mirror: bool) -> bool {
    !has_physical || (showing && mirror)
}

/// 不需要虚拟屏的情况持续这么久才关（随航重连、显示器接触不良的一两秒里不来回建、关，窗口不来回搬）。
pub const DROP_AFTER: Duration = Duration::from_secs(3);

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum VdAct {
    Create,
    Destroy,
}

#[derive(Debug, Default)]
pub struct VdPlan {
    unneeded_since: Option<Instant>,
}

impl VdPlan {
    /// 每秒调用一次。需要时马上建；不需要时持续 `DROP_AFTER` 才关。
    pub fn step(&mut self, want: bool, have: bool, now: Instant) -> Option<VdAct> {
        if want || !have {
            self.unneeded_since = None;
            return (want && !have).then_some(VdAct::Create);
        }
        let since = *self.unneeded_since.get_or_insert(now);
        if now.saturating_duration_since(since) >= DROP_AFTER {
            self.unneeded_since = None;
            Some(VdAct::Destroy)
        } else {
            None
        }
    }
}
