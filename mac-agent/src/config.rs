//! 配置文件：~/Library/Application Support/Afar/config.toml
//! 以后 iPad App 卡片上的"···"会改这里的值；现在可以手动改，改完重启程序生效。

use serde::Deserialize;
use std::path::PathBuf;

#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct Config {
    /// 要连接的 iPad 名称（iPad 上"设置 > 通用 > 关于本机 > 名称"）。留空 = 连第一台可用的。
    pub ipad_name: String,
    /// iPad 可用时自动连接。
    pub auto_connect: bool,
    /// 缩放档位："auto" | "larger" | "default" | "more"。auto 等于 default。
    pub scale: String,
    /// iPad 横屏物理像素，用来让虚拟屏比例和 iPad 一致、避免黑边。iPad Air 4 = [2360, 1640]。
    pub ipad_pixels: [u32; 2],
    /// 随航连上后让 iPad 镜像虚拟屏（只有一个桌面，后台截图和你看到的一致）。
    pub mirror: bool,
    /// 控制接口端口（iPad App 连这个端口）。
    pub port: u16,
    /// 虚拟屏额外的分辨率（像素，如 [[2560, 1600]]），给远程桌面等别的屏幕用；在"系统设置 > 显示器"里选。
    /// 每个尺寸会有原生 1 倍和清晰（一半大小的 HiDPI，如 2560×1600 → 1280×800）两种可选。默认没有。
    pub extra_sizes: Vec<[u32; 2]>,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            ipad_name: String::new(),
            auto_connect: true,
            scale: "auto".into(),
            ipad_pixels: [2360, 1640],
            mirror: true,
            port: afar_proto::PORT,
            extra_sizes: Vec::new(),
        }
    }
}

const TEMPLATE: &str = r#"# Afar Mac 端配置。改完后重启程序生效（launchctl kickstart -k gui/$(id -u)/com.afar.agent）

# 要连接的 iPad 名称（iPad：设置 > 通用 > 关于本机 > 名称）。留空 = 连第一台可用的 iPad
ipad_name = ""

# iPad 可用时自动连接
auto_connect = true

# 缩放档位：auto | larger（更大文字）| default（默认）| more（更多空间）
scale = "auto"

# iPad 横屏物理像素，决定虚拟屏比例（避免黑边）。iPad Air 4 = [2360, 1640]
ipad_pixels = [2360, 1640]

# 随航连上后 iPad 镜像虚拟屏（推荐保持 true）
mirror = true

# 控制接口端口（iPad App 连这个端口；改了要在 iPad App 里手动填地址）
port = 47911

# 虚拟屏额外的分辨率（像素），给远程桌面等别的屏幕用，在"系统设置 > 显示器"里选。例：[[2560, 1600]]
extra_sizes = []
"#;

pub fn dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    PathBuf::from(home).join("Library/Application Support/Afar")
}

pub fn path() -> PathBuf {
    dir().join("config.toml")
}

/// 读配置；文件不存在或写错时用默认值，并在日志里说明。
pub fn load() -> Config {
    let p = path();
    let text = match std::fs::read_to_string(&p) {
        Ok(t) => t,
        Err(_) => {
            info!("没有配置文件，使用默认配置（{}）", p.display());
            return Config::default();
        }
    };
    match toml::from_str::<Config>(&text) {
        Ok(mut c) => {
            if c.ipad_pixels[0] < 640 || c.ipad_pixels[1] < 480 {
                warn!("ipad_pixels 不合理，改用默认 [2360, 1640]");
                c.ipad_pixels = [2360, 1640];
            }
            c.extra_sizes.retain(|s| {
                let ok = (640..=8192).contains(&s[0]) && (480..=8192).contains(&s[1]) && s[0] % 2 == 0 && s[1] % 2 == 0;
                if !ok {
                    warn!("extra_sizes 里的 {}×{} 不合理（要偶数、640×480～8192×8192），不用", s[0], s[1]);
                }
                ok
            });
            c
        }
        Err(e) => {
            warn!("配置文件格式有误，使用默认配置：{e}");
            Config::default()
        }
    }
}

/// 没有配置文件时写一份带注释的默认配置。
pub fn write_default_if_missing() -> std::io::Result<PathBuf> {
    let p = path();
    if !p.exists() {
        std::fs::create_dir_all(dir())?;
        std::fs::write(&p, TEMPLATE)?;
    }
    Ok(p)
}
