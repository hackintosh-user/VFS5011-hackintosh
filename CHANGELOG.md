# Changelog

All notable changes to the Hackintosh-TouchID fingerprint authentication project are documented here.

---
## v1.1.0 - Current Development Target
**CHANGES ARE YET TO BE MERGED INTO ```MAIN```**

- New sensor: UPEK/AuthenTec TouchStrip (`147e:2016`), experimental, capture test only for now.
- New sensor: Synaptics Metallica MIS, experimental. A native port of python-validity (pairing, secure session, calibration, enroll and verify) now works on real hardware. `[1]` Enroll now enrolls this sensor straight from the menu: it asks before replacing prints already stored on the sensor, sets up a never-paired sensor by itself, and keeps diagnostics in `/var/log/htid-metallica-enroll.log`. If the sensor was paired on Windows or Linux, the client asks once for the laptop's real product name and serial (with hints on where to find them) and remembers them. The sensor light now comes back on before every touch. `--enroll-test`, `--verify-test`, `--list-records`, `--wipe-records`, `--host-product`, `--host-serial` and `--debug` remain for troubleshooting. Verified on `06cb:009a`, while `138a:0097` and `138a:009d` are detected but untested. Unlock integration is still in progress.
- Universal client: the project is now Hackintosh-TouchID, `hack-touchid` is added to your PATH, and the template store volume is now `HackTouchIDStore`.
- Sensor-aware setup: `prep_and_build.sh` detects your sensor and installs only what it needs.
- `[1]` Enroll now announces the detected sensor ("Declaring sensor type...") and stops with a clear message for sensors that are capture test only.
- Self-updater: checks at launch and while the menu is open, `[CU]` checks on demand, with `[A]` changelog / `[Y]` install / `[N]` cancel, progress bars, and the `--check-updates` and `--menu-updater` arguments. The menu bar's Update Client button now redeploys the daemon for you after installing.
- Menu bar app v1.4.0: client update notifications with an "Update Client" button, "Reinstall Daemon" health alerts, and 🔻 / ✅ / ❌ swipe notifications.
- Swipe to Lock (macOS Tahoe only): one swipe locks the screen, with adjustable sensitivity (`[L]`).
- Lockout after 4 failed swipes, plus a "swipe was too weak" notification.
- Sensor yield: Enroll, Verify and View Fingerprint work while Swipe to Lock is on.
- New menu items: `[T]` Sensor Test, `[D]` Diagnose (`--diag-pid`), `[X]` Uninstall for any sensor, and a regrouped Settings menu.
- New launch arguments: `--deploy-agent` and `--q` / `--quiet`.
- macOS 14 Sonoma is now the minimum, and `MATCH_THRESHOLD` went from 20 to 40.
- Project website (GitHub Pages) with a guide and the live changelog.

---
## v1.0.5 — August 18th, 2026
- Daemon/client version sync (shared version macro, `--version` flag, client blocks launch on mismatch).
- Added a sensor-presence gate on client startup.
- Moved daemon install path to avoid a recurring space-in-path quoting bug.
- Confirmed working end-to-end on real hardware.

---
## v1.0.4 — August 16th, 2026
- Lowered macOS floor from Sequoia 15 to Ventura 13 across the whole stack; fixed the menu bar app's actual version-floor bug (missing `swiftc -target` flag).
- Fixed a false-notification bug: the daemon would fire a swipe prompt (and automatic failure) on every lock even with no sensor attached.
- Investigated and closed pre-login (login-window) fingerprint auth — architecturally blocked on both approaches tried; decision is to not pursue it further.

---
## v1.0.3 — August 15th, 2026
- Fixed a 3-5 second delay between screen-lock and the swipe prompt appearing (template loading moved off the critical path onto a background thread).
- Added GitHub Actions CI (5 jobs, all green).
- Built the Swift/AppKit menu bar companion app.

---
## v1.0.2 — August 13th, 2026
- Confirmed Passwords.app and Keychain Access as valid auth surfaces.
- Added an OpenCore minimum-version warning gate.

---
## v1.0.1 - August 10th, 2026
- Fixed multi-finger support (daemon now reads all templates in `fingers/`, not just one).

---
## v1.0.0 — Initial release | August 5th 2026
- Built the full capture + matching pipeline from scratch (USB enumeration, sensor init handshake, swipe capture, NBIS minutiae matching).
- Enrollment/matching thresholds set; deployed as a LaunchAgent + NOPASSWD sudoers rule.
- Confirmed working end-to-end on real hardware.
- Template storage on an encrypted APFS volume ("VFSStore"), passphrase in the system keychain.
- Licensed BSD 3-Clause, with credit to NIST/NBIS and to Arseniy Lartsev + AceLan Kao for the original libfprint VFS5011 driver.
