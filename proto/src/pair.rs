//! 配对：SPAKE2 + 证书指纹确认；令牌生成与存储用的哈希。

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use hmac::{Hmac, Mac};
use rand::RngCore;
use sha2::{Digest, Sha256};
use spake2::{Ed25519Group, Identity, Password, Spake2};

type HmacSha256 = Hmac<Sha256>;

const ID_IPAD: &[u8] = b"afar-ipad";
const ID_MAC: &[u8] = b"afar-mac";
pub const ROLE_MAC: &[u8] = b"afar-confirm-mac";
pub const ROLE_IPAD: &[u8] = b"afar-confirm-ipad";

/// 证书指纹：SHA-256(证书 DER)。
pub fn fingerprint(cert_der: &[u8]) -> [u8; 32] {
    Sha256::digest(cert_der).into()
}

/// 配对进行中的一方。
pub struct Half(Spake2<Ed25519Group>);

/// iPad 端（A 方）开始配对，返回 (状态, 发给 Mac 的消息 base64)。
pub fn start_ipad(code: &str) -> (Half, String) {
    let (s, msg) = Spake2::<Ed25519Group>::start_a(
        &Password::new(code.as_bytes()),
        &Identity::new(ID_IPAD),
        &Identity::new(ID_MAC),
    );
    (Half(s), B64.encode(msg))
}

/// Mac 端（B 方）开始配对。
pub fn start_mac(code: &str) -> (Half, String) {
    let (s, msg) = Spake2::<Ed25519Group>::start_b(
        &Password::new(code.as_bytes()),
        &Identity::new(ID_IPAD),
        &Identity::new(ID_MAC),
    );
    (Half(s), B64.encode(msg))
}

impl Half {
    /// 收到对方消息后得出共享密钥。配对码不一致时双方得到的密钥不同（这里不会报错，确认那一步才会失败）。
    pub fn finish(self, peer_msg_b64: &str) -> Result<Vec<u8>, String> {
        let msg = B64.decode(peer_msg_b64).map_err(|_| "配对消息格式错误".to_string())?;
        self.0.finish(&msg).map_err(|_| "配对消息无效".to_string())
    }
}

/// 确认值：HMAC(共享密钥, 角色 || 证书指纹)，十六进制。
pub fn confirm(key: &[u8], role: &[u8], fp: &[u8; 32]) -> String {
    let mut m = HmacSha256::new_from_slice(key).expect("HMAC 接受任意长度密钥");
    m.update(role);
    m.update(fp);
    hex::encode(m.finalize().into_bytes())
}

/// 常数时间校验确认值。
pub fn verify_confirm(key: &[u8], role: &[u8], fp: &[u8; 32], got_hex: &str) -> bool {
    let Ok(got) = hex::decode(got_hex) else { return false };
    let mut m = HmacSha256::new_from_slice(key).expect("HMAC 接受任意长度密钥");
    m.update(role);
    m.update(fp);
    m.verify_slice(&got).is_ok()
}

/// 随机令牌（64 位十六进制）。
pub fn new_token() -> String {
    let mut b = [0u8; 32];
    rand::thread_rng().fill_bytes(&mut b);
    hex::encode(b)
}

/// 随机 ID（16 位十六进制）。
pub fn new_id() -> String {
    let mut b = [0u8; 8];
    rand::thread_rng().fill_bytes(&mut b);
    hex::encode(b)
}

/// Mac 端只保存令牌的哈希。
pub fn hash_token(token: &str) -> String {
    hex::encode(Sha256::digest(token.as_bytes()))
}

/// 常数时间比较两个字符串。
pub fn ct_eq(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}
