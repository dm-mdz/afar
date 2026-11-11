//! 苹果私有接口全部隔离在这个目录（虚拟屏、随航）。
//! 系统升级后如果出问题，只需要改这里；其余代码只用公开接口。
//! 思路参考：DeskPad（虚拟屏）、SidecarLauncher（随航），代码为自写。

pub mod sidecar;
pub mod virtual_display;
