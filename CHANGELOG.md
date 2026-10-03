# Changelog

All notable changes to the Hackintosh-TouchID fingerprint authentication project are documented here.

---
## v1.1.0 - Current Development Target
**CHANGES ARE YET TO BE MERGED INTO ```MAIN```**

- **Oct 3** - Live update notifications: while the client sits at the main menu it re-checks the branch's `VERSION.txt` every minute. If a newer version or build is published while it is open, it shows the usual `[A]` / `[Y]` / `[N]` update prompt, and a declined build is not announced again until an even newer one appears. New Settings `[U]` toggles it (on by default, saved across launches). The check at launch is unchanged.
- **Oct 3** - Updater: the download line now names the update instead of the branch, for example `Downloading HTID Update v1.1.0 26B240...`, using the version and build from the remote `VERSION.txt`.
- **Oct 3** - Updater: `[A] Show changelog` is now readable in the terminal. Entries are word-wrapped to the window width with a hanging indent and a bold date, instead of long lines that broke mid-word.
- **Oct 3** - Metallica MIS: clearer `--force-pair` and `0x0404` messages. The `--force-pair` banner and `--help` no longer claim it wipes a paired sensor. They now say a sensor that is already paired refuses unauthenticated flash writes (status `0x0404`) and point to `--host-product` / `--host-serial`. The `0x0404` status name and the partition write failure line carry the same hint.
- **Oct 2** - Metallica MIS TLS fix: the "client finished" and "server finished" PRF labels were hashed with a trailing NUL byte (16 bytes instead of 15), so the Finished verify data never matched python-validity's and the sensor rejected the handshake with `illegal_parameter`. Found by comparing the handshake against python-validity's `tls.py` after the tester's `--list-records` log showed the alert. This was the first run of the secure handshake on real hardware. Confirmed on hardware: with the fix, `--list-records` completes the TLS handshake and lists the sensor's records.
- **Oct 2** - Metallica MIS debug log: the PSK keys in `[diag]` lines (pairing, calibration session and the `handle_priv()` failure line) are now redacted and shown as a short fingerprint (`<redacted, fp=xxxxxxxx>`) that still lets two runs be compared. Earlier `--debug` logs printed the keys in full, so the Oct 1 note that key material is never logged was wrong until this change. A TLS alert from the sensor is now decoded (level and name, for example `illegal_parameter`) instead of a generic handshake parse failure.
- **Oct 2** - Updater: after a successful update build the client now removes the installed daemon (LaunchAgent, daemon binary and its Accessibility grant) and prints `Daemon un-installed, Please run [3] Again.`, so a stale daemon never keeps running or blocks the client from launching. Enrolled fingers and the encrypted volume are kept.
- **Oct 2** - New launch arguments `--host-product` and `--host-serial` (Metallica MIS). They replace the spoofed Mac model and serial that HTID uses to derive the sensor's pairing key. For a sensor paired on Linux or Windows, pass the laptop's real DMI `product_name` and `product_serial` so `--list-records` and `--wipe-records` can open a session without re-pairing.
- **Oct 2** - Tester finding (Metallica MIS): `--force-pair` cannot wipe a sensor that is already paired to another host identity. The partition write goes out in plaintext and the sensor rejects it with `0x0404`. Record listing fails the HMAC check because HTID derives its key from the spoofed Mac identity while Linux used the real one.
- **Oct 2** - Website: new "Releases" section on the home page (current release v1.0.5, upcoming v1.1.0) and a Fpbootd section in the guide.
- **Oct 2** - Project website: a GitHub Pages site (served from `docs/`) with a home page, a documentation page (live changelog plus the latest 5 commits on every branch), a usage guide, the security policy and the contributing guide ("Help grow this!").
- **Oct 1** - Metallica MIS: new sensor record DB layer (`metallica_mis_db.c/.h`), a C port of python-validity's `db.py`. Can list and wipe the prints stored on the sensor, and verifies a wipe by re-reading afterwards. The enroll path is not built yet.
- **Oct 1** - New launch arguments: `--debug` (logs every USB transfer, TLS command and DB call with timestamps and hex dumps, plus libusb's own debug log, key material is never logged, keys show only as `<redacted, fp=...>`) and `--debug-full` (removes the payload size cap). Known sensor status words such as `04c3` and `04b3` are named in the log.
- **Oct 1** - New launch arguments `--list-records` (read only) and `--wipe-records` (asks you to type WIPE, deletes every print on the sensor, does not touch HTID's saved templates on the Mac).
- **Oct 1** - Build scripts and CI source lists updated for the new Metallica MIS files.
- **Oct 1** — Updater: when an update is found it now offers `[A]` Show changelog / `[Y]` Download and install / `[N]` Cancel. `[A]` fetches `CHANGELOG.md` from your branch and prints what's new since your version, so you no longer need GitHub to see what an update contains.
- **Sep 30** - Tester finding: the Metallica MIS `04c3` enroll failure is caused by a print already stored on the sensor for that user. Deleting it first lets enrollment succeed, which is what the new record DB layer is for.
- **Sep 30** - `[D] Diagnose` / `--diag-pid` report expanded with more system, USB and daemon detail, can now be saved to a file, and `[X] Uninstall` also cleans up the agent log.
- **Sep 30** - Release ETA updated to mid October 2026.
- **Sep 29** - `[X] Uninstall` implemented natively: removes the daemon for any sensor. The `[A] About` screen no longer clears instantly (added a Press Return pause). CLI menu example and supported sensor statuses updated in the README.
- **Sep 25** - Menu polish: help, Uninstall and Fpbootd entries, status rows, and fixed the `H`/`X`/`A`/`FP` options flashing before the screen clears.
- **Sep 23-24** - README gains an ETA disclaimer and a credits section, CONTRIBUTING.md updated.
- **Sep 21-22** - Fpbootd (pre-login authentication) spike code committed: base resources, `Info.plist`, a test harness, and install/uninstall scripts. Fixed its clang build issues. `prep_and_build.sh` now mentions `fpbootd-install.sh` when it finishes.
- **Sep 15** - Metallica MIS pairing root cause found: `init_flash()` returned early whenever the sensor already reported partitions, so `[P] Pair` silently did nothing on an already-paired sensor. Added `--force-pair`, which runs the full fresh-pairing sequence in the correct order.
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
