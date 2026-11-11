//! 命令行子命令（macOS）。

use crate::private::sidecar;
use crate::{config, display, runloop, server};

pub fn cmd_ipads() -> i32 {
    let _ = sidecar::devices(); // 先触发一次发现
    runloop::run_for(1.5);
    match (sidecar::devices(), sidecar::connected()) {
        (Ok(av), Ok(con)) => {
            println!("可用：");
            for d in &av {
                println!("  {}", d.name);
            }
            println!("已连接：");
            for d in &con {
                println!("  {}", d.name);
            }
            if av.is_empty() {
                println!("（没有可用 iPad：确认 iPad 已解锁、同一 Apple 账号、蓝牙/Wi-Fi/接力已开、在附近）");
            }
            0
        }
        (Err(e), _) | (_, Err(e)) => {
            error!("{e}");
            1
        }
    }
}

/// 发起操作后转几秒 RunLoop 等回调，再报告结果。
fn wait_and_report() -> i32 {
    runloop::run_for(8.0);
    match sidecar::connected() {
        Ok(c) if !c.is_empty() => {
            println!("当前已连接：{}", c.iter().map(|d| d.name.as_str()).collect::<Vec<_>>().join("、"));
            0
        }
        Ok(_) => {
            println!("当前没有连接");
            0
        }
        Err(e) => {
            error!("{e}");
            1
        }
    }
}

pub fn cmd_connect(name: &str) -> i32 {
    let devs = match sidecar::devices() {
        Ok(d) => d,
        Err(e) => {
            error!("{e}");
            return 1;
        }
    };
    let dev = if name.is_empty() { devs.first() } else { devs.iter().find(|d| d.name == name) };
    let Some(dev) = dev else {
        error!("找不到可用的 iPad「{name}」，先运行 ipads 看看");
        return 1;
    };
    if let Err(e) = sidecar::connect(dev) {
        error!("{e}");
        return 1;
    }
    wait_and_report()
}

pub fn cmd_disconnect() -> i32 {
    // 先告诉正在运行的 Afar"这是用户断开的"，否则它会当成意外断开、几秒后自动连回
    let _ = std::fs::write(config::dir().join(crate::agent::DISCONNECT_REQUEST), b"");
    let con = match sidecar::connected() {
        Ok(c) => c,
        Err(e) => {
            error!("{e}");
            return 1;
        }
    };
    for d in &con {
        let _ = sidecar::disconnect(d);
    }
    wait_and_report()
}

pub fn cmd_displays() -> i32 {
    for d in display::online() {
        let cur = display::current_mode(d);
        let m = display::mirrors(d);
        let (vendor, model, serial, builtin) = display::ident(d);
        println!(
            "显示器 {d}：当前 {}{}",
            cur.map(|c| format!("{}×{}（像素 {}×{}）", c.w, c.h, c.pw, c.ph)).unwrap_or("未知".into()),
            if m != 0 { format!("，镜像显示器 {m}") } else { String::new() }
        );
        println!(
            "  编号：厂商 {vendor:#x} 型号 {model:#x} 序列号 {serial:#x}{}",
            if builtin { "（内建）" } else { "" }
        );
        let hidpi: Vec<String> =
            display::modes(d).into_iter().filter(|m| m.pw == m.w * 2).map(|m| format!("{}×{}", m.w, m.h)).collect();
        if !hidpi.is_empty() {
            println!("  可选 HiDPI：{}", hidpi.join(" / "));
        }
    }
    0
}

pub fn cmd_config() -> i32 {
    match config::write_default_if_missing() {
        Ok(p) => {
            println!("{}", p.display());
            0
        }
        Err(e) => {
            error!("{e}");
            1
        }
    }
}

/// 打开配对并显示配对码（和菜单栏"配对新 iPad…"一样，10 分钟内有效；要 Afar 在运行）。
pub fn cmd_pair_code() -> i32 {
    let dir = config::dir();
    let code = server::code_path(&dir);
    let _ = std::fs::remove_file(&code);
    if let Err(e) = std::fs::write(dir.join(crate::agent::PAIR_REQUEST), b"") {
        error!("没能打开配对：{e}");
        return 1;
    }
    // 正在运行的 Afar 每秒看一次请求（忙的时候可能晚几秒）
    for _ in 0..30 {
        std::thread::sleep(std::time::Duration::from_millis(500));
        if let Ok(c) = std::fs::read_to_string(&code) {
            println!("配对码：{}（已打开配对，{} 分钟内有效，用过即换）", c.trim(), server::PAIR_WINDOW.as_secs() / 60);
            return 0;
        }
    }
    let _ = std::fs::remove_file(dir.join(crate::agent::PAIR_REQUEST));
    error!("15 秒没等到配对码：Afar 可能没在运行（先运行 afar-agent run 或 install），也可能已经打开了配对，到菜单栏点\"配对新 iPad…\"看");
    1
}

/// 诊断蓝牙键盘（v0.3.5 测试：低功耗蓝牙键盘在 Afar 列表里看不到）。
/// 从"终端"运行时蓝牙权限算在终端头上，第一次会弹窗问。
pub fn cmd_bt_dump(action: Option<&str>, addr: Option<&str>) -> i32 {
    if let Some(a) = action {
        let (Some(addr), true) = (addr, a == "close" || a == "open") else {
            error!("用法：afar-agent bt-dump [close|open <地址>]，地址形如 a1-b2-c3-d4-e5-f6");
            return 1;
        };
        println!("{}", crate::bt::try_action(a, addr));
        return 0;
    }
    let auth = crate::bt::authorization();
    println!(
        "蓝牙权限：{auth}（0 未决定 1 受限 2 拒绝 3 允许）{}",
        match auth {
            0 => "：第一次运行，请在弹窗里允许，然后再运行一次（这次的结果不算）",
            1 | 2 => "：没有权限，下面的列表会是空的，请在 隐私与安全 › 蓝牙 里允许\"终端\"",
            _ => "",
        }
    );
    println!("经典蓝牙接口（IOBluetooth）看到的已配对设备：");
    let paired = crate::bt::dump_paired();
    if paired.is_empty() {
        println!("  （没有）");
    }
    for l in paired {
        println!("  {l}");
    }
    println!("低功耗蓝牙（CoreBluetooth）此刻连着 Mac 的设备：");
    for l in crate::bt::dump_ble() {
        println!("  {l}");
    }
    0
}
