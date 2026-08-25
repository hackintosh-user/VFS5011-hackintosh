#!/bin/bash
# Builds the menu bar app and packages it as a zip ready for a GitHub
# Release asset. Run from the vfs5011-menubar/ folder.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "==> Building app"
./build_menubar_app.sh

VERSION="${1:-1.0.0}"
ZIP_NAME="HackintoshTouchID-v${VERSION}.zip"

echo "==> Packaging release zip"
cd build
rm -f "../$ZIP_NAME"
zip -r "../$ZIP_NAME" "Hackintosh Touch-ID.app" > /dev/null
cd ..

echo "==> Done: $ZIP_NAME"
echo "    Upload this file as a Release asset on GitHub."
echo "    Suggested tag: menubar-v${VERSION}"
