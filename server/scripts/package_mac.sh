#!/bin/bash
# Builds the Mac app (demo 1.6): ~/Desktop/W2F_App/W2F.app -- the game client + the match in one app, WITH its own server inside.
#
#   scripts/package_mac.sh [path/to/work2fightgame]
#
# 1. the release server (make server-release) and the game data are copied into <project>/Content/W2F/Server/ (staged into the app as plain files;
#    the client starts it when the server address is this Mac, W2FClient.cpp StartLocalServer),
# 2. the catalog the app falls back on is refreshed (Content/W2F/Data/catalog.json),
# 3. (demo 1.9) the app icon: <project>/Build/Mac/Resources/Assets.xcassets with docs/app_icon/W2F_1024.png at every size,
# 4. Shipping build + cook + stage + pak + package + archive (RunUAT BuildCookRun), then Mac/work2fightgame-Mac-Shipping.app is renamed W2F.app.
# Needs the UE editor closed. From an automated shell where UBT's Xcode step fails ("Failed to launch task /bin/sh: Bad file descriptor"), point
# W2F_DEVELOPER_DIR at a mirror of Xcode.app whose xcodebuild gives it a clean stdin (updates/update-1.5.md); a normal Terminal does not need it.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
P="${1:-$HOME/Desktop/work2fightgame/work2fightgame}"
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
OUT="${W2F_APP_OUT:-$HOME/Desktop/W2F_App}"
[ -n "${W2F_DEVELOPER_DIR:-}" ] && export DEVELOPER_DIR="$W2F_DEVELOPER_DIR"

echo "== 1. the server the app carries"
make -C "$HERE" server-release sample-fights > /dev/null
rm -rf "$P/Content/W2F/Server"
mkdir -p "$P/Content/W2F/Server/data"
cp "$HERE/build/release/w2f_server" "$P/Content/W2F/Server/w2f_server"
chmod 755 "$P/Content/W2F/Server/w2f_server"
cp "$HERE"/data/*.json "$P/Content/W2F/Server/data/"

echo "== 2. the catalog"
cp "$HERE/docs/sample_fights/catalog.json" "$P/Content/W2F/Data/catalog.json"

echo "== 3. the app icon (docs/app_icon/W2F_1024.png, tools/make_client_art.py): the project's own asset catalog wins over the engine's"
CAT="$P/Build/Mac/Resources/Assets.xcassets"
rm -rf "$CAT"
cp -R "$UE_ROOT/Engine/Build/Mac/Resources/Assets.xcassets" "$CAT"
for f in "$CAT/AppIcon.appiconset/"icon_*.png; do
    b="$(basename "$f" .png)"; px="${b#icon_}"; px="${px%%x*}"; case "$b" in *@2x) px=$((px * 2));; esac
    sips -z "$px" "$px" "$HERE/docs/app_icon/W2F_1024.png" --out "$f" > /dev/null
done

echo "== 4. build, cook, package"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" work2fightgame Mac Shipping -Project="$P/work2fightgame.uproject"
"$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun -project="$P/work2fightgame.uproject" -platform=Mac -clientconfig=Shipping -skipbuild -cook -stage -pak -iostore \
    -package -archive -archivedirectory="$OUT" -map=/Game/W2F/Maps/L_Viewer -unattended -utf8output
rm -rf "$OUT/W2F.app"
mv "$OUT/Mac/work2fightgame-Mac-Shipping.app" "$OUT/W2F.app"
rmdir "$OUT/Mac" 2>/dev/null || true

SERVER_IN_APP="$(find "$OUT/W2F.app" -path '*W2F/Server/w2f_server' | head -1)"
if [ -z "$SERVER_IN_APP" ] || [ ! -x "$SERVER_IN_APP" ]; then echo "WARNING: the server is missing from the app or not executable ($SERVER_IN_APP)"; exit 1; fi
echo "done: $OUT/W2F.app (server: $SERVER_IN_APP)"
