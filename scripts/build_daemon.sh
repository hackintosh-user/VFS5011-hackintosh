#!/bin/bash
# Build script for vfs5011_daemon (background auth daemon test harness).
# Lives in scripts/, writes ./vfs5011_daemon to the repo root, works from any directory.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # scripts/
cd "$SCRIPT_DIR/.."                           # repo root: binaries land here
. "$SCRIPT_DIR/ht_env.sh"

if [ ! -f "$HT_LIBUSB_INC/libusb.h" ]; then
    echo "error: libusb not found under $HT_BREW_PREFIX -- install with: brew install libusb" >&2
    exit 1
fi

clang vfs5011/vfs5011_daemon.c shared/sen/hack-touchid-matcher.c vfs5011/hack-touchid-menubar-ipc.c \
    NBIS/mindtct/*.c NBIS/bozorth3/*.c \
    -o vfs5011_daemon \
    $HT_INC \
    -I"$HT_LIBUSB_INC" -L"$HT_LIBUSB_LIB" -lusb-1.0 \
    -framework CoreFoundation -framework ApplicationServices -framework IOKit \
    -lm -lpthread \
    -Wno-implicit-function-declaration

echo "Build complete: ./vfs5011_daemon"
