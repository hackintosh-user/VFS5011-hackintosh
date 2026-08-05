#!/bin/bash
# Build script for vfs_client (interactive menu frontend).
# Run this from the folder containing all the vfs5011_* files and the nbis/ folder.
set -e

clang vfs_client.c vfs5011_matcher.c \
    nbis/mindtct/*.c nbis/bozorth3/*.c \
    -o vfs_client \
    -I. -Inbis/include \
    -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0 \
    -lm \
    -Wno-implicit-function-declaration

echo "Build complete: ./vfs_client"
