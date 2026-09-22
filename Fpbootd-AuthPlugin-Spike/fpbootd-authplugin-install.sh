#!/bin/bash
# fpbootd-authplugin-install.sh
# Stage B: registers FpbootdSpike.bundle into the system.login.console
# authorization mechanism chain. Always backs up the current rule first
# and is fully reversible via fpbootd-authplugin-uninstall.sh.
#
# SAFETY MODEL:
#  - FpbootdSpike always calls SetResult(kAuthorizationResultAllow),
#    so it cannot itself deny a login.
#  - The one real risk is a HANG (plugin never returns) rather than a
#    denial. loginwindow has its own pluginhost timeout, but this has
#    never been exercised in this project. Do NOT test this on the
#    only macOS install on the machine without a rescue path (Recovery
#    mode / another OS / SSH) ready to run the uninstall script's
#    restore step by hand if needed.
#  - This script is idempotent: running it twice will not add a
#    duplicate mechanism entry.

set -euo pipefail

BUNDLE_SRC="./FpbootdSpike.bundle"
BUNDLE_DST="/Library/Security/SecurityAgentPlugins/FpbootdSpike.bundle"
BACKUP_DIR="/Library/Security/fpbootd-backups"
MECHANISM_ENTRY="FpbootdSpike:invoke,privileged"
RULE_NAME="system.login.console"

if [[ $EUID -ne 0 ]]; then
    echo "Must run as root (sudo)." >&2
    exit 1
fi

if [[ ! -d "$BUNDLE_SRC" ]]; then
    echo "FpbootdSpike.bundle not found next to this script ($BUNDLE_SRC)." >&2
    exit 1
fi

command -v python3 >/dev/null 2>&1 || { echo "python3 required (Homebrew or Xcode CLT)." >&2; exit 1; }

mkdir -p "$BACKUP_DIR"
TS=$(date +%Y%m%d%H%M%S)
BACKUP_FILE="$BACKUP_DIR/system.login.console.$TS.plist"

echo "[1/5] Backing up current $RULE_NAME rule -> $BACKUP_FILE"
security authorizationdb read "$RULE_NAME" > "$BACKUP_FILE"
chmod 600 "$BACKUP_FILE"

echo "[2/5] Installing bundle -> $BUNDLE_DST"
mkdir -p /Library/Security/SecurityAgentPlugins
rm -rf "$BUNDLE_DST"
cp -R "$BUNDLE_SRC" "$BUNDLE_DST"
chown -R root:wheel "$BUNDLE_DST"
chmod -R 755 "$BUNDLE_DST"

echo "[3/5] Checking whether mechanism is already registered"
if security authorizationdb read "$RULE_NAME" 2>/dev/null | grep -q "$MECHANISM_ENTRY"; then
    echo "    Already present — skipping mechanism insert (idempotent)."
else
    echo "[4/5] Inserting mechanism entry: $MECHANISM_ENTRY"
    NEW_PLIST=$(mktemp /tmp/fpbootd_authdb.XXXXXX.plist)
    python3 - "$BACKUP_FILE" "$NEW_PLIST" "$MECHANISM_ENTRY" <<'PYEOF'
import plistlib, sys

src, dst, entry = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src, "rb") as f:
    data = plistlib.load(f)

mechs = data.get("mechanisms", [])
if entry not in mechs:
    # Insert as the FIRST mechanism: fires early, never blocks (always
    # Allow), and if it hangs, it hangs before the password field would
    # have appeared rather than after — the least-confusing failure mode
    # to be looking at from a physical login screen.
    mechs.insert(0, entry)
data["mechanisms"] = mechs

with open(dst, "wb") as f:
    plistlib.dump(data, f)
PYEOF
    security authorizationdb write "$RULE_NAME" < "$NEW_PLIST"
    rm -f "$NEW_PLIST"
fi

echo "[5/5] Verifying"
if security authorizationdb read "$RULE_NAME" 2>/dev/null | grep -q "$MECHANISM_ENTRY"; then
    echo "OK — FpbootdSpike is registered in $RULE_NAME."
    echo "Backup for rollback: $BACKUP_FILE"
    echo ""
    echo "Next: lock the screen or log out and watch /Library/Logs/fpbootd.log"
    echo "and Console.app (process: FpbootdSpike / pluginhost) during the next login."
    echo "If login hangs at any point, reboot to Recovery/another OS and run:"
    echo "  security authorizationdb write $RULE_NAME < $BACKUP_FILE"
else
    echo "FAILED — mechanism not found after write. Restoring backup now." >&2
    security authorizationdb write "$RULE_NAME" < "$BACKUP_FILE"
    exit 1
fi
