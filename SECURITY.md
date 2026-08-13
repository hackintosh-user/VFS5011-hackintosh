# Security Policy

## Overview

VFS5011-hackintosh is a fingerprint authentication daemon that intercepts macOS system authentication prompts (login screen, System Settings/Preferences padlock, and — as of v1.0.2 — Keychain autofill prompts) and satisfies them using a fingerprint swipe captured from a Validity VFS5011 USB sensor, matched against locally stored templates.

This project interacts with sensitive parts of macOS (authentication, sudoers, LaunchAgents, Keychain) and is built and maintained by a hobbyist as an open-source community project. It is **not audited by Apple or any third-party security firm**, and it should be evaluated accordingly before you trust it with anything you care about.

## Threat Model / What This Project Does and Does Not Protect Against

**Intended use case:** convenience authentication on a personal Hackintosh, replacing password entry with a fingerprint swipe, on a machine you physically control.

**This project does NOT claim to:**
- Match the security guarantees of Apple's Secure Enclave-backed Touch ID (there is no secure enclave on Hackintosh hardware; matching happens in userspace).
- Protect against a sophisticated attacker with physical access and time (e.g. someone who can dump the encrypted volume and brute-force the passphrase, or replace the daemon binary).
- Defend against fingerprint spoofing (fake/molded fingerprints). The VFS5011 is a swipe sensor with no liveness detection.

**Known-sensitive components:**
- **Fingerprint templates** are stored in an encrypted APFS volume (`VFSStore`), with the passphrase held in the System keychain. If your System keychain or FileVault is compromised, this volume's protection is only as strong as that.
- **The daemon runs with elevated privileges** and includes a narrowly-scoped `sudoers` `NOPASSWD` rule to allow it to interact with authentication prompts without repeatedly asking for a password itself. It is scoped to `vfs5011_daemon` only — review the rule before installing. Any `NOPASSWD` rule increases attack surface if the daemon binary itself is compromised, so keep the daemon's file permissions locked down (root-owned, not writable by your user).
- **The auth-prompt watcher** (`is_system_auth_process()`) is allow-listed to specific system processes (login window, System Settings/Preferences padlock, and Keychain-related prompts). It is deliberately *not* wired up to arbitrary third-party app password fields or terminal `sudo` prompts.

## Supported Versions

Before opening an issue please download the latest source code for this repo, since there isn't a release (for security reasons). This is a hobbyist project maintained in spare time

## Reporting a Vulnerability

If you find a security issue (privilege escalation, sudoers misconfiguration, template/passphrase exposure, daemon impersonation, etc.),
Do the following:

1. Open a [GitHub Security Advisory](../../security/advisories/new) on this repo (preferred method of reporting a security matter), or
2. Open a Github Issue on this Repo 

Please include:
- macOS version and hardware
- Steps to reproduce
- Impact you believe it has (e.g. local privilege escalation vs. denial of service)

I'll do my best to respond and patch promptly, but please keep in mind this is a solo maintained project — response times won't match a corporate bug bounty program.

## Recommendations for Users

- Only build and install from source (Only this repo!! if anyone claims anything outside this repo its most likely fake!!).
- Review the `sudoers` rule and `LaunchAgent` plist before installing; don't run install scripts blindly.
- Don't rely on this as your *only* layer of security on a machine with sensitive data — treat it as a convenience layer on top of FileVault + a strong login password, not a replacement for either.
- Keep your enrolled templates private — don't share your `fingers/` directory or `VFSStore` volume as it literally has **Your biometric print! Which could be used against You in any forum of method**

## Disclaimer

This software interacts with macOS authentication internals on unsupported (Hackintosh) hardware. It is provided **as-is, without warranty of any kind** (see LICENSE). Use at your own risk, particularly on machines you use for sensitive work.
