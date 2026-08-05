#!/bin/bash
#
# vfs5011_setup_volume.sh
#
# ONE-TIME setup: creates a dedicated APFS volume inside your existing
# boot container to hold fingerprint templates/captures, separate from
# the main macOS filesystem. This is NOT a physical partition — APFS
# containers share a free-space pool, so this is a lightweight volume
# that lives alongside your existing macOS volume with no repartitioning
# and no risk to your OpenCore ESP or other boot entries.
#
# Security properties this sets up:
#   1. Encrypted  -> protects against Arch/Ubuntu/Windows (booted on the
#                    same physical disk) reading it directly; macOS file
#                    permissions mean nothing to a different OS reading
#                    raw disk blocks, only encryption does.
#   2. root:wheel, mode 700 -> the actual access-control boundary against
#                    a non-root macOS user; this is what stops someone
#                    from swapping in their own template to gain access.
#   3. noauto in fstab -> not mounted at boot/login, doesn't show up
#                    under /Volumes day-to-day. The daemon/CLI mounts it
#                    on demand and unmounts after each use.
#   4. Encryption passphrase stored in the SYSTEM keychain (not your
#                    login keychain) -> lets a root daemon unlock it
#                    automatically with no human typing a password,
#                    while staying inaccessible to non-root processes.
#
# Run this ONCE, as root:
#   sudo ./vfs5011_setup_volume.sh
#
# It will print the new volume's UUID at the end — save that, the
# daemon/CLI mount helper will need it.

set -e

VOLUME_NAME="VFSStore"
QUOTA="100m"
MOUNT_POINT="/private/var/db/vfsclient_data"
KEYCHAIN_SERVICE="com.mohammad.vfsclient.volume"
KEYCHAIN_ACCOUNT="vfsstore"

if [ "$(id -u)" -ne 0 ]; then
    echo "This must be run as root (sudo ./vfs5011_setup_volume.sh)." >&2
    exit 1
fi

echo "== Finding the APFS container for the boot volume =="
# "/" is always the running system volume; grab its container's disk
# identifier (e.g. disk3) so we add the new volume to the SAME
# container rather than guessing a disk number.
CONTAINER_ID=$(diskutil info / | awk -F': *' '/APFS Container/{print $2}' | awk '{print $1}')
if [ -z "$CONTAINER_ID" ]; then
    echo "Could not determine the APFS container for /. Aborting." >&2
    exit 1
fi
echo "Boot container: $CONTAINER_ID"

echo "== Generating a random encryption passphrase =="
# 32 random bytes, base64-encoded -> plenty of entropy, never typed by
# a human, never displayed after this run.
PASSPHRASE=$(openssl rand -base64 32)

echo "== Storing passphrase in the SYSTEM keychain =="
# -a account, -s service, -w password. The System keychain (not the
# user's login keychain) is unlocked at boot for privileged processes
# and isn't tied to any one user's login session — appropriate for a
# root daemon reading it unattended.
security add-generic-password \
    -a "$KEYCHAIN_ACCOUNT" \
    -s "$KEYCHAIN_SERVICE" \
    -w "$PASSPHRASE" \
    /Library/Keychains/System.keychain

echo "== Creating the encrypted APFS volume (quota: $QUOTA) =="
diskutil apfs addVolume "$CONTAINER_ID" APFS "$VOLUME_NAME" \
    -quota "$QUOTA" \
    -passphrase "$PASSPHRASE"

# Grab the UUID of the volume we just created by name.
VOLUME_UUID=$(diskutil info "$VOLUME_NAME" | awk -F': *' '/Volume UUID/{print $2}' | awk '{print $1}')
if [ -z "$VOLUME_UUID" ]; then
    echo "Volume created, but could not read back its UUID — check 'diskutil apfs list'." >&2
    exit 1
fi
echo "Created volume UUID: $VOLUME_UUID"

echo "== Unmounting (it auto-mounted under /Volumes/$VOLUME_NAME on creation) =="
diskutil unmount "$VOLUME_NAME" || true

echo "== Preventing auto-mount at boot/login (adding noauto to /etc/fstab) =="
# vifs is the supported way to edit fstab on macOS (regenerates the
# fstab -> mount map cleanly). We append via a temp file since vifs is
# interactive; this constructs the line the same way vifs would.
FSTAB_LINE="UUID=$VOLUME_UUID none apfs rw,noauto"
if ! grep -q "$VOLUME_UUID" /etc/fstab 2>/dev/null; then
    echo "$FSTAB_LINE" >> /etc/fstab
    echo "Added to /etc/fstab: $FSTAB_LINE"
else
    echo "fstab entry for this UUID already exists, skipping."
fi

echo "== Creating mount point with root-only permissions =="
mkdir -p "$MOUNT_POINT"
chown root:wheel "$MOUNT_POINT"
chmod 700 "$MOUNT_POINT"

echo ""
echo "Setup complete."
echo "  Volume name : $VOLUME_NAME"
echo "  Volume UUID : $VOLUME_UUID"
echo "  Mount point : $MOUNT_POINT"
echo "  Quota       : $QUOTA"
echo "  Encrypted   : yes (passphrase in System keychain, service=$KEYCHAIN_SERVICE)"
echo ""
echo "The volume is currently UNMOUNTED and will not auto-mount on"
echo "reboot. Use vfs5011_volume_mount.sh to mount it on demand, and"
echo "vfs5011_volume_unmount.sh to unmount it after use."
