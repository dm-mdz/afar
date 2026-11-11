//! 安装 / 卸载开机自启（LaunchAgent，登录后运行，崩溃自动拉起；从菜单栏「退出」属于正常退出，不会被拉起）。

use crate::config;
use std::path::PathBuf;
use std::process::Command;

pub const LABEL: &str = "com.afar.agent";

fn home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/tmp".into()))
}

fn plist_path() -> PathBuf {
    home().join(format!("Library/LaunchAgents/{LABEL}.plist"))
}

fn domain() -> String {
    format!("gui/{}", unsafe { libc::getuid() })
}

fn xml_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
}

/// 开机自启已经装好、而且指向 `exe`（换了位置、从命令行版换成 Afar.app 时要重装）。
pub fn installed_for(exe: &std::path::Path) -> bool {
    let want = format!("<string>{}</string>", xml_escape(&exe.to_string_lossy()));
    std::fs::read_to_string(plist_path()).is_ok_and(|p| p.contains(&want))
}

/// launchd 里正在运行。
pub fn running() -> bool {
    job().running
}

/// launchd 里有这个任务（装了开机自启、没被 bootout 掉）。
pub fn loaded() -> bool {
    Command::new("/bin/launchctl")
        .args(["print", &format!("{}/{LABEL}", domain())])
        .output()
        .is_ok_and(|o| o.status.success())
}

/// 开机自启指向的程序（plist 里的第一个参数）。
pub fn program() -> Option<PathBuf> {
    let p = std::fs::read_to_string(plist_path()).ok()?;
    let exe = p.split("<array><string>").nth(1)?.split("</string>").next()?;
    Some(PathBuf::from(exe.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")))
}

/// 停掉正在运行的 Afar（换新版用）：从 launchd 里卸下这个任务，launchd 不会再拉起；plist 留着，
/// 从"应用程序"里打开新版时重新装上（见 app::launch）。等它真的退出，最多 5 秒。
pub fn stop() -> bool {
    // bootout 后 launchctl 马上就查不到这个任务了：按进程号确认它真的退出
    let pid = job().pid.parse::<i32>().ok().filter(|p| *p > 0);
    let _ = Command::new("/bin/launchctl").args(["bootout", &format!("{}/{LABEL}", domain())]).status();
    let gone = || pid.is_none_or(|p| unsafe { libc::kill(p, 0) } != 0) && !running();
    for _ in 0..10 {
        if gone() {
            return true;
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    gone()
}

/// 让 launchd 把它拉起来（已在运行时不动）。
pub fn kickstart() {
    let _ = Command::new("/bin/launchctl").args(["kickstart", &format!("{}/{LABEL}", domain())]).status();
}

pub fn install() -> i32 {
    install_with(true)
}

/// `autorestart`：顺便用 sudo 打开"断电后自动开机"（命令行安装时；从 Afar.app 安装时由"设置 Afar"窗口一起做）。
pub fn install_with(autorestart: bool) -> i32 {
    let exe = match std::env::current_exe().and_then(|p| p.canonicalize()) {
        Ok(p) => p,
        Err(e) => {
            error!("找不到程序路径：{e}");
            return 1;
        }
    };
    let agents_dir = home().join("Library/LaunchAgents");
    if let Err(e) = std::fs::create_dir_all(&agents_dir) {
        error!("创建目录 {} 失败：{e}", agents_dir.display());
        return 1;
    }
    let log_file = match log_path() {
        Ok(p) => p,
        Err(e) => {
            error!("{e}");
            return 1;
        }
    };
    if exe.starts_with("/Volumes/") {
        warn!(
            "程序在外接盘上（{}）：开机后首次启动时 macOS 可能弹出「允许访问可移除宗卷」，需要在这台 Mac 上点「允许」，否则 Afar 会一直等待。\n  建议把程序拷到本机磁盘（例如 /Applications 或 ~/bin）后再运行 install",
            exe.display()
        );
    }
    match config::write_default_if_missing() {
        Ok(p) => info!("配置文件：{}", p.display()),
        Err(e) => warn!("写默认配置失败：{e}"),
    }

    let log = xml_escape(&log_file.to_string_lossy());
    let plist = format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>{LABEL}</string>
  <key>ProgramArguments</key>
  <array><string>{exe}</string><string>run</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardOutPath</key><string>{log}</string>
  <key>StandardErrorPath</key><string>{log}</string>
</dict>
</plist>
"#,
        exe = xml_escape(&exe.to_string_lossy()),
    );
    if let Err(e) = std::fs::write(plist_path(), plist) {
        error!("写入 {} 失败：{e}", plist_path().display());
        return 1;
    }

    if autorestart {
        set_autorestart();
    }

    let _ = Command::new("/bin/launchctl").args(["bootout", &format!("{}/{LABEL}", domain())]).status();
    let plist = plist_path();
    // 刚 bootout 后马上 bootstrap 常会报 "Bootstrap failed: 5"，等一下重试。
    let mut ok = false;
    for _ in 0..5 {
        ok = Command::new("/bin/launchctl")
            .args(["bootstrap", &domain(), &plist.to_string_lossy()])
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        if ok {
            break;
        }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
    if !ok {
        error!("launchctl 启动失败，可手动执行：launchctl bootstrap {} {}", domain(), plist.display());
        return 1;
    }
    // bootstrap 成功不等于程序真的跑起来了（AFAR-007：日志路径打不开时 launchd 直接以 78 退出）。
    // 等到 launchd 报告在运行、控制接口端口能连上，才算安装成功。
    info!("已交给 launchd，等待 Afar 实际启动…");
    let port = config::load().port;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(25);
    let mut last = Job::default();
    while std::time::Instant::now() < deadline {
        last = job();
        if last.running && port_open(port) {
            info!("已安装并启动（进程 {}，控制接口端口 {port}）。日志：{}", last.pid, log_file.display());
            return 0;
        }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
    error!("安装后 Afar 没有正常启动：");
    if last.running {
        error!("  进程在运行（{}），但 25 秒内控制接口没有开始监听，可能在等待系统授权弹窗（例如外接盘访问），请到这台 Mac 上查看", last.pid);
    } else {
        error!(
            "  launchd 报告没有在运行，上次退出码：{}",
            if last.exit.is_empty() { "无" } else { last.exit.as_str() }
        );
        if last.exit.starts_with("78") {
            error!("  退出码 78 通常是日志文件 {} 打不开（权限或外接盘）", log_file.display());
        }
    }
    error!("  日志：{}；详细状态：launchctl print {}/{LABEL}", log_file.display(), domain());
    1
}

/// 启动日志位置。默认 ~/Library/Logs/Afar/agent.log；这个目录若是指向外接盘的链接或在外接盘上，
/// launchd 可能打不开（AFAR-007），改用本机的 ~/Library/Logs/com.afar.agent.log。
fn log_path() -> Result<PathBuf, String> {
    let logs = home().join("Library/Logs");
    let dir = logs.join("Afar");
    let linked = std::fs::symlink_metadata(&dir).map(|m| m.file_type().is_symlink()).unwrap_or(false);
    let external = std::fs::canonicalize(&dir).map(|p| p.starts_with("/Volumes/")).unwrap_or(false);
    if linked || external {
        let f = logs.join(format!("{LABEL}.log"));
        warn!("{} 指向外接盘，launchd 可能无法写入，启动日志改写到本机：{}", dir.display(), f.display());
        std::fs::create_dir_all(&logs).map_err(|e| format!("创建目录 {} 失败：{e}", logs.display()))?;
        return Ok(f);
    }
    std::fs::create_dir_all(&dir).map_err(|e| format!("创建目录 {} 失败：{e}", dir.display()))?;
    Ok(dir.join("agent.log"))
}

/// 断电后自动开机（pmset 需要管理员权限）。已开就跳过；没有终端可输密码时不尝试，只给出做法。
fn set_autorestart() {
    let on = Command::new("/usr/bin/pmset")
        .arg("-g")
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).lines().any(|l| l.split_whitespace().eq(["autorestart", "1"])))
        .unwrap_or(false);
    if on {
        info!("「断电后自动开机」已开启");
        return;
    }
    let tty = unsafe { libc::isatty(0) } == 1;
    if tty {
        info!("设置「断电后自动开机」，需要输入管理员密码…");
    }
    let args: &[&str] = if tty { &[] } else { &["-n"] };
    let ok = Command::new("/usr/bin/sudo")
        .args(args)
        .args(["/usr/bin/pmset", "-a", "autorestart", "1"])
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if ok {
        info!("已开启断电后自动开机");
    } else {
        warn!("没能开启「断电后自动开机」（需要管理员密码）。请在终端运行 sudo pmset -a autorestart 1，或在 系统设置 > 能耗 里把「接入电源时启动」设为「断电后」");
    }
}

#[derive(Default)]
struct Job {
    running: bool,
    pid: String,
    exit: String,
}

/// launchd 里这个任务的状态（`launchctl print` 的 state / pid / last exit code）。
fn job() -> Job {
    let out = Command::new("/bin/launchctl")
        .args(["print", &format!("{}/{LABEL}", domain())])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    let mut j = Job::default();
    // 只看第一层（一个制表符缩进）的字段，嵌套块里也可能有 state = …
    for l in out.lines().filter(|l| l.starts_with('\t') && !l.starts_with("\t\t")) {
        let Some((k, v)) = l.trim().split_once(" = ") else { continue };
        match k {
            "state" => j.running = v == "running",
            "pid" => j.pid = v.to_string(),
            "last exit code" => j.exit = v.to_string(),
            _ => {}
        }
    }
    j
}

fn port_open(port: u16) -> bool {
    let addr = std::net::SocketAddr::from(([127, 0, 0, 1], port));
    std::net::TcpStream::connect_timeout(&addr, std::time::Duration::from_millis(500)).is_ok()
}

pub fn uninstall() -> i32 {
    let _ = Command::new("/bin/launchctl").args(["bootout", &format!("{}/{LABEL}", domain())]).status();
    match std::fs::remove_file(plist_path()) {
        Ok(()) => info!("已卸载开机自启（配置和日志保留）"),
        Err(_) => info!("没有安装过开机自启"),
    }
    0
}
