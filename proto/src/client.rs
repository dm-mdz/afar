//! 客户端（iPad App 的 Rust 核心会直接用它；现在也用于在电脑上测试 Mac 端）。
//!
//! 证书校验规则：
//! - 还没配对（没有记住指纹）：接受任何证书，并记下看到的指纹，交给配对流程去确认；
//! - 已配对：只接受记住的那一张证书。

use crate::http::{read_message, write_request};
use crate::pair;
use crate::{ErrorResp, PairFinishReq, PairFinishResp, PairStartReq, PairStartResp};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{verify_tls12_signature, verify_tls13_signature, CryptoProvider};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{ClientConfig, ClientConnection, DigitallySignedStruct, SignatureScheme, StreamOwned};
use serde::de::DeserializeOwned;
use serde::Serialize;
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::{Arc, Mutex};
use std::time::Duration;

#[derive(Debug)]
pub enum Error {
    /// 网络或 TLS 出错（连不上、证书不对）。
    Net(String),
    /// Mac 端返回了错误。
    Api { status: u16, code: String, message: String },
    /// 配对时证书确认失败（配对码错，或者有中间人）。
    PairRejected,
    /// 这个地址上是另一台 Mac（证书和配对时记住的不一致）。令牌不会发出去。
    WrongMac,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Net(e) => write!(f, "网络错误：{e}"),
            Error::Api { status, code, message } => write!(f, "{status} {code}：{message}"),
            Error::PairRejected => write!(f, "配对确认失败（配对码不对，或连到的不是你的 Mac）"),
            Error::WrongMac => write!(f, "这个地址上不是配对过的那台 Mac"),
        }
    }
}

impl std::error::Error for Error {}

#[derive(Debug)]
struct PinVerifier {
    pinned: Option<[u8; 32]>,
    seen: Mutex<Option<[u8; 32]>>,
    provider: Arc<CryptoProvider>,
}

impl ServerCertVerifier for PinVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let fp = pair::fingerprint(end_entity.as_ref());
        *self.seen.lock().unwrap() = Some(fp);
        match self.pinned {
            Some(p) if p != fp => Err(rustls::Error::General("证书和配对时记住的不一致".into())),
            _ => Ok(ServerCertVerified::assertion()),
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider.signature_verification_algorithms.supported_schemes()
    }
}

/// 连到一台 Mac。
#[derive(Debug, Clone)]
pub struct Client {
    pub addr: String,
    /// 配对后记住的证书指纹。
    pub pinned: Option<[u8; 32]>,
    pub token: Option<String>,
    /// 连接超时；没设 `read_timeout` 时也是读写超时。
    pub timeout: Duration,
    /// 读写超时（对方要慢慢算的接口用，比如读蓝牙设备）。None = 同 `timeout`。
    pub read_timeout: Option<Duration>,
}

/// 配对成功后要保存的东西。
#[derive(Debug, Clone)]
pub struct Paired {
    pub fingerprint: [u8; 32],
    pub device_id: String,
    pub token: String,
}

impl Client {
    /// `addr` 形如 "192.168.1.20:47911"。
    pub fn new(addr: impl Into<String>) -> Self {
        Client { addr: addr.into(), pinned: None, token: None, timeout: Duration::from_secs(10), read_timeout: None }
    }

    /// 发一个请求，返回 (状态码, 响应体, 本次看到的证书指纹)。
    pub fn raw(&self, method: &str, path: &str, body: &[u8]) -> Result<(u16, Vec<u8>, [u8; 32]), Error> {
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let verifier =
            Arc::new(PinVerifier { pinned: self.pinned, seen: Mutex::new(None), provider: provider.clone() });
        let cfg = ClientConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .map_err(|e| Error::Net(e.to_string()))?
            .dangerous()
            .with_custom_certificate_verifier(verifier.clone())
            .with_no_client_auth();
        // 主机名可能解析出多个地址（IPv4 / IPv6），依次尝试
        let mut last = Error::Net("地址无效".into());
        let mut tcp = None;
        for a in self.addr.to_socket_addrs().map_err(|e| Error::Net(e.to_string()))? {
            match TcpStream::connect_timeout(&a, self.timeout) {
                Ok(t) => {
                    tcp = Some(t);
                    break;
                }
                Err(e) => last = Error::Net(e.to_string()),
            }
        }
        let tcp = tcp.ok_or(last)?;
        let rw = self.read_timeout.unwrap_or(self.timeout);
        tcp.set_read_timeout(Some(rw)).ok();
        tcp.set_write_timeout(Some(rw)).ok();
        let name = ServerName::try_from("afar.local").expect("固定名称合法");
        let conn = ClientConnection::new(Arc::new(cfg), name).map_err(|e| Error::Net(e.to_string()))?;
        let mut tls = StreamOwned::new(conn, tcp);
        // 证书不一致时握手失败，单独报 WrongMac（调用方据此不再把这个地址当成"这台 Mac"）
        let wrong_mac = |e: std::io::Error| match (self.pinned, *verifier.seen.lock().unwrap()) {
            (Some(p), Some(seen)) if p != seen => Error::WrongMac,
            _ => Error::Net(e.to_string()),
        };
        write_request(&mut tls, method, path, self.token.as_deref(), body).map_err(wrong_mac)?;
        let resp = read_message(&mut tls).map_err(wrong_mac)?;
        let fp = verifier.seen.lock().unwrap().ok_or_else(|| Error::Net("没有拿到证书".into()))?;
        Ok((resp.status(), resp.body, fp))
    }

    /// 发 JSON 请求，解析 JSON 响应；非 200 转成 `Error::Api`。
    pub fn call<Req: Serialize, Resp: DeserializeOwned>(
        &self,
        method: &str,
        path: &str,
        req: &Req,
    ) -> Result<Resp, Error> {
        let body = serde_json::to_vec(req).map_err(|e| Error::Net(e.to_string()))?;
        let (status, body, _) = self.raw(method, path, &body)?;
        if status != 200 {
            let e: ErrorResp = serde_json::from_slice(&body)
                .unwrap_or(ErrorResp { code: "unknown".into(), error: String::from_utf8_lossy(&body).into() });
            return Err(Error::Api { status, code: e.code, message: e.error });
        }
        serde_json::from_slice(&body).map_err(|e| Error::Net(format!("响应格式错误：{e}")))
    }

    pub fn get<Resp: DeserializeOwned>(&self, path: &str) -> Result<Resp, Error> {
        self.call("GET", path, &serde_json::Value::Null)
    }

    pub fn post<Req: Serialize, Resp: DeserializeOwned>(&self, path: &str, req: &Req) -> Result<Resp, Error> {
        self.call("POST", path, req)
    }

    /// 用配对码配对。成功后本客户端会记住证书和令牌。
    pub fn pair(&mut self, code: &str, device_name: &str) -> Result<Paired, Error> {
        let (half, msg) = pair::start_ipad(code);
        let body = serde_json::to_vec(&PairStartReq { device_name: device_name.into(), msg })
            .map_err(|e| Error::Net(e.to_string()))?;
        // 第一步：此时还没记住证书，先记下看到的指纹
        let saved_pin = self.pinned.take();
        let (status, resp, fp) = self.raw("POST", "/pair/start", &body)?;
        if status != 200 {
            self.pinned = saved_pin;
            let e: ErrorResp =
                serde_json::from_slice(&resp).unwrap_or(ErrorResp { code: "unknown".into(), error: String::new() });
            return Err(Error::Api { status, code: e.code, message: e.error });
        }
        let start: PairStartResp = serde_json::from_slice(&resp).map_err(|e| Error::Net(e.to_string()))?;
        let key = half.finish(&start.msg).map_err(Error::Net)?;
        if !pair::verify_confirm(&key, pair::ROLE_MAC, &fp, &start.confirm) {
            self.pinned = saved_pin;
            return Err(Error::PairRejected);
        }
        // 第二步：从这里开始只认这张证书
        self.pinned = Some(fp);
        let fin: PairFinishResp = self.post(
            "/pair/finish",
            &PairFinishReq { pair_id: start.pair_id, confirm: pair::confirm(&key, pair::ROLE_IPAD, &fp) },
        )?;
        self.token = Some(fin.token.clone());
        Ok(Paired { fingerprint: fp, device_id: fin.device_id, token: fin.token })
    }
}
