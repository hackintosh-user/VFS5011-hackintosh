#!/bin/bash
# Convenience build script (scripts/build.sh; run it from anywhere).
# Builds hack-touchid (interactive menu frontend) and, depending on the
# sensor you pick, vfs5011_daemon (background auth daemon) and
# metallica_mis_daemon (Metallica MIS pairing test harness,
# active-development only) and metallica_mis_agent (Metallica MIS lock
# screen / padlock unlock agent) by invoking their individual build scripts.
#
# Usage:
#   ./scripts/build.sh                         builds everything (old behaviour)
#   ./scripts/build.sh --sensor metallica      only what the Metallica MIS needs
#   ./scripts/build.sh --sensor 06cb:009a      same, by USB ID
#   ./scripts/build.sh --sensor vfs5011        VFS5011 only, no OpenSSL needed
#
# The binaries (hack-touchid, vfs5011_daemon, metallica_mis_daemon,
# metallica_mis_agent) are written
# to the repo root. For ax_probe (standalone AX diagnostic tool,
# shared/access/ax_probe.c), build it separately, see README.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # scripts/
cd "$SCRIPT_DIR/.."                           # repo root: binaries land here
. "$SCRIPT_DIR/ht_env.sh"

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
"$SCRIPT_DIR/build_client.sh" --sensor "$FAMILIES"

BUILT="./hack-touchid"

case ",$FAMILIES," in *,vfs5011,*)
    echo ""
    echo "==> Building vfs5011_daemon..."
    "$SCRIPT_DIR/build_daemon.sh"
    BUILT="$BUILT, ./vfs5011_daemon"
;; esac

case ",$FAMILIES," in *,metallica,*)
    echo ""
    echo "==> Building metallica_mis_daemon (pairing test harness)..."
    "$SCRIPT_DIR/build_metallica_mis.sh"
    BUILT="$BUILT, ./metallica_mis_daemon"

    echo ""
    echo "==> Building metallica_mis_agent (lock screen / padlock unlock)..."
    "$SCRIPT_DIR/build_metallica_agent.sh"
    BUILT="$BUILT, ./metallica_mis_agent"
;; esac

echo ""
echo "Build complete (sensor family: $FAMILIES): $BUILT"
