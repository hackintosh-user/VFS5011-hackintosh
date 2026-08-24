#!/bin/bash
# Convenience build script.
# Builds hack-touchid (interactive menu frontend), vfs5011_daemon
# (background auth daemon), and metallica_mis_daemon (Metallica MIS
# pairing test harness, active-development only) by invoking their
# individual build scripts.
#
# Run this from the folder containing all the vfs5011_*/metallica_mis_*/
# supported_sensors.h files and the NBIS/ folder. For ax_probe
# (standalone AX diagnostic tool), build it separately — see README.
set -e

echo "==> Building hack-touchid..."
./build_client.sh

echo ""
echo "==> Building vfs5011_daemon..."
./build_daemon.sh

echo ""
echo "==> Building metallica_mis_daemon (pairing test harness)..."
./build_metallica_mis.sh

echo ""
echo "Build complete: ./hack-touchid, ./vfs5011_daemon, and ./metallica_mis_daemon"
