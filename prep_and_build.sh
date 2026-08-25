#!/bin/bash
#
# prep_and_build.sh
#
# Fixes the recurring "some .sh files aren't executable" problem
# (build.sh only ever chmod'd itself + build_client.sh + build_daemon.sh,
# never the runtime helper scripts like vfs5011_volume_mount.sh) by
# chmod +x'ing EVERY .sh file in this folder, and rebuilds both
# binaries from source.
#
# NOTE (v1.1): this used to also deploy the daemon itself (rebuild,
# copy to /usr/local/libexec/hack-touchid/, re-sign, re-grant
# Accessibility, reinstall the LaunchAgent) by shelling out to
# vfs5011_agent_install.sh directly. That's exactly what Deploy [3]
# in the client does -- doing it here too meant the daemon could end
# up live and running before the user ever launched the client once,
# which makes Deploy [3] pointless the first time around. Deploy is
# now the client's job alone. This script only gets the binaries
# built and the helper scripts executable; run the client yourself
# and use Deploy [3] when you're ready to actually install/reinstall
# the daemon.
#
# After this runs:
#
#   sudo ./hack-touchid
#
# then use Deploy [3] from the menu.
#
# Usage:
#   ./prep_and_build.sh
#
# Run this from inside the project folder (same folder as build.sh).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "== Fixing permissions on every .sh file in $SCRIPT_DIR =="
chmod +x ./*.sh
ls -la ./*.sh

echo
echo "== Reclaiming ownership of build artifacts (in case a previous sudo build left them root-owned) =="
CURRENT_USER="$(id -un)"
for bin in hack-touchid vfs5011_daemon metallica_mis_daemon; do
    if [ -e "$bin" ] && [ "$(stat -f '%Su' "$bin")" != "$CURRENT_USER" ]; then
        echo "  $bin is owned by $(stat -f '%Su' "$bin") -- reclaiming as $CURRENT_USER (you may be prompted for your password)"
        sudo chown "$CURRENT_USER":staff "$bin"
    fi
done

echo
echo "== Building client + daemon from source =="
./build.sh

echo
echo "Done. Binaries are built. Nothing has been deployed/installed yet."
echo "Run the client and use Deploy [3] from the menu when you're ready:"
echo "  sudo ./hack-touchid"
