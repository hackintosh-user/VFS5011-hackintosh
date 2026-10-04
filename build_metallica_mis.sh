#!/bin/bash
# Build script for metallica_mis_daemon (Metallica MIS 06cb:009a pairing
# test harness -- NOT a working capture daemon yet, see the file header
# in metallica_mis_daemon.c).
#
# This was the missing piece flagged in .github/workflows/ci.yml's
# cppcheck job: metallica_mis_tls.c/init_flash.c/flash.c/blobs_9a.c were
# being statically checked but never actually linked into a binary, so
# CI gave a false-green signal on link errors and missing declarations.
# This script is that missing link step. metallica_mis_upload_fwext.c
# (firmware upload, ported against real upload_fwext.py/flash.py/
# sensor.py source, session Aug 24) was added to this same link set
# once it existed -- do_pairing() now calls it in the same TLS session
# right after pairing succeeds, before any reboot.
#
# Needs OpenSSL (ECDH/HMAC/SHA/ECDSA for the session-cipher handshake in
# metallica_mis_tls.c and metallica_mis_init_flash.c) in addition to the
# libusb dependency build_daemon.sh already has. On Mohammad's Intel DV6,
# Homebrew installs to /usr/local and openssl@3 is keg-only, so its
# headers/libs aren't on the default search path the way libusb's are --
# hardcode the keg-only path the same way build_daemon.sh hardcodes
# /usr/local for libusb.
#
# Run this from the folder containing all the vfs5011_*/metallica_mis_*
# files. Requires: brew install openssl@3 (in addition to libusb).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
. ./ht_env.sh

OPENSSL_PREFIX="$HT_OPENSSL_PREFIX"

if [ ! -d "$OPENSSL_PREFIX" ]; then
    echo "error: $OPENSSL_PREFIX not found -- install with: brew install openssl@3" >&2
    exit 1
fi
if [ ! -f "$HT_LIBUSB_INC/libusb.h" ]; then
    echo "error: libusb not found under $HT_BREW_PREFIX -- install with: brew install libusb" >&2
    exit 1
fi

clang metallica_mis_daemon.c metallica_mis_tls.c metallica_mis_init_flash.c \
    metallica_mis_flash.c metallica_mis_blobs_9a.c metallica_mis_firmware.c \
    metallica_mis_upload_fwext.c mmis_rom_info.c \
    mmis_timeslot.c mmis_calibrate.c mmis_factory_bits.c \
    metallica_mis_debug.c metallica_mis_db.c \
    -o metallica_mis_daemon \
    -I. \
    -I"$HT_LIBUSB_INC" -L"$HT_LIBUSB_LIB" -lusb-1.0 \
    -I"$OPENSSL_PREFIX/include" -L"$OPENSSL_PREFIX/lib" -lssl -lcrypto \
    -framework CoreFoundation -framework IOKit \
    -lpthread \
    -Wno-implicit-function-declaration

echo "Build complete: ./metallica_mis_daemon"
echo ""
echo "This is a pairing test harness, not a deployable daemon -- see the"
echo "file header in metallica_mis_daemon.c before running with --pair."
