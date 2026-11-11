//! 把 Info.plist 嵌进 afar-agent 程序里（命令行程序没有 .app 包）。
//! 用蓝牙要有"为什么用蓝牙"的说明，否则 macOS 不让访问蓝牙。
fn main() {
    println!("cargo:rerun-if-changed=Info.plist");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        let plist = std::path::Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("Info.plist");
        println!("cargo:rustc-link-arg-bins=-Wl,-sectcreate,__TEXT,__info_plist,{}", plist.display());
    }
}
