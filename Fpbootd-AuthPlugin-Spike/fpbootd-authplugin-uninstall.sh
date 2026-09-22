#!/bin/bash
# fpbootd-authplugin-uninstall.sh
# Reverts Stage B: restores system.login.console from the most recent
# backup fpbootd-authplugin-install.sh made, then removes the bundle.
#
# Also runnable by hand from Recovery Mode / Single User Mode / another
# OS's Terminal if the login screen is ever unusable:
#   security authorizationdb write system.login.console < <backup path>

set -euo pipefail

BUNDLE_DST="/Library/Security/SecurityAgentPlugins/FpbootdSpike.bundle"
BACKUP_DIR="/Library/Security/fpbootd-backups"
RULE_NAME="system.login.console"

if [[ $EUID -ne 0 ]]; then
    echo "Must run as root (sudo)." >&2
    exit 1
fi

LATEST_BACKUP=$(ls -t "$BACKUP_DIR"/system.login.console.*.plist 2>/dev/null | head -n1 || true)

if [[ -z "$LATEST_BACKUP" ]]; then
    echo "No backup found in $BACKUP_DIR — nothing to restore automatically." >&2
    echo "If the login screen is broken and you have no backup, boot Recovery" >&2
    echo "Mode, mount the volume, and reset the rule with:" >&2
    echo "  security authorizationdb write system.login.console < /path/to/backup.plist" >&2
    exit 1
fi

echo "[1/3] Restoring $RULE_NAME from $LATEST_BACKUP"
security authorizationdb write "$RULE_NAME" < "$LATEST_BACKUP"

echo "[2/3] Removing bundle"
rm -rf "$BUNDLE_DST"

echo "[3/3] Verifying"
if security authorizationdb read "$RULE_NAME" 2>/dev/null | grep -q "FpbootdSpike"; then
    echo "WARNING: FpbootdSpike still referenced after restore — check $BACKUP_DIR manually." >&2
    exit 1
else
    echo "OK — $RULE_NAME restored, bundle removed."
fi
