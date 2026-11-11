//! Afar Mac 端里与平台无关的部分（日志、控制接口），做成库方便 iPad 端核心逻辑做联调测试。

#[macro_use]
pub mod log;
pub mod server;
