#!/bin/bash
#
# build_spike.sh
#
# Builds fpbootd_spike.c into a proper CFBundle (.bundle) that macOS's
# Authorization Plugin loader can load -- FpbootdSpike.bundle.
#
# STAGE A ONLY. This script builds and packages the bundle to a local
# folder. It does NOT copy it into /Library/Security/SecurityAgentPlugins
# and does NOT touch the authorization database in any way -- the
# built bundle is inert until something explicitly does that (Stage B,
# a separate script, deliberately not written yet).
#
# Usage:
#   ./build_spike.sh
#
# After building, worth checking before trusting it at all:
#   nm FpbootdSpike.bundle/Contents/MacOS/fpbootd_spike | grep AuthorizationPluginCreate
#   otool -L FpbootdSpike.bundle/Contents/MacOS/fpbootd_spike
# The first should show AuthorizationPluginCreate as an exported (T) symbol --
# that's the entry point macOS's plugin loader looks for by name. The
# second should show it's linked against Security.framework and
# CoreFoundation.framework.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BUNDLE_NAME="FpbootdSpike.bundle"
EXECUTABLE_NAME="fpbootd_spike"

rm -rf "$BUNDLE_NAME"
mkdir -p "$BUNDLE_NAME/Contents/MacOS"

echo "Compiling $EXECUTABLE_NAME..."
clang -bundle -fPIC \
    fpbootd_spike.c \
    -o "$BUNDLE_NAME/Contents/MacOS/$EXECUTABLE_NAME" \
    -framework Security \
    -framework CoreFoundation \
    -Wall

cp Info.plist "$BUNDLE_NAME/Contents/Info.plist"

if ! plutil -lint "$BUNDLE_NAME/Contents/Info.plist" >/dev/null 2>&1; then
    echo "Error: Info.plist failed validation." >&2
    exit 1
fi

echo ""
echo "Built: $SCRIPT_DIR/$BUNDLE_NAME"
echo ""
echo "Before trusting this bundle at all, check it exports the right symbol:"
echo "  nm $BUNDLE_NAME/Contents/MacOS/$EXECUTABLE_NAME | grep AuthorizationPluginCreate"
echo ""
echo "This does NOT install or register anything. The bundle is just"
echo "sitting here until Stage B (not written yet) does something with it."
