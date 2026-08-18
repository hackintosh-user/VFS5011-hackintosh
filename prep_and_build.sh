#!/bin/bash
#
# prep_and_build.sh
#
# Fixes the recurring "some .sh files aren't executable" problem
# (build.sh only ever chmod'd itself + build_client.sh + build_daemon.sh,
# never the runtime helper scripts like vfs5011_volume_mount.sh) by
# chmod +x'ing EVERY .sh file in this folder, rebuilds both binaries
# from source, and deploys the daemon (rebuild, copy to
# /usr/local/libexec/hack-touchid/, re-sign, re-grant Accessibility,
# reinstall the LaunchAgent).
#
# As of v1.0.5, the deploy step (vfs5011_agent_install.sh, invoked
# below) is the same one Deploy [3] in the client calls, and it lives
# at /usr/local/libexec/hack-touchid/ instead of the old "/Library/
# Application Support/VFSDaemon/" -- moved off a path with a space in
# it, which was a recurring source of quoting bugs across the sudoers
# rule, plist, and grant-accessibility scripts.
#
# NOTE (v1.1, active-development): this script is still VFS5011-only --
# it always runs vfs5011_agent_install.sh regardless of what's plugged
# in. Once a second sensor (e.g. UPEK) has a real capture backend and
# its own <sensor>_agent_install.sh, this needs the same
# detect-then-dispatch logic the client itself now has (see
# supported_sensors.h / hack_touchid_client.c) rather than hardcoding
# one installer. Not urgent while VFS5011 is the only working backend.
#
# After this runs, the daemon is live and up to date in the background --
# the only thing left to do yourself is run the client whenever you want
# to enroll/manage fingers or change settings:
#
#   sudo ./hack-touchid
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
for bin in hack-touchid vfs5011_daemon; do
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
echo "  sudo ./hack-touchid"
