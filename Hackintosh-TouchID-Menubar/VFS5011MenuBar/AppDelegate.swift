//
//  AppDelegate.swift
//  Hackintosh Touch-ID Menu Bar
//
//  Menu bar companion app for the Hackintosh Touch-ID daemon (multi-
//  sensor as of v1.1 -- was VFS5011-only when this app was first
//  built, hence the internal symbol/IPC-prefix names below still
//  saying "VFS5011"; renamed the user-facing branding only, see the
//  comment at the enum below for why the wire-protocol strings and
//  Swift symbol name were deliberately left alone).
//  Listens for distributed notifications posted by the daemon and
//  surfaces them as user notifications + a status item, since the
//  sensor's LED is sometimes too dim to notice on its own.
//
//  IPC DESIGN:
//  Uses CFNotificationCenterGetDistributedCenter() -- the same
//  mechanism vfs5011_daemon.c already uses for
//  com.apple.screenIsLocked/Unlocked. That pairing is proven in the
//  daemon's own code to cross the root-daemon / user-session boundary
//  correctly, so this reuses it instead of introducing an unverified
//  IPC path. IMPORTANT: distributed notifications don't reliably carry
//  userInfo payloads across that root/user boundary -- every event and
//  request below is its own distinct notification name rather than one
//  generic event with a payload dictionary.
//

import Cocoa
import UserNotifications
import ServiceManagement
import Darwin

// MARK: - Daemon health-check constants
//
// Must match hack-touchid-agent-install.sh's own LABEL exactly
// ("com.hackintosh.vfs5011agent", per-user domain as of the v2
// plist-location change -- see that script's header). Not derived
// from anywhere else at runtime since this app doesn't share a
// header with the shell installer; if that LABEL ever changes,
// this constant needs updating too.
private let daemonLaunchdLabel = "com.hackintosh.vfs5011agent"
private let daemonReinstallActionID = "com.vfs5011.hackintosh.reinstall_daemon"
private let daemonMissingCategoryID = "com.vfs5011.hackintosh.daemon_missing"

// MARK: - Notification names (must match hack-touchid-menubar-ipc.h exactly)
// NOTE: kept as "VFS5011Notification"/"com.vfs5011.hackintosh" even
// after the user-facing rename to "Hackintosh Touch-ID" -- this is a
// wire-protocol identifier shared with hack-touchid-menubar-ipc.c/.h (two
// copies: repo root + Hackintosh-TouchID-Menubar/daemon-patch/). Renaming it
// would mean updating three files in lockstep for zero user-visible
// benefit (nobody sees this string), so it was left alone. Only
// user-facing text (window titles, menu items, notification banners)
// was renamed.
enum VFS5011Notification {
    static let prefix = "com.vfs5011.hackintosh"

    // Daemon -> menu bar app (events)
    static let swipeRequested   = "\(prefix).swipe_requested"
    static let swipeSuccess     = "\(prefix).swipe_success"
    static let swipeFailed      = "\(prefix).swipe_failed"

    // Daemon -> menu bar app (state confirmation, so the UI is correct
    // even if the app launches after the daemon, or the daemon restarts)
    static let scanningEnabled  = "\(prefix).scanning_enabled"
    static let scanningDisabled = "\(prefix).scanning_disabled"

    // Menu bar app -> daemon (requests; daemon confirms back via the
    // two state notifications above rather than the app assuming the
    // toggle succeeded)
    static let requestEnable    = "\(prefix).request_enable"
    static let requestDisable   = "\(prefix).request_disable"
    static let requestRestart   = "\(prefix).request_restart"
    static let requestState     = "\(prefix).request_state_announce"
}

@main
class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {

    // Explicit entry point. NOT relying on any implicit @main wiring --
    // that magic is normally supplied by Xcode's app target build
    // settings and does not reliably kick in for a plain `swiftc`
    // command-line build. This is exactly what a classic main.swift
    // would do by hand.
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }

    private var statusItem: NSStatusItem!
    private var aboutWindow: NSWindow?
    private var scanningEnabled: Bool = true {
        didSet { updateMenuForCurrentState() }
    }

    // MARK: - Lifecycle

    func applicationDidFinishLaunching(_ notification: Notification) {
        // This is a menu bar-only utility -- no Dock icon, no main window.
        NSApp.setActivationPolicy(.accessory)

        setupStatusItem()
        registerNotificationCategories()
        requestNotificationPermission()
        registerForDaemonNotifications()
        registerAsLoginItemIfNeeded()

        // Known trigger for a missing/broken daemon (per project notes):
        // a macOS point update wipes it, similar to OCLP root patches
        // getting wiped on update -- the fix is just re-running deploy.
        // Checked on every launch (covers "logged back in after an
        // update installed at restart") and again after wake from
        // sleep (covers "update installed overnight, machine slept
        // through it"), since neither is guaranteed to relaunch this
        // app on its own.
        checkDaemonHealth()
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(handleWake),
            name: NSWorkspace.didWakeNotification,
            object: nil
        )
    }

    @objc private func handleWake() {
        checkDaemonHealth()
    }

    // MARK: - Login item

    // Registers this app to launch automatically at login, using the
    // modern SMAppService API (macOS 13+ -- matches our
    // LSMinimumSystemVersion). Checked every launch against the real
    // SMAppService status (not a UserDefaults flag) -- a flag would
    // survive even after the person removes the login item in System
    // Settings, wrongly skipping re-registration forever after.
    private func registerAsLoginItemIfNeeded() {
        guard SMAppService.mainApp.status != .enabled else { return }

        do {
            try SMAppService.mainApp.register()
            NSLog("VFS5011MenuBar: registered as a login item")
        } catch {
            NSLog("VFS5011MenuBar: failed to register as a login item: \(error.localizedDescription)")
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        unregisterForDaemonNotifications()
    }

    // MARK: - Status item / menu

    private func setupStatusItem() {
        // variableLength (not squareLength) lets the item size itself to
        // whatever content actually renders -- squareLength combined
        // with a nil image is exactly how a status item goes invisible.
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            if let customIcon = NSImage(named: "MenuBarIcon") {
                // Template mode: macOS recolors this automatically for
                // light/dark menu bars and Control Center, same as any
                // system status item. The PNG itself is pure black on
                // transparent -- isTemplate is what makes that adaptive.
                customIcon.isTemplate = true
                button.image = customIcon
            } else if let symbolImage = NSImage(
                systemSymbolName: "touchid",
                accessibilityDescription: "Hackintosh Touch-ID Daemon"
            ) {
                // Fallback if the bundled icon didn't load for any
                // reason (e.g. Resources weren't copied into the .app).
                button.image = symbolImage
            } else {
                // Last-resort fallback -- guarantees the item is never
                // an invisible, zero-width sliver in the menu bar.
                button.title = "🫆"
            }
        }
        statusItem.menu = buildMenu()
        statusItem.isVisible = true
    }

    private func buildMenu() -> NSMenu {
        let menu = NSMenu()

        let toggleItem = NSMenuItem(
            title: scanningEnabled ? "Disable Fingerprint Authentication" : "Enable Fingerprint Authentication",
            action: #selector(toggleScanningTapped),
            keyEquivalent: ""
        )
        toggleItem.target = self
        toggleItem.tag = 100 // used to find + relabel this item later
        menu.addItem(toggleItem)

        menu.addItem(NSMenuItem.separator())

        let aboutItem = NSMenuItem(
            title: "About Hackintosh Touch-ID",
            action: #selector(aboutTapped),
            keyEquivalent: ""
        )
        aboutItem.target = self
        menu.addItem(aboutItem)

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(
            title: "Quit Hackintosh Touch-ID",
            action: #selector(quitTapped),
            keyEquivalent: "q"
        )
        quitItem.target = self
        menu.addItem(quitItem)

        return menu
    }

    private func updateMenuForCurrentState() {
        guard let menu = statusItem.menu,
              let toggleItem = menu.item(withTag: 100) else { return }

        toggleItem.title = scanningEnabled
            ? "Disable Fingerprint Authentication"
            : "Enable Fingerprint Authentication"

        // Dim the icon while paused so the state is visible without
        // opening the menu.
        statusItem.button?.appearsDisabled = !scanningEnabled
    }

    // MARK: - Menu actions

    @objc private func aboutTapped() {
        if aboutWindow == nil {
            aboutWindow = makeAboutWindow()
        }
        NSApp.activate(ignoringOtherApps: true)
        aboutWindow?.center()
        aboutWindow?.makeKeyAndOrderFront(nil)
    }

    private func makeAboutWindow() -> NSWindow {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 360, height: 340),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = "About Hackintosh Touch-ID"
        window.isReleasedWhenClosed = false

        let content = NSView(frame: NSRect(x: 0, y: 0, width: 360, height: 340))

        let icon = NSImageView(frame: NSRect(x: 140, y: 240, width: 80, height: 80))
        icon.image = NSImage(named: "MenuBarIcon") ?? NSImage(systemSymbolName: "touchid", accessibilityDescription: nil)
        icon.imageScaling = .scaleProportionallyUpOrDown
        content.addSubview(icon)

        let title = NSTextField(labelWithString: "Hackintosh Touch-ID")
        title.font = NSFont.boldSystemFont(ofSize: 18)
        title.alignment = .center
        title.frame = NSRect(x: 0, y: 205, width: 360, height: 24)
        content.addSubview(title)

        let subtitle = NSTextField(labelWithString: "Fingerprint Authentication for Hackintosh")
        subtitle.font = NSFont.systemFont(ofSize: 12)
        subtitle.textColor = .secondaryLabelColor
        subtitle.alignment = .center
        subtitle.frame = NSRect(x: 0, y: 185, width: 360, height: 18)
        content.addSubview(subtitle)

        let body = NSTextField(wrappingLabelWithString:
            "Brings supported fingerprint sensors (Validity VFS5011, with more " +
            "in progress) to Hackintosh macOS as a real authentication " +
            "method -- lock screen, System Settings, Finder, installers, " +
            "and more. This menu bar app surfaces swipe prompts and " +
            "results as notifications, since the sensor's LED can be " +
            "hard to notice on its own.")
        body.font = NSFont.systemFont(ofSize: 12)
        body.alignment = .center
        body.frame = NSRect(x: 24, y: 70, width: 312, height: 110)
        content.addSubview(body)

        let link = NSTextField(labelWithString: "github.com/hackintosh-user/VFS5011-hackintosh")
        link.font = NSFont.systemFont(ofSize: 11)
        link.textColor = .linkColor
        link.alignment = .center
        link.frame = NSRect(x: 0, y: 35, width: 360, height: 18)
        let linkClick = NSClickGestureRecognizer(target: self, action: #selector(openProjectPage))
        link.addGestureRecognizer(linkClick)
        content.addSubview(link)

        window.contentView = content
        return window
    }

    @objc private func openProjectPage() {
        if let url = URL(string: "https://github.com/hackintosh-user/VFS5011-hackintosh") {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func toggleScanningTapped() {
        if scanningEnabled {
            postDistributedNotification(VFS5011Notification.requestDisable)
        } else {
            postDistributedNotification(VFS5011Notification.requestEnable)
        }
        // Deliberately NOT flipping `scanningEnabled` here. The menu
        // should reflect what the daemon confirms, not what we hope
        // happened -- avoids the UI lying if a request is ever dropped.
    }

    @objc private func quitTapped() {
        NSApp.terminate(nil)
    }

    // MARK: - Notification permission

    // Registers the "Reinstall Daemon" actionable notification category.
    // Must happen before requestNotificationPermission()/any notification
    // using this category is posted, so it's called first in
    // applicationDidFinishLaunching().
    private func registerNotificationCategories() {
        let reinstallAction = UNNotificationAction(
            identifier: daemonReinstallActionID,
            title: "Reinstall Daemon",
            options: [.foreground]
        )
        let category = UNNotificationCategory(
            identifier: daemonMissingCategoryID,
            actions: [reinstallAction],
            intentIdentifiers: [],
            options: []
        )
        UNUserNotificationCenter.current().setNotificationCategories([category])
        UNUserNotificationCenter.current().delegate = self
    }

    private func requestNotificationPermission() {
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert, .sound]) { granted, error in
            if let error = error {
                NSLog("VFS5011MenuBar: notification permission error: \(error.localizedDescription)")
            } else if !granted {
                NSLog("VFS5011MenuBar: notification permission denied by user")
            }
        }
    }

    private func fireLocalNotification(title: String, body: String) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default

        let request = UNNotificationRequest(
            identifier: UUID().uuidString,
            content: content,
            trigger: nil // deliver immediately
        )
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                NSLog("VFS5011MenuBar: failed to post notification: \(error.localizedDescription)")
            }
        }
    }

    // MARK: - Daemon health check

    // Checked with `launchctl print gui/<uid>/<label>` rather than just
    // testing for the plist file's existence -- the plist can be present
    // but the service still not bootstrapped (or crash-looping), and
    // `launchctl print` is the same command the install script itself
    // already uses to confirm success (see hack-touchid-agent-install.sh),
    // so this mirrors what "installed and running" actually means there.
    // A nonzero exit (launchctl's own convention for "no such service")
    // is treated as "missing" -- good enough to trigger the notification;
    // this deliberately doesn't try to distinguish "never installed" from
    // "wiped by an update" from "crashed", since the fix is the same
    // --deploy-agent re-run either way.
    private func checkDaemonHealth() {
        let uid = getuid()
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = ["print", "gui/\(uid)/\(daemonLaunchdLabel)"]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice

        do {
            try process.run()
            process.waitUntilExit()
            if process.terminationStatus != 0 {
                fireDaemonMissingNotification()
            }
        } catch {
            // Couldn't even launch launchctl -- don't nag the user over
            // something on our end; log and move on rather than firing
            // a possibly-false alarm.
            NSLog("VFS5011MenuBar: daemon health check failed to run launchctl: \(error.localizedDescription)")
        }
    }

    private func fireDaemonMissingNotification() {
        let content = UNMutableNotificationContent()
        content.title = "Hackintosh Touch-ID"
        content.body = "The daemon isn't installed or running. Tap Reinstall Daemon to fix it."
        content.sound = .default
        content.categoryIdentifier = daemonMissingCategoryID

        let request = UNNotificationRequest(
            identifier: "daemon-missing-\(UUID().uuidString)",
            content: content,
            trigger: nil // deliver immediately
        )
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                NSLog("VFS5011MenuBar: failed to post daemon-missing notification: \(error.localizedDescription)")
            }
        }
    }

    // Launches the user's default Terminal app running the actual
    // deploy command, via osascript rather than Process directly --
    // `sudo hack-touchid --deploy-agent` needs an interactive password
    // prompt and a visible window, which only a real Terminal.app
    // session (not a background Process) can give it. Relies on
    // ensure_path_symlink() (hack_touchid_client.c) having already put
    // `hack-touchid` on PATH via /usr/local/bin.
    private func launchDeployAgentInTerminal() {
        let script = """
        tell application "Terminal"
            activate
            do script "sudo hack-touchid --deploy-agent"
        end tell
        """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script]
        do {
            try process.run()
        } catch {
            NSLog("VFS5011MenuBar: failed to launch Terminal for --deploy-agent: \(error.localizedDescription)")
        }
    }

    // MARK: - UNUserNotificationCenterDelegate

    // Handles the "Reinstall Daemon" action button tap. Also shows
    // notifications while the app is frontmost (.banner/.sound) --
    // without this, UNUserNotificationCenter silently drops
    // notifications whose posting app is currently active, which would
    // otherwise make the daemon-missing alert invisible if this app
    // happens to be in the foreground (e.g. right after launch).
    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        completionHandler([.banner, .sound])
    }

    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        if response.notification.request.content.categoryIdentifier == daemonMissingCategoryID,
           response.actionIdentifier == daemonReinstallActionID {
            launchDeployAgentInTerminal()
        }
        completionHandler()
    }

    // MARK: - Distributed notifications (daemon IPC)
    //
    // Uses CFNotificationCenterGetDistributedCenter(), NOT the
    // lower-level Darwin notify center -- see the file header comment.

    private func registerForDaemonNotifications() {
        let center = CFNotificationCenterGetDistributedCenter()
        let observer = Unmanaged.passUnretained(self).toOpaque()

        let names = [
            VFS5011Notification.swipeRequested,
            VFS5011Notification.swipeSuccess,
            VFS5011Notification.swipeFailed,
            VFS5011Notification.scanningEnabled,
            VFS5011Notification.scanningDisabled,
        ]

        for name in names {
            CFNotificationCenterAddObserver(
                center,
                observer,
                { (_, observerPtr, name, _, _) in
                    guard let observerPtr = observerPtr, let name = name else { return }
                    let mySelf = Unmanaged<AppDelegate>.fromOpaque(observerPtr).takeUnretainedValue()
                    mySelf.handleDaemonNotification(name.rawValue as String)
                },
                name as CFString,
                nil,
                .deliverImmediately
            )
        }

        // On launch, ask the daemon to (re-)announce its current
        // scanning state, since this app may have launched after the
        // daemon and otherwise wouldn't know if scanning is paused.
        postDistributedNotification(VFS5011Notification.requestState)
    }

    private func unregisterForDaemonNotifications() {
        let center = CFNotificationCenterGetDistributedCenter()
        let observer = Unmanaged.passUnretained(self).toOpaque()
        CFNotificationCenterRemoveEveryObserver(center, observer)
    }

    private func postDistributedNotification(_ name: String) {
        let center = CFNotificationCenterGetDistributedCenter()
        CFNotificationCenterPostNotification(
            center,
            CFNotificationName(name as CFString),
            nil,
            nil,
            true
        )
    }

    private func handleDaemonNotification(_ name: String) {
        // Distributed notifications can arrive on a background thread.
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }

            switch name {
            case VFS5011Notification.swipeRequested:
                self.fireLocalNotification(
                    title: "Hackintosh Touch-ID",
                    body: "Swipe to authenticate! 🫆"
                )

            case VFS5011Notification.swipeSuccess:
                self.fireLocalNotification(
                    title: "Hackintosh Touch-ID",
                    body: "Authentication successful! 🫆"
                )

            case VFS5011Notification.swipeFailed:
                self.fireLocalNotification(
                    title: "Hackintosh Touch-ID",
                    body: "Authentication failed, try swiping better 🫆"
                )

            case VFS5011Notification.scanningEnabled:
                self.scanningEnabled = true

            case VFS5011Notification.scanningDisabled:
                self.scanningEnabled = false

            default:
                break
            }
        }
    }
}
