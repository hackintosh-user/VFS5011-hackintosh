# VFS5011 Menu Bar — Integration Notes

Optional companion app: shows a notification when the daemon wants a
swipe, and whether it succeeded or failed — useful since the sensor LED
can be too dim to notice. Also gives you a menu to restart the daemon or
pause/resume fingerprint scanning without touching Terminal.

Ships as a separate, optional download alongside the main VFS5011 client —
the daemon works exactly as it does today with zero code changes if this
app is never installed. All the IPC is one-directional-safe: if nothing
is listening for `notify_post()`, it's a no-op.

## What's in this folder

```
VFS5011MenuBar/
  AppDelegate.swift        - the whole app (NSStatusItem + notifications)
  Info.plist                - LSUIElement (no Dock icon), bundle id, etc.
daemon-patch/
  vfs5011_menubar_ipc.h     - shared notification names + flag file path
  vfs5011_menubar_ipc.c     - listener, flag persistence, event posts
com.vfs5011.hackintosh.menubar.plist   - LaunchAgent to auto-start the app
README_MENUBAR_INTEGRATION.md          - this file
```

## 1. Wiring into the existing daemon (`vfs5011_daemon.c`)

1. Copy `vfs5011_menubar_ipc.h` and `vfs5011_menubar_ipc.c` next to
   `vfs5011_daemon.c` and add both to your build (Makefile / build script
   — whatever `prep_and_build.sh` already invokes for the daemon target).

2. In the daemon's startup path (wherever it sets up logging, opens the
   sensor, etc.), add:

   ```c
   #include "vfs5011_menubar_ipc.h"
   ...
   vfs5011_menubar_ipc_init();
   ```

3. In `is_system_auth_process()`'s caller — the point where the daemon has
   already decided "yes, a recognized auth prompt is up, time to wake the
   sensor" — add the pause check **before** any sensor I/O:

   ```c
   if (!vfs5011_scanning_is_enabled()) {
       /* Paused via menu bar. Don't wake the sensor, don't post
        * swipe_requested. Keep polling for auth prompts as normal so a
        * resume takes effect on the very next detection, not just after
        * a fresh prompt appears later. */
       return;
   }

   vfs5011_notify_swipe_requested();
   /* ... existing sensor wake + capture + match flow ... */
   ```

4. Wherever the existing match result is already known (your Bozorth3
   threshold check, `MATCH_THRESHOLD=20`), add exactly one of:

   ```c
   if (match_succeeded) {
       vfs5011_notify_swipe_success();
   } else {
       vfs5011_notify_swipe_failed();
   }
   ```

That's the entire daemon-side change — no changes to `fingers/` handling,
enrollment, or the NVRAM OpenCore-version gate.

## 2. Building the menu bar app

Straightforward Xcode setup, no exotic entitlements needed since this app
only uses `NSStatusItem`, local `UNUserNotificationCenter` alerts, and
Darwin notifications (none of which require App Sandbox or special
capabilities):

1. New Xcode project → macOS → App → **Swift**, interface **AppKit** (not
   SwiftUI — keeps `NSStatusItem` handling simple and matches the code
   here), unchecked "Create Document-Based Application."
2. Replace the generated `AppDelegate.swift` and `Info.plist` with the
   ones in this folder. Delete the generated storyboard/`Main.storyboard`
   reference from Info.plist if Xcode added one — this app has no windows.
3. Set the bundle identifier to match `Info.plist`
   (`com.vfs5011.hackintosh.menubar`) — or pick your own, just keep it
   consistent, it doesn't need to match the daemon's bundle id.
4. Code sign with your existing Developer ID (same one you already use
   for the daemon/client) so Gatekeeper and notification permissions
   behave normally on first launch.
5. Build, then drag `VFS5011 Menu Bar.app` into `/Applications`.

## 3. Auto-start at login

Copy `com.vfs5011.hackintosh.menubar.plist` to
`~/Library/LaunchAgents/`, fix the `ProgramArguments` path if you renamed
anything, then:

```bash
launchctl load ~/Library/LaunchAgents/com.vfs5011.hackintosh.menubar.plist
```

This is a **per-user LaunchAgent**, not a LaunchDaemon — it needs a login
session to show UI, unlike the fingerprint daemon itself which stays a
root LaunchDaemon exactly as it is today.

## 4. Testing checklist

- [ ] Launch the app manually first (`open "/Applications/VFS5011 Menu
      Bar.app"`) and grant the notification permission prompt.
- [ ] Trigger any known-working auth surface (lock screen, a padlock,
      Finder authenticate) → confirm "Swipe to authenticate! 🫆" appears.
- [ ] Swipe correctly → confirm "Authentication successful! 🫆".
- [ ] Swipe incorrectly (or let it timeout, however your existing verify
      flow handles a bad match) → confirm "Authentication failed, try
      swiping better 🫆".
- [ ] Click "Disable Fingerprint Authentication" → trigger an auth prompt
      → confirm the sensor does **not** light up and no notification
      fires. Menu label should now read "Enable Fingerprint
      Authentication."
- [ ] Restart the daemon (`launchctl kickstart -k
      system/<daemon-label>`) while paused → confirm it comes back up
      still paused (flag file survived) and the menu bar app still shows
      "Enable..." after the app's own state-announce round trip.
- [ ] Click "Enable Fingerprint Authentication" → confirm scanning
      resumes immediately, no daemon restart required.
- [ ] Click "Restart Daemon" from the menu → confirm daemon restarts
      cleanly (check `syslog` for the two log lines from
      `vfs5011_menubar_ipc_init` and any pre-restart cleanup you add to
      `handle_restart_request()`).
- [ ] Quit the menu bar app entirely → confirm the daemon keeps running
      and auth prompts still work normally (just without notifications) —
      this proves the app is genuinely optional, not a hard dependency.

## Design notes / things intentionally left as-is

- **No userInfo payloads anywhere.** Darwin distributed notifications
  don't reliably carry them across the daemon/app process boundary, so
  every event and request is its own notification name instead. Don't
  try to consolidate these into fewer names with a payload later without
  re-verifying that assumption.
- **Global pause, not per-surface.** One flag disables scanning for every
  auth surface at once (lock screen, padlock, Finder, pkg, Time Machine,
  Apple ID's local-password step). Matches your stated preference and is
  much simpler to reason about than per-surface toggles.
- **UI reflects daemon-confirmed state, not optimistic state.** The menu
  bar app never flips its own label until the daemon posts
  `scanning_enabled`/`scanning_disabled` back. If a request is ever
  silently dropped, the UI stays honest instead of showing a state that
  didn't actually take effect.
