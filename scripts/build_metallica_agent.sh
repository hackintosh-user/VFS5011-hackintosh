#!/bin/bash
# Build script for metallica_mis_agent (the lock screen / padlock unlock
# agent for the Metallica MIS sensor: 06cb:009a, 138a:0097, 138a:009d).
#
# Lives in scripts/, writes ./metallica_mis_agent to the repo root, works
# from any directory. Needs libusb and openssl@3 (same as the daemon build).
#
# -DHACK_TOUCHID_CLIENT_BUILD drops the pairing harness main() inside
# metallica_mis_daemon.c, because the agent has its own main().
# -DHT_AGENT_VERSION comes from VERSION.txt so `metallica_mis_agent --version`
# reports the version it was built from.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # scripts/
cd "$SCRIPT_DIR/.."                           # repo root: binaries land here
. "$SCRIPT_DIR/ht_env.sh"

OPENSSL_PREFIX="$HT_OPENSSL_PREFIX"

if [ ! -d "$OPENSSL_PREFIX" ]; then
    echo "error: $OPENSSL_PREFIX not found -- install with: brew install openssl@3" >&2
    exit 1
fi
if [ ! -f "$HT_LIBUSB_INC/libusb.h" ]; then
    echo "error: libusb not found under $HT_BREW_PREFIX -- install with: brew install libusb" >&2
    exit 1
fi

AGENT_VERSION="$(sed -n 's/^VERSION=//p' VERSION.txt | head -n 1 | tr -d '\r')"
[ -n "$AGENT_VERSION" ] || AGENT_VERSION="unknown"

clang metallica/metallica_mis_agent.c \
    metallica/metallica_mis_daemon.c metallica/metallica_mis_tls.c metallica/metallica_mis_init_flash.c \
    metallica/metallica_mis_flash.c metallica/metallica_mis_blobs_9a.c metallica/metallica_mis_firmware.c \
    metallica/metallica_mis_upload_fwext.c metallica/mmis_rom_info.c \
    metallica/mmis_timeslot.c metallica/mmis_calibrate.c metallica/mmis_factory_bits.c \
    metallica/metallica_mis_debug.c metallica/metallica_mis_db.c metallica/metallica_mis_enroll.c \
    vfs5011/hack-touchid-menubar-ipc.c \
    -o metallica_mis_agent \
    -DHACK_TOUCHID_CLIENT_BUILD \
    -DHT_AGENT_VERSION="\"$AGENT_VERSION\"" \
    $HT_INC \
    -I"$HT_LIBUSB_INC" -L"$HT_LIBUSB_LIB" -lusb-1.0 \
    -I"$OPENSSL_PREFIX/include" -L"$OPENSSL_PREFIX/lib" -lssl -lcrypto \
    -framework CoreFoundation -framework ApplicationServices -framework IOKit \
    -lpthread \
    -Wno-implicit-function-declaration

echo "Build complete: ./metallica_mis_agent"
