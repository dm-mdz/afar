# 安全架构与漏洞报告 / Security Architecture & Vulnerability Reporting

最后更新：2026 年 11 月 25 日（v0.6.0） · Last updated: November 25, 2026 (v0.6.0)

## 报告安全问题 / Reporting Security Issues

**中文**：发现安全漏洞请不要开公开 Issue。请在本仓库的 **Security › Report a vulnerability**（GitHub 私密漏洞报告）里提交，写清受影响的版本、复现步骤和可能的影响。我们会尽快回复，修复发布前不会公开细节。

**English**: Please do not open a public issue for security vulnerabilities. Use **Security › Report a vulnerability** (GitHub private vulnerability reporting) in this repository and include the affected version, steps to reproduce, and the potential impact. We will reply as soon as we can and will not disclose details before a fix is released.

**支持的版本 / Supported versions**：只有最新发布的版本会收到安全修复。Only the latest release receives security fixes.

---

## v0.6.0 安全与加密模型（中文）

Afar 仅在局域网内工作，零云端中转、零落盘录像。v0.6.0 将控制面、视频流与输入控制面按信道和职责严格隔离：

1. **HTTPS 控制面（独立线程 + 证书钉扎 + 内存擦零）**
   - Mac 端由独立 `T_https` 线程运行内嵌 mbedTLS TLS 1.3（向下兼容已配对旧客户端的 TLS 1.2），仅使用本机生成的自签叶证书，通过 Bonjour `_afar._tcp` 发现。
   - **平时默认关闭配对**：仅在用户主动打开配对窗口或运行 `pair-code` 后的 10 分钟内开启；配对基于 **EC-JPAKE** 协商会话密钥并核验双端证书 SHA-256 指纹（配对码不在线路明文传输），单码最多允许 5 次失败并触发指数退避锁定（最长 1 小时）。
   - 配对完成后 iPad 钉扎 Mac 叶证书 SHA-256 指纹，Mac 仅存令牌哈希；登录密码先经系统校验再存入 macOS 钥匙串。
   - 含密钥/令牌/密码的敏感 HTTP 与 JSON 缓冲区在释放前统一执行安全擦零（`dm_secure_zero`）。

2. **日常模式密钥派生（单次会话隔离）**
   - 每次通过已认证的 `POST /direct/start` 建立直连会话时，生成全新随机 32B 会话主密钥与 8B 大端 `session_id`（严禁任何重连复用 `(master_key, session_id)`）。
   - 使用 **HKDF-SHA256**（以 `sid_be8[8]` 为盐）派生 3 把互不交叉的 32B 子密钥：`k_vid_auth`（UDP 视频流与视频控制加密）、`k_in_i2m`（iPad → Mac 输入加密）与 `k_ctl_m2i`（Mac → iPad 控制回执加密）。

3. **UDP 视频流通道（ChaCha20-Poly1305 AEAD + 交错 XOR FEC + 单客锁定）**
   - 所有视频分片与控制包均采用 **ChaCha20-Poly1305 AEAD** 认证加密（密钥 `k_vid_auth`，16B 外层头 `[u8 sid_be8[8]][u64_le pkt_seq]` 作为 AAD，12B 方向隔离 Nonce `dir=3` i2m / `dir=4` m2i），并配 **64-bit 滑动防重放窗口**。
   - 会话建立后，iPad 发送加密 `HELLO` 包完成握手，Mac 在 AEAD 验签与滑窗检查双双通过后**立即锁定单一对端地址（`peer_addr`）**并拒收任何其他来源；超时无 `KEEPALIVE`、收到 `BYE` 或设备被取消配对时立即断流。
   - 视频帧采用交错 XOR FEC 抗丢包分片并通过加密回传的丢包率反馈自适应码率，零云端中转、零落盘录像。

4. **UDP 控制与输入通道（ChaCha20-Poly1305 AEAD + 滑动防重放窗）**
   - 所有 UDP 报文均采用 **ChaCha20-Poly1305 AEAD** 认证加密（16B Poly1305 Tag）。
   - **16B 外层头作为 AAD**：`[u8 sid_be8[8]][u64_le pkt_seq]`，绑定会话标识与包序号，防篡改与跨会话混包。
   - **12B 方向隔离 Nonce**：`[u32_le dir (1=i2m, 2=m2i)][u64_le pkt_seq]`，双向密钥与 Nonce 完全隔离；`pkt_seq` 从 `1` 起单调递增（拒收 `0`，并在上限 `UINT64_MAX - 1024` 前停止复用）。
   - **64-bit 滑动防重放窗口**：拒绝任何重复或落后于窗口的旧包；仅在 AEAD 验签与滑窗检查双双通过后，才更新对端地址（`peer_addr`），防止未认证伪造源地址劫持。

5. **输入安全保护（SecureInput 状态感知提示 + `RELEASE_ALL`）**
   - Mac 端周期性检测 Apple **SecureInput** 状态（系统密码框、钥匙串授权框等安全输入场景），通过加密控制通道实时通知 iPad 端展示安全输入提示条（macOS SecureInput 原生阻断第三方进程监听按键，Afar 全程绝不记录、缓存或上传任何按键内容，同时保持无头 Mac 用户可完成系统密码与权限授权）。
   - 当 iPad 退后台、主动断开、设备被吊销配对，或 Mac 端看门狗连续 1.5 秒未收到合法加密 UDP 包（含心跳）时，自动执行 **`RELEASE_ALL`**，立即释放全部按下的按键、修饰键、鼠标键与 Apple Pencil 状态，杜绝卡键与残留输入。

---

## v0.6.0 Security & Encryption Model (English)

Afar operates strictly over your local network with zero cloud relays and zero on-disk recording. In v0.6.0, control, video streaming, and input channels are cryptographically isolated:

1. **HTTPS Control Plane (Dedicated Thread + Cert Pinning + Zero-on-Free)**
   - The Mac agent runs an isolated `T_https` thread powered by vendored mbedTLS TLS 1.3 (preserving TLS 1.2 compatibility for already-paired legacy clients) with a local self-signed leaf certificate discovered via Bonjour `_afar._tcp`.
   - **Pairing closed by default**: opens for 10 minutes only when explicitly triggered from the UI or `pair-code`. Uses **EC-JPAKE** key agreement with mutual certificate SHA-256 fingerprint verification (the 6-digit code is never sent in cleartext), capped at 5 attempts per code with exponential backoff (up to 1 hour).
   - Paired clients pin the Mac leaf certificate's SHA-256 fingerprint; the Mac stores only token hashes, and optional login passwords are verified before being stored in macOS Keychain.
   - Sensitive HTTP and JSON buffers carrying tokens, keys, or credentials are explicitly wiped (`dm_secure_zero`) before being freed.

2. **Per-Session Key Derivation (Daily Direct Mode)**
   - Each authenticated `POST /direct/start` generates a fresh random 32-byte session master key and 8-byte big-endian `session_id` (reusing `(master_key, session_id)` across reconnects is strictly forbidden).
   - **HKDF-SHA256** (salted with `sid_be8[8]`) derives three domain-separated 32-byte subkeys: `k_vid_auth` (UDP video stream & video control), `k_in_i2m` (iPad → Mac input), and `k_ctl_m2i` (Mac → iPad control/ACK).

3. **UDP Video Stream (ChaCha20-Poly1305 AEAD + Interleaved XOR FEC + Single-Peer Lock)**
   - Every video chunk and control datagram is authenticated and encrypted with **ChaCha20-Poly1305 AEAD** (`k_vid_auth`, 16-byte outer header `[u8 sid_be8[8]][u64_le pkt_seq]` as AAD, 12-byte direction-isolated Nonce `dir=3` i2m / `dir=4` m2i) and protected by a **64-bit sliding anti-replay window**.
   - After session setup, the iPad sends an encrypted `HELLO` datagram; once AEAD verification and replay checks pass, the Mac **locks to that single peer address (`peer_addr`)** and rejects packets from any other source. Missing `KEEPALIVE` past timeout, an encrypted `BYE`, or device revocation immediately tears down the stream.
   - Frames use interleaved XOR FEC for wireless packet-loss resilience and encrypted receiver loss feedback for adaptive bitrate—with zero cloud relay and zero on-disk recording.

4. **UDP Control & Input Channel (ChaCha20-Poly1305 AEAD + 64-bit Anti-Replay Window)**
   - Every UDP datagram is encrypted and authenticated with **ChaCha20-Poly1305 AEAD** (16-byte Poly1305 tag).
   - **16-byte outer header as AAD**: `[u8 sid_be8[8]][u64_le pkt_seq]`, binding both session identity and packet sequence number.
   - **12-byte direction-isolated Nonce**: `[u32_le dir (1=i2m, 2=m2i)][u64_le pkt_seq]`, with strictly monotonic `pkt_seq` starting at `1` (rejecting `0` and halting before `UINT64_MAX - 1024`).
   - **64-bit sliding anti-replay window**: drops duplicate and stale packets; the peer UDP endpoint address is updated only after both AEAD verification and replay-window checks succeed.

5. **Input Safety (Apple SecureInput Indicator & `RELEASE_ALL`)**
   - The Mac agent monitors macOS **Apple SecureInput** state (password prompts, Keychain dialogs, etc.) and notifies the iPad over the encrypted control channel to display a lightweight indicator (macOS SecureInput natively blocks third-party keylogging, and Afar never logs, caches, or transmits keystrokes elsewhere while still allowing headless Mac users to complete local authentication prompts).
   - App backgrounding, session disconnect, device revocation, or a 1.5-second UDP watchdog timeout triggers **`RELEASE_ALL`**, releasing all held keys, modifiers, mouse buttons, and Apple Pencil states immediately.
