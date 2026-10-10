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
#
# Oct 4: sensor-driven. Pass --sensor <family|VID:PID|all>[,...] (or set
# HT_FAMILIES) to build only what that sensor needs. Without the
# metallica family the client is compiled with -DHT_NO_METALLICA: the
# Metallica MIS sources, OpenSSL and innoextract are all left out, and
# ht_sensor_stubs.c provides do-nothing replacements that tell the user
# how to rebuild with Metallica support. Default is "all", so a bare
# ./build_client.sh behaves exactly as before.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
. ./ht_env.sh

SENSORS="${HT_FAMILIES:-all}"
while [ $# -gt 0 ]; do
    case "$1" in
        --sensor) SENSORS="$2"; shift 2 ;;
        *) echo "usage: $0 [--sensor <family|VID:PID|all>[,...]]" >&2; exit 2 ;;
    esac
done

ht_ensure_tool
FAMILIES="$("$HT_TOOL" resolve "$SENSORS" | paste -sd, -)" || exit 1
echo "Client build for sensor family: $FAMILIES"

# Every family needs libusb; only metallica adds OpenSSL.
MISSING="$("$HT_TOOL" deps build "$FAMILIES" | ht_filter_missing | paste -sd' ' -)"
if [ -n "$MISSING" ]; then
    echo "error: missing build dependencies for this sensor: $MISSING" >&2
    echo "       install with: brew install $MISSING" >&2
    echo "       or run ./prep_and_build.sh --sensor $FAMILIES to be offered the install." >&2
    exit 1
fi

COMMON_SRC="hack_touchid_client.c hack-touchid-matcher.c upek_daemon.c"
CFLAGS="-I. -Inbis/include -I$HT_LIBUSB_INC"
LDFLAGS="-L$HT_LIBUSB_LIB -lusb-1.0 -framework CoreFoundation -framework IOKit -lpthread -lm"

case ",$FAMILIES," in
    *,metallica,*)
        SENSOR_SRC="metallica_mis_firmware.c metallica_mis_daemon.c metallica_mis_tls.c \
            metallica_mis_init_flash.c metallica_mis_flash.c metallica_mis_blobs_9a.c \
            metallica_mis_upload_fwext.c mmis_rom_info.c mmis_timeslot.c mmis_calibrate.c \
            mmis_factory_bits.c metallica_mis_debug.c metallica_mis_db.c metallica_mis_enroll.c"
        CFLAGS="-DHACK_TOUCHID_CLIENT_BUILD $CFLAGS -I$HT_OPENSSL_PREFIX/include"
        LDFLAGS="$LDFLAGS -L$HT_OPENSSL_PREFIX/lib -lssl -lcrypto"
        ;;
    *)
        SENSOR_SRC="ht_sensor_stubs.c"
        CFLAGS="-DHT_NO_METALLICA $CFLAGS"
        ;;
esac

# shellcheck disable=SC2086
clang $CFLAGS \
    $COMMON_SRC $SENSOR_SRC \
    nbis/mindtct/*.c nbis/bozorth3/*.c \
    -o hack-touchid \
    $LDFLAGS \
    -Wno-implicit-function-declaration

echo "Build complete: ./hack-touchid  (sensor family: $FAMILIES)"
