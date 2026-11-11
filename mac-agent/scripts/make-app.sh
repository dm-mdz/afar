#!/bin/sh
# 在 Mac 上打包 Afar.app 和 Afar-<版本>.dmg（AFAR-008）。在仓库根目录或任意位置运行：sh mac-agent/scripts/make-app.sh
# 没有开发者账号时用"临时签名"（ad-hoc）：第一次打开要在 系统设置 › 隐私与安全性 里点"仍要打开"；
# 每次重新打包签名都会变，蓝牙等授权要重新允许一次。开发者账号生效后：
#   AFAR_SIGN_ID="Developer ID Application: 你的名字 (TEAMID)" sh mac-agent/scripts/make-app.sh
# 再按 README 的说明公证（notarytool）。
set -e
cd "$(dirname "$0")/../.."
VER=$(sed -n 's/^version = "\(.*\)"/\1/p' mac-agent/Cargo.toml | head -1)
# 发出去的程序里不留打包这台电脑的路径（报错信息里的源码位置会带上依赖库和项目所在的目录）
CARGO_DIR=${CARGO_HOME:-$HOME/.cargo}
export RUSTFLAGS="${RUSTFLAGS:-} --remap-path-prefix=$CARGO_DIR=/cargo --remap-path-prefix=$(pwd)=/afar"
cargo build -p afar-agent --release --locked
for p in "$HOME" "$(pwd)" "$CARGO_DIR"; do
  if strings -a target/release/afar-agent | grep -qF "$p"; then
    echo "程序里还留着本机路径 $p，停止打包" >&2
    exit 1
  fi
done

OUT=target/afar-app
APP="$OUT/Afar.app"
rm -rf "$OUT"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp target/release/afar-agent "$APP/Contents/MacOS/afar-agent"
sed "s/__VERSION__/$VER/g" mac-agent/app/Info.plist > "$APP/Contents/Info.plist"

# 图标：和 iPad App 同一个 logo；Mac 版自带圆角底板和阴影（1024×1024，透明边，按苹果图标网格留边）
SET="$OUT/AppIcon.iconset"
mkdir -p "$SET"
for s in 16 32 128 256 512; do
  sips -z $s $s mac-agent/app/AppIcon.png --out "$SET/icon_${s}x${s}.png" >/dev/null
  d=$((s * 2))
  sips -z $d $d mac-agent/app/AppIcon.png --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$SET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$SET"

if [ -n "$AFAR_SIGN_ID" ]; then
  codesign --force --options runtime --timestamp -s "$AFAR_SIGN_ID" "$APP"
else
  codesign --force -s - "$APP"
fi
codesign --verify --strict --verbose=1 "$APP"

# 磁盘映像：Afar.app + 指向"应用程序"的快捷方式，打开后拖过去
STAGE="$OUT/dmg"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/应用程序"
hdiutil create -volname "Afar" -srcfolder "$STAGE" -ov -format UDZO "$OUT/Afar-$VER.dmg" >/dev/null
rm -rf "$STAGE"
echo "已生成：$APP"
echo "已生成：$OUT/Afar-$VER.dmg"
