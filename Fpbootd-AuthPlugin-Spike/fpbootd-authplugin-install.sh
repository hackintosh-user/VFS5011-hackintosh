#!/bin/bash
# fpbootd-authplugin-install.sh
# Registers FpbootdSpike.bundle into the system.login.console
# authorization mechanism chain, right after builtin:auto-login,privileged.
# Backs up the current rule first (only when the rule is still clean) and
# is fully reversible via fpbootd-authplugin-uninstall.sh.
#
# SAFETY MODEL:
#  - FpbootdSpike always calls SetResult(kAuthorizationResultAllow),
#    so it cannot itself deny a login.
#  - The one real risk is a HANG (plugin never returns) rather than a
#    denial. Every blocking call in the plugin is bounded by a timeout,
#    but do NOT test this on the only macOS install on the machine
#    without a rescue path (Recovery mode / another OS / SSH) ready to
#    run the uninstall script's restore step by hand if needed.
#  - Idempotent: running it twice relocates the entry, it never adds a
#    duplicate. Backups only ever hold the rule WITHOUT FpbootdSpike, so
#    a rerun can never overwrite a clean backup with a dirty one.
#  - If the rule does not contain builtin:auto-login,privileged or
#    builtin:authenticate,privileged, the script aborts before changing
#    anything.

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
LIVE_RULE=$(mktemp /tmp/fpbootd_live.XXXXXX.plist)
NEW_PLIST=$(mktemp /tmp/fpbootd_authdb.XXXXXX.plist)
trap 'rm -f "$LIVE_RULE" "$NEW_PLIST"' EXIT

security authorizationdb read "$RULE_NAME" > "$LIVE_RULE"

echo "[1/4] Backing up current $RULE_NAME rule"
if grep -q "FpbootdSpike" "$LIVE_RULE"; then
    echo "    Rule already contains FpbootdSpike -- keeping the existing clean backup(s) in $BACKUP_DIR."
    BACKUP_FILE="(see $BACKUP_DIR, oldest file)"
else
    BACKUP_FILE="$BACKUP_DIR/system.login.console.$TS.plist"
    cp "$LIVE_RULE" "$BACKUP_FILE"
    chmod 600 "$BACKUP_FILE"
    echo "    -> $BACKUP_FILE"
fi

echo "[2/4] Building the new rule (nothing is changed yet)"
# Removes any existing occurrence first, then inserts right after
# builtin:auto-login,privileged. Position 0 stalled the boot progress
# bar until a swipe resolved, and gains nothing now that password-skip
# has been ruled out, so it is no longer used.
python3 - "$LIVE_RULE" "$NEW_PLIST" "$MECHANISM_ENTRY" <<'PYEOF'
import plistlib, sys

src, dst, entry = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src, "rb") as f:
    data = plistlib.load(f)

mechs = [m for m in data.get("mechanisms", []) if m != entry]

if "builtin:auto-login,privileged" in mechs:
    idx = mechs.index("builtin:auto-login,privileged") + 1
elif "builtin:authenticate,privileged" in mechs:
    idx = mechs.index("builtin:authenticate,privileged")
else:
    sys.stderr.write("Neither builtin:auto-login,privileged nor "
                     "builtin:authenticate,privileged found in the rule; "
                     "refusing to guess a position. Nothing was changed.\n")
    sys.exit(1)

mechs.insert(idx, entry)
data["mechanisms"] = mechs

with open(dst, "wb") as f:
    plistlib.dump(data, f)
PYEOF

echo "[3/4] Installing bundle -> $BUNDLE_DST"
mkdir -p /Library/Security/SecurityAgentPlugins
rm -rf "$BUNDLE_DST"
cp -R "$BUNDLE_SRC" "$BUNDLE_DST"
chown -R root:wheel "$BUNDLE_DST"
chmod -R 755 "$BUNDLE_DST"

echo "[4/4] Registering $MECHANISM_ENTRY in $RULE_NAME and verifying"
security authorizationdb write "$RULE_NAME" < "$NEW_PLIST"

if security authorizationdb read "$RULE_NAME" 2>/dev/null | grep -q "$MECHANISM_ENTRY"; then
    echo "OK -- FpbootdSpike is registered in $RULE_NAME."
    echo "Backup for rollback: $BACKUP_FILE"
    echo ""
    echo "Next: lock the screen or log out and watch /Library/Logs/fpbootd.log"
    echo "and Console.app (process: FpbootdSpike / authorizationhost) during the next login."
    echo "If login hangs at any point, reboot to Recovery/another OS and run:"
    echo "  security authorizationdb write $RULE_NAME < <a clean backup from $BACKUP_DIR>"
    echo "or just run fpbootd-authplugin-uninstall.sh."
else
    echo "FAILED -- mechanism not found after write. Restoring the previous rule now." >&2
    security authorizationdb write "$RULE_NAME" < "$LIVE_RULE"
    exit 1
fi
