#!/bin/bash
#
# prep_and_build.sh
#
# Fixes the recurring "some .sh files aren't executable" problem
# (build.sh only ever chmod'd itself + build_client.sh + build_daemon.sh,
# never the runtime helper scripts like vfs5011_volume_mount.sh) by
# chmod +x'ing EVERY .sh file in this folder, rebuilds both binaries
# from source, and deploys the daemon (rebuild, copy to
# /usr/local/libexec/vfs5011/, re-sign, re-grant Accessibility,
# reinstall the LaunchAgent).
#
# As of v1.0.5, the deploy step (vfs5011_agent_install.sh, invoked
# below) is the same one Deploy [3] in vfs_client calls, and it lives
# at /usr/local/libexec/vfs5011/ instead of the old "/Library/
# Application Support/VFSDaemon/" -- moved off a path with a space in
# it, which was a recurring source of quoting bugs across the sudoers
# rule, plist, and grant-accessibility scripts.
#
# After this runs, the daemon is live and up to date in the background --
# the only thing left to do yourself is run the client whenever you want
# to enroll/manage fingers or change settings:
#
#   sudo ./vfs_client
#
# Usage:
#   ./prep_and_build.sh
#
# Run this from inside the project folder (same folder as build.sh).
# The daemon deploy step needs root and will prompt you for your
# password via sudo.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "== Fixing permissions on every .sh file in $SCRIPT_DIR =="
chmod +x ./*.sh
ls -la ./*.sh

echo
echo "== Reclaiming ownership of build artifacts (in case a previous sudo build left them root-owned) =="
CURRENT_USER="$(id -un)"
for bin in vfs_client vfs5011_daemon; do
    if [ -e "$bin" ] && [ "$(stat -f '%Su' "$bin")" != "$CURRENT_USER" ]; then
        echo "  $bin is owned by $(stat -f '%Su' "$bin") -- reclaiming as $CURRENT_USER (you may be prompted for your password)"
        sudo chown "$CURRENT_USER":staff "$bin"
    fi
done

echo
echo "== Building client + daemon from source =="
./build.sh

echo
echo "== Deploying daemon (rebuild, install, re-sign, re-grant Accessibility, reload LaunchAgent) =="
echo "This step needs root -- you may be prompted for your password."
sudo ./vfs5011_agent_install.sh

echo
echo "Done. Daemon is deployed and running in the background."
echo "Run the client yourself whenever you need it:"
echo "  sudo ./vfs_client"
