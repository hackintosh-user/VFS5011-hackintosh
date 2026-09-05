#!/bin/bash
# Build script for hack_touchid_client (interactive menu frontend).
# Run this from the folder containing all the vfs5011_*/supported_sensors.h
# files and the nbis/ folder.
#
# Aug 28: now also links metallica_mis_daemon.c + its TLS/flash/
# firmware-upload dependencies, so the client's own [P] Pair Sensor
# menu item (do_pair_metallica_mis() in hack_touchid_client.c) can
# call metallica_mis_open_device()/metallica_mis_send_init()/
# metallica_mis_do_pairing() directly -- see metallica_mis_daemon.h.
# -DHACK_TOUCHID_CLIENT_BUILD compiles out metallica_mis_daemon.c's
# own main() (the standalone test-harness entry point) so it doesn't
# collide with this binary's main(). Needs OpenSSL (ECDH/HMAC/SHA/
# ECDSA for metallica_mis_tls.c/metallica_mis_init_flash.c), same
# keg-only Homebrew path as build_metallica_mis.sh.
# Aug 29: also links upek_daemon.c so the client's own [U] Test
# Capture (UPEK, experimental) menu item can call
# upek_capture_fingerprint_image() directly -- see upek_daemon.h.
# upek_daemon.c's own smoke-test main() is gated behind
# UPEK_STANDALONE_TEST (undefined here), so no macro is needed to
# avoid a duplicate main() the way Metallica MIS needed
# -DHACK_TOUCHID_CLIENT_BUILD.
set -e

OPENSSL_PREFIX="/usr/local/opt/openssl@3"

if [ ! -d "$OPENSSL_PREFIX" ]; then
    echo "error: $OPENSSL_PREFIX not found -- install with: brew install openssl@3" >&2
    exit 1
fi

clang -DHACK_TOUCHID_CLIENT_BUILD \
    hack_touchid_client.c hack-touchid-matcher.c metallica_mis_firmware.c \
    metallica_mis_daemon.c metallica_mis_tls.c metallica_mis_init_flash.c \
    metallica_mis_flash.c metallica_mis_blobs_9a.c metallica_mis_upload_fwext.c \
    mmis_rom_info.c \
    upek_daemon.c \
    nbis/mindtct/*.c nbis/bozorth3/*.c \
    -o hack-touchid \
    -I. -Inbis/include \
    -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0 \
    -I"$OPENSSL_PREFIX/include" -L"$OPENSSL_PREFIX/lib" -lssl -lcrypto \
    -framework CoreFoundation -framework IOKit \
    -lpthread -lm \
    -Wno-implicit-function-declaration

echo "Build complete: ./hack-touchid"
