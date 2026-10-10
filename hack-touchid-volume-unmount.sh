#!/bin/bash
#
# hack-touchid-volume-unmount.sh
#
# Unmounts HackTouchIDStore after the CLI/daemon is done touching templates,
# putting it back into its default "not visible anywhere" state.
#
# Unmounts by MOUNT_POINT first -- unambiguous even if more than one
# volume happens to be named HackTouchIDStore right now, since only
# one of them can be sitting at this specific path at a time. Falls
# back to the old by-name lookup for compatibility with anything still
# mounted the old way (e.g. left over from before this fix).
#
# Usage: ./hack-touchid-volume-unmount.sh

set -e

VOLUME_NAME="HackTouchIDStore"
MOUNT_POINT="/private/var/db/vfsclient_data"

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root." >&2
    exit 1
fi

if mount | grep -q " on $MOUNT_POINT "; then
    diskutil unmount "$MOUNT_POINT"
    echo "Unmounted $MOUNT_POINT."
    exit 0
fi

if diskutil info "$VOLUME_NAME" 2>/dev/null | grep -q "Mounted.*Yes"; then
    diskutil unmount "$VOLUME_NAME"
    echo "Unmounted $VOLUME_NAME."
else
    echo "$VOLUME_NAME was not mounted."
fi
