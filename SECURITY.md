# Security Policy

## Overview

Hackintosh TouchID is a fingerprint authentication daemon that intercepts macOS system authentication prompts (login screen, System Settings/Preferences padlock, and Keychain autofill prompts) and satisfies them using a fingerprint swipe captured from a supported USB fingerprint sensor, matched against locally stored templates.

The project supports multiple sensor backends rather than a single chip, and that list is expected to keep growing. Different sensor generations have real architectural differences (some do matching on the host, some do matching on-chip and never expose raw image data), so read the "Known Issues / Per-Backend Caveats" section below rather than assuming every sensor behaves identically.

This project interacts with sensitive parts of macOS (authentication, sudoers, LaunchAgents, Keychain) and is built and maintained by a hobbyist as an open-source community project. It is **not audited by Apple or any third-party security firm**, and it should be evaluated accordingly before you trust it with anything you care about.

## Threat Model / What This Project Does and Does Not Protect Against

**Intended use case:** convenience authentication on a personal Hackintosh, replacing password entry with a fingerprint swipe, on a machine you physically control.

**This project does NOT claim to:**
- Match the security guarantees of Apple's Secure Enclave-backed Touch ID (there is no secure enclave on Hackintosh hardware; matching for most backends happens in userspace).
- Protect against a sophisticated attacker with physical access and time (e.g. someone who can dump the encrypted volume and brute-force the passphrase, or replace the daemon binary).
- Defend against fingerprint spoofing (fake/molded fingerprints). None of the currently supported sensors do liveness detection.

**Known-sensitive components:**
- **Fingerprint templates** are stored in an encrypted APFS volume (`HackTouchIDStore`), with the passphrase held in the System keychain. If your System keychain or FileVault is compromised, this volume's protection is only as strong as that.
- **The daemon runs with elevated privileges** and includes a narrowly-scoped `sudoers` `NOPASSWD` rule to allow it to interact with authentication prompts without repeatedly asking for a password itself. Review the exact rule before installing — it should be scoped to the daemon binary only. Any `NOPASSWD` rule increases attack surface if the daemon binary itself is compromised, so keep the daemon's file permissions locked down (root-owned, not writable by your user).
- **The auth-prompt watcher** is allow-listed to specific system processes (login window, System Settings/Preferences padlock, and Keychain-related prompts). It is deliberately *not* wired up to arbitrary third-party app password fields or terminal `sudo` prompts.

## Known Issues / Per-Backend Caveats

- **Backend maturity varies.** Newly added sensor backends go through a period of real-hardware testing before they're considered stable — check a given backend's status before relying on it for anything you care about. A backend compiling cleanly is not the same as it being hardware-verified.
- **On-chip-matching backends store no local image or raw minutiae data** for matching purposes — enrollment and comparison happen on the sensor itself. Off-chip backends store extracted minutiae templates locally (in `HackTouchIDStore`), not raw fingerprint images, but a template is still derived biometric data and should be treated as sensitive.
- **Pairing is exclusive per host, per sensor, on chips that support it.** Pairing a sensor to this Hackintosh can orphan (not delete) fingerprint enrollments made under another OS on the same physical sensor, since some of these chips only trust one paired host identity at a time. This is a property of the hardware, not a bug in this project, but it's worth knowing before pairing a sensor you also use elsewhere.
- **Diagnostic/debug builds may log more than production builds should.** Some backends have gone through development phases with verbose diagnostic logging (key material, session state, etc.) enabled for debugging real-hardware issues. If you're building from a development branch rather than a tagged release, be aware that debug output may be more verbose than intended for daily use, and shouldn't be shared publicly without review.

## Supported Versions

Before opening an issue please download the latest source code for this repo, since there isn't a release (for security reasons). This is a hobbyist project maintained in spare time.

## Reporting a Vulnerability

If you find a security issue (privilege escalation, sudoers misconfiguration, template/passphrase exposure, daemon impersonation, etc.),
do the following:

1. Open a [GitHub Security Advisory](../../security/advisories/new) on this repo (preferred method of reporting a security matter), or
2. Open a GitHub Issue on this repo

Please include:
- macOS version and hardware, and which sensor/backend is involved
- Steps to reproduce
- Impact you believe it has (e.g. local privilege escalation vs. denial of service)

I'll do my best to respond and patch promptly, but please keep in mind this is a solo maintained project — response times won't match a corporate bug bounty program.

## Recommendations for Users

- Only build and install from source (only this repo!! if anyone claims anything outside this repo it's most likely fake!!).
- Review the `sudoers` rule and `LaunchAgent` plist before installing; don't run install scripts blindly.
- Don't rely on this as your *only* layer of security on a machine with sensitive data — treat it as a convenience layer on top of FileVault + a strong login password, not a replacement for either.
- Keep your enrolled templates private — don't share your `fingers/` directory or `HackTouchIDStore` volume, as it's derived from **your biometric print**, which could be used against you.

## Disclaimer

This software interacts with macOS authentication internals on unsupported (Hackintosh) hardware. It is provided **as-is, without warranty of any kind** (see LICENSE). Use at your own risk, particularly on machines you use for sensitive work.
