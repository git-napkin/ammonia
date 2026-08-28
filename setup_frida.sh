#!/bin/bash

set -e

FRIDA_VERSION=17.9.11
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
CACHE_DIR="$PROJECT_DIR/.frida-cache/$FRIDA_VERSION"

mkdir -p "$CACHE_DIR"

download() {
    local name="$1"
    local url="$2"
    if [ -f "$CACHE_DIR/$name" ]; then
        echo "$name already cached, skipping download..."
    else
        echo "Downloading $name..."
        curl -fL --output "$CACHE_DIR/$name" "$url"
    fi
}

download frida-gum-arm64e.xz \
    "https://github.com/frida/frida/releases/download/$FRIDA_VERSION/frida-gum-devkit-$FRIDA_VERSION-macos-arm64e.tar.xz"
download frida-gum-arm64.xz \
    "https://github.com/frida/frida/releases/download/$FRIDA_VERSION/frida-gum-devkit-$FRIDA_VERSION-macos-arm64.tar.xz"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
cd "$WORKDIR"

tar xf "$CACHE_DIR/frida-gum-arm64e.xz" libfrida-gum.a frida-gum.h
mv libfrida-gum.a libfrida-gum-arm64e.a
cp frida-gum.h "$PROJECT_DIR/syphon/frida-gum.h"

tar xf "$CACHE_DIR/frida-gum-arm64.xz" libfrida-gum.a
mv libfrida-gum.a libfrida-gum-arm64.a

if ! test -f "libfrida-gum-arm64e.a" || ! test -f "libfrida-gum-arm64.a"; then
    echo Failed to extract all libfrida-gum libraries
    exit 1
fi

lipo -create libfrida-gum-arm64e.a libfrida-gum-arm64.a -output libfrida-gum-arm64e-arm64.a

if ! test -f "libfrida-gum-arm64e-arm64.a"; then
    echo Failed to create libfrida-gum-arm64e-arm64.a
    exit 1
fi

echo "Building fat library and shared dylib..."

cp libfrida-gum-arm64e-arm64.a "$PROJECT_DIR"

clang -arch arm64e -arch arm64 -lresolv -fpic -shared -Wl,-all_load libfrida-gum-arm64e-arm64.a -o fridagum.dylib

cp fridagum.dylib "$PROJECT_DIR"

echo "Done. Built: libfrida-gum-arm64e-arm64.a, fridagum.dylib"
