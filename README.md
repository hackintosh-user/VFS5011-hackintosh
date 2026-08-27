<p align="center">
  <img src="vfs_client_logo.svg" alt="VFS Client — Validity VFS5011 Fingerprint Auth" width="560">
</p>

<h3 align="center">VFS5011 Fingerprint Daemon for macOS</h3>

<p align="center">
  A libusb-based capture pipeline and NBIS matcher bringing Fingerprint sensors
  fingerprint authentication to macOS on unsupported (Hackintosh) hardware —
  lock screen unlock and System Settings authentication prompts, driven by
  a real fingerprint sensor instead of a password.
  
  * Current version: v1.1.0
  * If you want to see the changes that happen to the code, please head into the Change log with [This link](https://github.com/hackintosh-user/VFS5011-hackintosh/blob/main/CHANGELOG.md)
  * Please refrence the Table that has the Supported sensors to make sure you can use this tool. [This link should take you there](https://github.com/hackintosh-user/VFS5011-hackintosh/tree/active-development#supported-sensors)
</p>

---

## Overview

This project ports the USB capture protocol for supported fingerprint sensors to a standalone
macOS C code, and pairs it with NIST's NBIS fingerprint matching suite
(`mindtct` for minutiae extraction, `bozorth3` for matching) to provide
working fingerprint authentication on Hackintosh hardware that shipped
with this sensor but has no native macOS driver support.

It is not a kernel extension or a libfprint port. It runs as a small
background daemon that watches for authentication prompts using the
Accessibility APIs, captures a fingerprint swipe on demand, matches it
against enrolled templates, and — on a match — types the stored password
into the prompt automatically.


## Supported Sensors

This Table will have the currently Supported Sensors or sensors in **Development** or **Planned Support**. Please keep your expectations in check, this is a hobby project.
| Sensor Name / Model    | PID  | VID  | Support status |
|------------------------|------|------|----------------|
| Validty VFS5011        | 138A | 0018 | **Supported**  |
| Synaptics Metallica MIS| 006cb| 009a | **Developing** |
| Synaptics Metallica MIS| 138A | 0097 | **Developing** |
| UPEK TouchStrip        | 147e | 2016 | **Planned**    |

 * Please Keep in Mind that some sensor names are **too long to fit in the table** the PID:VID for each currently or planned sensor will be in the table and regardless the client it self checks if your sensor is supported.
## Features

Please Keep in mind some features for other sensors may be **broken** or **buggy** this project isnt a full Dev-Ops team.

- Fingerprint capture and matching entirely native to macOS, no Linux
  kernel driver or libfprint dependency at runtime
- Multi-finger enrollment (multiple fingers, best-match-wins verification)
- Lock screen unlock via fingerprint swipe
- System Settings / System Preferences authentication sheet ("padlock")
  unlock via fingerprint swipe
- Authentication for Finder password prompts.
- Works with browsers (Currently confirmed working with Orion but im not sure for others)
- Passwords.app lock screen unlock via fingerprint swipe
- Keychain Access "confidential information" consent prompt unlock via fingerprint swipe
- Enrolled templates and the stored password are kept on a dedicated,
  encrypted APFS volume, not in plaintext on the boot volume
- Self-elevating daemon with a narrowly scoped, single-purpose sudoers
  rule rather than a blanket NOPASSWD grant
- Interactive terminal client (`vfs_client`) for enrollment, verification,
  and one-command deployment of the background daemon
-  **Optional** Menu bar Application for disabling, enabling, Restarting the Daemon + sends notifications when authentication is ready if Sensor light is too dim / too slow
-  Growing supported sensor list with testers.

## Menu Bar Companion App (optional)

`vfs5011-menubar/` contains an optional menu bar app that surfaces the
daemon's auth events as real notifications — useful since the sensor's
LED can be too dim to notice on its own.

- Notifies you the moment the daemon wants a swipe ("Swipe to
  authenticate! 🫆"), then whether it succeeded or failed
- One-click toggle to pause/resume fingerprint auth without touching Terminal
- Registers itself as a login item automatically on first launch (macOS
  13+, via `SMAppService` — no manual LaunchAgent setup needed)
- "About hackintosh Touch-ID" menu item with a short project summary and a link back
  here

Completely optional — the daemon works identically with or without it
running.

### Installing
A Pre compiled app is in the [Releases page](https://github.com/hackintosh-user/VFS5011-hackintosh/releases/) but if you prefer to compile, here's the commands to run:

```bash
cd vfs5011-menubar
chmod +x build_menubar_app.sh
./build_menubar_app.sh
open "build/Hackintosh Touch-ID.app"
```

Requires Xcode Command Line Tools (`xcode-select --install`) for `swiftc`
and `iconutil`. No other dependencies. And it needs **macOS 13 Ventura and later,** like the Daemon, Older versions may work but I don't know if they do you are on your own if you are on monterey and older.

First launch will be blocked by Gatekeeper since this is ad-hoc signed,
not notarized with a paid Apple Developer account — right-click the app →
**Open** → **Open** again, or run `xattr -cr "Hackintosh Touch-ID.app"` once.

### Wiring it to the daemon

The menu bar app talks to the daemon over macOS distributed
notifications — the same mechanism the daemon already uses for its own
`screenIsLocked`/`screenIsUnlocked` handling. **The daemon needs a small
patch to actually send those events**; see
[`vfs5011-menubar/daemon-patch/README.md`](Vfs5011-menubar/daemon-patch/README.md)
for the exact 6-line diff against `vfs5011_daemon.c`. Without the patch,
the app runs standalone and just never receives anything — harmless, but
silent.

Known limitations:

- Pause/resume is global — disabling fingerprint auth pauses it for every
  recognized prompt at once (lock screen, padlock, Finder, pkg installer,
  Time Machine, Apple ID's local password step), not just one surface.
- Not notarized (see Gatekeeper note above).
- Pre-Login is not a simple workaround. Could takes months or years to fix.
- Requires you to go into System settings -> notfifications -> show Previews: set to ```Always``` otherwise it will show up but wont tell you if its ready to swipe or a failed the swipe test.


## Requirements
- **macOS 13 Ventura and later** (older versions may work, but I offer **0 support for them**)
- Xcode Command Line Tools (`xcode-select --install`)
- [Homebrew](https://brew.sh)
- `libusb` (`brew install libusb`)
- `openssl@3` (`brew install openssl@3`) [will offically be Deprecated on Novemeber 1st 2026 but should work fine stil]
- `innoextract` (`brew install innoextract`)
- Accessibility permission granted to the daemon (handled automatically
  by the deploy script, see below)
- OpenCore v1.0.6 or later (For maximum Security | Release or Debug are fine) but on older versions of OpenCore will work but I don't support it. You are on your own if you use v1.0.5 or older.

## Building

```bash
git clone https://github.com/hackintosh-user/VFS5011-hackintosh.git
cd /path/to/vfs5011-hackintosh-main
chmod +x prep_and_build.sh
./prep_and_build.sh
```

This produces two binaries in the project directory:

- `vfs_client` — the interactive menu used for enrollment, verification,
  and deployment
- `vfs5011_daemon` — the background daemon that watches for
  authentication prompts and performs the fingerprint check

`ax_probe`, a standalone Accessibility-API diagnostic tool used during
development, is not built by `build.sh`. It has no dependency on
`libusb` or NBIS and can be built on its own if needed:

```bash
clang ax_probe.c -o ax_probe -framework CoreFoundation -framework ApplicationServices
```

## Usage

**WARINING 1**: On macOS 12 Monterey And older you will be shown a message that clearly states there is **0 Support for any OS older than macOS 13 Ventura and later** you are on your own for any issues that may arise on this version of macOS.

**WARNING 2**: Same goes for the OpenCore Boot loader: the minimum version is OpenCore v1.0.6 or later. v1.0.5 and older are officially not supported. You are on your own if you encounter any issues on v1.0.5 or older.

Run the client with this command:

```bash
sudo ./vfs_client
```
Then, you should be greeted with this **interactive CLI menu for VFS client**
```
[1] Enroll a Finger
[2] Verify Fingerprint Match [Score / 20]
[3] Deploy VFS Client for Authentication Services

[S] Settings
[A] About
[Q] Quit
```

- **Enroll a Finger** — captures several swipes of a finger and stores
  them as a named template on the encrypted volume. Repeat for as many
  fingers as needed; enrollment is capped and uses best-match-wins
  verification across all enrolled fingers.
- **Verify Fingerprint Match** — a standalone test of the capture and
  matching pipeline, independent of the daemon, useful for confirming
  the sensor and templates are working before deploying.
- **Deploy VFS Client for Authentication Services** — rebuilds the
  daemon from source, installs it as a per-user LaunchAgent, re-signs it
  and regenerates its Accessibility permission grant, and installs a
  narrowly scoped sudoers rule so the daemon can self-elevate when it
  needs root access to the USB device. This step is re-run every time
  you deploy, so the daemon is never left running a stale build.

Once deployed, the daemon runs continuously in the background. At the
lock screen, or when a System Settings authentication sheet appears, it
prompts for a fingerprint swipe and, on a match, types the stored
password automatically.

## How it works

- **Capture**: the VFS5011's raw USB protocol (initialization sequence,
  swipe capture, image reassembly) is implemented directly against
  `libusb`, without going through any Linux-specific driver stack.
- **Matching**: captured swipes are run through NIST's `mindtct` for
  minutiae extraction and `bozorth3` for match scoring, both compiled
  directly into the client and daemon.
- **Storage**: enrolled templates and the authentication password live
  on a dedicated, encrypted APFS volume rather than in a plaintext file
  on the boot volume. The passphrase for that volume is stored in the
  System keychain.(Not seen by finder or the user)
- **Detection**: the daemon polls the system's focused UI element via
  the Accessibility APIs to recognize when a lock screen or System
  Settings password field is active, rather than hooking or patching
  any system process.
- **Privilege model**: the daemon runs as the console user and only
  escalates to root, via a narrowly scoped sudoers rule, for the
  specific operations that require it (accessing the USB device). It is
  not installed as a system-wide LaunchDaemon.

## Tested configurations

| macOS Version | Result |
|---|---|
| Sequoia| Lock screen and padlock unlock confirmed working |
| Tahoe  | Lock screen and padlock unlock confirmed working (separate test volume) |
| Sonoma | lock screen And padlock confirmed working on DV6-7080ee Sandy bridge.   |
| Ventura| Remains to be TESTED.                                                   |

Tested on an Intel Ivy Bridge Hackintosh laptop (HP Pavilion DV6/EliteBook
class hardware) with the VFS5011 sensor at USB `138A:0018`.

## Limitations

- macOS only. This is not a libfprint driver and is not intended to run
  on Linux — if you're on Linux with this sensor, use the existing
  open-source libfprint driver directly instead (see Acknowledgments).
- Targets macOS 13 Ventura and later only; the client prints a warning if
  run on an older Darwin version.
- No PAM module exists for `sudo` in a terminal. An Accessibility-based
  approach for `sudo` prompts was prototyped during development and
  intentionally removed to keep scope limited to lock screen and System
  Settings authentication. (there are still plans to attempt coding This)
- Ad hoc code signing is used for the daemon binary and its
  Accessibility grant, both regenerated on every deploy. There is no
  notarization or Developer ID signing.
- a Cold boot sign in is not supported. Could take Months or Years to produce something usable.

## Acknowledgments

This project would not exist without the following prior work:

- **NBIS (mindtct, bozorth3)** — the fingerprint minutiae extraction and
  matching engine used in this project is NIST's public NBIS
  distribution. Developed by employees of the U.S. federal government
  in the course of their official duties and, per 17 U.S.C. section 105,
  not subject to copyright protection in the United States.
  https://www.nist.gov/services-resources/software/nist-biometric-image-software-nbis

- **VFS5011 capture protocol** — the USB initialization and capture
  sequence for the VFS5011 sensor was originally implemented as a
  libfprint driver by Arseniy Lartsev and AceLan Kao (Canonical),
  licensed under the GNU Lesser General Public License v2.1. This
  project's macOS capture pipeline was independently written in C,
  informed by that original driver's protocol logic.
  https://github.com/ars3niy/fprint_vfs5011

If you are on Linux and have a Fingerprint sensor, the libfprint driver
linked above is the right tool to use directly — this project exists
specifically to bring the same capability to macOS, where no equivalent
driver exists.

## License

Released under the BSD 3-Clause License. See [LICENSE](LICENSE) for
the full text, including third-party attribution for NBIS and the
VFS5011 capture protocol.
