#!/bin/bash
#
# fpbootd-install.sh
#
# Builds hack-touchid from source, installs a dedicated copy of it to
# a fixed system path, and registers a LaunchDaemon (system domain,
# root, RunAtLoad) that runs it in --fpbootd-daemon mode -- the
# pre-login capture-and-match socket server.
#
# WHY A DEDICATED COPY, NOT THE /usr/local/bin/hack-touchid SYMLINK:
# normal interactive use resolves that symlink back to wherever the
# project checkout happens to live (see ensure_path_symlink() in
# hack_touchid_client.c) -- often somewhere under the user's home
# directory. If FileVault is on, that volume isn't decrypted until
# AFTER first unlock. A LaunchDaemon needs to be running and ready
# BEFORE loginwindow, which is well before that unlock happens -- if
# it were pointed at the symlink, it would fail to even find the
# binary at boot (ENOENT) until the exact moment it's no longer
# useful. Installing a private copy under /usr/local/libexec, on the
# system volume, sidesteps this entirely. Same reasoning
# hack-touchid-agent-install.sh already applies to vfs5011_daemon.
#
# WHY A LAUNCHDAEMON (SYSTEM DOMAIN), NOT A LAUNCHAGENT LIKE THE
# EXISTING LOCK-SCREEN DAEMON: the existing agent (see
# hack-touchid-agent-install.sh) deliberately uses a per-user
# LaunchAgent, because lock/unlock notifications are scoped to a GUI
# session's Mach namespace and a LaunchDaemon never receives them.
# Fpbootd is the opposite case -- there IS no GUI session yet at the
# point it needs to be running, so it has to be a genuine system
# LaunchDaemon, no self-elevation trick needed (launchd starts it as
# root directly).
#
# KeepAlive is set true here, unlike the lock-screen agent (which
# deliberately omits it -- see that script's comments on the LWCR/
# EX_CONFIG issues that came from combining certain keys on a
# per-user agent). Those issues were specific to the GUI-session
# LaunchAgent domain; this hasn't been empirically validated the same
# way for a system LaunchDaemon yet. If bootstrap fails with a similar
# EX_CONFIG-style rejection, KeepAlive is the first thing to try
# removing, mirroring how that was diagnosed there.
#
# Run with sudo:
#
#   sudo ./fpbootd-install.sh
#
set -e

BINARY_PATH="/usr/local/libexec/hack-touchid/hack-touchid-fpbootd"
LABEL="com.hackintosh.fpbootd"
LOG_PATH="/Library/Logs/fpbootd.log"
PLIST_PATH="/Library/LaunchDaemons/$LABEL.plist"

if [ "$(id -u)" -ne 0 ]; then
    echo "This needs root (writes to /Library/LaunchDaemons and /usr/local/libexec). Run with sudo." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_SCRIPT="$SCRIPT_DIR/build_client.sh"
BUILT_BINARY="$SCRIPT_DIR/hack-touchid"
INSTALL_DIR="$(dirname "$BINARY_PATH")"

if [ ! -x "$BUILD_SCRIPT" ]; then
    echo "Error: $BUILD_SCRIPT not found -- run this from inside the project folder." >&2
    exit 1
fi

echo "Building hack-touchid from source..."
if ! (cd "$SCRIPT_DIR" && ./build_client.sh > /tmp/fpbootd_build.log 2>&1); then
    echo "Error: build failed. See /tmp/fpbootd_build.log for the compiler output." >&2
    exit 1
fi

if [ ! -x "$BUILT_BINARY" ]; then
    echo "Error: build_client.sh reported success but $BUILT_BINARY wasn't produced." >&2
    exit 1
fi

mkdir -p "$INSTALL_DIR"
cp "$BUILT_BINARY" "$BINARY_PATH"
chown root:wheel "$BINARY_PATH"
chmod 755 "$BINARY_PATH"

# Same reasoning as hack-touchid-agent-install.sh: mount_template_volume()/
# unmount_template_volume() resolve these scripts relative to the
# daemon's OWN running directory (g_exec_dir), not the project folder
# it happened to be built from. Deployed alone without these, every
# capture attempt would fail at the very first mount.
for script in "hack-touchid-volume-mount.sh" "hack-touchid-volume-unmount.sh"; do
    if [ ! -f "$SCRIPT_DIR/$script" ]; then
        echo "Error: $SCRIPT_DIR/$script not found -- can't deploy a working daemon without it." >&2
        exit 1
    fi
    cp "$SCRIPT_DIR/$script" "$INSTALL_DIR/$script"
    chown root:wheel "$INSTALL_DIR/$script"
    chmod 755 "$INSTALL_DIR/$script"
done

touch "$LOG_PATH"
chown root:wheel "$LOG_PATH"
chmod 644 "$LOG_PATH"

echo "Installing LaunchDaemon pointing at: $BINARY_PATH --fpbootd-daemon"
echo "Plist location: $PLIST_PATH (system domain)"

# Tear down any previous registration first -- bootstrapping over an
# already-loaded LaunchDaemon with the same label is a common source
# of confusing "already loaded"/stale-state errors.
launchctl bootout system "$PLIST_PATH" 2>/dev/null || true

cat > "$PLIST_PATH" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BINARY_PATH</string>
        <string>--fpbootd-daemon</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$LOG_PATH</string>
    <key>StandardErrorPath</key>
    <string>$LOG_PATH</string>
</dict>
</plist>
PLIST

chown root:wheel "$PLIST_PATH"
chmod 644 "$PLIST_PATH"

if ! plutil -lint "$PLIST_PATH" >/dev/null 2>&1; then
    echo "Error: generated plist failed validation." >&2
    exit 1
fi

launchctl bootstrap system "$PLIST_PATH"

sleep 1
# See hack-touchid-agent-install.sh's comment on why 'exit' after the
# first match matters here -- launchctl print emits multiple "state ="
# lines and awk would otherwise report a nested one, not the
# top-level state.
STATE="$(launchctl print "system/$LABEL" 2>/dev/null | awk -F'= ' '/state =/{print $2; exit}')"

if [ "$STATE" = "running" ]; then
    echo ""
    echo "Installed and running: $LABEL (system domain)"
    echo "Logs: $LOG_PATH"
    echo ""
    echo "Socket: /var/run/fpbootd.sock (root-only -- see the SECURITY NOTE"
    echo "in run_fpbootd_daemon()'s comment in hack_touchid_client.c)."
    echo "Watch the log with:"
    echo "  tail -f $LOG_PATH"
else
    echo "" >&2
    echo "Warning: daemon did not reach 'running' state (state=$STATE)." >&2
    echo "Check: sudo launchctl print system/$LABEL | grep -E 'state|last exit code|properties'" >&2
    exit 1
fi
