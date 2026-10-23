// HackTouchIDPane.swift
//
// System Settings pane for Hackintosh Touch-ID ("HackTouchID" in the sidebar).
// Version 0.1: General tab + About/GitHub tab.
//
// Design rules:
//  * The pane runs as the logged-in user and never touches the sensor, the
//    encrypted volume or any root-owned file for writing. Everything that
//    changes state is a request to the daemon over the same distributed
//    notifications the menu bar app already uses (see
//    hack-touchid-menubar-ipc.h; names must stay byte-for-byte in sync).
//  * Everything it shows is read from world-readable files, the client's
//    VERSION.txt and `launchctl`, so it still works if the daemon is down.

import Cocoa
import PreferencePanes

// MARK: - Shared constants (keep in sync with hack-touchid-menubar-ipc.h)

private enum Notify {
    static let prefix = "com.vfs5011.hackintosh"
    static let requestEnable = "\(prefix).request_enable"
    static let requestDisable = "\(prefix).request_disable"
    static let requestState = "\(prefix).request_state_announce"
    static let requestLockSwipeEnable = "\(prefix).request_lockswipe_enable"
    static let requestLockSwipeDisable = "\(prefix).request_lockswipe_disable"
    static let thresholdPrefix = "\(prefix).request_threshold_"
    static let scanningEnabled = "\(prefix).scanning_enabled"
    static let scanningDisabled = "\(prefix).scanning_disabled"
    static let lockSwipeEnabled = "\(prefix).lockswipe_enabled"
    static let lockSwipeDisabled = "\(prefix).lockswipe_disabled"
    static let thresholdChanged = "\(prefix).threshold_changed"
}

private enum Paths {
    static let scanningDisabledFlag = "/Library/Application Support/VFS5011/scanning_disabled"
    static let lockSwipeFlag = "/Library/Application Support/VFS5011/lockswipe_enabled"
    static let thresholdConf = "/usr/local/libexec/hack-touchid/match_threshold.conf"
    static let clientLink = "/usr/local/bin/hack-touchid"
}

private enum Links {
    static let website = "https://hackintosh-user.github.io/VFS5011-hackintosh/"
    static let repo = "https://github.com/hackintosh-user/VFS5011-hackintosh"
    static let releases = "https://github.com/hackintosh-user/VFS5011-hackintosh/releases"
    static let issues = "https://github.com/hackintosh-user/VFS5011-hackintosh/issues"
    static let rawBase = "https://raw.githubusercontent.com/hackintosh-user/VFS5011-hackintosh"
    static let releasesAPI = "https://api.github.com/repos/hackintosh-user/VFS5011-hackintosh/releases?per_page=30"
}

private let thresholdChoices = [20, 25, 30, 35, 40]
private let thresholdDefault = 20

// MARK: - Small helpers

private func readSmallFile(_ path: String) -> String? {
    guard let data = FileManager.default.contents(atPath: path) else { return nil }
    return String(data: data, encoding: .utf8)
}

private func fileExists(_ path: String) -> Bool {
    FileManager.default.fileExists(atPath: path)
}

private func parseKeyValues(_ text: String) -> [String: String] {
    var out: [String: String] = [:]
    for line in text.split(separator: "\n") {
        let parts = line.split(separator: "=", maxSplits: 1).map(String.init)
        if parts.count == 2 { out[parts[0].trimmingCharacters(in: .whitespaces)] = parts[1].trimmingCharacters(in: .whitespaces) }
    }
    return out
}

/// Runs a tool and returns (exit status, stdout). Never throws; a launch
/// failure is reported as status -1.
private func run(_ tool: String, _ args: [String]) -> (Int32, String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: tool)
    p.arguments = args
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = Pipe()
    do { try p.run() } catch { return (-1, "") }
    p.waitUntilExit()
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    return (p.terminationStatus, String(data: data, encoding: .utf8) ?? "")
}

private func postRequest(_ name: String) {
    DistributedNotificationCenter.default().postNotificationName(
        NSNotification.Name(name), object: nil, userInfo: nil, deliverImmediately: true)
}

// MARK: - Running the client as root (admin password dialog)

private struct PrivResult { var out: String; var err: String }

private func shellQuote(_ s: String) -> String {
    "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
}

/// Runs `hack-touchid <args>` as root through the standard macOS administrator dialog
/// (AppleScript "do shell script ... with administrator privileges"). The command always
/// ends with `true`, so its output comes back even when it fails; the client's own JSON says
/// whether it worked. Blocks until the dialog is answered, so call it off the main thread.
private func runPrivileged(_ args: [String]) -> PrivResult {
    let cmd = ([Paths.clientLink] + args).map(shellQuote).joined(separator: " ") + " 2>&1; true"
    let escaped = cmd.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
    let script = "do shell script \"\(escaped)\" with administrator privileges"
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
    p.arguments = ["-e", script]
    let outPipe = Pipe()
    let errPipe = Pipe()
    p.standardOutput = outPipe
    p.standardError = errPipe
    do { try p.run() } catch { return PrivResult(out: "", err: "launch failed") }
    let out = outPipe.fileHandleForReading.readDataToEndOfFile()
    let err = errPipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return PrivResult(out: String(data: out, encoding: .utf8) ?? "", err: String(data: err, encoding: .utf8) ?? "")
}

/// The client prints its result as JSON between HTID-JSON-BEGIN and HTID-JSON-END; anything
/// else around it (volume scripts, sensor messages) is ignored.
private func parseClientJSON(_ raw: String) -> [String: Any]? {
    // AppleScript's "do shell script" turns every newline in its output into a carriage return.
    let text = raw.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
    guard let b = text.range(of: "HTID-JSON-BEGIN\n"),
          let e = text.range(of: "\nHTID-JSON-END", range: b.upperBound..<text.endIndex) else { return nil }
    let json = String(text[b.upperBound..<e.lowerBound])
    guard let data = json.data(using: .utf8),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
    return obj
}

// MARK: - Update checking (same rules as the client and the menu bar app)

private struct VersionInfo {
    var version = ""
    var build = ""
    var branch = ""
    var critical = false

    /// Parses the client's VERSION.txt (KEY=value lines). Nil without VERSION=.
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

    // Same as parse_version_code() in hack_touchid_client.c.
    var versionCode: Int? {
        let trimmed = version.hasPrefix("v") || version.hasPrefix("V") ? String(version.dropFirst()) : version
        let parts = trimmed.split(separator: ".").map { Int($0) }
        guard let major = parts.first ?? nil else { return nil }
        let minor = parts.count > 1 ? (parts[1] ?? 0) : 0
        let patch = parts.count > 2 ? (parts[2] ?? 0) : 0
        return major * 10000 + minor * 100 + patch
    }

    // Same as parse_build_code(): "26B254" = year, letter, number.
    var buildCode: Int? {
        let chars = Array(build)
        guard chars.count >= 4,
              let y1 = chars[0].wholeNumberValue, let y2 = chars[1].wholeNumberValue,
              chars[2].isASCII, chars[2].isUppercase,
              let number = Int(String(chars[3...])),
              let letter = chars[2].asciiValue else { return nil }
        return (y1 * 10 + y2) * 1_000_000 + Int(letter - 65) * 10_000 + number
    }

    func isNewer(than local: VersionInfo) -> Bool {
        guard let remoteCode = versionCode, let localCode = local.versionCode else { return false }
        if remoteCode != localCode { return remoteCode > localCode }
        guard let remoteBuild = buildCode, let localBuild = local.buildCode else { return false }
        return remoteBuild > localBuild
    }

    var display: String {
        "v\(version)" + (build.isEmpty ? "" : " (\(build))") + (branch.isEmpty ? "" : ", \(branch)")
    }
}

private func semver(_ s: String) -> [Int]? {
    let t = s.hasPrefix("v") || s.hasPrefix("V") ? String(s.dropFirst()) : s
    let parts = t.split(separator: ".").prefix(3).map { Int($0.prefix(while: { $0.isNumber })) }
    guard !parts.isEmpty, parts.allSatisfy({ $0 != nil }) else { return nil }
    var out = parts.compactMap { $0 }
    while out.count < 3 { out.append(0) }
    return out
}

/// Downloads through /usr/bin/curl: it behaves the same inside System Settings as in Terminal,
/// without depending on what the pane's host process is allowed to do on the network.
private func fetchText(_ url: String, github: Bool = false) -> String? {
    var args = ["-sfL", "--max-time", "10", "-A", "HackTouchID-Pane"]
    if github { args += ["-H", "Accept: application/vnd.github+json"] }
    args.append(url)
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/curl")
    p.arguments = args
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = Pipe()
    do { try p.run() } catch { return nil }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return p.terminationStatus == 0 ? String(data: data, encoding: .utf8) : nil
}

private final class UpdateResults {
    var installedClient: VersionInfo?
    var remoteClient: VersionInfo?
    var installedMenubar: String?
    var latestMenubarTag: String?
    var latestMenubarURL: String?
    var clientChecked = false
    var menubarChecked = false
}

private struct FingerRow {
    var id: String
    var name: String
    var named: Bool
    var readonly: Bool = false
}

private func label(_ text: String, bold: Bool = false, secondary: Bool = false) -> NSTextField {
    let f = NSTextField(labelWithString: text)
    f.font = bold ? .boldSystemFont(ofSize: NSFont.systemFontSize) : .systemFont(ofSize: NSFont.systemFontSize)
    if secondary { f.textColor = .secondaryLabelColor }
    f.lineBreakMode = .byWordWrapping
    f.maximumNumberOfLines = 0
    return f
}

private func macOSMajor() -> Int { ProcessInfo.processInfo.operatingSystemVersion.majorVersion }

// MARK: - The pane

@objc(HackTouchIDPane)
class HackTouchIDPane: NSPreferencePane {

    // General tab controls
    private let authSwitch = NSSwitch()
    private let authStatus = label("", secondary: true)
    private let daemonValue = label("")
    private let clientValue = label("")
    private let menubarValue = label("")
    private let thresholdPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let thresholdHint = label("", secondary: true)
    private let lockSwipeSwitch = NSSwitch()
    private let lockSwipeHint = label("", secondary: true)
    private var tahoeBox: NSView?
    private var generalPage: NSView?
    private var aboutPage: NSView?
    private var fingersPage: NSView?
    private var updatesPage: NSView?

    // Updates tab state
    private var updatesChecked = false
    private var updatesBusy = false
    private var menubarReleaseURL: String?
    private let updatesStatus = label("Not checked yet.", secondary: true)
    private let updatesCheckButton = NSButton(title: "Check for Updates", target: nil, action: nil)
    private let updatesBar = NSProgressIndicator()
    private var updatesTimer: Timer?
    private var pendingResults: UpdateResults?
    private var barTarget = 0.0
    private let clientInstalledValue = label("")
    private let clientLatestValue = label("")
    private let clientState = label("", secondary: true)
    private let clientUpdateButton = NSButton(title: "Update Client", target: nil, action: nil)
    private let menubarInstalledValue = label("")
    private let menubarLatestValue = label("")
    private let menubarState = label("", secondary: true)
    private let menubarDownloadButton = NSButton(title: "Download", target: nil, action: nil)

    // Fingerprints tab state
    private var fingers: [FingerRow] = []
    private var fingerCapacity = 0
    private var canDelete = true
    private var fingersLoaded = false
    private var fingersBusy = false
    private let fingersStatus = label("Click Load to read your fingerprints.", secondary: true)
    private let fingersLoadButton = NSButton(title: "Load", target: nil, action: nil)
    private let fingersSpinner = NSProgressIndicator()
    private let fingerListHolder = NSStackView()

    // Sensor tab state (Metallica MIS / Prometheus: connect and calibrate)
    private var sensorPage: NSView?
    private var sensorBusy = false
    private var sensorDetails = ""
    private let sensorStatus = label("Not run yet. Connect checks that this Mac can talk to the sensor.", secondary: true)
    private let sensorSpinner = NSProgressIndicator()
    private let sensorProduct = NSTextField()
    private let sensorSerial = NSTextField()
    private let sensorConnectButton = NSButton(title: "Connect", target: nil, action: nil)
    private let sensorCalibrateButton = NSButton(title: "Calibrate", target: nil, action: nil)
    private let sensorDetailsButton = NSButton(title: "Details", target: nil, action: nil)

    private var refreshTimer: Timer?
    private var observers: [NSObjectProtocol] = []

    // MARK: Lifecycle

    override func loadMainView() -> NSView {
        let root = NSView(frame: NSRect(x: 0, y: 0, width: 668, height: 640))
        root.autoresizingMask = [.width, .height]

        let page = NSStackView()
        page.orientation = .vertical
        page.alignment = .leading
        page.spacing = 16
        page.edgeInsets = NSEdgeInsets(top: 20, left: 24, bottom: 20, right: 24)
        page.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(page)
        NSLayoutConstraint.activate([
            page.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            page.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            page.topAnchor.constraint(equalTo: root.topAnchor),
        ])

        page.addArrangedSubview(buildHeader())

        let seg = NSSegmentedControl(labels: ["General", "Fingerprints", "Sensor", "Updates", "About"], trackingMode: .selectOne,
                                     target: self, action: #selector(pageChanged(_:)))
        seg.selectedSegment = 0
        page.addArrangedSubview(seg)
        seg.centerXAnchor.constraint(equalTo: page.centerXAnchor).isActive = true

        let general = buildGeneralPage()
        let fingersView = buildFingersPage()
        fingersView.isHidden = true
        let sensorView = buildSensorPage()
        sensorView.isHidden = true
        let updatesView = buildUpdatesPage()
        updatesView.isHidden = true
        let about = buildAboutPage()
        about.isHidden = true
        for v in [general, fingersView, sensorView, updatesView, about] {
            page.addArrangedSubview(v)
            fillWidth(v, of: page)
        }
        generalPage = general
        fingersPage = fingersView
        sensorPage = sensorView
        updatesPage = updatesView
        aboutPage = about

        self.mainView = root
        mainViewDidLoad()
        return root
    }

    @objc private func pageChanged(_ sender: NSSegmentedControl) {
        let pages = [generalPage, fingersPage, sensorPage, updatesPage, aboutPage]
        for (i, p) in pages.enumerated() { p?.isHidden = i != sender.selectedSegment }
        if sender.selectedSegment == 3 && !updatesChecked { checkForUpdates() }
    }

    override func mainViewDidLoad() {
        refreshAll()
    }

    override func willSelect() {
        // Ask the daemon to re-announce its state so the switches are right
        // even if the menu bar app is not running.
        let center = DistributedNotificationCenter.default()
        let names = [Notify.scanningEnabled, Notify.scanningDisabled,
                     Notify.lockSwipeEnabled, Notify.lockSwipeDisabled, Notify.thresholdChanged]
        for n in names {
            let token = center.addObserver(forName: NSNotification.Name(n), object: nil, queue: .main) { [weak self] _ in
                self?.refreshAll()
            }
            observers.append(token)
        }
        postRequest(Notify.requestState)
        refreshAll()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.refreshAll() }
    }

    override func didUnselect() {
        if updatesTimer != nil {
            // Left the pane mid-check: drop the unfinished check so the tab is usable next time.
            updatesTimer?.invalidate()
            updatesTimer = nil
            updatesBar.isHidden = true
            updatesBusy = false
            updatesChecked = false
            updatesCheckButton.isEnabled = true
        }
        refreshTimer?.invalidate()
        refreshTimer = nil
        for t in observers { DistributedNotificationCenter.default().removeObserver(t) }
        observers.removeAll()
    }

    // MARK: Layout helpers (grouped rows, like native System Settings)

    private func fillWidth(_ v: NSView, of stack: NSStackView) {
        let inset = stack.edgeInsets.left + stack.edgeInsets.right
        v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -inset).isActive = true
    }

    /// Title (and optional small subtitle) on the left, control on the right.
    private func settingRow(_ title: String, subtitle: NSTextField? = nil, control: NSView) -> NSView {
        let text = NSStackView()
        text.orientation = .vertical
        text.alignment = .leading
        text.spacing = 2
        text.addArrangedSubview(label(title))
        if let s = subtitle {
            s.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            s.preferredMaxLayoutWidth = 380
            text.addArrangedSubview(s)
        }
        let spacer = NSView()
        spacer.setContentHuggingPriority(NSLayoutConstraint.Priority(1), for: .horizontal)
        let row = NSStackView(views: [text, spacer, control])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 12
        row.edgeInsets = NSEdgeInsets(top: 10, left: 14, bottom: 10, right: 14)
        return row
    }

    /// A rounded box of rows with hairline separators and an optional caption above it.
    private func group(_ title: String?, rows: [NSView]) -> NSStackView {
        let outer = NSStackView()
        outer.orientation = .vertical
        outer.alignment = .leading
        outer.spacing = 6
        if let t = title {
            let cap = label(t, secondary: true)
            cap.font = .boldSystemFont(ofSize: NSFont.smallSystemFontSize)
            outer.addArrangedSubview(cap)
        }

        let inner = NSStackView()
        inner.orientation = .vertical
        inner.alignment = .leading
        inner.spacing = 0
        for (i, r) in rows.enumerated() {
            if i > 0 {
                let sep = NSBox()
                sep.boxType = .separator
                inner.addArrangedSubview(sep)
                fillWidth(sep, of: inner)
            }
            inner.addArrangedSubview(r)
            fillWidth(r, of: inner)
        }

        let box = NSBox()
        box.boxType = .custom
        box.titlePosition = .noTitle
        box.cornerRadius = 10
        box.borderWidth = 1
        box.borderColor = NSColor.separatorColor
        box.fillColor = NSColor.labelColor.withAlphaComponent(0.05)
        box.contentViewMargins = .zero
        if let cv = box.contentView {
            inner.translatesAutoresizingMaskIntoConstraints = false
            cv.addSubview(inner)
            NSLayoutConstraint.activate([
                inner.leadingAnchor.constraint(equalTo: cv.leadingAnchor),
                inner.trailingAnchor.constraint(equalTo: cv.trailingAnchor),
                inner.topAnchor.constraint(equalTo: cv.topAnchor),
                inner.bottomAnchor.constraint(equalTo: cv.bottomAnchor),
            ])
        }
        outer.addArrangedSubview(box)
        fillWidth(box, of: outer)
        return outer
    }

    private func buildHeader() -> NSView {
        let icon = NSImageView()
        icon.image = NSImage(systemSymbolName: "touchid", accessibilityDescription: nil)
        icon.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 34, weight: .regular)
        icon.contentTintColor = .controlAccentColor
        icon.widthAnchor.constraint(equalToConstant: 48).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 48).isActive = true

        let title = label("Hackintosh Touch-ID")
        title.font = .boldSystemFont(ofSize: 17)
        let sub = label("Fingerprint authentication for your Hackintosh", secondary: true)
        let texts = NSStackView(views: [title, sub])
        texts.orientation = .vertical
        texts.alignment = .leading
        texts.spacing = 2

        let header = NSStackView(views: [icon, texts])
        header.orientation = .horizontal
        header.alignment = .centerY
        header.spacing = 12
        return header
    }

    // MARK: General page

    private func buildGeneralPage() -> NSView {
        let page = NSStackView()
        page.orientation = .vertical
        page.alignment = .leading
        page.spacing = 16

        authSwitch.target = self
        authSwitch.action = #selector(authSwitchChanged)
        let authGroup = group(nil, rows: [
            settingRow("Fingerprint authentication", subtitle: authStatus, control: authSwitch),
        ])

        for v in thresholdChoices { thresholdPopup.addItem(withTitle: "\(v)") }
        thresholdPopup.target = self
        thresholdPopup.action = #selector(thresholdChanged)
        for f in [daemonValue, clientValue, menubarValue] { f.alignment = .right }
        let statusGroup = group("Status", rows: [
            settingRow("Daemon", control: daemonValue),
            settingRow("Client", control: clientValue),
            settingRow("Menu bar app", control: menubarValue),
        ])
        let thresholdGroup = group(nil, rows: [
            settingRow("Match threshold", subtitle: thresholdHint, control: thresholdPopup),
        ])

        let openClient = NSButton(title: "Open Client", target: self, action: #selector(openClientTapped))
        let openNotif = NSButton(title: "Open Notification Settings", target: self, action: #selector(openNotificationSettings))
        let actionsGroup = group(nil, rows: [
            settingRow("Client", control: openClient),
            settingRow("Notifications", control: openNotif),
        ])

        lockSwipeSwitch.target = self
        lockSwipeSwitch.action = #selector(lockSwipeChanged)
        let tahoeGroup = group("macOS Tahoe", rows: [
            settingRow("Swipe to Lock", subtitle: lockSwipeHint, control: lockSwipeSwitch),
        ])
        tahoeGroup.isHidden = macOSMajor() < 26
        tahoeBox = tahoeGroup

        for g in [authGroup, statusGroup, thresholdGroup, actionsGroup, tahoeGroup] {
            page.addArrangedSubview(g)
            fillWidth(g, of: page)
        }
        return page
    }

    // MARK: Fingerprints page

    private func buildFingersPage() -> NSView {
        let page = NSStackView()
        page.orientation = .vertical
        page.alignment = .leading
        page.spacing = 16

        fingersSpinner.style = .spinning
        fingersSpinner.controlSize = .small
        fingersSpinner.isDisplayedWhenStopped = false
        fingersLoadButton.target = self
        fingersLoadButton.action = #selector(loadFingersTapped)
        let controls = NSStackView(views: [fingersSpinner, fingersLoadButton])
        controls.spacing = 8
        let header = group(nil, rows: [
            settingRow("Enrolled fingerprints", subtitle: fingersStatus, control: controls),
        ])

        fingerListHolder.orientation = .vertical
        fingerListHolder.alignment = .leading
        fingerListHolder.spacing = 0

        let note = label("Reading and changing fingerprints needs your administrator password, "
                         + "because they are stored in the encrypted volume or on the sensor.", secondary: true)
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        note.preferredMaxLayoutWidth = 560

        for v in [header, fingerListHolder, note] {
            page.addArrangedSubview(v)
        }
        fillWidth(header, of: page)
        fillWidth(fingerListHolder, of: page)
        return page
    }

    private func setFingersBusy(_ busy: Bool, message: String? = nil) {
        fingersBusy = busy
        if let m = message { fingersStatus.stringValue = m }
        fingersLoadButton.isEnabled = !busy
        if busy { fingersSpinner.startAnimation(nil) } else { fingersSpinner.stopAnimation(nil) }
    }

    /// Runs the client as root and hands back its JSON. `cancelled` is true when the person
    /// dismissed the password dialog.
    private func runClient(_ args: [String], done: @escaping ([String: Any]?, Bool) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let r = runPrivileged(args)
            let cancelled = r.err.contains("-128") || r.err.localizedCaseInsensitiveContains("canceled")
            let json = parseClientJSON(r.out)
            DispatchQueue.main.async { done(json, cancelled) }
        }
    }

    private func showMessage(_ title: String, _ info: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = info
        alert.alertStyle = .warning
        alert.addButton(withTitle: "OK")
        if let w = mainView.window { alert.beginSheetModal(for: w, completionHandler: nil) } else { alert.runModal() }
    }

    private func rebuildFingerList() {
        for v in fingerListHolder.arrangedSubviews {
            fingerListHolder.removeArrangedSubview(v)
            v.removeFromSuperview()
        }
        fingersLoadButton.title = "Refresh"
        if fingers.isEmpty {
            fingersStatus.stringValue = "No fingerprints enrolled yet. Use Enroll in the client to add one."
            return
        }
        var status = "\(fingers.count) of \(fingerCapacity) prints used."
        if !canDelete { status += " Removing a single print is not supported on this sensor yet." }
        fingersStatus.stringValue = status

        let rows: [NSView] = fingers.map { f in
            let sub: NSTextField? = f.readonly ? label("Created on another OS", secondary: true)
                                               : (f.named ? nil : label("Not named yet", secondary: true))
            var buttons: [NSView] = []
            if !f.readonly {
                let rename = NSButton(title: "Rename", target: self, action: #selector(renameFingerTapped(_:)))
                rename.identifier = NSUserInterfaceItemIdentifier(f.id)
                buttons.append(rename)
            }
            if canDelete {
                let remove = NSButton(title: "Remove", target: self, action: #selector(removeFingerTapped(_:)))
                remove.identifier = NSUserInterfaceItemIdentifier(f.id)
                buttons.append(remove)
            }
            let box = NSStackView(views: buttons)
            box.spacing = 8
            return settingRow(f.name, subtitle: sub, control: box)
        }
        let g = group(nil, rows: rows)
        fingerListHolder.addArrangedSubview(g)
        fillWidth(g, of: fingerListHolder)
    }

    @objc private func loadFingersTapped() {
        loadFingers([])
    }

    /// Reads the prints through the client. `extra` carries the host identity (and
    /// --save-host-identity) after the user typed it in.
    private func loadFingers(_ extra: [String]) {
        guard !fingersBusy else { return }
        setFingersBusy(true, message: "Waiting for your administrator password...")
        runClient(["--list-fingers"] + extra) { [weak self] json, cancelled in
            guard let self = self else { return }
            self.setFingersBusy(false)
            if cancelled { self.fingersStatus.stringValue = "Cancelled. Click Load to try again."; return }
            guard let json = json else {
                self.fingersStatus.stringValue = "Could not run the client. Is it installed? Try sudo hack-touchid in Terminal."
                return
            }
            if (json["ok"] as? Bool) != true {
                if (json["code"] as? String) == "need_host_identity" {
                    self.fingersStatus.stringValue = "This sensor needs this laptop's real product name and serial number."
                    self.askHostIdentity(refused: (json["refused"] as? Bool) ?? false)
                    return
                }
                self.fingersStatus.stringValue = (json["error"] as? String) ?? "Reading the fingerprints failed."
                return
            }
            self.canDelete = (json["can_delete"] as? Bool) ?? false
            self.fingerCapacity = (json["capacity"] as? Int) ?? 0
            let list = (json["fingers"] as? [[String: Any]]) ?? []
            self.fingers = list.compactMap { d in
                guard let id = d["id"] as? String, let name = d["name"] as? String else { return nil }
                return FingerRow(id: id, name: name, named: (d["named"] as? Bool) ?? false,
                                 readonly: (d["readonly"] as? Bool) ?? false)
            }
            self.fingersLoaded = true
            self.rebuildFingerList()
        }
    }

    /// The sensor was paired on another operating system. macOS only sees the spoofed Mac values,
    /// so the real product name and serial have to be copied from Windows or Linux once.
    private func askHostIdentity(refused: Bool) {
        let alert = NSAlert()
        alert.messageText = refused ? "The sensor refused those values" : "This sensor needs this laptop's real identity"
        var info = ""
        if refused { info += "Check both values against the other operating system.\n\n" }
        info += "The sensor was paired on another operating system. macOS can't read the real product name and serial on a Hackintosh, so copy them once:\n\n"
        info += "Windows (PowerShell):\nGet-CimInstance Win32_ComputerSystemProduct | Select Name,IdentifyingNumber\n\n"
        info += "Linux:\ncat /sys/class/dmi/id/product_name /sys/class/dmi/id/product_serial\n\n"
        info += "Name is the product name, IdentifyingNumber is the serial."
        alert.informativeText = info
        alert.addButton(withTitle: "Save and Load")
        alert.addButton(withTitle: "Cancel")
        let product = NSTextField(frame: NSRect(x: 0, y: 30, width: 300, height: 24))
        product.placeholderString = "Product name"
        let serial = NSTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
        serial.placeholderString = "Serial number"
        let box = NSView(frame: NSRect(x: 0, y: 0, width: 300, height: 56))
        box.addSubview(product)
        box.addSubview(serial)
        alert.accessoryView = box
        alert.window.initialFirstResponder = product
        let finish: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard let self = self else { return }
            guard response == .alertFirstButtonReturn else {
                self.fingersStatus.stringValue = "Cancelled. Click Load to try again."
                return
            }
            let p = product.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            let s = serial.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            if p.isEmpty || s.isEmpty || p.count > 120 || s.count > 120 || p.contains("\n") || s.contains("\n") {
                self.fingersStatus.stringValue = "Both values are needed (up to 120 characters). Click Load to try again."
                return
            }
            self.loadFingers(["--host-product", p, "--host-serial", s, "--save-host-identity"])
        }
        if let w = mainView.window { alert.beginSheetModal(for: w, completionHandler: finish) } else { finish(alert.runModal()) }
    }

    @objc private func renameFingerTapped(_ sender: NSButton) {
        guard !fingersBusy, let id = sender.identifier?.rawValue,
              let f = fingers.first(where: { $0.id == id }) else { return }
        let alert = NSAlert()
        alert.messageText = "Rename fingerprint"
        alert.informativeText = "Enter a new name for \"\(f.name)\"."
        alert.addButton(withTitle: "Rename")
        alert.addButton(withTitle: "Cancel")
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        field.stringValue = f.named ? f.name : ""
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        let finish: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard let self = self, response == .alertFirstButtonReturn else { return }
            let newName = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            if newName.isEmpty { return }
            self.setFingersBusy(true, message: "Waiting for your administrator password...")
            self.runClient(["--rename-finger", "--finger-id", id, "--finger-name", newName]) { json, cancelled in
                self.setFingersBusy(false)
                if cancelled { self.rebuildFingerList(); return }
                guard let json = json, (json["ok"] as? Bool) == true else {
                    self.rebuildFingerList()
                    self.showMessage("Could not rename", (json?["error"] as? String) ?? "The client did not answer.")
                    return
                }
                // Update the list from the answer, so there is no second password prompt.
                if let idx = self.fingers.firstIndex(where: { $0.id == id }) {
                    self.fingers[idx].id = (json["id"] as? String) ?? id
                    self.fingers[idx].name = (json["name"] as? String) ?? newName
                    self.fingers[idx].named = true
                }
                self.rebuildFingerList()
            }
        }
        if let w = mainView.window { alert.beginSheetModal(for: w, completionHandler: finish) } else { finish(alert.runModal()) }
    }

    @objc private func removeFingerTapped(_ sender: NSButton) {
        guard !fingersBusy, let id = sender.identifier?.rawValue,
              let f = fingers.first(where: { $0.id == id }) else { return }
        let alert = NSAlert()
        alert.messageText = "Remove \"\(f.name)\"?"
        alert.informativeText = "This fingerprint can no longer be used to authenticate. It cannot be undone."
        alert.alertStyle = .warning
        let removeButton = alert.addButton(withTitle: "Remove")
        removeButton.hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel")
        let finish: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard let self = self, response == .alertFirstButtonReturn else { return }
            self.setFingersBusy(true, message: "Waiting for your administrator password...")
            self.runClient(["--delete-finger", "--finger-id", id]) { json, cancelled in
                self.setFingersBusy(false)
                if cancelled { self.rebuildFingerList(); return }
                guard let json = json, (json["ok"] as? Bool) == true else {
                    self.rebuildFingerList()
                    self.showMessage("Could not remove", (json?["error"] as? String) ?? "The client did not answer.")
                    return
                }
                self.fingers.removeAll { $0.id == id }
                self.rebuildFingerList()
            }
        }
        if let w = mainView.window { alert.beginSheetModal(for: w, completionHandler: finish) } else { finish(alert.runModal()) }
    }

    // MARK: Sensor page (Metallica MIS / Prometheus)

    private func buildSensorPage() -> NSView {
        let page = NSStackView()
        page.orientation = .vertical
        page.alignment = .leading
        page.spacing = 16

        for f in [sensorProduct, sensorSerial] {
            f.translatesAutoresizingMaskIntoConstraints = false
            f.widthAnchor.constraint(equalToConstant: 260).isActive = true
        }
        sensorProduct.placeholderString = "Product name"
        sensorSerial.placeholderString = "Serial number"
        let identity = group("Host identity", rows: [
            settingRow("Product name", control: sensorProduct),
            settingRow("Serial number", control: sensorSerial),
        ])

        let hint = label("Only needed when Windows or Linux paired the sensor first. macOS cannot read the real "
                         + "values on a Hackintosh, so copy them once and they are saved on this Mac.\n\n"
                         + "Windows (PowerShell): Get-CimInstance Win32_ComputerSystemProduct | Select Name,IdentifyingNumber\n"
                         + "Linux: cat /sys/class/dmi/id/product_name /sys/class/dmi/id/product_serial\n\n"
                         + "Leave both boxes empty to try the saved values (or the default) first.", secondary: true)
        hint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        hint.preferredMaxLayoutWidth = 560

        sensorSpinner.style = .spinning
        sensorSpinner.controlSize = .small
        sensorSpinner.isDisplayedWhenStopped = false
        sensorConnectButton.target = self
        sensorConnectButton.action = #selector(sensorConnectTapped)
        sensorCalibrateButton.target = self
        sensorCalibrateButton.action = #selector(sensorCalibrateTapped)
        sensorDetailsButton.target = self
        sensorDetailsButton.action = #selector(sensorDetailsTapped)
        sensorDetailsButton.isHidden = true
        let connectControls = NSStackView(views: [sensorSpinner, sensorDetailsButton, sensorConnectButton])
        connectControls.spacing = 8
        let actions = group("Sensor", rows: [
            settingRow("Connect", subtitle: sensorStatus, control: connectControls),
            settingRow("Calibration", subtitle: label("Rewrites the sensor's calibration data. Keep your finger off the sensor.", secondary: true),
                       control: sensorCalibrateButton),
        ])

        let note = label("These actions need your administrator password. The Details button shows what the "
                         + "sensor code logged during the last run, so you can copy it into a bug report.", secondary: true)
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        note.preferredMaxLayoutWidth = 560

        for v in [identity, hint, actions, note] { page.addArrangedSubview(v) }
        fillWidth(identity, of: page)
        fillWidth(actions, of: page)
        return page
    }

    private func setSensorBusy(_ busy: Bool, message: String? = nil) {
        sensorBusy = busy
        if let m = message { sensorStatus.stringValue = m }
        sensorConnectButton.isEnabled = !busy
        sensorCalibrateButton.isEnabled = !busy
        if busy { sensorSpinner.startAnimation(nil) } else { sensorSpinner.stopAnimation(nil) }
    }

    /// The two identity boxes as client arguments. nil means the values are not usable (a message is shown).
    private func sensorIdentityArgs() -> [String]? {
        let p = sensorProduct.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        let s = sensorSerial.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        if p.isEmpty && s.isEmpty { return [] }
        if p.isEmpty || s.isEmpty || p.count > 120 || s.count > 120 || p.contains("\n") || s.contains("\n") {
            sensorStatus.stringValue = "Fill in both boxes (up to 120 characters each), or leave both empty."
            return nil
        }
        return ["--host-product", p, "--host-serial", s, "--save-host-identity"]
    }

    @objc private func sensorConnectTapped() {
        guard !sensorBusy, let extra = sensorIdentityArgs() else { return }
        runSensorAction(["--mmis-connect"] + extra, what: "Connect")
    }

    @objc private func sensorCalibrateTapped() {
        guard !sensorBusy, let extra = sensorIdentityArgs() else { return }
        let alert = NSAlert()
        alert.messageText = "Calibrate the sensor?"
        alert.informativeText = "This runs 3 capture passes and a blank capture, then erases and rewrites the sensor's "
            + "calibration partition (flash partition 6). If the sensor is not paired yet, it can trigger a real pairing "
            + "and a reboot of the sensor instead. Keep your finger off the sensor.\n\nType CALIBRATE to continue."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Calibrate")
        alert.addButton(withTitle: "Cancel")
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        field.placeholderString = "CALIBRATE"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        let finish: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard let self = self, response == .alertFirstButtonReturn else { return }
            if field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines) != "CALIBRATE" {
                self.sensorStatus.stringValue = "Not started. Type CALIBRATE (all caps) to confirm."
                return
            }
            self.runSensorAction(["--mmis-calibrate"] + extra, what: "Calibration")
        }
        if let w = mainView.window { alert.beginSheetModal(for: w, completionHandler: finish) } else { finish(alert.runModal()) }
    }

    private func runSensorAction(_ args: [String], what: String) {
        setSensorBusy(true, message: "Waiting for your administrator password...")
        sensorDetailsButton.isHidden = true
        runClient(args) { [weak self] json, cancelled in
            guard let self = self else { return }
            self.setSensorBusy(false)
            if cancelled { self.sensorStatus.stringValue = "Cancelled."; return }
            guard let json = json else {
                self.sensorDetails = "The client printed no result. Is it installed? Try sudo hack-touchid in Terminal."
                self.sensorStatus.stringValue = "\(what) failed: could not run the client."
                self.sensorDetailsButton.isHidden = false
                return
            }
            let details = (json["details"] as? String) ?? ""
            if (json["ok"] as? Bool) == true {
                let msg = (json["message"] as? String) ?? "Done."
                self.sensorStatus.stringValue = "Success: \(msg)"
                self.sensorDetails = "Result: \(msg)\n\n" + (details.isEmpty ? "No sensor log lines for this run." : details)
            } else {
                var msg = (json["error"] as? String) ?? "\(what) failed."
                if (json["code"] as? String) == "need_host_identity" {
                    let refused = (json["refused"] as? Bool) ?? false
                    msg = refused ? "The sensor refused those values. Check both against the other operating system."
                                  : "This sensor needs this laptop's real product name and serial. Fill in both boxes above and press Connect."
                }
                self.sensorStatus.stringValue = "Failed: \(msg)"
                self.sensorDetails = "Result: \(msg)\n\n" + (details.isEmpty ? "No sensor log lines for this run." : details)
            }
            self.sensorDetailsButton.isHidden = false
        }
    }

    @objc private func sensorDetailsTapped() {
        let alert = NSAlert()
        alert.messageText = "What happened"
        alert.informativeText = "The end of the sensor log from the last run."
        let tv = NSTextView(frame: NSRect(x: 0, y: 0, width: 520, height: 260))
        tv.isEditable = false
        tv.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        tv.string = sensorDetails
        let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: 520, height: 260))
        scroll.hasVerticalScroller = true
        scroll.documentView = tv
        alert.accessoryView = scroll
        alert.addButton(withTitle: "Copy")
        alert.addButton(withTitle: "Close")
        let finish: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard let self = self, response == .alertFirstButtonReturn else { return }
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(self.sensorDetails, forType: .string)
        }
        if let w = mainView.window { alert.beginSheetModal(for: w, completionHandler: finish) } else { finish(alert.runModal()) }
    }

    // MARK: Updates page

    private func buildUpdatesPage() -> NSView {
        let page = NSStackView()
        page.orientation = .vertical
        page.alignment = .leading
        page.spacing = 16

        updatesCheckButton.target = self
        updatesCheckButton.action = #selector(checkUpdatesTapped)
        let top = group(nil, rows: [
            settingRow("Updates", subtitle: updatesStatus, control: updatesCheckButton),
        ])

        // Fills left to right while GitHub is being asked, like the macOS copy dialog.
        updatesBar.style = .bar
        updatesBar.isIndeterminate = false
        updatesBar.minValue = 0
        updatesBar.maxValue = 100
        updatesBar.doubleValue = 0
        updatesBar.isHidden = true

        clientUpdateButton.target = self
        clientUpdateButton.action = #selector(updateClientTapped)
        clientUpdateButton.isEnabled = false
        for f in [clientInstalledValue, clientLatestValue, menubarInstalledValue, menubarLatestValue] { f.alignment = .right }
        let clientGroup = group("Client", rows: [
            settingRow("Installed", control: clientInstalledValue),
            settingRow("Latest", control: clientLatestValue),
            settingRow("Update", subtitle: clientState, control: clientUpdateButton),
        ])

        menubarDownloadButton.target = self
        menubarDownloadButton.action = #selector(downloadMenubarTapped)
        menubarDownloadButton.isEnabled = false
        let menubarGroup = group("Menu bar app", rows: [
            settingRow("Installed", control: menubarInstalledValue),
            settingRow("Latest release", control: menubarLatestValue),
            settingRow("Update", subtitle: menubarState, control: menubarDownloadButton),
        ])

        let note = label("Update Client opens Terminal and runs the client's own updater, which asks for your "
                         + "password there. The menu bar app update opens its GitHub release page.", secondary: true)
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        note.preferredMaxLayoutWidth = 560

        for v in [top, updatesBar, clientGroup, menubarGroup, note] { page.addArrangedSubview(v) }
        for v in [top, updatesBar, clientGroup, menubarGroup] { fillWidth(v, of: page) }
        return page
    }

    @objc private func checkUpdatesTapped() { checkForUpdates() }

    /// The installed client's VERSION.txt sits next to the real binary the symlink points at.
    private func readInstalledClient() -> VersionInfo? {
        let real = (try? FileManager.default.destinationOfSymbolicLink(atPath: Paths.clientLink)) ?? Paths.clientLink
        let dir = (real as NSString).deletingLastPathComponent
        guard let text = readSmallFile(dir + "/VERSION.txt") else { return nil }
        return VersionInfo(text: text)
    }

    private func installedMenubarVersion() -> String? {
        guard let app = NSWorkspace.shared.runningApplications.first(where: { $0.bundleIdentifier == "com.vfs5011.hackintosh.menubar" }),
              let url = app.bundleURL else { return nil }
        return Bundle(url: url)?.infoDictionary?["CFBundleShortVersionString"] as? String
    }

    private func checkForUpdates() {
        guard !updatesBusy else { return }
        updatesBusy = true
        updatesCheckButton.isEnabled = false
        updatesStatus.stringValue = "Checking GitHub..."
        startUpdateBar()

        let results = UpdateResults()
        results.installedClient = readInstalledClient()
        results.installedMenubar = installedMenubarVersion()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            // Client: the branch to compare against comes from the installed VERSION.txt.
            if let local = results.installedClient, !local.branch.isEmpty,
               let text = fetchText("\(Links.rawBase)/\(local.branch)/VERSION.txt") {
                results.remoteClient = VersionInfo(text: text)
                results.clientChecked = results.remoteClient != nil
            }
            DispatchQueue.main.async { self?.barTarget = max(self?.barTarget ?? 0, 55) }
            // Menu bar app: the newest release whose title says "menubar".
            if let text = fetchText(Links.releasesAPI, github: true),
               let data = text.data(using: .utf8),
               let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] {
                results.menubarChecked = true
                var best: (code: [Int], tagName: String, url: String)?
                for r in list {
                    if (r["draft"] as? Bool) == true { continue }
                    let name = (r["name"] as? String) ?? ""
                    let tag = (r["tag_name"] as? String) ?? ""
                    let lower = (name + " " + tag).lowercased()
                    guard lower.contains("menubar") || lower.contains("menu bar"),
                          let v = semver(tag), let url = r["html_url"] as? String else { continue }
                    if best == nil || best!.code.lexicographicallyPrecedes(v) { best = (v, tag, url) }
                }
                results.latestMenubarTag = best?.tagName
                results.latestMenubarURL = best?.url
            }
            DispatchQueue.main.async { self?.pendingResults = results }
        }
    }

    private func startUpdateBar() {
        updatesBar.doubleValue = 0
        updatesBar.isHidden = false
        barTarget = 25
        pendingResults = nil
        updatesTimer?.invalidate()
        updatesTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            self?.tickUpdateBar()
        }
    }

    /// Eases toward the current target; the target creeps up while waiting so it never looks
    /// stuck, and jumps to 100 once the answers are in. The result is shown when the bar is full.
    private func tickUpdateBar() {
        let target: Double
        if pendingResults == nil {
            barTarget = min(85, barTarget + 0.1)
            target = barTarget
        } else {
            target = 100
        }
        let shown = updatesBar.doubleValue
        updatesBar.doubleValue = min(target, shown + max((target - shown) * 0.06, 0.35))
        if pendingResults != nil && updatesBar.doubleValue >= 100 { finishUpdateBar() }
    }

    private func finishUpdateBar() {
        updatesTimer?.invalidate()
        updatesTimer = nil
        guard let r = pendingResults else { return }
        pendingResults = nil
        applyUpdateResults(r)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
            self?.updatesBar.isHidden = true
        }
    }

    private func applyUpdateResults(_ r: UpdateResults) {
        updatesBusy = false
        updatesChecked = true
        updatesCheckButton.isEnabled = true

        // Client
        clientInstalledValue.stringValue = r.installedClient?.display ?? "Not installed"
        clientUpdateButton.isEnabled = false
        if let local = r.installedClient, let remote = r.remoteClient {
            clientLatestValue.stringValue = remote.display
            if remote.isNewer(than: local) {
                clientState.stringValue = remote.critical ? "A critical update is available." : "An update is available."
                clientUpdateButton.isEnabled = true
            } else {
                clientState.stringValue = "Up to date."
            }
        } else {
            clientLatestValue.stringValue = "Unknown"
            clientState.stringValue = r.installedClient == nil
                ? "The client is not installed."
                : "Could not check. Are you online?"
        }

        // Menu bar app
        menubarInstalledValue.stringValue = r.installedMenubar.map { "v\($0)" } ?? "Not running"
        menubarDownloadButton.isEnabled = false
        menubarReleaseURL = r.latestMenubarURL
        if let tag = r.latestMenubarTag, let latest = semver(tag) {
            menubarLatestValue.stringValue = tag
            if let installed = r.installedMenubar.flatMap({ semver($0) }) {
                if installed.lexicographicallyPrecedes(latest) {
                    menubarState.stringValue = "A newer release is available."
                    menubarDownloadButton.isEnabled = true
                } else {
                    menubarState.stringValue = "Up to date."
                }
            } else {
                menubarState.stringValue = "Start the menu bar app to compare. The latest release is on GitHub."
                menubarDownloadButton.isEnabled = true
            }
        } else {
            menubarLatestValue.stringValue = "Unknown"
            menubarState.stringValue = r.menubarChecked ? "No menu bar release found." : "Could not check. Are you online?"
        }

        let clientNew = clientUpdateButton.isEnabled
        let menubarNew = menubarDownloadButton.isEnabled && r.installedMenubar != nil
        updatesStatus.stringValue = (clientNew || menubarNew) ? "Updates are available." : "Everything is up to date."
        if !r.clientChecked && !r.menubarChecked { updatesStatus.stringValue = "Could not reach GitHub." }
    }

    /// Same approach as the menu bar app: a small .command file opened with the default
    /// terminal, running the client's own updater (it asks for sudo in that window).
    @objc private func updateClientTapped() {
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
            NSSound.beep()
        }
    }

    @objc private func downloadMenubarTapped() {
        open(menubarReleaseURL ?? Links.releases)
    }

    // MARK: About page

    private func buildAboutPage() -> NSView {
        let page = NSStackView()
        page.orientation = .vertical
        page.alignment = .leading
        page.spacing = 16

        let version = (Bundle(for: HackTouchIDPane.self).infoDictionary?["CFBundleShortVersionString"] as? String) ?? "?"
        let info = group(nil, rows: [
            settingRow("Preference pane version", control: label("v\(version)", secondary: true)),
            settingRow("License", control: label("BSD 3-Clause", secondary: true)),
        ])
        let links = group("Links", rows: [
            settingRow("Website", control: NSButton(title: "Open", target: self, action: #selector(openWebsite))),
            settingRow("GitHub repository", control: NSButton(title: "Open", target: self, action: #selector(openRepo))),
            settingRow("Releases", control: NSButton(title: "Open", target: self, action: #selector(openReleases))),
            settingRow("Report an issue", control: NSButton(title: "Open", target: self, action: #selector(openIssues))),
        ])
        for g in [info, links] {
            page.addArrangedSubview(g)
            fillWidth(g, of: page)
        }
        return page
    }

    // MARK: Refresh (all read-only)

    private func refreshAll() {
        // Authentication switch: the flag file's presence means scanning is paused.
        let enabled = !fileExists(Paths.scanningDisabledFlag)
        authSwitch.state = enabled ? .on : .off
        authStatus.stringValue = enabled ? "On. Swipe your finger when asked to authenticate."
                                         : "Off. Fingerprint authentication is paused, use your password."

        // Daemon: launchd label check in the user's GUI domain, same idea as
        // the menu bar app's health check.
        let (status, _) = run("/bin/launchctl", ["print", "gui/\(getuid())/com.hackintosh.vfs5011agent"])
        let daemonLabelFound = status == 0
        daemonValue.stringValue = daemonLabelFound ? "Running" : "Not running (run Deploy [3] in the client)"
        daemonValue.textColor = daemonLabelFound ? .labelColor : .systemOrange

        // Client version from the real binary's folder (the symlink target).
        clientValue.stringValue = clientVersionString()

        // Menu bar app: found through the running process, since users run it
        // from /Applications or straight from the build folder.
        if let app = NSWorkspace.shared.runningApplications.first(where: { $0.bundleIdentifier == "com.vfs5011.hackintosh.menubar" }),
           let url = app.bundleURL,
           let v = Bundle(url: url)?.infoDictionary?["CFBundleShortVersionString"] as? String {
            menubarValue.stringValue = "v\(v), running"
        } else {
            menubarValue.stringValue = "Not running"
        }

        // Threshold
        let current = currentThreshold()
        if let idx = thresholdChoices.firstIndex(of: current) { thresholdPopup.selectItem(at: idx) }
        thresholdHint.stringValue = "Lower accepts more swipes, higher is stricter. Default \(thresholdDefault)."

        // Tahoe section
        tahoeBox?.isHidden = macOSMajor() < 26
        lockSwipeSwitch.state = fileExists(Paths.lockSwipeFlag) ? .on : .off
        lockSwipeHint.stringValue = "Arm the sensor and lock the screen with a swipe (Control-Command-Q)."
    }

    private func currentThreshold() -> Int {
        guard let text = readSmallFile(Paths.thresholdConf),
              let v = Int(text.trimmingCharacters(in: .whitespacesAndNewlines)),
              thresholdChoices.contains(v) else { return thresholdDefault }
        return v
    }

    private func clientVersionString() -> String {
        let real = (try? FileManager.default.destinationOfSymbolicLink(atPath: Paths.clientLink)) ?? Paths.clientLink
        let dir = (real as NSString).deletingLastPathComponent
        guard let text = readSmallFile(dir + "/VERSION.txt") else {
            return fileExists(Paths.clientLink) ? "Installed (version unreadable)" : "Not installed"
        }
        let kv = parseKeyValues(text)
        let v = kv["VERSION"] ?? "?"
        let b = kv["BUILD"].map { " (\($0))" } ?? ""
        let br = kv["BRANCH"].map { ", \($0)" } ?? ""
        return "v\(v)\(b)\(br)"
    }

    // MARK: Actions

    @objc private func authSwitchChanged() {
        postRequest(authSwitch.state == .on ? Notify.requestEnable : Notify.requestDisable)
        // The daemon confirms; the observer or the 3 second timer refreshes the UI.
    }

    @objc private func thresholdChanged() {
        guard let title = thresholdPopup.titleOfSelectedItem, let v = Int(title) else { return }
        postRequest(Notify.thresholdPrefix + String(v))
    }

    @objc private func lockSwipeChanged() {
        postRequest(lockSwipeSwitch.state == .on ? Notify.requestLockSwipeEnable : Notify.requestLockSwipeDisable)
    }

    /// The client needs sudo, so it opens in a real Terminal window. A .command
    /// file opened with Terminal avoids the "System Settings wants to control
    /// Terminal" automation prompt that osascript would trigger from here.
    @objc private func openClientTapped() {
        let path = NSTemporaryDirectory() + "hack-touchid-open.command"
        let body = "#!/bin/bash\nclear\nsudo hack-touchid\n"
        do {
            try body.write(toFile: path, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: path)
            _ = run("/usr/bin/open", ["-a", "Terminal", path])
        } catch {
            NSSound.beep()
        }
    }

    @objc private func openNotificationSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.Notifications-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func openWebsite() { open(Links.website) }
    @objc private func openRepo() { open(Links.repo) }
    @objc private func openReleases() { open(Links.releases) }
    @objc private func openIssues() { open(Links.issues) }
    private func open(_ s: String) { if let u = URL(string: s) { NSWorkspace.shared.open(u) } }
}
