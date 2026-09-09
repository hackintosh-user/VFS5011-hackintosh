# Daemon-side menu bar IPC

This folder holds the small piece of code that lets `vfs5011_daemon.c`
talk to the [Hackintosh Touch-ID menu bar app](../) — two files:

- [`hack-touchid-menubar-ipc.h`](hack-touchid-menubar-ipc.h)
- [`hack-touchid-menubar-ipc.c`](hack-touchid-menubar-ipc.c)

**This is already integrated into `vfs5011_daemon.c`** in this repo — if
you're building from a normal checkout, there's nothing to apply, this
just documents what's there and why. It's only relevant if you're
working from an older fork/copy of `vfs5011_daemon.c` that predates
this integration, in which case the "Applying it to a fork" section
below shows what to add.

## What it does

Uses `CFNotificationCenterGetDistributedCenter()` — the same
distributed-notification mechanism `vfs5011_daemon.c` already uses for
`com.apple.screenIsLocked`/`screenIsUnlocked` — to cross the
root-daemon / user-session boundary. That pairing was already proven
to work in this codebase, so this reuses it instead of introducing an
unverified IPC path (e.g. the lower-level Darwin `notify` API).

Distributed notifications don't reliably carry `userInfo` across that
root/user boundary, so every event and request is its own distinct
notification name rather than one name plus a payload dictionary. The
full set of names is defined in `hack-touchid-menubar-ipc.h` under
`VFS5011_NOTIFY_*`, and must stay byte-for-byte in sync with
`VFS5011Notification` in `AppDelegate.swift` — the string values are
the actual wire protocol between the two processes.

| Direction              | Notification            | Meaning                                   |
|-------------------------|-------------------------|--------------------------------------------|
| daemon → menu bar       | `swipe_requested`        | entered `STATE_POLLING`, waiting on a swipe |
| daemon → menu bar       | `swipe_success`           | a match was confirmed and typed             |
| daemon → menu bar       | `swipe_failed`             | a captured swipe scored below `MATCH_THRESHOLD`, or a prompt disappeared mid-poll |
| daemon → menu bar       | `scanning_enabled` / `scanning_disabled` | confirms the daemon's actual pause state, sent after any state change and on request |
| menu bar → daemon       | `request_enable` / `request_disable`     | user toggled the menu bar's pause switch |
| menu bar → daemon       | `request_restart`         | user clicked "Restart Daemon" — currently a no-op, see Known limitations |
| menu bar → daemon       | `request_state_announce`   | menu bar app just launched and wants the current pause state |

Pause state is persisted to a flag file
(`VFS5011_SCANNING_DISABLED_FLAG_PATH`, `/Library/Application
Support/VFS5011/scanning_disabled`) rather than kept only in memory —
its presence/absence is the actual source of truth, restored on every
`vfs5011_menubar_ipc_init()` call, so a daemon restart doesn't
silently forget a pause the user asked for.

## Integration points in `vfs5011_daemon.c`

For reference, here's everywhere the integration touches the daemon:

- `#include "hack-touchid-menubar-ipc.h"` near the top, alongside the
  other local includes
- `vfs5011_menubar_ipc_init()` called once from `main()`, after the
  `screenIsLocked`/`screenIsUnlocked` observers are registered and
  before `CFRunLoopRun()`
- `arm_polling_for_trigger()` checks `vfs5011_scanning_is_enabled()`
  as its very first line — if paused, it logs and returns before
  touching the sensor or posting anything
- `vfs5011_notify_swipe_requested()` right after `arm_polling_for_trigger()`
  successfully enters `STATE_POLLING`
- `vfs5011_notify_swipe_success()` after a match is confirmed **and**
  typed
- `vfs5011_notify_swipe_failed()` at each of its real failure points —
  a captured swipe scoring below `MATCH_THRESHOLD`, and the "we
  already said swipe, but the auth prompt went away first" walk-back
  case

## Applying it to a fork

If you're working from a copy of `vfs5011_daemon.c` that doesn't have
the integration points above:

1. Drop `hack-touchid-menubar-ipc.c` and `hack-touchid-menubar-ipc.h`
   next to `vfs5011_daemon.c` (or update their `#include` path to
   wherever you put them)
2. Add the six touch points listed above, in the same order
3. Add `hack-touchid-menubar-ipc.c` to whatever builds `vfs5011_daemon.c`

Without this, the menu bar app still runs fine standalone — it just
never receives any events. Harmless, just silent.

## Known limitations

- **`request_restart` is currently a no-op on the daemon side.** This
  daemon is launched via a `sudo` re-exec (see `main()`'s `geteuid()`
  check), not installed as a real `LaunchDaemon`, so there's no
  `launchctl kickstart` target yet to hook "Restart Daemon" up to.
  Clicking it in the menu bar app is safe, it just doesn't do anything
  yet — see `handle_restart_request()`'s own comment in
  `hack-touchid-menubar-ipc.c`.
- **Pause is global, not per-surface.** Disabling fingerprint auth
  pauses it for every recognized prompt at once (lock screen, padlock,
  Finder, pkg installer, Time Machine, Apple ID's local password step)
  — there's no way to pause just one of them.
