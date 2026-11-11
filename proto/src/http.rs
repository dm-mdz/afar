//! 极简 HTTP/1.1 收发：只支持 Content-Length，每次请求一个连接。

use std::io::{self, BufRead, BufReader, Read, Write};

/// 单条消息最大体积（请求和响应都很小）。
pub const MAX_BODY: usize = 64 * 1024;
const MAX_HEADER_LINES: usize = 64;

#[derive(Debug, Clone, Default)]
pub struct Message {
    /// 请求："POST /pair/start HTTP/1.1"；响应："HTTP/1.1 200 OK"
    pub start: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Message {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }

    /// 请求的 (方法, 路径)。
    pub fn method_path(&self) -> (&str, &str) {
        let mut it = self.start.split(' ');
        (it.next().unwrap_or(""), it.next().unwrap_or(""))
    }

    /// 响应的状态码。
    pub fn status(&self) -> u16 {
        self.start.split(' ').nth(1).and_then(|s| s.parse().ok()).unwrap_or(0)
    }

    /// Bearer 令牌。
    pub fn bearer(&self) -> Option<&str> {
        self.header("authorization")?.strip_prefix("Bearer ")
    }
}

fn bad(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.to_string())
}

pub fn read_message<R: Read>(r: R) -> io::Result<Message> {
    let mut br = BufReader::new(r.take((MAX_BODY + 16 * 1024) as u64));
    let mut line = String::new();
    br.read_line(&mut line)?;
    let start = line.trim_end().to_string();
    if start.is_empty() {
        return Err(bad("空请求"));
    }
    let mut headers = Vec::new();
    loop {
        line.clear();
        br.read_line(&mut line)?;
        let l = line.trim_end();
        if l.is_empty() {
            break;
        }
        if headers.len() >= MAX_HEADER_LINES {
            return Err(bad("请求头太多"));
        }
        if let Some((k, v)) = l.split_once(':') {
            headers.push((k.trim().to_string(), v.trim().to_string()));
        }
    }
    let len: usize = headers
        .iter()
        .find(|(k, _)| k.eq_ignore_ascii_case("content-length"))
        .and_then(|(_, v)| v.parse().ok())
        .unwrap_or(0);
    if len > MAX_BODY {
        return Err(bad("内容太大"));
    }
    let mut body = vec![0u8; len];
    br.read_exact(&mut body)?;
    Ok(Message { start, headers, body })
}

pub fn write_request<W: Write>(
    w: &mut W,
    method: &str,
    path: &str,
    token: Option<&str>,
    body: &[u8],
) -> io::Result<()> {
    let mut head = format!(
        "{method} {path} HTTP/1.1\r\nHost: afar\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: {}\r\n",
        body.len()
    );
    if let Some(t) = token {
        head.push_str(&format!("Authorization: Bearer {t}\r\n"));
    }
    head.push_str("\r\n");
    w.write_all(head.as_bytes())?;
    w.write_all(body)?;
    w.flush()
}

pub fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        409 => "Conflict",
        429 => "Too Many Requests",
        _ => "Internal Server Error",
    }
}

pub fn write_response<W: Write>(w: &mut W, status: u16, body: &[u8]) -> io::Result<()> {
    let head = format!(
        "HTTP/1.1 {status} {}\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n",
        reason(status),
        body.len()
    );
    w.write_all(head.as_bytes())?;
    w.write_all(body)?;
    w.flush()
}
