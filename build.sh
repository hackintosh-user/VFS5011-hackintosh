#!/bin/bash
# Convenience build script for vfs5011_matcher.
# Builds both vfs_client (interactive menu frontend) and vfs5011_daemon
# (background auth daemon) by invoking their individual build scripts.
#
# Run this from the folder containing all the vfs5011_* files and the
# NBIS/ folder. For ax_probe (standalone AX diagnostic tool), build it
# separately — see README.
set -e

echo "==> Building vfs_client..."
./build_client.sh

echo ""
echo "==> Building vfs5011_daemon..."
./build_daemon.sh

echo ""
echo "Build complete: ./vfs_client and ./vfs5011_daemon"
