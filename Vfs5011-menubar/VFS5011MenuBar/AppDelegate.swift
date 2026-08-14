//
//  AppDelegate.swift
//  VFS5011 Menu Bar
//
//  Menu bar companion app for the VFS5011 fingerprint daemon.
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

// MARK: - Notification names (must match vfs5011_menubar_ipc.h exactly)
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
class AppDelegate: NSObject, NSApplicationDelegate {

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
        requestNotificationPermission()
        registerForDaemonNotifications()
        registerAsLoginItemIfNeeded()
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
                accessibilityDescription: "VFS5011 Fingerprint Daemon"
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
            title: "About VFS5011",
            action: #selector(aboutTapped),
            keyEquivalent: ""
        )
        aboutItem.target = self
        menu.addItem(aboutItem)

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(
            title: "Quit VFS5011 Menu Bar",
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
        window.title = "About VFS5011"
        window.isReleasedWhenClosed = false

        let content = NSView(frame: NSRect(x: 0, y: 0, width: 360, height: 340))

        let icon = NSImageView(frame: NSRect(x: 140, y: 240, width: 80, height: 80))
        icon.image = NSImage(named: "MenuBarIcon") ?? NSImage(systemSymbolName: "touchid", accessibilityDescription: nil)
        icon.imageScaling = .scaleProportionallyUpOrDown
        content.addSubview(icon)

        let title = NSTextField(labelWithString: "VFS5011")
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
            "Brings Validity VFS5011 fingerprint sensors (common on older " +
            "HP laptops) to Hackintosh macOS as a real authentication " +
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
                    title: "VFS5011",
                    body: "Swipe to authenticate! 🫆"
                )

            case VFS5011Notification.swipeSuccess:
                self.fireLocalNotification(
                    title: "VFS5011",
                    body: "Authentication successful! 🫆"
                )

            case VFS5011Notification.swipeFailed:
                self.fireLocalNotification(
                    title: "VFS5011",
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
