#!/bin/bash
# Convenience build script.
# Builds both hack-touchid (interactive menu frontend) and vfs5011_daemon
# (background auth daemon) by invoking their individual build scripts.
#
# Run this from the folder containing all the vfs5011_*/supported_sensors.h
# files and the NBIS/ folder. For ax_probe (standalone AX diagnostic tool),
# build it separately — see README.
set -e

echo "==> Building hack-touchid..."
./build_client.sh

echo ""
echo "==> Building vfs5011_daemon..."
./build_daemon.sh

echo ""
echo "Build complete: ./hack-touchid and ./vfs5011_daemon"
