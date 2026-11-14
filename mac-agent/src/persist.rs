//! 运行时状态（程序自己读写，不是给人改的）：~/Library/Application Support/Afar/state.json
//! 存 iPad App 改过的设置（名称、缩放档位）和上次连接的 iPad。

use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Saved {
    /// iPad App 里给这台 Mac 起的名称。
    pub name: Option<String>,
    /// iPad App 里选的缩放档位（优先于 config.toml）。
    pub scale: Option<String>,
    /// 上次通过 iPad App 连接的 iPad（随航名称），自动连接只连它。
    pub last_ipad: Option<String>,
    /// 选定的蓝牙键盘（地址、名称；connected 不保存）。
    #[serde(default)]
    pub keyboard: Option<afar_proto::KeyboardInfo>,
    /// "用随航时把键盘交给 Mac"开关（默认关）。
    #[serde(default)]
    pub keyboard_auto: bool,
    /// 这次随航期间键盘是 Afar 接管的（程序重启后也要在断开随航时松开）。
    #[serde(default)]
    pub keyboard_taken: bool,
    /// 用户从 Afar 断开了（菜单栏、命令行、App）：重启后也不自动连随航，等用户自己点连接（或看到随航在显示）才清掉。
    #[serde(default)]
    pub user_off: bool,
    /// 菜单栏"开机自动连接 iPad"关掉了（默认开）。
    #[serde(default)]
    pub no_boot_connect: bool,
    /// "设置 Afar"向导走完了。
    #[serde(default)]
    pub setup_done: bool,
    /// 断电恢复方案："auto" | "fv"（没选过时按文件保险箱开没开来定）。
    #[serde(default)]
    pub plan: Option<String>,
    /// 各台 iPad（随航名称）的屏幕物理像素（长边、短边），iPad App 连接时报上来；三档显示大小按它算。
    /// 没有时用 config.toml 的 ipad_pixels。
    #[serde(default)]
    pub ipad_screens: std::collections::HashMap<String, [u32; 2]>,
    /// 各台 iPad 按随航屏校正过的三档尺寸（更大文字、默认、更多空间），开机直接用，不用等随航屏出现再校正一次（少闪一次）。
    #[serde(default)]
    pub ipad_sizes: std::collections::HashMap<String, Vec<[u32; 2]>>,
    /// 每块实体显示器上次用的模式（键：厂商-型号-序列号，值：宽、高、像素宽、像素高、刷新率×100），插回后系统选错时改回去。
    #[serde(default)]
    pub display_modes: std::collections::HashMap<String, (usize, usize, usize, usize, usize)>,
}

impl Saved {
    pub fn load(dir: &Path) -> Saved {
        std::fs::read(dir.join("state.json")).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
    }

    pub fn save(&self, dir: &Path) {
        let r = serde_json::to_vec_pretty(self).map_err(|e| e.to_string()).and_then(|j| {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
            let tmp = dir.join("state.json.tmp");
            std::fs::write(&tmp, j).map_err(|e| e.to_string())?;
            std::fs::rename(tmp, dir.join("state.json")).map_err(|e| e.to_string())
        });
        if let Err(e) = r {
            warn!("保存状态失败：{e}");
        }
    }
}
