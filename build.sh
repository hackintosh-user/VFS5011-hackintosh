#!/bin/bash
# Convenience build script.
# Builds hack-touchid (interactive menu frontend) and, depending on the
# sensor you pick, vfs5011_daemon (background auth daemon) and
# metallica_mis_daemon (Metallica MIS pairing test harness,
# active-development only) by invoking their individual build scripts.
#
# Usage:
#   ./build.sh                         builds everything (old behaviour)
#   ./build.sh --sensor metallica      only what the Metallica MIS needs
#   ./build.sh --sensor 06cb:009a      same, by USB ID
#   ./build.sh --sensor vfs5011        VFS5011 only, no OpenSSL needed
#
# Run this from the folder containing all the vfs5011_*/metallica_mis_*/
# supported_sensors.h files and the NBIS/ folder. For ax_probe
# (standalone AX diagnostic tool), build it separately, see README.
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

echo "==> Building hack-touchid..."
./build_client.sh --sensor "$FAMILIES"

BUILT="./hack-touchid"

case ",$FAMILIES," in *,vfs5011,*)
    echo ""
    echo "==> Building vfs5011_daemon..."
    ./build_daemon.sh
    BUILT="$BUILT, ./vfs5011_daemon"
;; esac

case ",$FAMILIES," in *,metallica,*)
    echo ""
    echo "==> Building metallica_mis_daemon (pairing test harness)..."
    ./build_metallica_mis.sh
    BUILT="$BUILT, ./metallica_mis_daemon"
;; esac

echo ""
echo "Build complete (sensor family: $FAMILIES): $BUILT"
