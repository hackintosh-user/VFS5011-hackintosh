#!/bin/bash
# fpbootd-authplugin-uninstall.sh
# Reverts the registration: removes FpbootdSpike from the live
# system.login.console rule, then removes the bundle.
#
# The entry is removed from the LIVE rule rather than restoring "the
# latest backup": every install run makes a backup, and a backup taken
# after an earlier install already contains FpbootdSpike, so restoring it
# would leave the rule pointing at a plugin that is about to be deleted.
# The bundle is only removed once the rule is verified clean.
#
# Also runnable by hand from Recovery Mode / Single User Mode / another
# OS's Terminal if the login screen is ever unusable:
#   security authorizationdb write system.login.console < <clean backup path>
# (a clean backup is one that does not mention FpbootdSpike).

set -euo pipefail

BUNDLE_DST="/Library/Security/SecurityAgentPlugins/FpbootdSpike.bundle"
BACKUP_DIR="/Library/Security/fpbootd-backups"
RULE_NAME="system.login.console"

if [[ $EUID -ne 0 ]]; then
    echo "Must run as root (sudo)." >&2
    exit 1
fi

LIVE_RULE=$(mktemp /tmp/fpbootd_live.XXXXXX.plist)
CLEAN_RULE=$(mktemp /tmp/fpbootd_clean.XXXXXX.plist)
trap 'rm -f "$LIVE_RULE" "$CLEAN_RULE"' EXIT

echo "[1/3] Removing FpbootdSpike from the live $RULE_NAME rule"
security authorizationdb read "$RULE_NAME" > "$LIVE_RULE"

if ! grep -q "FpbootdSpike" "$LIVE_RULE"; then
    echo "    Rule does not reference FpbootdSpike -- nothing to remove from it."
elif command -v python3 >/dev/null 2>&1; then
    python3 - "$LIVE_RULE" "$CLEAN_RULE" <<'PYEOF'
import plistlib, sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = plistlib.load(f)

data["mechanisms"] = [m for m in data.get("mechanisms", [])
                      if not str(m).startswith("FpbootdSpike:")]

with open(dst, "wb") as f:
    plistlib.dump(data, f)
PYEOF
    security authorizationdb write "$RULE_NAME" < "$CLEAN_RULE"
else
    # No python3: fall back to the newest backup that does NOT mention
    # FpbootdSpike (never blindly the newest one).
    FALLBACK=""
    for f in "$BACKUP_DIR"/system.login.console.*.plist; do
        [[ -e "$f" ]] || continue
        grep -q "FpbootdSpike" "$f" && continue
        # keep the newest clean backup
        if [[ -z "$FALLBACK" || "$f" -nt "$FALLBACK" ]]; then FALLBACK="$f"; fi
    done
    if [[ -z "$FALLBACK" ]]; then
        echo "python3 not found and no clean backup in $BACKUP_DIR." >&2
        echo "Bundle NOT removed. From Recovery Mode, restore a rule that does" >&2
        echo "not mention FpbootdSpike with:" >&2
        echo "  security authorizationdb write $RULE_NAME < /path/to/clean-backup.plist" >&2
        exit 1
    fi
    echo "    python3 not found, restoring clean backup $FALLBACK"
    security authorizationdb write "$RULE_NAME" < "$FALLBACK"
fi

echo "[2/3] Verifying the rule is clean"
if security authorizationdb read "$RULE_NAME" 2>/dev/null | grep -q "FpbootdSpike"; then
    echo "WARNING: FpbootdSpike still referenced in $RULE_NAME. Bundle NOT removed." >&2
    echo "Check $BACKUP_DIR for a backup that does not mention FpbootdSpike and restore it by hand." >&2
    exit 1
fi

echo "[3/3] Removing bundle"
rm -rf "$BUNDLE_DST"
echo "OK -- $RULE_NAME no longer references FpbootdSpike, bundle removed."
