# Afar · Mac 端

[中文](#中文) · [English](#english)

---

## 中文

**iPad 点一下，就成为没接显示器的 Mac（比如家里的 Mac mini）的触控屏。**

这个仓库是 Afar 的 **Mac 端常驻程序**和 **Mac 与 iPad 之间的通信协议**。配套的 iPad App 单独发布，不在本仓库。

**下载**：到本仓库的 [Releases](../../releases) 页下载 `Afar-<版本>.dmg`。

### 能做什么
- **没接显示器也能用**：建一块虚拟屏、防休眠，Mac mini 不接显示器也有桌面。
- **随航自动连接**：开机、点卡片时自动连，没连上会再试 2 次；随航中途断了（iPad 锁屏、信号不好），iPad 回到 Afar 时自动连回（Mac 不对着睡着的 iPad 反复重连，不会没完没了地弹"无法连接"）；从 Afar 主动断开后不会被自动连回，直到你再点连接。
- **显示大小三档**：更大文字 / 默认 / 更多空间，按每台 iPad 的屏幕计算。
- **菜单栏**：状态、立即连接 / 断开 iPad、「开机自动连接 iPad」开关、配对新 iPad。从 Afar 断开后，重启 Mac 也不会自动连回，直到你再点连接。
- **"设置 Afar"向导和状态页**：防火墙放行、断电后自动开机、重启助手、文件保险箱和自动登录的引导；列出已配对的 iPad，可逐台取消配对。
- **给 iPad App 的控制接口**：配对、状态、连接 / 断开、回到 Mac 桌面、重启 Mac、改名称和显示大小、保存登录密码。
- **蓝牙键盘交接**：用随航时把你选的蓝牙键盘接到 Mac，用完还给 iPad（经典蓝牙键盘，不支持低功耗蓝牙键盘）。
- **插回显示器时恢复分辨率**。

### 设备要求
| | 要求 |
|---|---|
| Mac | Apple 芯片，macOS 27 及以上 |
| iPad | iPadOS 27 及以上（随航的手指直接点击要 macOS 27 + iPadOS 27） |
| 随航本身的要求 | 同一个 Apple 账户（开双重认证）；蓝牙、Wi-Fi、接力打开；相距 10 米内；iPad 和 Mac 在同一个局域网 |

### 编译和安装
需要 Xcode 命令行工具和 Rust（[rustup](https://rustup.rs)）。

```bash
sh mac-agent/scripts/make-app.sh   # 生成 target/afar-app/Afar.app 和 Afar-<版本>.dmg
```
打开 dmg，把 **Afar** 拖进"应用程序"，再从"应用程序"里打开：自动装好开机自启，并弹出"设置 Afar"向导。

- 没有开发者签名时是临时签名：第一次打开要在 系统设置 › 隐私与安全性 里点"仍要打开"。
- 有 Developer ID 证书时：`AFAR_SIGN_ID="Developer ID Application: …" sh mac-agent/scripts/make-app.sh`，再用 `xcrun notarytool` 公证。签名证书、公证账号和密码**只在打包时用环境变量或钥匙串传入，不要提交到仓库**（`.gitignore` 已排除常见的证书文件）。
- 日志：`~/Library/Logs/Afar/agent.log`。

命令行：程序在 `/Applications/Afar.app/Contents/MacOS/afar-agent`。

| 命令 | 作用 |
|---|---|
| `run` | 前台运行（开机自启跑的也是它） |
| `install` / `uninstall` | 安装 / 卸载开机自启 |
| `pair-code` | 打开配对并显示 6 位配对码（10 分钟内有效；要 Afar 在运行） |
| `ipads` | 列出附近可用 / 已连接的 iPad |
| `connect [名称]` / `disconnect` | 连接 / 断开随航 |
| `displays` | 列出显示器、分辨率、镜像关系 |
| `bt-dump [close\|open <地址>]` | 诊断：列出系统看到的蓝牙设备，可对某个地址试一次断开 / 连上 |
| `config` | 显示配置文件路径 |

配置文件：`~/Library/Application Support/Afar/config.toml`

| 项 | 默认 | 说明 |
|---|---|---|
| `ipad_name` | `""` | 只自动连这台 iPad；空 = 上次连过的那台 |
| `auto_connect` | `true` | 总开关：关掉后 Mac 不发起任何自动连接（含点连接没连上时的补试）；只想关开机自动连接，用菜单栏的开关 |
| `scale` | `"auto"` | 显示大小：`larger` / `default` / `more` |
| `ipad_pixels` | `[2360, 1640]` | iPad 横屏像素，iPad 没报过屏幕尺寸时用来算虚拟屏比例 |
| `mirror` | `true` | 随航连上后 iPad 镜像虚拟屏 |
| `port` | `47911` | 控制接口端口 |
| `extra_sizes` | `[]` | 虚拟屏额外的分辨率（像素），给远程桌面等别的屏幕用，如 `[[2560, 1600]]`；在"系统设置 › 显示器"里选（系统给的是一半大小的清晰模式，如 1280×800） |

### 安全设计
- 只在局域网工作，全程 TLS 加密（Mac 端自签证书）。
- **平时不接受配对**：只有在 Mac 菜单栏点"配对新 iPad…"（或向导走到配对那一屏、命令行 `pair-code`）后的 10 分钟内才接受。配对成功、点"停止配对"、关掉窗口或到时间，都会立即停止。
- **配对**：Mac 显示 6 位配对码，iPad 输入。用 SPAKE2 协商密钥，配对码不在网络上传输，抓包也无法离线暴力破解；双方再确认各自看到的证书指纹一致，有中间人就配对失败。
- 配对码用过即作废；每个配对码最多试 5 次，用完暂停配对并换码，暂停时间一次比一次长（1、2、4、8…分钟，最长 1 小时）。
- 配对后 iPad 只认这一张证书，并拿到一个令牌；Mac 端只保存令牌的哈希。
- **在 Mac 上能看到、能取消每台已配对的 iPad**；有新 iPad 配对时 Mac 弹系统通知。
- **登录密码**：先用系统接口校验正确才保存，存进 macOS 钥匙串；连续错 5 次锁 1 分钟（校验前先计数，同时发很多请求也只能试 5 次）。
- 局域网发现用系统 Bonjour 广播 `_afar._tcp`。

### 关于私有接口
随航连接、虚拟屏、部分显示模式切换用的是 Apple 未公开的接口，集中在 `mac-agent/src/private/` 和 `mac-agent/src/display.rs`。系统升级后可能失效；也因为这一点，Mac 端用 dmg 分发，不上 Mac App Store。

### 许可证
Copyright (C) 2026 DM

- `mac-agent/`：[GPL-3.0-or-later](LICENSE)。修改后再发布，必须同样开源。
- `proto/`（通信协议）：[MIT](proto/LICENSE)，可以自由用在任何客户端里。

**商标**："Afar"这个名称和图标不在上述许可范围内。基于本仓库修改后再发布，请换用别的名称和图标。

### 报告安全问题
请不要在公开 Issue 里贴漏洞细节，做法见 [SECURITY.md](SECURITY.md)。

本软件按"现状"提供，不附带任何担保。

---

## English

**Tap once on your iPad and it becomes the touchscreen of a Mac with no display attached — such as a headless Mac mini.**

This repository contains Afar's **Mac agent** and the **protocol** spoken between the Mac and the iPad. The companion iPad app is distributed separately and is not part of this repository.

**Download**: get `Afar-<version>.dmg` from this repository's [Releases](../../releases) page.

### Features
- **Works without a monitor**: creates a virtual display and keeps the Mac awake.
- **Sidecar auto-connect**: connects at startup and when you tap a card, retrying twice if it fails; if Sidecar drops (iPad locked, weak signal) the iPad reconnects when you return to the Afar app, so the Mac never keeps calling a sleeping iPad and you don't get endless "couldn't connect" alerts; a disconnect you start from Afar is respected until you connect again.
- **Three display sizes** (larger text / default / more space), computed per iPad model.
- **Menu bar item**: status, connect / disconnect, a "connect iPad at startup" switch, pair a new iPad. A disconnect you start from Afar survives a Mac restart until you connect again.
- **"Set up Afar" assistant and status page**: firewall, automatic start after power loss, restart helper, guidance for FileVault and automatic login; lists paired iPads and lets you unpair each one.
- **Control API for the iPad app**: pairing, status, connect / disconnect, back to the Mac desktop, restart the Mac, rename, display size, storing the login password.
- **Bluetooth keyboard hand-over**: moves a classic Bluetooth keyboard to the Mac while Sidecar is on and back to the iPad afterwards (Bluetooth LE keyboards are not supported).
- **Restores the resolution** of an external monitor when it is plugged back in.

### Requirements
- Mac with Apple silicon, macOS 27 or later.
- iPad with iPadOS 27 or later (direct finger input in Sidecar needs macOS 27 + iPadOS 27).
- Sidecar's own requirements: same Apple Account with two-factor authentication, Bluetooth, Wi-Fi and Handoff on, within 10 m, iPad and Mac on the same local network.

### Build and install
Requires the Xcode command line tools and Rust ([rustup](https://rustup.rs)).

```bash
sh mac-agent/scripts/make-app.sh   # produces target/afar-app/Afar.app and Afar-<version>.dmg
```
Open the dmg, drag **Afar** into Applications and open it from there. It installs itself as a login item and opens the setup assistant.

- Without a Developer ID the app is ad-hoc signed; on first launch allow it in System Settings › Privacy & Security.
- With a Developer ID: `AFAR_SIGN_ID="Developer ID Application: …" sh mac-agent/scripts/make-app.sh`, then notarize with `xcrun notarytool`. Signing certificates and notarization credentials are passed only at build time (environment variables or the Keychain) and **must never be committed** (`.gitignore` excludes the usual certificate files).
- Log: `~/Library/Logs/Afar/agent.log`. Config: `~/Library/Application Support/Afar/config.toml` (`ipad_name`, `auto_connect`, `scale`, `ipad_pixels`, `mirror`, `port` — default port 47911).
- CLI: `/Applications/Afar.app/Contents/MacOS/afar-agent run | install | uninstall | pair-code | ipads | connect [name] | disconnect | displays | bt-dump | config`.

### Security design
- LAN only, everything over TLS (self-signed certificate on the Mac).
- **Pairing is closed by default**: it is accepted only for 10 minutes after you choose "Pair a new iPad…" in the menu bar (or reach the pairing step of the assistant, or run `pair-code`). It closes immediately when pairing succeeds, when you stop it, close the window, or time runs out.
- **Pairing**: the Mac shows a 6-digit code that is typed on the iPad. Keys are agreed with SPAKE2, so the code never crosses the network and captured traffic cannot be brute-forced offline; both sides then confirm the certificate fingerprints they see, so a man-in-the-middle makes pairing fail.
- Codes are single-use; each code allows at most 5 attempts, after which pairing pauses and the code changes, with pauses doubling each time (1, 2, 4, 8… minutes, up to 1 hour).
- After pairing the iPad pins that certificate and receives a token; the Mac stores only a hash of the token.
- **Paired iPads are listed on the Mac and can be unpaired there**; the Mac shows a notification whenever a new iPad pairs.
- **Login password**: verified against the system before it is stored, then kept in the macOS Keychain; 5 wrong attempts lock for 1 minute (attempts are counted before verification, so parallel requests cannot exceed the limit).
- Discovery via Bonjour (`_afar._tcp`).

### Private APIs
Sidecar control, the virtual display and part of the display-mode switching use undocumented Apple APIs, kept in `mac-agent/src/private/` and `mac-agent/src/display.rs`. They may break with macOS updates, which is also why the Mac agent ships as a dmg rather than through the Mac App Store.

### License
Copyright (C) 2026 DM

- `mac-agent/`: [GPL-3.0-or-later](LICENSE). Modified versions you distribute must be open source as well.
- `proto/` (protocol): [MIT](proto/LICENSE), free to use in any client.

**Trademark**: the name "Afar" and its icon are not covered by these licenses. If you distribute a modified version, please use a different name and icon.

### Reporting security issues
Please do not post vulnerability details in public issues — see [SECURITY.md](SECURITY.md).

This software is provided "as is", without warranty of any kind.
