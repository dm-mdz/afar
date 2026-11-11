//! 缩放档位：按 iPad 的屏幕比例算出几档"看起来的分辨率"（点），全部是 HiDPI（像素 = 点 × 2），保证清晰、无黑边。

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Preset {
    pub key: &'static str,
    pub label: &'static str,
    /// 点（系统里"看起来像"的分辨率）
    pub w: u32,
    pub h: u32,
}

impl Preset {
    pub fn pixels(&self) -> (u32, u32) {
        (self.w * 2, self.h * 2)
    }
}

const LEVELS: [(&str, &str, f64); 3] =
    [("larger", "更大文字", 0.87), ("default", "默认", 1.0), ("more", "更多空间", 1.16)];

fn even(x: f64) -> u32 {
    ((x / 2.0).round() * 2.0) as u32
}

/// 由 iPad 横屏物理像素得到 3 档。iPad Air 4（2360×1640）的"默认"正好是 1180×820。
pub fn presets(ipad_pixels: [u32; 2]) -> Vec<Preset> {
    let base_w = ipad_pixels[0] as f64 / 2.0;
    let ratio = ipad_pixels[1] as f64 / ipad_pixels[0] as f64;
    LEVELS
        .iter()
        .map(|&(key, label, f)| {
            // "默认"就是屏幕的一半（不取偶：iPad mini 2266×1488 的一半是 1133×744，取偶成 1134 就和随航屏对不上）
            if key == "default" {
                return Preset { key, label, w: ipad_pixels[0] / 2, h: ipad_pixels[1] / 2 };
            }
            let w = even(base_w * f);
            let h = even(w as f64 * ratio);
            Preset { key, label, w, h }
        })
        .collect()
}

/// 用保存过的三档尺寸（更大文字、默认、更多空间，见 `sizes_of`）替换算出来的；个数不对就不用。
pub fn with_sizes(base: &[Preset], sizes: &[[u32; 2]]) -> Vec<Preset> {
    if sizes.len() != base.len() {
        return base.to_vec();
    }
    base.iter().zip(sizes).map(|(p, s)| Preset { w: s[0], h: s[1], ..*p }).collect()
}

pub fn sizes_of(presets: &[Preset]) -> Vec<[u32; 2]> {
    presets.iter().map(|p| [p.w, p.h]).collect()
}

/// 配置里的档位名 → 档位；auto 或写错都回到"默认"。
pub fn pick(presets: &[Preset], key: &str) -> Preset {
    let key = if key == "auto" { "default" } else { key };
    presets
        .iter()
        .copied()
        .find(|p| p.key == key)
        .unwrap_or_else(|| presets.iter().copied().find(|p| p.key == "default").unwrap())
}

/// 虚拟屏的模式列表，`want` 排第一个。
/// - 写法 0（一直用的，"默认"档实测可用）：给像素尺寸（2 倍），比如 2360×1640。
/// - 写法 1（重建后还不是清晰模式时再试）：照 Chromium 建 HiDPI 虚拟屏的写法，只给"点"尺寸（像素的一半），
///   最大像素仍是 2 倍（见 `vd_max`）。
///
/// 实测（AFAR-001）：想要的档位不排在第一个时，切换 HiDPI 可能返回 1001；排第一个，虚拟屏一创建出来就是这个档位。
pub fn vd_modes(presets: &[Preset], want: Preset, style: u8) -> Vec<(u32, u32)> {
    let size = |p: Preset| if style == 0 { p.pixels() } else { (p.w, p.h) };
    let mut v = vec![size(want)];
    v.extend(presets.iter().copied().filter(|p| p.key != want.key).map(size));
    v
}

/// 虚拟屏的最大像素：各档像素尺寸里最大的（两种写法一样）。
pub fn vd_max(presets: &[Preset]) -> (u32, u32) {
    (presets.iter().map(|p| p.w * 2).max().unwrap_or(0), presets.iter().map(|p| p.h * 2).max().unwrap_or(0))
}

/// 三档改用随航屏自己支持的清晰尺寸（Mac 的显示器设置里 iPad 本来就有"更大文字 / 默认 / 更多空间"）：
/// 两块屏从一开始就一样大，镜像时不会对不上（v0.4.0 测试："更多空间"按比例算出的 1368×950 随航屏没有，
/// 系统给随航屏选了 800×556）。
/// `modes`：随航屏的模式（点宽、点高、像素宽、像素高、能当桌面用）。"默认"不动；"更大文字"取比默认小的里面最大的、
/// "更多空间"取比默认大的里面最小的，都要和 iPad 同样宽高比、差别不超过 35%；找不到的档保留原来算的。
pub fn from_sidecar(base: &[Preset], modes: &[(usize, usize, usize, usize, bool)]) -> Vec<Preset> {
    let d = pick(base, "default");
    let ratio = d.h as f64 / d.w as f64;
    let ok: Vec<(u32, u32)> = modes
        .iter()
        .filter(|m| m.4 && m.2 == m.0 * 2 && m.3 == m.1 * 2 && m.0 > 0)
        .filter(|m| ((m.1 as f64 / m.0 as f64) - ratio).abs() < 0.015)
        .map(|m| (m.0 as u32, m.1 as u32))
        .collect();
    let dw = d.w as f64;
    let smaller = ok.iter().filter(|m| m.0 < d.w && m.0 as f64 >= dw * 0.65).max_by_key(|m| m.0);
    let bigger = ok.iter().filter(|m| m.0 > d.w && m.0 as f64 <= dw * 1.35).min_by_key(|m| m.0);
    base.iter()
        .map(|p| {
            let pick = match p.key {
                "larger" => smaller,
                "more" => bigger,
                _ => None,
            };
            match pick {
                Some(&(w, h)) => Preset { w, h, ..*p },
                None => *p,
            }
        })
        .collect()
}

/// 随航屏镜像虚拟屏时，它自己的模式要和虚拟屏一样（看起来 w×h、像素 2 倍）。
/// v0.4.0 测试："更多空间"时虚拟屏 1368×950 是对的，随航屏却被系统选成 800×556，两边尺寸对不上。
/// `cur`：随航屏当前（点宽、点高、像素宽、像素高）；`has_mode`：它的模式列表里有没有这个清晰模式。
#[derive(Debug, PartialEq)]
pub enum Follow {
    Ok,
    /// 去把随航屏切到这个模式
    Switch,
    /// 随航屏没有这个清晰模式
    Missing,
}

pub fn follow(cur: Option<(usize, usize, usize, usize)>, want: Preset, has_mode: bool) -> Follow {
    let (w, h) = (want.w as usize, want.h as usize);
    match cur {
        Some((cw, ch, pw, ph)) if cw == w && ch == h && pw == w * 2 && ph == h * 2 => Follow::Ok,
        _ if has_mode => Follow::Switch,
        _ => Follow::Missing,
    }
}
