#!/bin/bash
# Build script for vfs5011_daemon (background auth daemon test harness).
# Run this from the folder containing all the vfs5011_* files and the nbis/ folder.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
. ./ht_env.sh

if [ ! -f "$HT_LIBUSB_INC/libusb.h" ]; then
    echo "error: libusb not found under $HT_BREW_PREFIX -- install with: brew install libusb" >&2
    exit 1
fi

clang vfs5011_daemon.c hack-touchid-matcher.c hack-touchid-menubar-ipc.c \
    nbis/mindtct/*.c nbis/bozorth3/*.c \
    -o vfs5011_daemon \
    -I. -Inbis/include \
    -I"$HT_LIBUSB_INC" -L"$HT_LIBUSB_LIB" -lusb-1.0 \
    -framework CoreFoundation -framework ApplicationServices -framework IOKit \
    -lm -lpthread \
    -Wno-implicit-function-declaration

echo "Build complete: ./vfs5011_daemon"
