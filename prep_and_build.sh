#!/bin/bash
#
# prep_and_build.sh
#
# Fixes the recurring "some .sh files aren't executable" problem
# (build.sh only ever chmod'd itself + build_client.sh + build_daemon.sh,
# never the runtime helper scripts like vfs5011_volume_mount.sh) by
# chmod +x'ing EVERY .sh file in this folder, then rebuilds both
# binaries from source.
#
# Usage:
#   ./prep_and_build.sh            # chmod all scripts + build client & daemon
#   ./prep_and_build.sh --deploy   # also runs the daemon's full install/deploy
#                                   step (rebuilds + copies to
#                                   /Library/Application Support/VFSDaemon/,
#                                   re-signs, re-grants Accessibility,
#                                   reinstalls the LaunchAgent). Requires sudo
#                                   and will prompt for your password.
#
# Run this from inside the project folder (same folder as build.sh).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "== Fixing permissions on every .sh file in $SCRIPT_DIR =="
chmod +x ./*.sh
ls -la ./*.sh

echo
echo "== Building client + daemon from source =="
./build.sh

echo
if [ "$1" == "--deploy" ]; then
    echo "== Deploying daemon (rebuild, install, re-sign, re-grant Accessibility, reload LaunchAgent) =="
    echo "This step needs root -- you may be prompted for your password."
    sudo ./vfs5011_agent_install.sh
else
    echo "Build complete. Daemon binary at ./vfs5011_daemon is NOT yet deployed --"
    echo "the LaunchAgent still runs the copy at /Library/Application Support/VFSDaemon/."
    echo "Re-run with --deploy to rebuild + install + reload the LaunchAgent in one step:"
    echo "  ./prep_and_build.sh --deploy"
fi
