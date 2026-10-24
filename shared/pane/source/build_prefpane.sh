#!/bin/bash
# Builds "HackTouchID.prefPane" (macOS only, needs swiftc from Xcode or the
# Command Line Tools). Output: shared/pane/build/HackTouchID.prefPane
#
# This script only BUILDS the pane. It never installs it. Installing is the
# client's job: [I] Install Preference Pane, or the question at the end of
# Deploy [3]. Both copy the pre-compiled bundle from shared/pane/build.
#
# The pane version always follows the client version: it is read from
# VERSION.txt in the repo root and written into the bundle's Info.plist.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$HERE"   # this script sits next to the sources: shared/pane/source/
ASSETS_DIR="$HERE/../../../Hackintosh-TouchID-Menubar/Assets"   # optional, icon is skipped if missing
BUILD_DIR="$HERE/../build"   # shared/pane/build/
PANE="$BUILD_DIR/HackTouchID.prefPane"

# Fingerprint of what goes into the pane: the Swift source and the Info.plist template. The client
# version is left out on purpose, so a VERSION.txt bump alone never makes a committed bundle look stale.
# It is stamped into the bundle (HTIDPaneSourceHash) so the client can tell whether the committed or the
# installed pane was built from the current source. "--source-hash" prints it and exits.
source_hash() {
    cat "$SRC_DIR/HackTouchIDPane.swift" "$SRC_DIR/Info.plist" | shasum -a 256 | cut -c1-16
}
if [ "${1:-}" = "--source-hash" ]; then
    source_hash
    exit 0
fi

echo "==> Cleaning old pane bundle"
rm -rf "$PANE"
mkdir -p "$PANE/Contents/MacOS" "$PANE/Contents/Resources"

echo "==> Compiling Swift source"
# Loaded in-process by System Settings, so this is a bundle, not an app.
# Pinned to macOS 14.0 like the menu bar app (the project floor).
swiftc "$SRC_DIR/HackTouchIDPane.swift" \
    -o "$PANE/Contents/MacOS/HackTouchID" \
    -target x86_64-apple-macosx14.0 \
    -parse-as-library \
    -Xlinker -bundle \
    -framework Cocoa \
    -framework PreferencePanes

echo "==> Copying Info.plist"
cp "$SRC_DIR/Info.plist" "$PANE/Contents/Info.plist"

# Pane version = client version (VERSION= line of the repo's VERSION.txt).
VERSION_FILE="$HERE/../../../VERSION.txt"
CLIENT_VERSION=""
if [ -f "$VERSION_FILE" ]; then
    CLIENT_VERSION="$(sed -n 's/^VERSION=//p' "$VERSION_FILE" | head -1 | tr -d '[:space:]')"
fi
if [ -n "$CLIENT_VERSION" ]; then
    echo "==> Setting pane version to $CLIENT_VERSION (client version)"
    PB=/usr/libexec/PlistBuddy
    for key in CFBundleShortVersionString CFBundleVersion; do
        "$PB" -c "Set :$key $CLIENT_VERSION" "$PANE/Contents/Info.plist" 2>/dev/null || \
            "$PB" -c "Add :$key string $CLIENT_VERSION" "$PANE/Contents/Info.plist"
    done
else
    echo "    warning: no VERSION= in $VERSION_FILE, keeping the version from Info.plist"
fi

echo "==> Stamping the source hash and build date"
PBS=/usr/libexec/PlistBuddy
for kv in "HTIDPaneSourceHash:$(source_hash)" "HTIDPaneBuildDate:$(date -u +%Y-%m-%d)"; do
    key="${kv%%:*}"; val="${kv#*:}"
    "$PBS" -c "Set :$key $val" "$PANE/Contents/Info.plist" 2>/dev/null || \
        "$PBS" -c "Add :$key string $val" "$PANE/Contents/Info.plist"
done

echo "==> Building the pane icon from the menu bar app icon set"
if [ -f "$ASSETS_DIR/AppIcon.iconset/icon_128x128.png" ]; then
    tiffutil -cathidpicheck "$ASSETS_DIR/AppIcon.iconset/icon_128x128.png" \
        "$ASSETS_DIR/AppIcon.iconset/icon_128x128@2x.png" \
        -out "$PANE/Contents/Resources/PaneIcon.tiff" 2>/dev/null || \
        cp "$ASSETS_DIR/AppIcon.iconset/icon_128x128.png" "$PANE/Contents/Resources/PaneIcon.png"
fi

echo "==> Ad-hoc code signing"
codesign --force --deep --sign - "$PANE"

echo "==> Done: $PANE"
echo "    Built only, NOT installed. Install it from the client:"
echo "    sudo hack-touchid, then [I] Install Preference Pane (or answer Y at the end of [3] Deploy)."
