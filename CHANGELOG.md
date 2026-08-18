# Changelog

All notable changes to the VFS5011 Hackintosh fingerprint authentication project are documented here.

---
## v1.1.0 - Current Development Target, August 18th, 2026
This list will change during development:

- Rename: vfs_client.c -> hack_touchid.c
- implementation of checking for supported sensor(s)

  
## v1.0.5 — August 18th, 2026 (1:50AM KSA time)

**Daemon/client version sync**
- New `VFS5011_PROJECT_VERSION` macro in `vfs5011_proto.h` — single source of truth for the version string, shared by both `vfs_client.c` and `vfs5011_daemon.c` so they can never silently drift apart.
- Daemon gains a `--version` flag, checked before the root re-exec / OpenCore gate / daemon startup — just prints the version and exits(0). Cheap and side-effect-free.
- Client startup now shells out to the installed daemon binary (`popen(... --version ...)`) and compares its version against its own. On mismatch, prints the daemon's detected version and blocks launch, pointing to `prep_and_build.sh` / `vfs5011_agent_install.sh` (run from outside the client) rather than the in-menu Deploy option, since the client has already exited by that point. Skips the check (and continues) if no daemon is installed yet.

**Sensor presence gate**
- Client startup now checks for the VFS5011 (VID `0x138a` / PID `0x0018`) on the USB bus before proceeding, printing a verbose "Checking if Sensor is active / enabled..." line.
- If found: "continuing...". If not found: prints a "Sensor not found. Launching Failed" message with the VID:PID and blocks/exits the client entirely.
- `vfs5011_agent_install.sh` independently hard-gates on sensor presence too (via `system_profiler`), since `prep_and_build.sh` calls it directly and bypasses the client's check.

**Daemon install path**
- Moved from `/Library/Application Support/VFSDaemon/vfs5011_daemon` to `/usr/local/libexec/vfs5011/vfs5011_daemon`.
- The old path's embedded space was a recurring source of quoting bugs across the sudoers rule, plist generation, and the grant-accessibility script — the new path has none.
- Updated in `vfs_client.c` (`DAEMON_INSTALL_PATH`), `vfs5011_agent_install.sh` (`BINARY_PATH`), and `vfs5011_grant_accessibility.sh` (default arg).

**Confirmed working live on the DV6**: OpenCore check, sensor gate, and daemon version gate (v1.0.5 detected) all pass cleanly, landing on the main menu with the daemon Deployed and 2 fingers enrolled.

---

## v1.0.4 — August 16th, 2026

**macOS floor lowered to Ventura 13**
- Whole stack (daemon, client, menu bar app) lowered from a Sequoia 15 floor to Ventura 13 (Darwin 22.0.0+), after confirming via source review that no actual Sequoia/Sonoma-only API is used anywhere.
- `check_macos_version_warning()` in `vfs_client.c` now warns rather than blocks below Darwin 22.
- Root cause of the menu bar app's real blocker wasn't `Info.plist` (which already said `LSMinimumSystemVersion 13.0`) — it was `build_menubar_app.sh`'s `swiftc` invocation missing a `-target` flag, so the compiled binary's `LC_BUILD_VERSION` silently inherited the build host's toolchain default (~15.0 on the Sequoia dev machine). Fixed by adding `-target x86_64-apple-macosx13.0`, making the build deterministic regardless of build host OS.
- Confirmed working end-to-end on Sonoma 14 real hardware (second DV6, "Sandy," build 7080ee).

**No-sensor false-notification fix**
- The daemon previously had no sensor-presence check at all — `arm_polling_for_trigger()` fired the "Swipe to authenticate!" notification on every lock/auth-prompt regardless of whether a sensor was attached, only discovering "Device not found" later inside capture. On a no-sensor install this meant a notification + automatic failure notification on every single lock, forever.
- Fixed with `vfs5011_sensor_is_present()` — a cheap, non-invasive USB enumeration check (its own short-lived libusb context, VID/PID comparison only, never opens/claims the device) — called fresh on every trigger before any notification fires, so it self-recovers if a sensor is plugged in mid-session with no daemon restart needed. Also added a one-time non-fatal startup warning if no sensor is detected at launch.

**Pre-login (login-window) fingerprint auth: investigated and closed**
- Fully investigated across two approaches and abandoned as not worth pursuing:
  - **AX-based**: Built `ax_probe_loginwindow` + a TCC.db insert to pre-grant Accessibility pre-login. `AXIsProcessTrusted()` confirmed `TRUE` at genuine pre-login boot (3 separate tests) — but even fully trusted, `AXUIElementCreateApplication` on `loginwindow.app` returned only an empty root element every time. The login screen's UI isn't exposed via a normal AX tree at all — an architectural wall, not a permissions problem.
  - **IOHIDUserDevice (virtual HID keyboard)**: Failed at `IOHIDUserDeviceCreate` with "not entitled" — requires an `amfi_get_out_of_my_way=1` boot-arg (a system-wide entitlement-verification bypass) to work around illegitimately. Explicitly rejected as not worth that tradeoff.
- **Decision**: closed/abandoned. The existing post-login lock-screen flow (LaunchAgent + AX) remains the sole supported auth surface.

---

## v1.0.3 — August 15th, 2026

**Instant swipe-prompt fix**
- Diagnosed and fixed a 3–5 second delay between screen-lock and the sensor lighting up / "swipe now" prompt appearing.
- Root cause: `arm_polling_for_trigger()` called the template loader synchronously — a full encrypted APFS volume mount/decrypt/read/unmount — *before* setting `STATE_POLLING` and firing the swipe-requested notification, so the entire disk+crypto round trip sat in the critical path before the user saw anything.
- Fix: the trigger handler now sets `STATE_POLLING` and fires the notification immediately, then spawns a detached background thread to do the actual mount/load. A new `g_templates_ready` atomic + `g_templates_lock` mutex hold/synchronize a real swipe that comes in before templates finish loading, rather than discarding it. Verified live on the DV6: lock-to-match went from a felt 3–5s delay to instant.

**CI**
- Added GitHub Actions CI (`.github/workflows/ci.yml`) with 5 jobs: shellcheck, cppcheck, build-daemon-and-client, matcher-unit-tests, build-menubar-app. All green.

**Menu bar companion app**
- Built a Swift/AppKit status-bar app using `CFNotificationCenterGetDistributedCenter` (matching the daemon's proven lock/unlock IPC pattern across the root/user boundary).
- Custom fingerprint-glyph icon, working enable/disable toggle. Restart Daemon button still a no-op stub at this point.

---

## v1.0.2 — August 13th, 2026

- Confirmed (via empirical `ax_probe.c`-driven methodology) that Passwords.app and Keychain Access (`coreautha`) are valid auth surfaces for the existing post-login flow.
- Added an OpenCore minimum-version NVRAM gate — warns (no hard block) if the detected OpenCore version is below 1.0.6.

---

## v1.0.1 - August 10th, 2026

- Fixed the daemon to load templates from the `fingers/` directory, enabling proper multi-finger support (previously only read a single template location).

---

## v1.0.0 — Initial release | August 5th 2026

- Built the full capture + matching pipeline from scratch: USB enumeration, the sensor's 77-step init handshake, swipe capture with offset-correlation alignment, and NBIS (`mindtct`/`bozorth3`) minutiae extraction and matching.
- Enrollment: `ENROLL_SWIPES=5` with a self-consistency gate (`MIN_SELF_CONSISTENCY=15`); matching threshold `MATCH_THRESHOLD=20`.
- Deployed as a LaunchAgent + NOPASSWD sudoers rule. Confirmed working end-to-end on real hardware: lock → swipe → match → password auto-typed → unlock.
- USB stability fixes (retry logic, per-swipe open/close cycle).
- Fixed an exit code 78 (`EX_CONFIG`) issue caused by a root-owned log file preventing `launchd`'s pre-exec file open.
- Template storage on an encrypted APFS volume ("VFSStore"), with the volume passphrase kept in the system keychain.
- Licensed BSD 3-Clause, with credit to NIST/NBIS and to Arseniy Lartsev + AceLan Kao for the original libfprint VFS5011 driver (LGPL 2.1).
