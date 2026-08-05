#!/bin/bash
# Build script for vfs5011_daemon (background auth daemon test harness).
# Run this from the folder containing all the vfs5011_* files and the nbis/ folder.
set -e

clang vfs5011_daemon.c vfs5011_matcher.c \
    nbis/mindtct/*.c nbis/bozorth3/*.c \
    -o vfs5011_daemon \
    -I. -Inbis/include \
    -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0 \
    -framework CoreFoundation -framework ApplicationServices \
    -lm -lpthread \
    -Wno-implicit-function-declaration

echo "Build complete: ./vfs5011_daemon"
