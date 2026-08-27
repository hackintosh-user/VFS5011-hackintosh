#!/bin/bash
# Build script for hack_touchid_client (interactive menu frontend).
# Run this from the folder containing all the vfs5011_*/supported_sensors.h
# files and the nbis/ folder.
set -e

clang hack_touchid_client.c hack-touchid-matcher.c metallica_mis_firmware.c \
    nbis/mindtct/*.c nbis/bozorth3/*.c \
    -o hack-touchid \
    -I. -Inbis/include \
    -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0 \
    -framework CoreFoundation -framework IOKit \
    -lm \
    -Wno-implicit-function-declaration

echo "Build complete: ./hack-touchid"
