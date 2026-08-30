#!/bin/sh
set -eu

VERSION="1.0.0"
OUTPUT="${1:-Ammonia-${VERSION}.pkg}"
SRC="$(cd "$(dirname "$0")" && pwd)"

if ! test -f "$SRC/fridagum.dylib"; then
    echo "[+] Running Frida prerequisite setup..."
    sh "$SRC/setup_frida.sh"
fi

echo "[+] Building..."
mkdir -p "$SRC/.build"
cmake -S "$SRC" -B "$SRC/.build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DBUILD_ARM64E="${BUILD_ARM64E:-ON}"
cmake --build "$SRC/.build"

echo "[+] Staging..."
STAGING="$(mktemp -d)"
trap "rm -rf '$STAGING'" EXIT

CORE="$STAGING/root/private/var/ammonia/core"
mkdir -p "$CORE/tweaks" "$CORE/include" "$CORE/share"
mkdir -p "$STAGING/root/Applications"
mkdir -p "$STAGING/root/Library/LaunchDaemons"

cp "$SRC/.build/ammonia"                    "$CORE/"
codesign -f -s - "$CORE/ammonia"
cp "$SRC/.build/libinject.dylib"            "$CORE/"
cp "$SRC/.build/libopener.dylib"            "$CORE/"
cp "$SRC/fridagum.dylib"                    "$CORE/"
cp "$SRC/grant/com.ammonia.inject.plist"    "$CORE/share/"
cp "$SRC/grant/com.ammonia.inject.plist"    "$STAGING/root/Library/LaunchDaemons/"
if [ -f "$SRC/grant/ammonia.blacklist" ]; then
    cp "$SRC/grant/ammonia.blacklist"       "$CORE/"
    cp "$SRC/grant/ammonia.blacklist"       "$CORE/share/"
    chmod 644 "$CORE/ammonia.blacklist" "$CORE/share/ammonia.blacklist"
fi
cp "$SRC/include/playground_tweak.h"        "$CORE/include/"
cp -R "$SRC/.build/configurator.app"        "$STAGING/root/Applications/Ammonia.app/"

/usr/bin/sudo -A chown -R 0:0 "$STAGING/root"
chmod 755 "$CORE/ammonia"
chmod 755 "$CORE/libinject.dylib"
chmod 755 "$CORE/libopener.dylib"
chmod 755 "$CORE/fridagum.dylib"
chmod 755 "$CORE/tweaks"
chmod 644 "$CORE/share/com.ammonia.inject.plist"
chmod 644 "$STAGING/root/Library/LaunchDaemons/com.ammonia.inject.plist"
chmod 644 "$CORE/include/playground_tweak.h"

echo "[+] Building component package..."
pkgbuild --root "$STAGING/root" \
         --identifier "com.ammonia.core" \
         --version "$VERSION" \
         --install-location "/" \
         --scripts "$SRC/installer/scripts" \
         "$STAGING/AmmoniaCore.pkg" > /dev/null

echo "[+] Building distribution package..."
productbuild --distribution "$SRC/installer/Distribution.xml" \
             --package-path "$STAGING" \
             --resources "$SRC/installer" \
             "$SRC/$OUTPUT"

echo "[+] Created $OUTPUT"
echo "    Install: sudo installer -pkg \"$OUTPUT\" -target /"
