# Afar 隐私政策 / Privacy Policy

最后更新：2026 年 11 月 25 日（v0.6.0） · Last updated: November 25, 2026 (v0.6.0)

## 中文

Afar（iPad App 和 Mac 端）**不收集、不上传、不出售任何个人数据**，也没有任何追踪、广告或统计分析。

- **只在你的局域网里工作（零云端中转）**：iPad App 只和你自己配对过的 Mac 在本地局域网内点对点加密通信，不经过我们或任何第三方服务器。
- **保存在你设备上的信息**：已配对的 Mac 列表、配对令牌（Mac 端仅存哈希）、你选择保存的 Mac 登录密码，只存在你设备的系统钥匙串和 App 本地存储里，用于连接和远程解锁你的 Mac；在 App 里删除设备或取消配对即可清除。
- **屏幕录制权限（仅「日常」直连模式可选）**：在「日常」模式下，Mac 端使用 Apple ScreenCaptureKit 捕获虚拟屏画面，并通过局域网端到端加密（ChaCha20-Poly1305 AEAD UDP 直传，控制面为 TLS 1.3）传输至你已配对的 iPad。**零云端中转、零本地落盘录像或截屏存储**。如果只使用「画板」（Sidecar）模式，可完全不授予此权限。
- **辅助功能权限（仅「日常」直连模式可选）**：**仅在直连会话活跃期间**将 iPad 端发来的键盘、鼠标、触控、Apple Pencil 及输入法文本事件注入 macOS；会话结束或断线时立即停止注入并释放全部按键状态。当系统进入安全密码输入态（Apple SecureInput）时，Afar 会在 iPad 端给出轻量提示，全程绝不记录、缓存或上传任何按键内容。如果只使用「画板」模式，可完全不授予此权限。
- **蓝牙权限（仅「画板」模式可选）**：仍仅用于「画板」（Sidecar）模式下的经典蓝牙键盘自动交接；「日常」直连模式完全不依赖蓝牙。
- **面容 ID / 触控 ID**：只用于在远程解锁前确认是你本人，由系统完成，App 拿不到你的生物特征信息。
- **本地网络与系统事件（Apple Events）权限**：本地网络用于在局域网发现并连接你的 Mac；系统事件仅用于辅助系统设置向导或你主动发起的远程重启。
- **不面向儿童收集信息**：我们不收集任何人的信息，包括儿童。

如果以后有任何变化，会在这里更新本政策。有问题请在 GitHub 提 issue：https://github.com/dm-mdz/afar/issues

## English

Afar (the iPad app and the Mac agent) **does not collect, upload, or sell any personal data**. There is no tracking, advertising, or analytics.

- **Local network only (zero cloud relay)**: the iPad app communicates exclusively with Macs you have paired, over encrypted peer-to-peer connections on your local network. Nothing passes through our servers or any third party.
- **Stored on your device**: the list of paired Macs, pairing tokens (stored hashed on Mac), and—if you choose to save it—your Mac login password are kept only in your device's Keychain and local app storage to connect to and remotely unlock your Mac. Removing or unpairing a device erases them.
- **Screen Recording permission (optional, "Daily" direct mode only)**: in Daily mode, the Mac agent uses Apple ScreenCaptureKit to capture the virtual display and stream it directly over an encrypted LAN peer-to-peer connection (ChaCha20-Poly1305 AEAD over UDP, with TLS 1.3 control plane) to your paired iPad. **Zero cloud relay and zero on-disk recording or screenshot storage.** You do not need to grant this permission if you only use "Pro Draw" (Sidecar) mode.
- **Accessibility permission (optional, "Daily" direct mode only)**: used **solely while a direct session is active** to inject keyboard, mouse, touch, Apple Pencil, and IME text events from your iPad into macOS, and immediately stops and releases all held input when the session ends or disconnects. When macOS enters a secure password field (Apple SecureInput), Afar displays a lightweight status notice on iPad; keystrokes are never logged, cached, or transmitted anywhere else. You do not need to grant this permission if you only use "Pro Draw" mode.
- **Bluetooth permission (optional, "Pro Draw" mode only)**: continues to be used solely for classic Bluetooth keyboard hand-over during Sidecar ("Pro Draw") sessions. "Daily" direct mode does not use Bluetooth.
- **Face ID / Touch ID**: used only to confirm it is you before a remote unlock. The system performs the check; the app never receives biometric data.
- **Local Network & Apple Events permissions**: Local Network is used to discover and connect to your Mac via Bonjour; Apple Events is used only to assist with setup guidance or user-initiated remote restarts.
- **Children**: we collect no information from anyone, including children.

Any change will be posted here. Questions: https://github.com/dm-mdz/afar/issues
