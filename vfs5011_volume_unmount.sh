#!/bin/bash
#
# vfs5011_volume_unmount.sh
#
# Unmounts VFSStore after the CLI/daemon is done touching templates,
# putting it back into its default "not visible anywhere" state.
#
# Usage: ./vfs5011_volume_unmount.sh

set -e

VOLUME_NAME="VFSStore"

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root." >&2
    exit 1
fi

if diskutil info "$VOLUME_NAME" 2>/dev/null | grep -q "Mounted.*Yes"; then
    diskutil unmount "$VOLUME_NAME"
    echo "Unmounted $VOLUME_NAME."
else
    echo "$VOLUME_NAME was not mounted."
fi
