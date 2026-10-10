#!/bin/bash
#
# prep_and_build.sh
#
# Fixes the recurring "some .sh files aren't executable" problem
# (build.sh only ever chmod'd itself + build_client.sh + build_daemon.sh,
# never the runtime helper scripts like hack-touchid-volume-mount.sh) by
# chmod +x'ing EVERY .sh file in this folder, and rebuilds both
# binaries from source.
#
# NOTE (v1.1): this used to also deploy the daemon itself (rebuild,
# copy to /usr/local/libexec/hack-touchid/, re-sign, re-grant
# Accessibility, reinstall the LaunchAgent) by shelling out to
# hack-touchid-agent-install.sh directly. That's exactly what Deploy [3]
# in the client does -- doing it here too meant the daemon could end
# up live and running before the user ever launched the client once,
# which makes Deploy [3] pointless the first time around. Deploy is
# now the client's job alone. This script only gets the binaries
# built and the helper scripts executable; run the client yourself
# and use Deploy [3] when you're ready to actually install/reinstall
# the daemon.
#
# After this runs:
#
#   sudo ./hack-touchid
#
# then use Deploy [3] from the menu.
#
# Sensor-driven (Oct 4): you tell it which sensor you have, and it only
# installs and builds what that sensor needs. Dependencies come from the
# sensor table in supported_sensors.h, so nothing here goes stale.
#
# Usage:
#   ./prep_and_build.sh                      detect the plugged-in sensor
#   ./prep_and_build.sh --sensor vfs5011     by family
#   ./prep_and_build.sh --sensor 06cb:009a   by USB ID (hex)
#   ./prep_and_build.sh --sensor vfs5011,upek   more than one
#   ./prep_and_build.sh --sensor all         everything (old behaviour)
#   ./prep_and_build.sh --list               show families and what each needs
#   ./prep_and_build.sh --yes                install missing deps without asking
#   ./prep_and_build.sh --no-install         never touch Homebrew, just report
#
# Run this from inside the project folder (same folder as build.sh).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
. ./ht_env.sh

SENSORS=""
ASSUME_YES=0
NO_INSTALL=0
LIST_ONLY=0
FALLBACK_ALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --sensor)     SENSORS="$2"; shift 2 ;;
        --yes|-y)     ASSUME_YES=1; shift ;;
        --no-install) NO_INSTALL=1; shift ;;
        --list)       LIST_ONLY=1; shift ;;
        -h|--help)    sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

echo "== Fixing permissions on every .sh file in $SCRIPT_DIR =="
chmod +x ./*.sh
ls -la ./*.sh

echo
echo "== Reclaiming ownership of build artifacts (in case a previous sudo build left them root-owned) =="
CURRENT_USER="$(id -un)"
for bin in hack-touchid vfs5011_daemon metallica_mis_daemon; do
    if [ -e "$bin" ] && [ "$(stat -f '%Su' "$bin")" != "$CURRENT_USER" ]; then
        echo "  $bin is owned by $(stat -f '%Su' "$bin") -- reclaiming as $CURRENT_USER (you may be prompted for your password)"
        sudo chown "$CURRENT_USER":staff "$bin"
    fi
done

echo
echo "== Choosing your sensor =="
ht_ensure_tool

if [ "$LIST_ONLY" = "1" ]; then
    echo "Families (USB IDs), and the Homebrew packages each one needs:"
    "$HT_TOOL" families | while IFS='|' read -r fam name ids; do
        need="$("$HT_TOOL" deps all "$fam" | paste -sd' ' -)"
        printf '  %-10s %-28s [%s]\n             needs: %s\n' "$fam" "$name" "$ids" "$need"
    done
    exit 0
fi

# Output is not a terminal: the in-app updater runs this script with its
# output captured, so a prompt would be invisible and the update would hang
# forever (older clients also leave stdin attached to the terminal). Never
# ask in that case: install what is missing, and build everything if no
# sensor can be detected, which matches how updates behaved before the
# sensor-driven build.
INTERACTIVE=1
if [ ! -t 1 ]; then
    INTERACTIVE=0
    ASSUME_YES=1
fi

if [ -z "$SENSORS" ]; then
    DETECTED="$(ioreg -p IOUSB -l -w0 2>/dev/null | "$HT_TOOL" detect | paste -sd, -)"
    if [ -n "$DETECTED" ]; then
        echo "Detected a supported sensor on USB: $DETECTED"
        SENSORS="$DETECTED"
    elif [ "$INTERACTIVE" = "0" ]; then
        echo "No supported sensor detected on USB. Not interactive, so building for all families."
        SENSORS="all"
        FALLBACK_ALL=1
    elif [ -t 0 ]; then
        echo "No supported sensor detected on USB. Which one do you have?"
        "$HT_TOOL" families | while IFS='|' read -r fam name ids; do
            printf '  %-10s %s [%s]\n' "$fam" "$name" "$ids"
        done
        printf 'Type a family name (or "all"): '
        read -r SENSORS
    else
        echo "error: no sensor detected and no --sensor given. Run with --sensor <family>, or --list." >&2
        exit 1
    fi
fi

FAMILIES="$("$HT_TOOL" resolve "$SENSORS" | paste -sd, -)" || exit 1
[ -n "$FAMILIES" ] || { echo "error: nothing matched '$SENSORS' (try --list)" >&2; exit 1; }

# The "all" fallback (updater with no sensor detected) must never install
# anything: on Intel Macs Homebrew compiles from source, which can take
# very long, and the missing packages may belong to a sensor the user does
# not have. Keep only the families whose dependencies are already present.
if [ "$FALLBACK_ALL" = "1" ]; then
    KEEP=""
    SKIPPED=""
    for fam in $(echo "$FAMILIES" | tr ',' ' '); do
        if [ -z "$("$HT_TOOL" deps all "$fam" | ht_filter_missing | paste -sd' ' -)" ]; then
            KEEP="${KEEP:+$KEEP,}$fam"
        else
            SKIPPED="${SKIPPED:+$SKIPPED }$fam"
        fi
    done
    if [ -n "$KEEP" ]; then
        [ -z "$SKIPPED" ] || echo "Skipping $SKIPPED: its Homebrew packages are not installed and no sensor of that kind was detected."
        FAMILIES="$KEEP"
    fi
fi
echo "Building for: $FAMILIES"

echo
echo "== Checking dependencies for that sensor only =="
NEEDED="$("$HT_TOOL" deps all "$FAMILIES" | paste -sd' ' -)"
MISSING="$("$HT_TOOL" deps all "$FAMILIES" | ht_filter_missing | paste -sd' ' -)"
echo "  needs:   $NEEDED"
if [ -z "$MISSING" ]; then
    echo "  missing: nothing, everything is already installed"
else
    echo "  missing: $MISSING"
    if [ "$NO_INSTALL" = "1" ]; then
        echo "error: --no-install given. Install with: brew install $MISSING" >&2
        exit 1
    fi
    # shellcheck disable=SC2086
    ht_install_missing "$ASSUME_YES" $MISSING || exit 1
fi

echo
echo "== Building client + daemon from source =="
./build.sh --sensor "$FAMILIES"

echo
echo "Done. Binaries are built. Nothing has been deployed/installed yet."
echo "Run the client and use Deploy [3] from the menu when you're ready:"
echo "  sudo ./hack-touchid"
echo
echo "If you're setting up Fpbootd (the pre-login daemon), that's a separate"
echo "opt-in install, same reasoning as above -- run it yourself when ready:"
echo "  sudo ./fpbootd-install.sh"
