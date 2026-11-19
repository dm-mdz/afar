#![cfg_attr(not(target_os = "macos"), allow(dead_code))]
//! Afar Mac 端：常驻保活 + 随航自动连接 + 给 iPad App 的控制接口。
//! 用法：afar-agent <命令>，命令见 help。

#[macro_use]
extern crate afar_agent;
// 日志和控制接口在库里（iPad 端的测试也要用控制接口）
pub use afar_agent::{log, server};
mod config;
mod desk;
mod kbflow;
mod persist;
mod reconnect;
mod scale;
mod screens;
mod setupcore;

#[cfg(target_os = "macos")]
mod agent;
#[cfg(target_os = "macos")]
mod app;
#[cfg(target_os = "macos")]
mod bt;
#[cfg(target_os = "macos")]
mod bt_pair;
#[cfg(target_os = "macos")]
mod cli;
#[cfg(target_os = "macos")]
mod display;
#[cfg(target_os = "macos")]
mod dnssd;
#[cfg(target_os = "macos")]
mod keepalive;
#[cfg(target_os = "macos")]
mod launchd;
#[cfg(target_os = "macos")]
mod mac_backend;
#[cfg(target_os = "macos")]
mod menubar;
#[cfg(target_os = "macos")]
mod notify;
#[cfg(target_os = "macos")]
mod private;
#[cfg(target_os = "macos")]
mod runloop;
#[cfg(target_os = "macos")]
mod setup;

const HELP: &str = "Afar Mac 端

用法：afar-agent <命令>

  run            前台运行（开机自启时也是跑这个）
  install        安装开机自启并立即启动
  uninstall      卸载开机自启
  pair-code      显示 iPad 配对用的 6 位配对码
  ipads          列出附近可用 / 已连接的 iPad
  connect [名称]  立即连接 iPad（不写名称 = 第一台可用的）
  disconnect     断开随航
  displays       列出显示器、当前分辨率、镜像关系
  bt-dump [close|open <地址>]
                 诊断：列出系统看到的全部蓝牙设备（不过滤），可试着断开 / 连上某个地址
  config         显示配置文件位置（没有就生成一份）
";

#[cfg(target_os = "macos")]
fn main() {
    let args: Vec<String> = std::env::args().collect();
    // 从"应用程序"里双击 Afar.app：没有参数（老系统会带一个 -psn_ 参数）
    if (args.len() == 1 || args.get(1).is_some_and(|a| a.starts_with("-psn_"))) && app::in_bundle() {
        std::process::exit(app::launch());
    }
    let cmd = args.get(1).map(String::as_str).unwrap_or("help");
    let code = match cmd {
        "run" => agent::run(),
        "install" => launchd::install(),
        "uninstall" => launchd::uninstall(),
        "pair-code" => cli::cmd_pair_code(),
        "ipads" => cli::cmd_ipads(),
        "connect" => cli::cmd_connect(args.get(2).map(String::as_str).unwrap_or("")),
        "disconnect" => cli::cmd_disconnect(),
        "displays" => cli::cmd_displays(),
        "bt-dump" => cli::cmd_bt_dump(args.get(2).map(String::as_str), args.get(3).map(String::as_str)),
        "config" => cli::cmd_config(),
        _ => {
            print!("{HELP}");
            0
        }
    };
    std::process::exit(code);
}

#[cfg(not(target_os = "macos"))]
fn main() {
    print!("{HELP}");
    eprintln!("\n只支持 macOS（在其它系统上只能跑测试：cargo test）");
    std::process::exit(1);
}
