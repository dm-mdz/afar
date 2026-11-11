//! "设置 Afar"窗口里与平台无关的部分：页面状态、页面指令、要以管理员身份执行的脚本（可在任何电脑上测试）。

use serde::Serialize;
use std::path::Path;

/// 页面要显示的全部状态。
#[derive(Debug, Clone, Default, Serialize, PartialEq)]
pub struct View {
    pub version: String,
    pub name: String,
    /// 配对开着时的配对码（关着为空）。
    pub code: String,
    /// 配对还开多少秒（0 = 没开）。
    pub pair_left: u64,
    /// 试错太多、暂停配对中：还要等多少秒（0 = 没暂停）。
    pub pair_lock: u64,
    /// 这次配对页上刚配对上的 iPad 名称（空 = 还没有）。
    pub pair_done: String,
    /// 要页面跳到哪："pair"（菜单栏点了"配对新 iPad…"）| "devices"（点了新配对的通知）| "status"（"检查并配置…"）| ""。
    /// 页面跳过去后回一个 went，Mac 端才清掉（窗口刚建、页面还没加载好时也不会丢）。
    pub go: String,
    /// 向导走完了（之后打开的是状态页）。
    pub done: bool,
    /// 开机自启已装好（指向这个程序）。
    pub agent: bool,
    /// 防火墙没拦 Afar。
    pub fw: bool,
    pub power: bool,
    pub helper: bool,
    /// 附近能用的 iPad（随航名称），空 = 没找到。
    pub ipad: String,
    pub showing: String,
    pub paired: usize,
    /// 已配对的 iPad（状态页列出来，可以逐台取消配对）。
    pub devices: Vec<DeviceView>,
    pub filevault: bool,
    pub auto_login: bool,
    pub remote_login: bool,
    /// 断电恢复方案："auto"（来电后自己进桌面）| "fv"（保持文件保险箱）。
    pub plan: String,
    /// 正在执行的管理员操作："one" | "ssh" | ""。
    pub busy: String,
    /// 上次管理员操作失败的原因。
    pub error: String,
}

/// 状态页上一台已配对的 iPad。
#[derive(Debug, Clone, Default, Serialize, PartialEq)]
pub struct DeviceView {
    pub id: String,
    pub name: String,
    /// 配对日期，如"11 月 9 日"。
    pub date: String,
    /// 正在显示到这台。
    pub showing: bool,
}

/// unix 秒 → 本地日期"11 月 9 日"（不是今年的带上年份）。
pub fn day_label(secs: u64) -> String {
    let local = |t: i64| {
        let mut tm: libc::tm = unsafe { std::mem::zeroed() };
        let t = t as libc::time_t;
        unsafe { libc::localtime_r(&t, &mut tm) };
        (tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)
    };
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let (y, m, d) = local(secs as i64);
    if y == local(now as i64).0 {
        format!("{m} 月 {d} 日")
    } else {
        format!("{y} 年 {m} 月 {d} 日")
    }
}

impl View {
    /// 还差几项（菜单栏提示用）：第 1 屏那 4 项 + 所选断电恢复方案要做的。
    pub fn todo(&self) -> usize {
        let base = [self.agent, self.fw, self.power, self.helper].iter().filter(|x| !**x).count();
        let rec = if self.plan == "fv" {
            usize::from(!self.remote_login)
        } else {
            usize::from(self.filevault) + usize::from(!self.auto_login)
        };
        base + rec
    }
}

/// 页面发来的指令。
#[derive(Debug, PartialEq)]
pub enum Msg {
    Ready,
    One,
    Ssh,
    Open(String),
    Plan(String),
    Done,
    Again,
    Close,
    /// 打开配对（进了配对页）。
    PairOpen,
    /// 停止配对（离开配对页、点了"停止配对"）。
    PairClose,
    /// 在 Mac 上取消某台 iPad 的配对（设备 ID）。
    Unpair(String),
    /// 页面已按 `View::go` 跳过去了。
    Went,
}

pub fn parse(s: &str) -> Option<Msg> {
    let v: serde_json::Value = serde_json::from_str(s).ok()?;
    let arg = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or_default().to_string();
    Some(match v.get("cmd")?.as_str()? {
        "ready" => Msg::Ready,
        "one" => Msg::One,
        "ssh" => Msg::Ssh,
        "open" => Msg::Open(arg("what")),
        "plan" => Msg::Plan(arg("plan")),
        "done" => Msg::Done,
        "again" => Msg::Again,
        "close" => Msg::Close,
        "pair_open" => Msg::PairOpen,
        "pair_close" => Msg::PairClose,
        "unpair" => Msg::Unpair(arg("id")),
        "went" => Msg::Went,
        _ => return None,
    })
}

/// 重启助手（root 的 LaunchDaemon）：看到请求文件出现就删掉它并重启 Mac。
/// 从 iPad 重启就不用 osascript 控制 System Events（第一次会弹授权，没人点就一直等）。
/// 请求文件放在配置目录下单独的 `restart/` 子目录里：文件不存在时 launchd 可能改为监视它所在的目录，
/// 单独一个目录就只有重启请求会触发它。
pub const HELPER_PLIST: &str = "/Library/LaunchDaemons/com.afar.restart.plist";
pub const HELPER_LABEL: &str = "system/com.afar.restart";
pub const RESTART_DIR: &str = "restart";
pub const RESTART_FILE: &str = "request";

fn xml(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
}

pub fn sh_quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', r"'\''"))
}

/// 放进 AppleScript 字符串字面量里（`do shell script "…"`）：转义反斜杠、双引号，换行写成 \n。
pub fn applescript_str(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n")
}

pub fn helper_plist(request: &Path) -> String {
    let f = request.to_string_lossy();
    // 先删再重启：不会因为文件还在而反复重启
    let cmd = format!("f={}; if [ -f \"$f\" ]; then rm -f \"$f\"; /sbin/shutdown -r now; fi", sh_quote(&f));
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.afar.restart</string>
  <key>ProgramArguments</key>
  <array><string>/bin/sh</string><string>-c</string><string>{}</string></array>
  <key>WatchPaths</key>
  <array><string>{}</string></array>
</dict>
</plist>
"#,
        xml(&cmd),
        xml(&f)
    )
}

/// 第 1 屏"一键配置"要以管理员身份执行的脚本。每一步各自报错，不因为一步失败就停。
/// 整个脚本直接写在 osascript 的参数里，不在用户能写的目录里落文件（落文件的话，同一用户下的程序可以在
/// 执行前把它换掉、借这次输密码拿到 root）。重启助手的 plist 在脚本里写到 root 自己的临时文件再装。
pub fn one_script(exe: &Path, request: &Path) -> String {
    let exe = sh_quote(&exe.to_string_lossy());
    let plist = helper_plist(request);
    format!(
        r#"fail=""
FW=/usr/libexec/ApplicationFirewall/socketfilterfw
$FW --add {exe} >/dev/null 2>&1; $FW --unblockapp {exe} >/dev/null 2>&1 || fail="$fail 防火墙"
/usr/bin/pmset -a autorestart 1 >/dev/null 2>&1 || fail="$fail 断电后自动开机"
P={hp}
T=$(/usr/bin/mktemp)
/bin/cat > "$T" <<'AFAR_PLIST_EOF'
{plist}AFAR_PLIST_EOF
if /usr/bin/cmp -s "$T" "$P" && /bin/launchctl print {label} >/dev/null 2>&1; then :; else
  /bin/cp "$T" "$P" && /usr/sbin/chown root:wheel "$P" && /bin/chmod 644 "$P"
  /bin/launchctl bootout {label} >/dev/null 2>&1
  for i in 1 2 3 4 5; do /bin/launchctl bootstrap system "$P" >/dev/null 2>&1 && break; /bin/sleep 1; done
  /bin/launchctl print {label} >/dev/null 2>&1 || fail="$fail 重启助手"
fi
/bin/rm -f "$T"
echo "AFAR-FAIL:$fail""#,
        hp = HELPER_PLIST,
        label = HELPER_LABEL,
    )
}

/// 远程登录：用 launchctl 打开系统自带的 sshd（`systemsetup -setremotelogin` 要"完全磁盘访问"，这里不用它）。
/// 打开后 22 端口连不上就如实报。
pub fn ssh_script() -> String {
    r#"/bin/launchctl enable system/com.openssh.sshd
/bin/launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist >/dev/null 2>&1
/bin/sleep 2
if /usr/bin/nc -z -G 2 127.0.0.1 22 >/dev/null 2>&1; then echo "AFAR-FAIL:"; else echo "AFAR-FAIL: 远程登录（请到 系统设置 › 通用 › 共享 里打开）"; fi"#
        .into()
}

/// 从管理员脚本的输出里取出失败项。`do shell script` 会把换行改成 \r，两种都要认。
pub fn parse_fail(out: &str) -> Option<String> {
    out.split(['\r', '\n']).find_map(|l| l.trim().strip_prefix("AFAR-FAIL:")).map(|f| f.trim().to_string())
}

/// 版本号 a 比 b 新（"0.4.3" 这种格式；读不出来的段按 0）。
pub fn newer(a: &str, b: &str) -> bool {
    let p = |v: &str| v.split('.').map(|n| n.trim().parse::<u32>().unwrap_or(0)).collect::<Vec<_>>();
    p(a) > p(b)
}
