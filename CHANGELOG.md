# Changelog

All notable changes to the VFS5011 Hackintosh fingerprint authentication project are documented here.

---
## v1.1.0 - Current Development Target
**CHANGES ARE YET TO BE MERGED INTO ```MAIN```** 

**Metallica MIS pairing wired into the client (Aug 28)**
- `hack_touchid_client.c` gains a `[P] Pair Sensor` menu item, shown only when a Metallica MIS identity (`06cb:009a` / `138a:0097` / `138a:009d`) is detected. It runs the same plaintext-bootstrap + pairing + firmware-upload sequence the standalone `metallica_mis_daemon` test harness does, with the same destructive-write warnings and a typed `PAIR` confirmation gate.
- `metallica_mis_open_device()`, `metallica_mis_close_device()`, `metallica_mis_send_init()`, and `metallica_mis_do_pairing()` are now exposed from `metallica_mis_daemon.c` via a new `metallica_mis_daemon.h`, so the client links and calls them directly instead of requiring a separate binary. `metallica_mis_daemon.c`'s own `main()` is compiled out for this build via `-DHACK_TOUCHID_CLIENT_BUILD`, so the standalone test harness (`build_metallica_mis.sh`) still works unchanged.
- `build_client.sh` now links `metallica_mis_daemon.c` + its TLS/flash/firmware-upload dependencies and requires OpenSSL, same as `build_metallica_mis.sh`.
- Pairing only — capture (`Enroll`/`Verify`) for this sensor family still doesn't exist, so `backend_available` stays `0` in `supported_sensors.h` and those menu items still refuse for Metallica MIS exactly as before.

**UPEK capture wired into the client (Aug 29)**
- `hack_touchid_client.c`'s capture dispatch is no longer hardcoded to VFS5011: `capture_fingerprint_image()` is now a real dispatch wrapper that routes to `vfs5011_capture_fingerprint_image()` or the new `upek_capture_fingerprint_image()` based on `g_detected_sensor`. `open_device()`/`close_device()` are similarly generalized (correct VID:PID, correct endpoints to clear per sensor family).
- New `[U] Test Capture (UPEK, experimental, no save)` menu item, shown only when a UPEK/AuthenTec TouchStrip (`147e:2016`) is detected — runs one real capture + minutiae extraction and reports the result (plus a debug `.pgm` saved to `/tmp`), without touching enrolled-finger storage. Lets Cold_Salamander7764 test straight from the client instead of separately building `upek_daemon.c`'s own `UPEK_STANDALONE_TEST` smoke-test binary.
- `upek_capture_fingerprint_image()` is now exposed via a new `upek_daemon.h`. `upek_daemon.c`'s own smoke-test `main()` was already gated behind `#ifdef UPEK_STANDALONE_TEST`, so no build-flag gymnastics were needed to link it into the client (unlike Metallica MIS's `-DHACK_TOUCHID_CLIENT_BUILD`).
- `build_client.sh` now also links `upek_daemon.c`.
- This is a capture *test*, not full Enroll/Verify support — `backend_available` stays `0` for UPEK until this has an actual successful real-hardware pass; `Enroll`/`Verify`/`Deploy` still refuse for this sensor exactly as before.

**Fun verbose boot + Linux-style `[ OK ]` status (Aug 29)**
- New `vfsc_verbose_boot_flood()` prints a ~67-line fake macOS/IOKit/XNU-style kernel log flood before the real startup checks, with randomized 60-220ms per-line delays (~10 seconds total, organic uneven pacing) — a real reference to a Hackintosh's own `-v` boot flag. Runs automatically on launch, respects `--q`/`--quiet`.
- The 3 real startup gate checks (OpenCore version, sensor detected, daemon version match) plus the final init-complete line now print a green Linux/systemd-style `[ OK ]` tag via a new `vfsc_status_line_ok()` helper — kept strictly separate from the purely decorative flood lines above, which never get the tag.
- A bold `Welcome to HTID Client!` greeting now prints right before the menu shows for the first time.

**Sensor backend parity: UPEK gains real open/close/presence, Metallica MIS gains a presence check (Aug 29-30)**
- `upek_daemon.c` gains `upek_open_device()` / `upek_close_device()` / `upek_sensor_is_present()` / `upek_image_width()`, promoted out of its standalone smoke-test `main()`'s old bare inline logic — now uses the same retry-with-backoff pattern (`vfs5011_daemon.c`'s `open_device()`) since there's no reason to assume a T420's USB stack is any less finicky than the one that pattern was originally written for. Declared in `upek_daemon.h`.
- `metallica_mis_daemon.c` gains `metallica_mis_sensor_is_present()` — its own short-lived libusb context, never opens/claims, checks all three known OEM identities (`06cb:009a` / `138a:0097` / `138a:009d`) rather than just one. `metallica_mis_open_device()`/`close_device()` already existed with full retry + multi-identity logic from the pairing work above, so this was the one missing piece to match the other two sensors' backend shape. Declared in `metallica_mis_daemon.h`.
- All three sensor backends (VFS5011, Metallica MIS, UPEK) now expose the same consistent shape (open/close/presence-check), which is prep work for an eventual shared daemon-core refactor — deferred for now since it would require moving ~1,270 lines of `vfs5011_daemon.c`'s sensor-agnostic logic (state machine, AX/padlock watcher, notification callbacks, password typing) behind a backend interface, which needs a real compiler in the loop to do safely rather than blind chat-based edits.


  
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
