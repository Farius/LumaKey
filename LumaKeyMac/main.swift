import Cocoa
import Carbon.HIToolbox
import IOKit

// MARK: - Configuration

private let kDefaultStepPercent = 10
private let kOsdHideDelay: TimeInterval = 3.0

// Option+Shift+] = brightness up, Option+Shift+[ = brightness down.
// One-hand friendly; Cmd+Shift+[ / ] would shadow the standard
// tab-switching shortcuts in browsers and editors.
private let kHotkeyModifiers = UInt32(optionKey | shiftKey)
private let kHotkeyUpKey = UInt32(kVK_ANSI_RightBracket)
private let kHotkeyDownKey = UInt32(kVK_ANSI_LeftBracket)

private let kHotkeySignature: OSType = 0x4C4D4B59 // 'LMKY'
private let kHotkeyUpId: UInt32 = 1
private let kHotkeyDownId: UInt32 = 2

// MARK: - Paths, log, settings

private let executableDir: URL = {
    let path = Bundle.main.executablePath ?? CommandLine.arguments[0]
    return URL(fileURLWithPath: path).resolvingSymlinksInPath().deletingLastPathComponent()
}()

private let logURL = executableDir.appendingPathComponent("LumaKey.log")
private let settingsURL = executableDir.appendingPathComponent("settings.ini")

private let logDateFormatter: DateFormatter = {
    let formatter = DateFormatter()
    formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
    return formatter
}()

func logLine(_ message: String) {
    let line = "[\(logDateFormatter.string(from: Date()))] \(message)\n"
    guard let data = line.data(using: .utf8) else { return }
    if let handle = try? FileHandle(forWritingTo: logURL) {
        handle.seekToEndOfFile()
        handle.write(data)
        handle.closeFile()
    } else {
        try? data.write(to: logURL)
    }
}

func loadStepPercent() -> Int {
    guard let content = try? String(contentsOf: settingsURL, encoding: .utf8) else {
        try? "[Brightness]\nStep=10\n".write(to: settingsURL, atomically: true, encoding: .utf8)
        return kDefaultStepPercent
    }
    for rawLine in content.split(whereSeparator: { $0.isNewline }) {
        let line = rawLine.trimmingCharacters(in: .whitespaces)
        guard line.lowercased().hasPrefix("step"), let eq = line.firstIndex(of: "=") else { continue }
        let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
        if let step = Int(value), (1...100).contains(step) {
            return step
        }
    }
    return kDefaultStepPercent
}

// MARK: - Brightness

// Primary path is the private DisplayServices framework (works on Monterey
// for the built-in panel); IODisplayConnect is the Intel-only fallback.
private typealias DSGetBrightness = @convention(c) (CGDirectDisplayID, UnsafeMutablePointer<Float>) -> Int32
private typealias DSSetBrightness = @convention(c) (CGDirectDisplayID, Float) -> Int32
private typealias IODisplayGetFloat = @convention(c) (io_service_t, IOOptionBits, CFString, UnsafeMutablePointer<Float>) -> kern_return_t
private typealias IODisplaySetFloat = @convention(c) (io_service_t, IOOptionBits, CFString, Float) -> kern_return_t

final class BrightnessController {
    private var dsGet: DSGetBrightness?
    private var dsSet: DSSetBrightness?
    private var ioGet: IODisplayGetFloat?
    private var ioSet: IODisplaySetFloat?

    init() {
        if let handle = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_NOW) {
            if let sym = dlsym(handle, "DisplayServicesGetBrightness") {
                dsGet = unsafeBitCast(sym, to: DSGetBrightness.self)
            }
            if let sym = dlsym(handle, "DisplayServicesSetBrightness") {
                dsSet = unsafeBitCast(sym, to: DSSetBrightness.self)
            }
        }
        if let handle = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW) {
            if let sym = dlsym(handle, "IODisplayGetFloatParameter") {
                ioGet = unsafeBitCast(sym, to: IODisplayGetFloat.self)
            }
            if let sym = dlsym(handle, "IODisplaySetFloatParameter") {
                ioSet = unsafeBitCast(sym, to: IODisplaySetFloat.self)
            }
        }
        logLine("DisplayServices available: \(dsGet != nil && dsSet != nil), IODisplay fallback available: \(ioGet != nil && ioSet != nil)")
    }

    private var builtinDisplayID: CGDirectDisplayID {
        var displays = [CGDirectDisplayID](repeating: 0, count: 16)
        var count: UInt32 = 0
        if CGGetOnlineDisplayList(16, &displays, &count) == .success {
            for i in 0..<Int(count) where CGDisplayIsBuiltin(displays[i]) != 0 {
                return displays[i]
            }
        }
        return CGMainDisplayID()
    }

    private func withIoDisplayService<T>(_ body: (io_service_t) -> T?) -> T? {
        let service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IODisplayConnect"))
        guard service != 0 else { return nil }
        defer { IOObjectRelease(service) }
        return body(service)
    }

    func current() -> Float? {
        if let get = dsGet {
            var value: Float = 0
            if get(builtinDisplayID, &value) == 0 {
                return value
            }
            logLine("DisplayServicesGetBrightness failed")
        }
        if let get = ioGet {
            let value: Float? = withIoDisplayService { service in
                var value: Float = 0
                if get(service, 0, "brightness" as CFString, &value) == KERN_SUCCESS {
                    return value
                }
                logLine("IODisplayGetFloatParameter failed")
                return nil
            }
            if let value = value {
                return value
            }
        }
        return nil
    }

    func set(_ value: Float) -> Bool {
        let clamped = max(0, min(1, value))
        if let set = dsSet {
            if set(builtinDisplayID, clamped) == 0 {
                return true
            }
            logLine("DisplayServicesSetBrightness failed")
        }
        if let set = ioSet {
            let ok: Bool? = withIoDisplayService { service in
                let result = set(service, 0, "brightness" as CFString, clamped)
                if result != KERN_SUCCESS {
                    logLine("IODisplaySetFloatParameter failed: \(result)")
                }
                return result == KERN_SUCCESS
            }
            if ok == true {
                return true
            }
        }
        return false
    }
}

// MARK: - OSD

final class OsdController {
    private let panel: NSPanel
    private let label = NSTextField(labelWithString: "")
    private let fill = NSView()
    private var hideTimer: Timer?

    private let width: CGFloat = 220
    private let height: CGFloat = 64
    private let inset: CGFloat = 16

    init() {
        panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: width, height: height),
                        styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: false)
        panel.level = .screenSaver
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]

        let content = NSView(frame: NSRect(x: 0, y: 0, width: width, height: height))
        content.wantsLayer = true
        content.layer?.backgroundColor = NSColor(calibratedWhite: 0.12, alpha: 0.92).cgColor
        content.layer?.cornerRadius = 12

        label.frame = NSRect(x: inset, y: height - 34, width: width - inset * 2, height: 20)
        label.font = .systemFont(ofSize: 15, weight: .semibold)
        label.textColor = NSColor(calibratedWhite: 0.94, alpha: 1)
        content.addSubview(label)

        let track = NSView(frame: NSRect(x: inset, y: 14, width: width - inset * 2, height: 4))
        track.wantsLayer = true
        track.layer?.backgroundColor = NSColor(calibratedWhite: 0.28, alpha: 1).cgColor
        track.layer?.cornerRadius = 2
        content.addSubview(track)

        fill.frame = NSRect(x: inset, y: 14, width: 0, height: 4)
        fill.wantsLayer = true
        fill.layer?.backgroundColor = NSColor.white.cgColor
        fill.layer?.cornerRadius = 2
        content.addSubview(fill)

        panel.contentView = content
    }

    func show(percent: Int) {
        label.stringValue = "Brightness  \(percent)%"
        fill.frame.size.width = (width - inset * 2) * CGFloat(percent) / 100
        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            panel.setFrameOrigin(NSPoint(x: frame.midX - width / 2, y: frame.minY + 24))
        }
        panel.orderFrontRegardless()
        hideTimer?.invalidate()
        hideTimer = Timer.scheduledTimer(withTimeInterval: kOsdHideDelay, repeats: false) { [weak self] _ in
            self?.panel.orderOut(nil)
        }
    }
}

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem?
    private let brightness = BrightnessController()
    private let osd = OsdController()
    private var stepPercent = kDefaultStepPercent
    private var hotKeyUp: EventHotKeyRef?
    private var hotKeyDown: EventHotKeyRef?
    private var hotKeyHandler: EventHandlerRef?

    func applicationDidFinishLaunching(_ notification: Notification) {
        stepPercent = loadStepPercent()
        logLine("LumaKey started, step \(stepPercent)%")
        setupStatusItem()
        registerHotkeys()
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let ref = hotKeyUp { UnregisterEventHotKey(ref) }
        if let ref = hotKeyDown { UnregisterEventHotKey(ref) }
        if let handler = hotKeyHandler { RemoveEventHandler(handler) }
    }

    func hotkeyUpPressed() { adjust(by: stepPercent) }
    func hotkeyDownPressed() { adjust(by: -stepPercent) }

    private func setupStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = item.button {
            if let image = NSImage(systemSymbolName: "sun.max.fill", accessibilityDescription: "LumaKey") {
                button.image = image
            } else {
                button.title = "☀"
            }
            button.toolTip = "LumaKey"
        }

        let menu = NSMenu()
        let up = NSMenuItem(title: "Brightness +", action: #selector(menuBrightnessUp), keyEquivalent: "")
        up.target = self
        menu.addItem(up)
        let down = NSMenuItem(title: "Brightness −", action: #selector(menuBrightnessDown), keyEquivalent: "")
        down.target = self
        menu.addItem(down)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit LumaKey", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        item.menu = menu
        statusItem = item
    }

    @objc private func menuBrightnessUp() { adjust(by: stepPercent) }
    @objc private func menuBrightnessDown() { adjust(by: -stepPercent) }

    private func adjust(by deltaPercent: Int) {
        guard let current = brightness.current() else {
            logLine("Failed to read brightness")
            return
        }
        let currentPercent = Int((current * 100).rounded())
        let nextPercent = max(0, min(100, currentPercent + deltaPercent))
        logLine("Brightness \(currentPercent)% -> \(nextPercent)%")
        guard brightness.set(Float(nextPercent) / 100) else {
            logLine("Failed to set brightness")
            return
        }
        osd.show(percent: nextPercent)
        statusItem?.button?.toolTip = "LumaKey — \(nextPercent)%"
    }

    private func registerHotkeys() {
        var eventSpec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard),
                                      eventKind: UInt32(kEventHotKeyPressed))
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        let installStatus = InstallEventHandler(GetEventDispatcherTarget(), { _, event, userData in
            guard let event = event, let userData = userData else { return noErr }
            var hotKeyID = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject),
                              EventParamType(typeEventHotKeyID), nil,
                              MemoryLayout<EventHotKeyID>.size, nil, &hotKeyID)
            let delegate = Unmanaged<AppDelegate>.fromOpaque(userData).takeUnretainedValue()
            if hotKeyID.id == kHotkeyUpId { delegate.hotkeyUpPressed() }
            if hotKeyID.id == kHotkeyDownId { delegate.hotkeyDownPressed() }
            return noErr
        }, 1, &eventSpec, selfPtr, &hotKeyHandler)
        if installStatus != noErr {
            logLine("InstallEventHandler failed: \(installStatus)")
        }

        let upStatus = RegisterEventHotKey(kHotkeyUpKey, kHotkeyModifiers,
                                           EventHotKeyID(signature: kHotkeySignature, id: kHotkeyUpId),
                                           GetEventDispatcherTarget(), 0, &hotKeyUp)
        if upStatus != noErr {
            logLine("RegisterEventHotKey Option+Shift+] failed: \(upStatus)")
        }
        let downStatus = RegisterEventHotKey(kHotkeyDownKey, kHotkeyModifiers,
                                             EventHotKeyID(signature: kHotkeySignature, id: kHotkeyDownId),
                                             GetEventDispatcherTarget(), 0, &hotKeyDown)
        if downStatus != noErr {
            logLine("RegisterEventHotKey Option+Shift+[ failed: \(downStatus)")
        }
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
