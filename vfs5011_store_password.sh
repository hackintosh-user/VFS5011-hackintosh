#!/bin/bash
#
# vfs5011_store_password.sh
#
# Stores the macOS login password inside the encrypted VFSStore volume,
# alongside the fingerprint templates. Same trust model as templates:
# the encryption + root-only permissions + not-auto-mounted behavior
# ARE the protection — there's no extra encryption layer on top of the
# password file itself, because VFSStore already provides that.
#
# Run this whenever you need to set/update the stored password:
#   sudo ./vfs5011_store_password.sh
#
# You'll be prompted with no echo (like a normal sudo password prompt).

set -e

MOUNT_POINT="/private/var/db/vfsclient_data"
PASSWORD_FILE="$MOUNT_POINT/password.txt"

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root (sudo ./vfs5011_store_password.sh)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "== Mounting VFSStore =="
"$SCRIPT_DIR/vfs5011_volume_mount.sh" >/dev/null

# -s suppresses echo, same as a normal password prompt.
read -r -s -p "Enter the password the daemon should type on a fingerprint match: " PASSWORD
echo ""
read -r -s -p "Confirm: " PASSWORD_CONFIRM
echo ""

if [ "$PASSWORD" != "$PASSWORD_CONFIRM" ]; then
    echo "Passwords did not match. Nothing was saved." >&2
    "$SCRIPT_DIR/vfs5011_volume_unmount.sh" >/dev/null
    exit 1
fi

# printf, not echo -- avoids a trailing newline sneaking into the file
# and getting typed as part of the password later.
printf '%s' "$PASSWORD" > "$PASSWORD_FILE"
chown root:wheel "$PASSWORD_FILE"
chmod 600 "$PASSWORD_FILE"

unset PASSWORD PASSWORD_CONFIRM

echo "== Unmounting VFSStore =="
"$SCRIPT_DIR/vfs5011_volume_unmount.sh" >/dev/null

echo "Password stored."
