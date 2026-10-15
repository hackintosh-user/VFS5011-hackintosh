#!/bin/bash
#
# metallica_mis_agent_install.sh
#
# Builds metallica_mis_agent from source, installs it to its fixed system
# path and registers it as a per-user LaunchAgent. Metallica counterpart of
# vfs5011/hack-touchid-agent-install.sh, and it follows the same rules (read
# the long comment block at the top of that script for the launchd history):
#
#   - plain per-user LaunchAgent in ~/Library/LaunchAgents, with no
#     LimitLoadToSessionType and no KeepAlive key (both trigger EX_CONFIG)
#   - the agent self-elevates through `sudo`, so a narrowly scoped NOPASSWD
#     sudoers rule is written for this exact binary path
#   - the log file is owned by the console user, launchd opens it before the
#     agent ever elevates
#
# It deliberately reuses the SAME LaunchAgent label, plist path, log path and
# sudoers path as the VFS5011 agent, so the menu bar status, the client's
# native [X] Uninstall and the updater's daemon cleanup all keep working
# without knowing which sensor is deployed. Only one sensor agent can be
# installed at a time, which matches reality (one sensor per machine).
#
# The saved host identity (metallica_host_identity.conf, written by the
# client the first time a sensor session needs it) already lives next to the
# binary in /usr/local/libexec/hack-touchid/ and is left alone here.
#
# Run with sudo:   sudo sh metallica/metallica_mis_agent_install.sh
#
set -e

BINARY_PATH="/usr/local/libexec/hack-touchid/metallica_mis_agent"
LABEL="com.hackintosh.vfs5011agent"
LOG_PATH="/Library/Logs/vfs5011agent.log"
SUDOERS_PATH="/etc/sudoers.d/vfs5011daemon"

# Superseded locations to clean up on every install.
OLD_DAEMON_LABEL="com.hackintosh.vfs5011daemon"
OLD_DAEMON_PLIST="/Library/LaunchDaemons/$OLD_DAEMON_LABEL.plist"
OLD_AGENT_PLIST_SYSTEM="/Library/LaunchAgents/$LABEL.plist"

if [ "$(id -u)" -ne 0 ]; then
    echo "This needs root (writes to /etc/sudoers.d and /Library/Logs, bootstraps another user's session). Run with sudo." >&2
    exit 1
fi

# Hardware gate. The client already confirmed the sensor over libusb and sets
# HT_SENSOR_VERIFIED=1; a bare run of this script checks ioreg for any of the
# three Metallica MIS USB IDs (06cb:009a, 138a:0097, 138a:009d).
sensor_present_ioreg() {
    ioreg -p IOUSB -l -w0 2>/dev/null | awk '
        function check() {
            if ((v == 1739 && p == 154) || (v == 5002 && p == 151) || (v == 5002 && p == 157)) found = 1
        }
        /\+-o / { check(); v = -1; p = -1 }
        /"idVendor" *=/  { sub(/.*= */, ""); v = $1 + 0 }
        /"idProduct" *=/ { sub(/.*= */, ""); p = $1 + 0 }
        END { check(); exit (found ? 0 : 1) }'
}

if [ "${HT_SENSOR_VERIFIED:-0}" != "1" ]; then
    if ! sensor_present_ioreg; then
        echo "Error: No Metallica MIS sensor detected on the USB bus (06cb:009a, 138a:0097 or 138a:009d)." >&2
        echo "Refusing to install a background service tied to hardware that isn't present." >&2
        exit 1
    fi
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # metallica/
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"      # repo root
BUILD_SCRIPT="$ROOT_DIR/scripts/build_metallica_agent.sh"
BUILT_BINARY="$ROOT_DIR/metallica_mis_agent"
INSTALL_DIR="$(dirname "$BINARY_PATH")"

if [ ! -x "$BUILD_SCRIPT" ]; then
    echo "Error: $BUILD_SCRIPT not found -- run this from inside the project folder." >&2
    exit 1
fi

echo "Building agent from source..."
if ! (cd "$ROOT_DIR" && ./scripts/build_metallica_agent.sh > /tmp/metallica_agent_build.log 2>&1); then
    echo "Error: build failed. See /tmp/metallica_agent_build.log for the compiler output." >&2
    exit 1
fi

if [ ! -x "$BUILT_BINARY" ]; then
    echo "Error: build_metallica_agent.sh reported success but $BUILT_BINARY wasn't produced." >&2
    exit 1
fi

mkdir -p "$INSTALL_DIR"

# A previous VFS5011 install leaves its daemon behind, and its sudoers rule is
# replaced below. Remove the old binary so only one agent stays deployed.
rm -f "$INSTALL_DIR/vfs5011_daemon"

cp "$BUILT_BINARY" "$BINARY_PATH"
chown root:wheel "$BINARY_PATH"
chmod 755 "$BINARY_PATH"

# The agent resolves the volume mount/unmount scripts next to its own binary,
# so the deployed copy has to be self-contained (it mounts the volume once per
# episode to read the stored login password).
for script in "hack-touchid-volume-mount.sh" "hack-touchid-volume-unmount.sh"; do
    if [ ! -f "$ROOT_DIR/vol/$script" ]; then
        echo "Error: $ROOT_DIR/vol/$script not found -- can't deploy a working agent without it." >&2
        exit 1
    fi
    cp "$ROOT_DIR/vol/$script" "$INSTALL_DIR/$script"
    chown root:wheel "$INSTALL_DIR/$script"
    chmod 755 "$INSTALL_DIR/$script"
done

# Every rebuild changes the cdhash, so re-sign and re-grant Accessibility on
# every deploy (see hack-touchid-grant-accessibility.sh).
GRANT_SCRIPT="$ROOT_DIR/shared/access/hack-touchid-grant-accessibility.sh"
if [ -x "$GRANT_SCRIPT" ]; then
    echo "Re-granting Accessibility for the freshly built binary..."
    "$GRANT_SCRIPT" "$BINARY_PATH"
else
    echo "Warning: $GRANT_SCRIPT not found -- Accessibility was NOT (re-)granted." >&2
    echo "Auto-type will silently fail to do anything until you run it manually." >&2
fi

if [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != "root" ]; then
    CONSOLE_USER="$SUDO_USER"
else
    CONSOLE_USER="$(stat -f%Su /dev/console)"
fi
if [ -z "$CONSOLE_USER" ] || [ "$CONSOLE_USER" = "root" ]; then
    echo "Error: could not determine the logged-in console user (got: '$CONSOLE_USER')." >&2
    echo "Make sure you're logged into the GUI session (not just SSH'd in) and try again." >&2
    exit 1
fi
CONSOLE_UID="$(id -u "$CONSOLE_USER")"
CONSOLE_HOME="$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
if [ -z "$CONSOLE_HOME" ] || [ ! -d "$CONSOLE_HOME" ]; then
    echo "Error: could not resolve home directory for $CONSOLE_USER." >&2
    exit 1
fi
PLIST_PATH="$CONSOLE_HOME/Library/LaunchAgents/$LABEL.plist"
echo "Console user: $CONSOLE_USER (uid $CONSOLE_UID, home $CONSOLE_HOME)"

echo "Removing any existing registrations..."
launchctl bootout "gui/$CONSOLE_UID/$LABEL" 2>/dev/null || true
if [ -f "$OLD_DAEMON_PLIST" ]; then
    echo "Removing old LaunchDaemon ($OLD_DAEMON_LABEL)..."
    launchctl bootout system "$OLD_DAEMON_PLIST" 2>/dev/null || true
    rm -f "$OLD_DAEMON_PLIST"
fi
if [ -f "$OLD_AGENT_PLIST_SYSTEM" ]; then
    echo "Removing old v1 system-domain plist ($OLD_AGENT_PLIST_SYSTEM)..."
    rm -f "$OLD_AGENT_PLIST_SYSTEM"
fi

echo "Adding scoped NOPASSWD sudo rule for $CONSOLE_USER -> this binary only..."
ESCAPED_PATH=$(printf '%s' "$BINARY_PATH" | sed 's/ /\\ /g')
TMP_SUDOERS="$(mktemp)"
cat > "$TMP_SUDOERS" <<SUDOERS
# Allows metallica_mis_agent (running as a LaunchAgent, console user) to
# self-elevate to root non-interactively via execvp("sudo", argv[0]),
# with NO arguments and NO other commands. Managed by
# metallica_mis_agent_install.sh -- safe to delete if this project is removed.
$CONSOLE_USER ALL=(root) NOPASSWD: $ESCAPED_PATH
Defaults!$ESCAPED_PATH !requiretty
SUDOERS

if visudo -c -f "$TMP_SUDOERS" >/dev/null 2>&1; then
    cp "$TMP_SUDOERS" "$SUDOERS_PATH"
    chown root:wheel "$SUDOERS_PATH"
    chmod 440 "$SUDOERS_PATH"
    rm -f "$TMP_SUDOERS"
    echo "Sudoers rule installed at $SUDOERS_PATH"
else
    echo "Error: generated sudoers file failed validation -- NOT installing it." >&2
    echo "See: $TMP_SUDOERS" >&2
    exit 1
fi

# Owned by the console user: launchd opens it before the agent elevates.
touch "$LOG_PATH"
chown "$CONSOLE_USER:staff" "$LOG_PATH"
chmod 644 "$LOG_PATH"

echo "Installing LaunchAgent pointing at: $BINARY_PATH"
echo "Plist location: $PLIST_PATH (per-user domain -- NOT /Library/LaunchAgents)"

mkdir -p "$CONSOLE_HOME/Library/LaunchAgents"
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
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$LOG_PATH</string>
    <key>StandardErrorPath</key>
    <string>$LOG_PATH</string>
</dict>
</plist>
PLIST
# Deliberately OMITTED: LimitLoadToSessionType, KeepAlive (see the VFS5011 installer).

chown "$CONSOLE_USER" "$PLIST_PATH"
chmod 644 "$PLIST_PATH"

if ! plutil -lint "$PLIST_PATH" >/dev/null 2>&1; then
    echo "Error: generated plist failed validation." >&2
    exit 1
fi

launchctl bootstrap "gui/$CONSOLE_UID" "$PLIST_PATH"

sleep 1
STATE="$(launchctl print "gui/$CONSOLE_UID/$LABEL" 2>/dev/null | awk -F'= ' '/state =/{print $2; exit}')"

if [ "$STATE" = "running" ]; then
    echo ""
    echo "Installed and running: $LABEL in gui/$CONSOLE_UID (running as $CONSOLE_USER, self-elevating via sudo)"
    echo "Logs: $LOG_PATH"
    echo ""
    echo "Now lock the screen and touch the sensor. Watch the log with:"
    echo "  tail -f $LOG_PATH"
else
    echo "" >&2
    echo "Warning: agent did not reach 'running' state (state=$STATE)." >&2
    echo "Check: sudo launchctl print gui/$CONSOLE_UID/$LABEL | grep -E 'state|last exit code|properties'" >&2
    exit 1
fi
