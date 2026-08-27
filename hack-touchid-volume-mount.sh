#!/bin/bash
#
# hack-touchid-volume-mount.sh
#
# Mounts the encrypted HackTouchIDStore volume on demand, pulling its passphrase
# from the System keychain automatically (no human prompt). Meant to be
# called by the CLI/daemon right before touching templates, and paired
# with hack-touchid-volume-unmount.sh right after.
#
# Must run as root (the keychain item and the volume itself are both
# root-only).
#
# Usage: ./hack-touchid-volume-mount.sh
# On success, prints the mount point path on stdout (last line) so a
# caller can capture it: MOUNT_PATH=$(./hack-touchid-volume-mount.sh | tail -1)

set -e

VOLUME_NAME="HackTouchIDStore"
MOUNT_POINT="/private/var/db/vfsclient_data"
KEYCHAIN_SERVICE="com.mohammad.vfsclient.volume"
KEYCHAIN_ACCOUNT="vfsstore"

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root." >&2
    exit 1
fi

# Already mounted? Nothing to do, just report the path (idempotent —
# safe for the daemon to call even if a previous session left it mounted).
if diskutil info "$VOLUME_NAME" 2>/dev/null | grep -q "Mounted.*Yes"; then
    echo "$MOUNT_POINT"
    exit 0
fi

PASSPHRASE=$(security find-generic-password \
    -a "$KEYCHAIN_ACCOUNT" \
    -s "$KEYCHAIN_SERVICE" \
    -w \
    /Library/Keychains/System.keychain 2>/dev/null)

if [ -z "$PASSPHRASE" ]; then
    echo "Could not retrieve volume passphrase from System keychain. Was setup run?" >&2
    exit 1
fi

# -stdinpassphrase reads the passphrase from stdin rather than an
# interactive prompt, which is what makes this scriptable/unattended.
echo "$PASSPHRASE" | diskutil apfs unlockVolume "$VOLUME_NAME" -mountpoint "$MOUNT_POINT" -stdinpassphrase

# Passphrase is gone from the shell as soon as this script exits since
# it only ever lived in a local variable, never written to disk.

# CRITICAL: once mounted, the path reflects the VOLUME's own root
# directory permissions, not whatever the empty placeholder folder had
# before mounting. macOS defaults a freshly created APFS volume's root
# to root:admin 775 — which means any admin-group user (Mohammad's own
# login included) can read/write it. Re-enforce root:wheel/700 on
# EVERY mount, not just once during setup, or this silently regresses
# back to admin-writable.
chown root:wheel "$MOUNT_POINT"
chmod 700 "$MOUNT_POINT"

echo "$MOUNT_POINT"
