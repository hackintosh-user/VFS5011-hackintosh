#!/bin/bash
# One-time (well -- every-rebuild, see below) TCC seed: grants
# Accessibility to the vfs5011 daemon binary directly by inserting
# into the system TCC database, bypassing the usual GUI consent
# prompt.
#
# This ONLY works because "Filesystem Protections" are disabled in
# this Hackintosh's csrutil configuration (confirmed via `csrutil
# status`). On a stock Mac with SIP fully enabled, this database is
# read-only even to root and this script would fail to write at all.
#
# IMPORTANT (added after hitting this on a fresh Tahoe install): an
# earlier version of this script inserted csreq = NULL, which appears
# to have worked on Sequoia's TCC but silently did nothing on Tahoe --
# the grant showed no error, but the daemon's AX/CGEvent calls all
# no-op'd as if Accessibility had never been granted. The daemon binary
# was (and still is) completely unsigned -- built with plain clang, no
# codesign step anywhere in the pipeline -- so there was no code
# identity for a NULL csreq to even plausibly validate against.
#
# Fix: ad-hoc sign the binary right here, derive its actual designated
# requirement, and store a REAL compiled csreq blob instead of NULL.
# Ad-hoc signing has no Developer ID / no notarization, just enough to
# give the binary a cdhash-based identity -- but that identity changes
# EVERY time the binary's bytes change, i.e. every rebuild-from-source
# Deploy. That's why hack-touchid-agent-install.sh now calls this script
# automatically after every build, instead of this being a true
# one-time step like it used to be.
#
# Run this AFTER the daemon binary is installed at its final path, and
# BEFORE starting it as a LaunchAgent — the grant is tied to the exact
# binary bytes.
#
# Usage: sudo ./hack-touchid-grant-accessibility.sh [path-to-daemon-binary]
#        (defaults to the standard install path if omitted)
set -e

DAEMON_PATH="${1:-/usr/local/libexec/hack-touchid/vfs5011_daemon}"
TCC_DB="/Library/Application Support/com.apple.TCC/TCC.db"

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root (sudo)." >&2
    exit 1
fi

if [ ! -f "$TCC_DB" ]; then
    echo "TCC database not found at: $TCC_DB" >&2
    exit 1
fi

if [ ! -f "$DAEMON_PATH" ]; then
    echo "Daemon binary not found at: $DAEMON_PATH" >&2
    echo "Install it there first (see deploy step), then re-run this script." >&2
    exit 1
fi

echo "Ad-hoc signing: $DAEMON_PATH"
# --force so re-signing an already-signed binary (e.g. re-running this
# script by hand after agent_install already signed it) doesn't error.
codesign --force --sign - "$DAEMON_PATH"

echo "Deriving the binary's actual designated requirement..."
REQ_TEXT=$(codesign -d -r- "$DAEMON_PATH" 2>&1 | awk -F ' => ' '/designated/{print $2}')
if [ -z "$REQ_TEXT" ]; then
    echo "Error: could not read back a designated requirement after signing -- codesign output was:" >&2
    codesign -d -r- "$DAEMON_PATH" 2>&1 >&2
    exit 1
fi
echo "  $REQ_TEXT"

REQ_BIN="$(mktemp)"
# -r= (equals, no space) tells csreq the argument is literal
# requirement source text, not a path to an already-compiled file.
csreq -r="$REQ_TEXT" -b "$REQ_BIN"
REQ_HEX=$(xxd -p "$REQ_BIN" | tr -d '\n')
rm -f "$REQ_BIN"

echo "Granting Accessibility to: $DAEMON_PATH"

sqlite3 "$TCC_DB" <<SQL
INSERT OR REPLACE INTO access
    (service, client, client_type, auth_value, auth_reason, auth_version,
     csreq, policy_id, indirect_object_identifier_type, indirect_object_identifier,
     indirect_object_code_identity, flags, pid, pid_version, boot_uuid)
VALUES
    ('kTCCServiceAccessibility', '$DAEMON_PATH', 1, 2, 2, 1,
     X'$REQ_HEX', NULL, NULL, 'UNUSED', NULL, NULL, NULL, NULL, 'UNUSED');
SQL

echo "Restarting system tccd so it picks up the change..."
launchctl kickstart -k system/com.apple.tccd.system 2>/dev/null || killall tccd 2>/dev/null || true

echo ""
echo "Done. Verify with:"
echo "  sqlite3 \"$TCC_DB\" \"SELECT service, client, auth_value, length(csreq) FROM access WHERE client='$DAEMON_PATH';\""
echo ""
echo "auth_value of 2 means Allowed. length(csreq) should now be a real"
echo "nonzero byte count (a compiled requirement blob), not 0/NULL."
echo "If it's missing entirely, the insert didn't take -- re-check that"
echo "Filesystem Protections are still disabled."
