#!/bin/bash
#
# hack-touchid-volume-unmount.sh
#
# Unmounts HackTouchIDStore after the CLI/daemon is done touching templates,
# putting it back into its default "not visible anywhere" state.
#
# Usage: ./hack-touchid-volume-unmount.sh

set -e

VOLUME_NAME="HackTouchIDStore"

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
