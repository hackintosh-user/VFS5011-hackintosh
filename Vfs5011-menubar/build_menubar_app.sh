#!/bin/bash
set -e
APP_NAME="VFS5011 Menu Bar"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/VFS5011MenuBar"
ASSETS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/assets"
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build"
APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"

echo "==> Cleaning old build"
rm -rf "$BUILD_DIR"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"

echo "==> Compiling Swift source"
swiftc "$SRC_DIR/AppDelegate.swift" \
    -o "$APP_BUNDLE/Contents/MacOS/VFS5011MenuBar" \
    -parse-as-library \
    -framework Cocoa \
    -framework UserNotifications \
    -framework ServiceManagement

echo "==> Copying Info.plist"
cp "$SRC_DIR/Info.plist" "$APP_BUNDLE/Contents/Info.plist"

echo "==> Copying menu bar status icon (template image)"
cp "$ASSETS_DIR/MenuBarIcon.png"     "$APP_BUNDLE/Contents/Resources/"
cp "$ASSETS_DIR/MenuBarIcon@2x.png"  "$APP_BUNDLE/Contents/Resources/"
cp "$ASSETS_DIR/MenuBarIcon@3x.png"  "$APP_BUNDLE/Contents/Resources/"

echo "==> Building AppIcon.icns from iconset (requires macOS iconutil)"
if [ -d "$ASSETS_DIR/AppIcon.iconset" ]; then
    iconutil -c icns "$ASSETS_DIR/AppIcon.iconset" -o "$APP_BUNDLE/Contents/Resources/AppIcon.icns"
    echo "    AppIcon.icns built."
else
    echo "    AppIcon.iconset not found, skipping app icon (menu bar icon still works)."
fi

echo "==> Ad-hoc code signing"
codesign --force --deep --sign - "$APP_BUNDLE"

echo "==> Done: $APP_BUNDLE"
echo "    Run:  open \"$APP_BUNDLE\""
