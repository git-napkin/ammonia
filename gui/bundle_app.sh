#!/bin/sh
set -eu

BUILD_PATH="$1"
OUT_APP="$2"
SRC_ROOT="$3"

BIN="$BUILD_PATH/release/Ammonia"
if [ ! -x "$BIN" ]; then
    echo "Missing Ammonia binary at $BIN" >&2
    exit 1
fi

rm -rf "$OUT_APP"
mkdir -p "$OUT_APP/Contents/MacOS" "$OUT_APP/Contents/Resources"

cp "$BIN" "$OUT_APP/Contents/MacOS/Ammonia"
cp "$SRC_ROOT/gui/Info.plist" "$OUT_APP/Contents/Info.plist"
cp "$SRC_ROOT/gui/Resources/gearbox.icns" "$OUT_APP/Contents/Resources/gearbox.icns"
cp "$SRC_ROOT/gui/Resources/drill.png" "$OUT_APP/Contents/Resources/drill.png"

chmod 755 "$OUT_APP/Contents/MacOS/Ammonia"
codesign -f -s - "$OUT_APP/Contents/MacOS/Ammonia" 2>/dev/null || true
