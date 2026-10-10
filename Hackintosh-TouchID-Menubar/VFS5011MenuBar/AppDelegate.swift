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
// WidgetKit's ControlCenter reload API only exists in the macOS 26 SDK
// (Swift 6.2 toolchain). Older toolchains skip it and still build.
#if canImport(WidgetKit) && compiler(>=6.2)
import WidgetKit
#endif

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

// MARK: - Client update check constants
//
// The client (hack-touchid) is updated by its own updater. This app only
// notices that a newer build exists and offers to run
// `hack-touchid --menu-updater` in a terminal. The branch to compare
// against is read from the client's own VERSION.txt, the same rule the
// client uses, so someone on active-development is only told about newer
// active-development builds.
private let clientUpdateActionID = "com.vfs5011.hackintosh.update_client"
private let clientUpdateCategoryID = "com.vfs5011.hackintosh.client_update"
private let clientSymlinkPath = "/usr/local/bin/hack-touchid"
private let clientRawVersionBaseURL = "https://raw.githubusercontent.com/hackintosh-user/VFS5011-hackintosh"
private let clientUpdateCheckInterval: TimeInterval = 60 * 60
private let clientUpdateFirstCheckDelay: TimeInterval = 15
// UserDefaults key: the last "version|build" we already notified about,
// so one published build produces one notification, not one per hour.
private let clientUpdateNotifiedKey = "lastNotifiedClientUpdate"

private struct ClientVersionInfo {
    var version = ""
    var build = ""
    var branch = ""
    var critical = false

    // Parses VERSION.txt's KEY=value lines. Returns nil without VERSION=.
    init?(text: String) {
        for rawLine in text.split(whereSeparator: \.isNewline) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("VERSION=") { version = String(line.dropFirst(8)) }
            else if line.hasPrefix("BUILD=") { build = String(line.dropFirst(6)) }
            else if line.hasPrefix("BRANCH=") { branch = String(line.dropFirst(7)) }
            else if line.hasPrefix("CRITICAL=") { critical = line.dropFirst(9).hasPrefix("true") }
        }
        if version.isEmpty { return nil }
    }

    // Same comparison as parse_version_code() in hack_touchid_client.c.
    var versionCode: Int? {
        let trimmed = version.hasPrefix("v") || version.hasPrefix("V") ? String(version.dropFirst()) : version
        let parts = trimmed.split(separator: ".").map { Int($0) }
        guard let major = parts.first ?? nil else { return nil }
        let minor = parts.count > 1 ? (parts[1] ?? 0) : 0
        let patch = parts.count > 2 ? (parts[2] ?? 0) : 0
        return major * 10000 + minor * 100 + patch
    }

    // Same comparison as parse_build_code(): "26B254" = year, letter, number.
    var buildCode: Int? {
        let chars = Array(build)
        guard chars.count >= 4,
              let y1 = chars[0].wholeNumberValue, let y2 = chars[1].wholeNumberValue,
              chars[2].isASCII, chars[2].isUppercase,
              let number = Int(String(chars[3...])),
              let letter = chars[2].asciiValue else { return nil }
        return (y1 * 10 + y2) * 1_000_000 + Int(letter - 65) * 10_000 + number
    }

    func isNewer(than local: ClientVersionInfo) -> Bool {
        guard let remoteCode = versionCode, let localCode = local.versionCode else { return false }
        if remoteCode != localCode { return remoteCode > localCode }
        guard let remoteBuild = buildCode, let localBuild = local.buildCode else { return false }
        return remoteBuild > localBuild
    }
}

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
    static let swipeLockout     = "\(prefix).swipe_lockout"
    static let swipeWeak        = "\(prefix).swipe_weak"

    // Daemon -> menu bar app (state confirmation, so the UI is correct
    // even if the app launches after the daemon, or the daemon restarts)
    static let scanningEnabled  = "\(prefix).scanning_enabled"
    static let scanningDisabled = "\(prefix).scanning_disabled"

    // Swipe-to-lock (macOS Tahoe only). Daemon confirms its state with
    // these two, same pattern as scanning.
    static let lockswipeEnabled  = "\(prefix).lockswipe_enabled"
    static let lockswipeDisabled = "\(prefix).lockswipe_disabled"

    // Menu bar app -> daemon (requests; daemon confirms back via the
    // two state notifications above rather than the app assuming the
    // toggle succeeded)
    static let requestEnable    = "\(prefix).request_enable"
    static let requestDisable   = "\(prefix).request_disable"
    static let requestRestart   = "\(prefix).request_restart"
    static let requestState     = "\(prefix).request_state_announce"
    static let requestLockswipeEnable  = "\(prefix).request_lockswipe_enable"
    static let requestLockswipeDisable = "\(prefix).request_lockswipe_disable"
}

// MARK: - macOS version helpers

enum MacOSInfo {
    private static func sysctlString(_ name: String) -> String? {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var buf = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { return nil }
        return String(cString: buf)
    }

    /// Darwin 25 == macOS 26 Tahoe. Uses the kernel's own report, so a
    /// spoofed product version does not fool the feature gate.
    static var isTahoeOrNewer: Bool {
        guard let rel = sysctlString("kern.osrelease"),
              let major = Int(rel.split(separator: ".").first ?? "") else { return false }
        return major >= 25
    }

    /// e.g. "Reported macOS Version: 26.7.1 Tahoe (Darwin: 25.6.0)".
    /// "Reported" because both values come from the kernel and can be
    /// spoofed or compat-mode.
    static var reportedVersionLine: String {
        let product = sysctlString("kern.osproductversion")
        let darwin = sysctlString("kern.osrelease")
        let major = Int((product ?? "").split(separator: ".").first ?? "") ?? 0
        let names = [26: "Tahoe", 15: "Sequoia", 14: "Sonoma", 13: "Ventura", 12: "Monterey", 11: "Big Sur"]
        var parts = product ?? "unknown"
        if let name = names[major] { parts += " \(name)" }
        if let darwin = darwin { parts += " (Darwin: \(darwin))" }
        return "Reported macOS Version: \(parts)"
    }
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
    private var clientUpdateTimer: Timer?
    private var aboutWindow: NSWindow?
    private var scanningEnabled: Bool = true {
        didSet { updateMenuForCurrentState() }
    }
    // Swipe-to-lock: mirrors what the daemon confirmed, never a guess.
    private var lockSwipeEnabled: Bool = false {
        didSet {
            updateMenuForCurrentState()
            reloadControlCenterControl()
        }
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
        scheduleClientUpdateChecks()
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(handleWake),
            name: NSWorkspace.didWakeNotification,
            object: nil
        )
    }

    @objc private func handleWake() {
        checkDaemonHealth()
        checkForClientUpdate()
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

        // Swipe-to-lock is a macOS Tahoe feature, so the item only
        // exists there.
        if MacOSInfo.isTahoeOrNewer {
            let lockItem = NSMenuItem(
                title: lockSwipeEnabled ? "Disable Swipe to Lock" : "Enable Swipe to Lock",
                action: #selector(toggleLockSwipeTapped),
                keyEquivalent: ""
            )
            lockItem.target = self
            lockItem.tag = 101
            menu.addItem(lockItem)
        }

        menu.addItem(NSMenuItem.separator())

        let versionItem = NSMenuItem(title: MacOSInfo.reportedVersionLine, action: nil, keyEquivalent: "")
        versionItem.isEnabled = false
        versionItem.tag = 102
        menu.addItem(versionItem)

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

        if let lockItem = menu.item(withTag: 101) {
            lockItem.title = lockSwipeEnabled ? "Disable Swipe to Lock" : "Enable Swipe to Lock"
            lockItem.state = lockSwipeEnabled ? .on : .off
        }

        // Dim the icon while paused so the state is visible without
        // opening the menu.
        statusItem.button?.appearsDisabled = !scanningEnabled
    }

    // Tells Control Center to re-read the swipe-to-lock control's state
    // (the control itself lives in the optional Tahoe extension, see
    // ControlCenterExtension/README.md). No-op without the macOS 26 SDK.
    private func reloadControlCenterControl() {
        #if canImport(WidgetKit) && compiler(>=6.2)
        if #available(macOS 26.0, *) {
            ControlCenter.shared.reloadControls(ofKind: "com.vfs5011.hackintosh.lockswipe")
        }
        #endif
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

    @objc private func toggleLockSwipeTapped() {
        if lockSwipeEnabled {
            postDistributedNotification(VFS5011Notification.requestLockswipeDisable)
        } else {
            postDistributedNotification(VFS5011Notification.requestLockswipeEnable)
        }
        // Same rule as scanning: the menu follows the daemon's confirmation.
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
        let updateAction = UNNotificationAction(
            identifier: clientUpdateActionID,
            title: "Update Client",
            options: [.foreground]
        )
        let updateCategory = UNNotificationCategory(
            identifier: clientUpdateCategoryID,
            actions: [updateAction],
            intentIdentifiers: [],
            options: []
        )
        UNUserNotificationCenter.current().setNotificationCategories([category, updateCategory])
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

    // Removes this app's swipe notifications from Notification Center so
    // they don't pile up. The daemon-missing alert (it has a category and
    // an action button) is left alone.
    private func clearSwipeNotifications(after delay: TimeInterval, then completion: (() -> Void)? = nil) {
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            let center = UNUserNotificationCenter.current()
            center.getDeliveredNotifications { delivered in
                let ids = delivered
                    .filter { $0.request.content.categoryIdentifier != daemonMissingCategoryID
                           && $0.request.content.categoryIdentifier != clientUpdateCategoryID }
                    .map { $0.request.identifier }
                if !ids.isEmpty {
                    center.removeDeliveredNotifications(withIdentifiers: ids)
                }
                if let completion = completion {
                    DispatchQueue.main.async { completion() }
                }
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

    // MARK: - Client update check

    // First check shortly after launch (so it does not compete with the
    // daemon health check and the login-time rush), then hourly. Wake from
    // sleep also checks, see handleWake().
    private func scheduleClientUpdateChecks() {
        DispatchQueue.main.asyncAfter(deadline: .now() + clientUpdateFirstCheckDelay) { [weak self] in
            self?.checkForClientUpdate()
        }
        clientUpdateTimer = Timer.scheduledTimer(withTimeInterval: clientUpdateCheckInterval, repeats: true) { [weak self] _ in
            self?.checkForClientUpdate()
        }
    }

    // The client's VERSION.txt sits next to the real binary, which
    // /usr/local/bin/hack-touchid points at. Missing or unreadable just
    // means "can't check", never an error: the client may not have been
    // run yet.
    private func readLocalClientVersion() -> ClientVersionInfo? {
        let link = URL(fileURLWithPath: clientSymlinkPath).resolvingSymlinksInPath()
        let versionFile = link.deletingLastPathComponent().appendingPathComponent("VERSION.txt")
        guard let text = try? String(contentsOf: versionFile, encoding: .utf8) else { return nil }
        return ClientVersionInfo(text: text)
    }

    private func checkForClientUpdate() {
        guard let local = readLocalClientVersion(), !local.branch.isEmpty,
              let url = URL(string: "\(clientRawVersionBaseURL)/\(local.branch)/VERSION.txt") else { return }

        var request = URLRequest(url: url)
        request.timeoutInterval = 10
        request.cachePolicy = .reloadIgnoringLocalCacheData
        URLSession.shared.dataTask(with: request) { [weak self] data, response, _ in
            // Offline, GitHub down, bad branch: stay quiet, try again next time.
            guard let self = self,
                  let http = response as? HTTPURLResponse, http.statusCode == 200,
                  let data = data, let text = String(data: data, encoding: .utf8),
                  let remote = ClientVersionInfo(text: text),
                  remote.isNewer(than: local) else { return }

            let marker = "\(remote.version)|\(remote.build)"
            DispatchQueue.main.async {
                if UserDefaults.standard.string(forKey: clientUpdateNotifiedKey) == marker { return }
                UserDefaults.standard.set(marker, forKey: clientUpdateNotifiedKey)
                self.fireClientUpdateNotification(local: local, remote: remote)
            }
        }.resume()
    }

    private func fireClientUpdateNotification(local: ClientVersionInfo, remote: ClientVersionInfo) {
        let content = UNMutableNotificationContent()
        content.title = remote.critical ? "Hackintosh Touch-ID: critical update" : "Hackintosh Touch-ID update"
        content.body = "Client v\(remote.version) (\(remote.build)) is available. You are on v\(local.version) (\(local.build))."
        content.sound = .default
        content.categoryIdentifier = clientUpdateCategoryID

        let request = UNNotificationRequest(
            identifier: "client-update-\(UUID().uuidString)",
            content: content,
            trigger: nil // deliver immediately
        )
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                NSLog("VFS5011MenuBar: failed to post client-update notification: \(error.localizedDescription)")
            }
        }
    }

    // Opens a terminal running `hack-touchid --menu-updater`. A small
    // .command file is opened with the system default handler, so it
    // lands in whatever terminal app the person has set as default
    // (Terminal.app unless they changed it). The client asks for sudo
    // itself, so the password prompt appears in that window.
    private func launchClientUpdaterInTerminal() {
        let script = """
        #!/bin/zsh
        export PATH="/usr/local/bin:$PATH"
        hack-touchid --menu-updater
        echo
        echo "Press Return to close this window."
        read
        """
        let fileURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("HTID-Update-Client.command")
        do {
            try script.write(to: fileURL, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: fileURL.path)
            NSWorkspace.shared.open(fileURL)
        } catch {
            NSLog("VFS5011MenuBar: failed to launch the client updater: \(error.localizedDescription)")
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
        if response.notification.request.content.categoryIdentifier == clientUpdateCategoryID,
           response.actionIdentifier == clientUpdateActionID {
            launchClientUpdaterInTerminal()
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
            VFS5011Notification.swipeLockout,
            VFS5011Notification.swipeWeak,
            VFS5011Notification.scanningEnabled,
            VFS5011Notification.scanningDisabled,
            VFS5011Notification.lockswipeEnabled,
            VFS5011Notification.lockswipeDisabled,
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
        clearSwipeNotifications(after: 1.0) // leftovers from before this launch

        // After a cold boot this app can launch before the daemon is up,
        // so the first request goes unanswered. Ask again a few times;
        // the daemon's answer is idempotent.
        for delay in [2.0, 5.0, 10.0, 20.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                self?.postDistributedNotification(VFS5011Notification.requestState)
            }
        }
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
                // Drop leftovers first, and only then post the new prompt,
                // so the clean-up can never remove the prompt itself.
                self.clearSwipeNotifications(after: 0) { [weak self] in
                    self?.fireLocalNotification(
                        title: "Hackintosh Touch-ID",
                        body: "Swipe to authenticate! 🔻"
                    )
                }

            case VFS5011Notification.swipeSuccess:
                self.fireLocalNotification(
                    title: "Hackintosh Touch-ID",
                    body: "Authentication successful! ✅"
                )
                self.clearSwipeNotifications(after: 10.0)

            case VFS5011Notification.swipeFailed:
                self.fireLocalNotification(
                    title: "Hackintosh Touch-ID",
                    body: "Authentication failed, try swiping better ❌"
                )
                self.clearSwipeNotifications(after: 10.0)

            case VFS5011Notification.swipeWeak:
                self.clearSwipeNotifications(after: 0) { [weak self] in
                    self?.fireLocalNotification(
                        title: "Hackintosh Touch-ID",
                        body: "Swipe was too weak, please try again 🔻"
                    )
                    self?.clearSwipeNotifications(after: 10.0)
                }

            case VFS5011Notification.swipeLockout:
                // Same ordering as the prompt: clear the leftover "try
                // swiping better" banner first, then post the final one.
                self.clearSwipeNotifications(after: 0) { [weak self] in
                    self?.fireLocalNotification(
                        title: "Hackintosh Touch-ID",
                        body: "Authentication failed, use your password ❌"
                    )
                    self?.clearSwipeNotifications(after: 10.0)
                }

            case VFS5011Notification.scanningEnabled:
                self.scanningEnabled = true

            case VFS5011Notification.scanningDisabled:
                self.scanningEnabled = false

            case VFS5011Notification.lockswipeEnabled:
                self.lockSwipeEnabled = true

            case VFS5011Notification.lockswipeDisabled:
                self.lockSwipeEnabled = false

            default:
                break
            }
        }
    }
}
