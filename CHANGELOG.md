# Changelog

All notable changes to the Hackintosh-TouchID fingerprint authentication project are documented here.

---
## v1.1.0 - Current Development Target
**CHANGES ARE YET TO BE MERGED INTO ```MAIN```**

- **Sep 14** — Metallica MIS pairing bug: pinpointed the decrypt failure to the HMAC check specifically (not key derivation, which is now confirmed correct). Added a write-then-immediate-readback diagnostic to isolate whether the write or the post-write reboot is at fault. Root cause still open.
- **Sep 13** — Universal rebrand: `VFSStore` volume renamed to `HackTouchIDStore`, 10 `vfs5011_*` files renamed to `hack-touchid-*` (core VFS5011-specific daemon files kept their names on purpose). Verified nothing broke end-to-end post-rename.
- **Sep 13** — macOS floor raised from Ventura 13 to Sonoma 14 (Homebrew wasn't practically usable on Ventura).
- **Sep 13** — Auto-updater gets real progress bars for download/extract/build instead of a garbled spinner.
- **Sep 13** — `MATCH_THRESHOLD` raised 20 → 40; added `--q`/`--quiet` flag and verbose launch mode.
- **Sep 13** — Metallica MIS calibrate bug traced to `parse_tls_flash()` returning a generic failure for every error case; added per-check diagnostics so the real failure mode is now visible in logs.
- **Sep 12** — Fixed a real pairing bug: `do_pairing()` was reporting success before the sensor actually finished re-enumerating post-reboot. Now polls for real re-enumeration before declaring pairing done.
- **Sep 11-12** — Found and fixed the root cause of Metallica MIS calibrate failing with status `0x0404`: on an already-paired device, the secure session was never actually being established, so commands were silently sent in plaintext. Ported `read_flash`/`read_tls_flash` and wired up session re-establishment to fix it.
- **Sep 10** — Added `[D] Diagnose` menu item + `--diag-pid` flag: prints a copy-paste-friendly health report (versions, sensor/backend status, daemon state, template setup, Accessibility grant).
- **Sep 8-9** — Menu bar app gains daemon-health checks + a "Reinstall Daemon" notification action (`--deploy-agent` flag). Project folder renamed `Vfs5011-menubar` → `Hackintosh-TouchID-Menubar` across all references.
- **Sep 8** — Fixed a real Metallica MIS calibrate bug: two transcribed-wrong hex tables were garbling chunk boundaries. Replaced with verified-correct upstream data.
- **Sep 7** — Client can now self-add to `PATH` (self-healing symlink); fixed a latent path-resolution bug this exposed.
- **Sep 7** — Fixed the update-checker silently never firing in default (verbose) boot mode.
- **Sep 7** — Metallica MIS `calibrate()` orchestration loop ported and wired into the client, plus several build-system/compile bugs found and fixed along the way.
- **Aug 30** — Fixed a real-hardware pairing failure (over-strict status check that upstream doesn't have). Added a diagnostic sensor-identify probe to pairing.
- **Aug 29-30** — UPEK and Metallica MIS backends brought up to parity with VFS5011 (open/close/presence-check).
- **Aug 29** — UPEK capture wired into the client (test-only, not full Enroll/Verify yet). Added the verbose boot flood + `[ OK ]` status styling.
- **Aug 28** — Metallica MIS pairing wired into the client.

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
