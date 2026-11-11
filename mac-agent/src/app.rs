//! 从"应用程序"里双击 Afar.app（AFAR-008）：这个进程只负责交接，常驻的那份由 launchd 运行。
//! - 不在"应用程序"里（比如直接在下载的磁盘映像里双击）：提示先拖进去；Afar 已在运行就问要不要换成这一版（见 upgrade）；
//! - 开机自启没装、或指向别处（以前用命令行装在 ~/bin）：装成指向这个 App（配对、设置都在同一个目录，不丢）；
//! - 然后留个请求文件，让正在运行的 Afar 打开"设置 Afar"窗口。

use crate::{agent, config, launchd};
use objc2::MainThreadMarker;
use objc2_app_kit::{NSAlert, NSApplication, NSApplicationActivationPolicy};
use objc2_foundation::NSString;
use std::path::{Path, PathBuf};
use std::process::Command;

/// 这个程序是在某个 .app 包里运行的。
pub fn in_bundle() -> bool {
    std::env::current_exe().is_ok_and(|p| p.to_string_lossy().contains(".app/Contents/MacOS/"))
}

fn alert(title: &str, text: &str) {
    let Some(mtm) = MainThreadMarker::new() else { return };
    let app = NSApplication::sharedApplication(mtm);
    app.setActivationPolicy(NSApplicationActivationPolicy::Regular);
    #[allow(deprecated)]
    app.activateIgnoringOtherApps(true);
    let a = NSAlert::new(mtm);
    a.setMessageText(&NSString::from_str(title));
    a.setInformativeText(&NSString::from_str(text));
    a.runModal();
}

pub fn launch() -> i32 {
    let exe = match std::env::current_exe().and_then(|p| p.canonicalize()) {
        Ok(p) => p,
        Err(e) => {
            alert("Afar 打不开", &format!("找不到程序位置：{e}"));
            return 1;
        }
    };
    let home = std::env::var("HOME").unwrap_or_default();
    let in_apps = exe.starts_with("/Applications/") || exe.starts_with(Path::new(&home).join("Applications"));
    if !in_apps {
        if launchd::running() {
            upgrade(&exe);
        } else {
            alert(
                "请先把 Afar 拖进\"应用程序\"",
                "在打开的窗口里把 Afar 图标拖到\"应用程序\"文件夹，再从\"应用程序\"里打开它。\n放在别处的话，开机自启和系统授权都会出问题。",
            );
        }
        return 0;
    }
    if !launchd::installed_for(&exe) {
        info!("从 Afar.app 安装开机自启：{}", exe.display());
        if launchd::install_with(false) != 0 {
            alert(
                "Afar 没能启动",
                "开机自启装好了，但 Afar 没有正常运行起来。详细原因在 ~/Library/Logs/Afar/agent.log。",
            );
            return 1;
        }
    } else if !launchd::running() {
        // 换新版时旧版被 bootout 掉了（见 upgrade）：直接重装，不用先等 kickstart
        if launchd::loaded() {
            launchd::kickstart();
            std::thread::sleep(std::time::Duration::from_secs(3));
        }
        // 拉不起来（被 bootout 过、在"登录项 › 允许在后台"里被关了）：重装一次
        if !launchd::running() && launchd::install_with(false) != 0 {
            alert(
                "Afar 没能启动",
                "请看 系统设置 › 通用 › 登录项 里\"允许在后台\"有没有关掉 Afar。详细原因在 ~/Library/Logs/Afar/agent.log。",
            );
            return 1;
        }
    }
    let _ = std::fs::create_dir_all(config::dir());
    let _ = std::fs::write(config::dir().join(agent::OPEN_WINDOW_REQUEST), b"");
    0
}

/// 正在运行时又打开了别处（多半是新下载的磁盘映像里）的 Afar.app：问要不要换成这一版，要换就替用户退出旧版、
/// 把这一版复制过去替换、再从"应用程序"打开——一次点完。
/// v0.4.4 测试：从磁盘映像打开时系统启动的是这一份新进程（不是给运行中的 Afar 发"重新打开"），所以在这里问。
/// v0.4.6 测试：原来点"退出旧版"后要用户自己拖进"应用程序"替换；不接显示器时旧版一退出虚拟屏就没了、iPad 画面断开，
/// 后面的提示只在 Mac 上、用户看不到。改为自动替换并打开，iPad 断开几秒后由新版自动连回；自动替换不成才退回手动提示。
fn upgrade(exe: &Path) {
    let new = env!("CARGO_PKG_VERSION");
    // 正在用的那份的版本：开机自启指向的程序 → 它所在 .app 的 Info.plist
    let cur = launchd::program()
        .and_then(|exe| exe.parent()?.parent().map(|c| c.join("Info.plist")))
        .and_then(|p| bundle_version(&p));
    if let Some(cur) = cur.as_deref().filter(|c| !crate::setupcore::newer(new, c)) {
        alert(
            &format!("Afar {cur} 已经在运行"),
            &format!("这个窗口里的是 {new}，不比正在用的新，不用换。\n要打开设置，请从\"应用程序\"里打开 Afar。"),
        );
        return;
    }
    let using = cur.map(|c| format!("正在用的是 {c}。")).unwrap_or_default();
    if !confirm(
        &format!("要换成 Afar {new} 吗？"),
        &format!(
            "{using}点\"更新\"后，Afar 会自动退出旧版、换成这一版并重新打开，大约 10 秒。\n这期间 iPad 会断开几秒，之后自动连回；配对和设置都会保留。"
        ),
        "更新",
        "以后再说",
    ) {
        return;
    }
    if launchd::stop() {
        let src = bundle_of(exe);
        let dest = installed_app();
        match src.as_deref().ok_or_else(|| "找不到这个 Afar.app".to_string()).and_then(|s| replace_app(s, &dest)) {
            Ok(()) => {
                info!("换新版：已把 {} 复制到 {}，打开它", src.as_deref().unwrap_or(exe).display(), dest.display());
                // 用 open 打开，走正常的"从应用程序打开"（见 launch：装开机自启、拉起常驻的那份）。
                // -n：磁盘映像里这一份（同一个 App）此刻还在运行，不加的话系统可能只把它激活、不启动新装的那份
                if let Err(e) = Command::new("/usr/bin/open").arg("-n").arg(&dest).status() {
                    alert("新版没能自动打开", &format!("请从\"应用程序\"里打开 Afar。（{e}）"));
                }
            }
            Err(e) => {
                warn!("换新版：自动替换失败：{e}");
                alert(
                    "旧版已退出",
                    &format!("自动替换没成功（{e}）。\n请把这个窗口里的 Afar 拖到\"应用程序\"，选\"替换\"，再从\"应用程序\"里打开它。"),
                );
            }
        }
    } else {
        alert(
            "没能退出旧版",
            "请点屏幕右上角菜单栏里的 Afar › 退出 Afar，再把这个窗口里的 Afar 拖到\"应用程序\"替换。",
        );
    }
}

/// 程序所在的 .app（…/Afar.app/Contents/MacOS/afar-agent → …/Afar.app）。
fn bundle_of(exe: &Path) -> Option<PathBuf> {
    let app = exe.parent()?.parent()?.parent()?;
    app.extension().is_some_and(|e| e == "app").then(|| app.to_path_buf())
}

/// 要替换的那份：开机自启指向的 .app；没有（或不是 .app）就用 /Applications/Afar.app。
fn installed_app() -> PathBuf {
    launchd::program()
        .and_then(|p| bundle_of(&p))
        .filter(|p| p.starts_with("/Applications/") || p.to_string_lossy().contains("/Applications/"))
        .unwrap_or_else(|| PathBuf::from("/Applications/Afar.app"))
}

/// 把 src（.app）换到 dest：先完整复制到同目录的临时名，再改名替换；中途失败时旧的留着不动。
fn replace_app(src: &Path, dest: &Path) -> Result<(), String> {
    let dir = dest.parent().ok_or("目标位置不对")?;
    let tmp = dir.join(".Afar-new.app");
    let old = dir.join(".Afar-old.app");
    let _ = std::fs::remove_dir_all(&tmp);
    let _ = std::fs::remove_dir_all(&old);
    let st = Command::new("/usr/bin/ditto").arg(src).arg(&tmp).status().map_err(|e| format!("复制失败：{e}"))?;
    if !st.success() || !tmp.join("Contents/MacOS/afar-agent").exists() {
        let _ = std::fs::remove_dir_all(&tmp);
        return Err(format!("复制失败（{st}）"));
    }
    if dest.exists() {
        std::fs::rename(dest, &old).map_err(|e| {
            let _ = std::fs::remove_dir_all(&tmp);
            format!("挪开旧版失败：{e}")
        })?;
    }
    if let Err(e) = std::fs::rename(&tmp, dest) {
        let _ = std::fs::rename(&old, dest);
        let _ = std::fs::remove_dir_all(&tmp);
        return Err(format!("放入新版失败：{e}"));
    }
    let _ = std::fs::remove_dir_all(&old);
    Ok(())
}

/// .app 的版本号。Info.plist 是 make-app.sh 生成的文本格式：<key>CFBundleShortVersionString</key><string>0.4.3</string>
fn bundle_version(plist: &Path) -> Option<String> {
    let text = std::fs::read_to_string(plist).ok()?;
    let v = text
        .split("<key>CFBundleShortVersionString</key>")
        .nth(1)?
        .split("<string>")
        .nth(1)?
        .split("</string>")
        .next()?;
    Some(v.trim().to_string())
}

/// 弹一个两个按钮的提示框。点第一个按钮返回 true。
fn confirm(title: &str, text: &str, ok: &str, cancel: &str) -> bool {
    let Some(mtm) = MainThreadMarker::new() else { return false };
    let app = NSApplication::sharedApplication(mtm);
    // 常驻的 Afar 只在菜单栏（Accessory），这样激活不一定能把提示框放到最前面；临时切成普通 App，问完再切回去
    app.setActivationPolicy(NSApplicationActivationPolicy::Regular);
    #[allow(deprecated)]
    app.activateIgnoringOtherApps(true);
    let a = NSAlert::new(mtm);
    a.setMessageText(&NSString::from_str(title));
    a.setInformativeText(&NSString::from_str(text));
    let _ = a.addButtonWithTitle(&NSString::from_str(ok));
    let _ = a.addButtonWithTitle(&NSString::from_str(cancel));
    // 第一个按钮 = NSAlertFirstButtonReturn（1000）
    let ok = a.runModal() == 1000;
    app.setActivationPolicy(NSApplicationActivationPolicy::Accessory);
    ok
}
