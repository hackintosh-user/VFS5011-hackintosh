#!/bin/bash
#
# vfs5011_agent_install.sh (v2)
#
# Builds vfs5011_daemon from source, installs it to its fixed system
# path, and registers it as a per-user LaunchAgent. This is the one
# command a fresh checkout needs to go from source to a running
# service -- run it after making any change to vfs5011_daemon.c and
# it always deploys whatever's currently on disk, never a stale
# previously-built binary.
#
# WHY LAUNCHAGENT AT ALL: com.apple.screenIsLocked / screenIsUnlocked
# are delivered via CFNotificationCenterGetDistributedCenter(), which
# is scoped to a specific GUI login session's Mach bootstrap
# namespace -- NOT the system-wide domain. A LaunchDaemon (system
# domain) never receives these notifications at all, regardless of
# TCC/Accessibility status; confirmed empirically (daemon ran, logged
# its startup line, but never logged a single lock/unlock event).
#
# WHY NOT "UserName root" ON THE LAUNCHAGENT: tried that first --
# launchd rejected it outright with "last exit code = 78: EX_CONFIG"
# and a "managed LWCR" flag in `launchctl print`, even after ad-hoc
# code-signing the binary. A GUI-session agent may only run as the
# session's own owning user, full stop.
#
# WHY v2 (this version) MOVES THE PLIST TO ~/Library/LaunchAgents:
# v1 installed to /Library/LaunchAgents (system-wide) with
# LimitLoadToSessionType=Aqua and KeepAlive=true. That combination
# got launchd's constraint system (LWCR) to flag the agent as an
# "untrusted" / "managed LWCR" system service, and it refused to
# spawn it at all -- last exit code 78: EX_CONFIG, runs=N with
# active count=0 every time, regardless of code signature. Isolated
# via a control test (a trivial /bin/echo LaunchAgent loaded fine
# from ~/Library/LaunchAgents with neither of those keys) and then a
# binary-specific test (same binary, same ~/Library/LaunchAgents
# location, plist with LimitLoadToSessionType+KeepAlive removed --
# ran successfully, state=running, self-elevated via sudo, logged a
# full lock -> swipe -> match -> unlock cycle). Fix: plain per-user
# LaunchAgent, no LimitLoadToSessionType key (not needed -- a
# per-user LaunchAgent only ever runs in that user's GUI session
# anyway), no KeepAlive key.
#
# vfs5011_daemon.c already self-elevates via `execvp("sudo", ...)`
# when it detects geteuid() != 0 -- that's a plain fork()+exec(),
# which keeps the resulting root process attached to the SAME
# session/Mach namespace it was born into, rather than a "root
# LaunchDaemon" that starts outside any session entirely. This
# script adds a narrowly-scoped NOPASSWD sudoers rule (only for this
# exact binary path, nothing else) so that self-elevation can happen
# non-interactively -- there's no terminal attached for sudo to
# prompt on.
#
# This replaces both the earlier LaunchDaemon install
# (vfs5011_daemon_install.sh, retired) and v1 of this script.
#
# WHY THE LOG FILE IS EXPLICITLY CHOWN'D TO THE CONSOLE USER:
# Even after fixing the LWCR issue above, deploys still failed with
# EX_CONFIG. Root cause: launchd opens StandardOutPath/
# StandardErrorPath as the agent's owning user BEFORE it execs the
# program -- this happens before the daemon's own `sudo`
# self-elevation ever runs. The log file had been created via a plain
# `sudo touch` + `chmod 644`, leaving it root:wheel-owned; the console
# user couldn't open it for writing, and launchd folded that failure
# into the same generic EX_CONFIG as the constraint rejection,
# making it look identical to the already-fixed LWCR problem.
# Confirmed via a control test: chown'ing the existing log file to
# the console user, with zero other changes, flipped state from
# "not running" to "running" immediately.
#
# Run with sudo (writes to /etc/sudoers.d and /Library/Logs, and
# bootstraps into another user's session -- all need root):
#
#   sudo ./vfs5011_agent_install.sh
#
set -e

BINARY_PATH="/usr/local/libexec/vfs5011/vfs5011_daemon"
LABEL="com.hackintosh.vfs5011agent"
LOG_PATH="/Library/Logs/vfs5011agent.log"
SUDOERS_PATH="/etc/sudoers.d/vfs5011daemon"

# Superseded locations/labels to clean up on every install, so
# re-running this script (or upgrading from v1) never leaves two
# copies of the agent both trying to claim the same USB device.
OLD_DAEMON_LABEL="com.hackintosh.vfs5011daemon"
OLD_DAEMON_PLIST="/Library/LaunchDaemons/$OLD_DAEMON_LABEL.plist"
OLD_AGENT_PLIST_SYSTEM="/Library/LaunchAgents/$LABEL.plist"

if [ "$(id -u)" -ne 0 ]; then
    echo "This needs root (writes to /etc/sudoers.d and /Library/Logs, bootstraps another user's session). Run with sudo." >&2
    exit 1
fi

# v1.0.5: same hardware gate as vfs_client's Deploy option, duplicated
# here because prep_and_build.sh and a bare `sudo ./vfs5011_agent_install.sh`
# both call this script directly, bypassing vfs_client entirely. Uses
# system_profiler rather than a libusb helper since this is a plain
# shell script -- "Product ID: 0x0018" is always immediately followed
# by its own "Vendor ID: 0x138a" line in SPUSBDataType's per-device
# block, so pairing them with -A1 (rather than grepping each field
# separately across the whole tree) avoids a false match against some
# other 0x138a or 0x0018 device that isn't actually a VFS5011.
if ! system_profiler SPUSBDataType 2>/dev/null | grep -A1 "Product ID: 0x0018" | grep -qi "Vendor ID: 0x138a"; then
    echo "Error: No VFS5011 sensor detected on the USB bus (VID 0x138a / PID 0x0018 not found)." >&2
    echo "Refusing to install a background service tied to hardware that isn't present." >&2
    exit 1
fi

# Resolve this script's own directory so build_daemon.sh and the
# source files can be found regardless of the caller's cwd -- both
# a bare `sudo ./vfs5011_agent_install.sh` from the project folder
# and vfs_client's Deploy option (which invokes this by absolute
# path from g_exec_dir) land here correctly.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_SCRIPT="$SCRIPT_DIR/build_daemon.sh"
BUILT_BINARY="$SCRIPT_DIR/vfs5011_daemon"
INSTALL_DIR="$(dirname "$BINARY_PATH")"

if [ ! -x "$BUILD_SCRIPT" ]; then
    echo "Error: $BUILD_SCRIPT not found -- run this from inside the project folder." >&2
    exit 1
fi

echo "Building daemon from source..."
if ! (cd "$SCRIPT_DIR" && ./build_daemon.sh > /tmp/vfs5011_build.log 2>&1); then
    echo "Error: build failed. See /tmp/vfs5011_build.log for the compiler output." >&2
    exit 1
fi

if [ ! -x "$BUILT_BINARY" ]; then
    echo "Error: build_daemon.sh reported success but $BUILT_BINARY wasn't produced." >&2
    exit 1
fi

mkdir -p "$INSTALL_DIR"
cp "$BUILT_BINARY" "$BINARY_PATH"
chown root:wheel "$BINARY_PATH"
chmod 755 "$BINARY_PATH"

# The daemon resolves vfs5011_volume_mount.sh / _unmount.sh relative to
# its OWN running directory (g_exec_dir, derived from argv[0]) -- not
# the source project folder. That's invisible while testing via
# vfs_client or the raw daemon binary run directly from this folder
# (g_exec_dir just happens to BE this folder in that case), but once
# deployed as a LaunchAgent pointed at $INSTALL_DIR, the daemon is
# alone there with no mount/unmount scripts alongside it -- every lock
# episode fails at the very first step ("sh: .../vfs5011_volume_mount.sh:
# No such file or directory"). Keep the deployed copy self-contained.
for script in "vfs5011_volume_mount.sh" "vfs5011_volume_unmount.sh"; do
    if [ ! -f "$SCRIPT_DIR/$script" ]; then
        echo "Error: $SCRIPT_DIR/$script not found -- can't deploy a working daemon without it." >&2
        exit 1
    fi
    cp "$SCRIPT_DIR/$script" "$INSTALL_DIR/$script"
    chown root:wheel "$INSTALL_DIR/$script"
    chmod 755 "$INSTALL_DIR/$script"
done

# The binary's cdhash changes on every rebuild, so any prior TCC grant
# tied to the old cdhash is now stale -- re-sign and re-grant every
# single Deploy, not just once. See vfs5011_grant_accessibility.sh for
# why this became necessary (a NULL-csreq grant that worked fine on
# Sequoia silently did nothing on Tahoe).
GRANT_SCRIPT="$SCRIPT_DIR/vfs5011_grant_accessibility.sh"
if [ -x "$GRANT_SCRIPT" ]; then
    echo "Re-granting Accessibility for the freshly built binary..."
    "$GRANT_SCRIPT" "$BINARY_PATH"
else
    echo "Warning: $GRANT_SCRIPT not found -- Accessibility was NOT (re-)granted." >&2
    echo "Auto-type will silently fail to do anything until you run it manually." >&2
fi

# Figure out who's actually logged into the GUI console session --
# this is whose gui/<uid> domain we need to bootstrap into, and whose
# sudoers rule and ~/Library/LaunchAgents we need to write to.
# $SUDO_USER is set when this script is invoked via sudo (including
# from vfs_client's Deploy option, since vfs_client itself is already
# running as root by that point) -- prefer it, fall back to the
# console-owner lookup for a bare `sudo ./vfs5011_agent_install.sh`.
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

# Tear down anything from a previous run or the retired architecture.
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

# Narrowly-scoped passwordless sudo, ONLY for this exact binary path,
# with no arguments. Validated with visudo -c against a temp file
# before it's ever installed live, so a typo here can't break sudo
# system-wide.
echo "Adding scoped NOPASSWD sudo rule for $CONSOLE_USER -> this binary only..."
ESCAPED_PATH=$(printf '%s' "$BINARY_PATH" | sed 's/ /\\ /g')
TMP_SUDOERS="$(mktemp)"
cat > "$TMP_SUDOERS" <<SUDOERS
# Allows vfs5011_daemon (running as a LaunchAgent, console user) to
# self-elevate to root non-interactively via execvp("sudo", argv[0]),
# with NO arguments and NO other commands. Managed by
# vfs5011_agent_install.sh -- safe to delete if this project is removed.
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

# Log file lives in /Library/Logs (shared location) but MUST be
# owned by the console user, not root. launchd opens StandardOutPath/
# StandardErrorPath as the *agent's owning user* before it execs the
# program at all -- this happens before the daemon's own internal
# `sudo` self-elevation ever runs. A root-owned, non-world-writable
# log file here silently fails that pre-exec step and launchd reports
# it as a generic EX_CONFIG, indistinguishable at first glance from
# an actual launch-constraint rejection. (Root can still write to a
# user-owned file just fine once the daemon elevates itself, since
# root bypasses normal permission checks.)
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
# Deliberately OMITTED: LimitLoadToSessionType, KeepAlive.
# See the comment block at the top of this file -- both were
# confirmed via testing to trigger launchd's EX_CONFIG rejection on
# this install. Do not re-add either without re-testing against a
# fresh `launchctl print` (watch for "untrusted" / "managed LWCR" in
# the properties line).

chown "$CONSOLE_USER" "$PLIST_PATH"
chmod 644 "$PLIST_PATH"

if ! plutil -lint "$PLIST_PATH" >/dev/null 2>&1; then
    echo "Error: generated plist failed validation." >&2
    exit 1
fi

launchctl bootstrap "gui/$CONSOLE_UID" "$PLIST_PATH"

sleep 1
# 'exit' after the first match is deliberate -- launchctl print emits
# several "state = ..." lines (the top-level agent state, plus nested
# sub-object states). Without it, awk keeps overwriting its match and
# ends up reporting the LAST one (usually a nested "active"), not the
# top-level state we actually care about -- this caused a false
# "deployment failed" even when the agent was genuinely running.
STATE="$(launchctl print "gui/$CONSOLE_UID/$LABEL" 2>/dev/null | awk -F'= ' '/state =/{print $2; exit}')"

if [ "$STATE" = "running" ]; then
    echo ""
    echo "Installed and running: $LABEL in gui/$CONSOLE_UID (running as $CONSOLE_USER, self-elevating via sudo)"
    echo "Logs: $LOG_PATH"
    echo ""
    echo "Now lock the screen and swipe. Watch the log with:"
    echo "  tail -f $LOG_PATH"
else
    echo "" >&2
    echo "Warning: agent did not reach 'running' state (state=$STATE)." >&2
    echo "Check: sudo launchctl print gui/$CONSOLE_UID/$LABEL | grep -E 'state|last exit code|properties'" >&2
    exit 1
fi
