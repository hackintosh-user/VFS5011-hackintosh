# VFS5011 Menu Bar

Optional companion app for [VFS5011-hackintosh](../). Sits in your menu bar
and gives you:

- A notification the moment the daemon wants a swipe ("Swipe to
  authenticate! 🫆") — useful since the sensor's LED can be too dim to
  notice on its own
- A follow-up notification for success or failure
- A quick toggle to pause/resume fingerprint auth without touching Terminal
- A "Restart Daemon" menu item *(not yet wired up — see Known Limitations)*

Completely optional. The daemon works exactly the same with or without this
app running.

## Installing

**Option A — download the pre-built app (easiest):**

1. Grab `VFS5011MenuBar.app.zip` from the [Releases page](https://github.com/hackintosh-user/VFS5011-hackintosh/releases/)
2. Unzip it and drag `VFS5011 Menu Bar.app` to `/Applications`.
3. **First launch will be blocked by Gatekeeper** — this app is ad-hoc
   signed, not notarized with a paid Apple Developer account, so macOS
   flags it as being from an "unidentified developer." To open it anyway:
   - Right-click (or Control-click) the app → **Open** → click **Open**
     again in the dialog that appears.
   - If that option doesn't show up, run this once in Terminal instead:
     ```bash
     xattr -cr "/Applications/VFS5011 Menu Bar.app"
     ```
4. After that first launch, it opens normally like any other app.

**Option B — build it yourself:**

```bash
git clone <this repo>
cd vfs5011-menubar
chmod +x build_menubar_app.sh
./build_menubar_app.sh
open "build/VFS5011 Menu Bar.app"
```

Requires Xcode Command Line Tools (`xcode-select --install`) for `swiftc`
and `iconutil`. No other dependencies.

## Auto-start at login

```bash
cp com.vfs5011.hackintosh.menubar.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.vfs5011.hackintosh.menubar.plist
```

## Requires the patched daemon

This app talks to the main VFS5011 daemon over macOS distributed
notifications. **The daemon must include the patch in
[`daemon-patch/`](daemon-patch/)** — without it, the app runs fine
standalone but never receives any events (harmless no-op, just silent).

See [`daemon-patch/README.md`](daemon-patch/README.md) for the exact
integration diff.

## Known limitations

- **"Restart Daemon" is currently a no-op.** The daemon isn't packaged as
  a real `LaunchDaemon` yet (it's a `sudo` re-exec test harness), so
  there's no `launchctl kickstart` target to hook this up to. Clicking it
  is safe, it just doesn't do anything yet.
- **Not notarized.** Ad-hoc signed only (`codesign --sign -`). See the
  Gatekeeper workaround above.
- **Pause is global, not per-surface.** Disabling fingerprint auth pauses
  it for every recognized prompt at once (lock screen, padlock, Finder,
  pkg installer, Time Machine, Apple ID's local password step) — there's
  no way to pause just one of them.

## License

Same as the main VFS5011-hackintosh project (BSD 3-Clause).
