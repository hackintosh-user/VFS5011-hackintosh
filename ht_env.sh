#!/bin/bash
# ht_env.sh
#
# Shared helpers for the build scripts. Source it, do not run it:
#
#   . "$(dirname "$0")/ht_env.sh"
#
# Everything sensor-specific comes from supported_sensors.h through
# ht_sensor_tool (see ht_sensor_tool.c), so there is one place to edit
# when a sensor or its dependencies change.

HT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HT_TOOL="$HT_DIR/.ht_sensor_tool"

# Intel Homebrew lives under /usr/local. Ask brew when it is on PATH, and
# fall back to /usr/local so the scripts still work for people who
# installed the libraries some other way.
if command -v brew >/dev/null 2>&1; then
    HT_BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /usr/local)"
else
    HT_BREW_PREFIX="/usr/local"
fi
HT_LIBUSB_INC="$HT_BREW_PREFIX/include/libusb-1.0"
HT_LIBUSB_LIB="$HT_BREW_PREFIX/lib"
HT_OPENSSL_PREFIX="$HT_BREW_PREFIX/opt/openssl@3"

# Builds the sensor tool (needs clang only). Rebuilt when the header is
# newer so a table edit is picked up automatically.
ht_ensure_tool() {
    if [ ! -x "$HT_TOOL" ] || [ "$HT_DIR/supported_sensors.h" -nt "$HT_TOOL" ] \
       || [ "$HT_DIR/ht_sensor_tool.c" -nt "$HT_TOOL" ]; then
        if ! command -v clang >/dev/null 2>&1; then
            echo "error: clang not found. Install the Xcode Command Line Tools: xcode-select --install" >&2
            return 1
        fi
        clang -O1 -I"$HT_DIR" "$HT_DIR/ht_sensor_tool.c" -o "$HT_TOOL" || return 1
    fi
}

# Is this Homebrew formula's content present? Checked by looking for the
# files the build actually uses, not by asking brew, so it also works
# when brew itself is slow or broken.
ht_dep_installed() {
    case "$1" in
        libusb)      [ -f "$HT_LIBUSB_INC/libusb.h" ] ;;
        openssl@3)   [ -f "$HT_OPENSSL_PREFIX/include/openssl/evp.h" ] ;;
        innoextract) command -v innoextract >/dev/null 2>&1 || [ -x "$HT_BREW_PREFIX/bin/innoextract" ] ;;
        *)           return 1 ;;
    esac
}

# Prints the formulas from stdin that are NOT installed, one per line.
ht_filter_missing() {
    local f
    while read -r f; do
        [ -n "$f" ] || continue
        ht_dep_installed "$f" || echo "$f"
    done
}

# brew refuses to run as root. When a script is started with sudo, drop
# back to the invoking user for the brew call.
ht_brew() {
    if [ "$(id -u)" = "0" ] && [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != "root" ]; then
        sudo -u "$SUDO_USER" -H brew "$@"
    else
        brew "$@"
    fi
}

# ht_install_missing <assume_yes 0|1> <formula>...
# Installs only the formulas passed in. Asks first unless assume_yes.
ht_install_missing() {
    local assume_yes="$1"; shift
    [ "$#" -gt 0 ] || return 0

    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew was not found. Install these yourself, then run this again:" >&2
        printf '  %s\n' "$@" >&2
        return 1
    fi

    echo "Missing for your sensor: $*"
    if [ "$assume_yes" != "1" ]; then
        if [ ! -t 0 ]; then
            echo "Not an interactive terminal. Run: brew install $*" >&2
            return 1
        fi
        printf 'Install them now with Homebrew? [y/N] '
        read -r ans
        case "$ans" in y|Y|yes|YES) ;; *) echo "Skipped. Install later with: brew install $*"; return 1 ;; esac
    fi
    ht_brew install "$@"
}
