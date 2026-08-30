import AppKit
import Foundation

enum TweakService {
    static let tweaksDirectory = "/private/var/ammonia/core/tweaks"

    static func scanTweaks() -> [TweakItem] {
        var opts = OptionsService.load()
        var found: [String] = []

        guard let entries = try? FileManager.default.contentsOfDirectory(atPath: tweaksDirectory) else {
            return []
        }

        var disabledMarkers: [String] = []
        for name in entries {
            if name.hasSuffix(".dylib.disabled") {
                disabledMarkers.append(name)
                continue
            }
            if name.hasSuffix(".dylib"), !name.contains(".."), !name.contains("/") {
                found.append(name)
            }
        }

        if !disabledMarkers.isEmpty {
            let markerSet = Set(disabledMarkers)
            opts.enabledTweaks = found.filter { !markerSet.contains($0 + ".disabled") }
            for marker in disabledMarkers {
                try? FileManager.default.removeItem(atPath: "\(tweaksDirectory)/\(marker)")
            }
            _ = OptionsService.save(opts)
        }

        let enabled = Set(opts.enabledTweaks)
        return found.sorted().map { name in
            TweakItem(name: name, isDisabled: !enabled.contains(name))
        }
    }

    static func iconURL(for tweakName: String) -> URL? {
        let path = "\(tweaksDirectory)/\(tweakName).png"
        return FileManager.default.isReadableFile(atPath: path) ? URL(fileURLWithPath: path) : nil
    }

    static func defaultIconURL() -> URL? {
        if let bundled = Bundle.main.url(forResource: "drill", withExtension: "png") {
            return bundled
        }
        return Bundle.module.url(forResource: "drill", withExtension: "png")
    }

    @discardableResult
    static func toggleTweak(named name: String) -> Bool {
        var opts = OptionsService.load()
        if let idx = opts.enabledTweaks.firstIndex(of: name) {
            opts.enabledTweaks.remove(at: idx)
        } else {
            guard isTweakSafe(path: "\(tweaksDirectory)/\(name)") else { return false }
            opts.enabledTweaks.append(name)
        }
        return OptionsService.save(opts)
    }

    static func hasDeveloperTools() -> Bool {
        ProcessRunner.run(executable: "/usr/bin/xcode-select", arguments: ["-p"])
    }

    static func checkSipStatus() -> SipStatus {
        let (result, _) = ProcessRunner.capture(
            executable: "/usr/bin/csrutil",
            arguments: ["status"]
        )
        if result.contains("System Integrity Protection status: disabled.") {
            return .disabled
        }
        if result.contains("Debugging Restrictions: disabled") {
            return .partiallyDisabled
        }
        if result.contains("System Integrity Protection status: enabled.") {
            return .enabled
        }
        return .unknown
    }

    @discardableResult
    static func installTweak(from sourceURL: URL) -> Bool {
        let base = sourceURL.lastPathComponent
        guard isSafeTweakName(base), base.hasSuffix(".dylib") else { return false }

        let dest = "\(tweaksDirectory)/\(base)"
        let script = """
        do shell script "install -o root -g wheel -m 755 " & quoted form of "\(sourceURL.path)" & " " & quoted form of "\(dest)" with administrator privileges
        """
        guard ProcessRunner.privilegedAppleScript(script) else { return false }

        var opts = OptionsService.load()
        if !opts.enabledTweaks.contains(base) {
            opts.enabledTweaks.append(base)
            _ = OptionsService.save(opts)
        }
        return true
    }

    @discardableResult
    static func packageTweak(named name: String) -> Bool {
        guard isSafeTweakName(name) else { return false }
        let dylibPath = "\(tweaksDirectory)/\(name)"
        guard FileManager.default.isReadableFile(atPath: dylibPath) else { return false }

        let staging = "/tmp/ammonia_tweak_\(name)"
        let tweakDest = "\(staging)/private/var/ammonia/core/tweaks"
        do {
            try FileManager.default.createDirectory(
                atPath: tweakDest,
                withIntermediateDirectories: true
            )
        } catch {
            return false
        }

        let destFile = "\(tweakDest)/\(name)"
        guard copyFile(from: dylibPath, to: destFile) else {
            try? FileManager.default.removeItem(atPath: staging)
            return false
        }

        for suffix in [".whitelist", ".blacklist", ".options", ".png"] {
            let src = "\(tweaksDirectory)/\(name)\(suffix)"
            if FileManager.default.isReadableFile(atPath: src) {
                _ = copyFile(from: src, to: "\(tweakDest)/\(name)\(suffix)")
            }
        }

        let pkgName = "/tmp/\(name).pkg"
        let ident = "com.ammonia.tweak.\(name)"
        let ok = ProcessRunner.run(
            executable: "/usr/bin/pkgbuild",
            arguments: [
                "--root", staging,
                "--identifier", ident,
                "--version", "1.0.0",
                "--install-location", "/",
                pkgName,
            ]
        )
        try? FileManager.default.removeItem(atPath: staging)
        return ok
    }

    static func loadSidecarOptions(for name: String) -> TweakSidecarOptions {
        var opts = TweakSidecarOptions()
        opts.processWhitelist = readSidecarLines(path: "\(tweaksDirectory)/\(name).whitelist")

        let optionsPath = "\(tweaksDirectory)/\(name).options"
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: optionsPath)),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil),
              let dict = plist as? [String: Any]
        else {
            return opts
        }

        if let apps = dict["blacklistedApps"] as? [String] {
            opts.blacklistedApps = apps
        }
        if let deps = dict["frameworkDependencies"] as? [String] {
            opts.frameworkDependencies = deps
        }
        return opts
    }

    @discardableResult
    static func saveSidecarOptions(for name: String, options: TweakSidecarOptions) -> Bool {
        let optionsPath = "\(tweaksDirectory)/\(name).options"
        let dict: [String: Any] = [
            "blacklistedApps": options.blacklistedApps,
            "frameworkDependencies": options.frameworkDependencies,
        ]
        guard let data = try? PropertyListSerialization.data(
            fromPropertyList: dict,
            format: .xml,
            options: 0
        ) else {
            return false
        }

        let tmp = optionsPath + ".tmp"
        do {
            try data.write(to: URL(fileURLWithPath: tmp))
            if FileManager.default.fileExists(atPath: optionsPath) {
                try FileManager.default.removeItem(atPath: optionsPath)
            }
            try FileManager.default.moveItem(atPath: tmp, toPath: optionsPath)
        } catch {
            try? FileManager.default.removeItem(atPath: tmp)
            return false
        }

        return writeSidecarLines(path: "\(tweaksDirectory)/\(name).whitelist", lines: options.processWhitelist)
    }

    @discardableResult
    static func ensurePermissions() -> Bool {
        if FileManager.default.isReadableFile(atPath: tweaksDirectory),
           FileManager.default.isWritableFile(atPath: tweaksDirectory)
        {
            return true
        }

        let alert = NSAlert()
        alert.messageText = "Ammonia needs folder access"
        alert.informativeText =
            "Ammonia needs permission to write to:\n\(tweaksDirectory)\n\nClick Fix to authenticate and fix permissions."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Fix")
        alert.addButton(withTitle: "Exit")
        if alert.runModal() == .alertSecondButtonReturn {
            return false
        }

        let script = """
        do shell script "mkdir -p \(tweaksDirectory) && chmod 777 \(tweaksDirectory)" with administrator privileges
        """
        return ProcessRunner.privilegedAppleScript(script)
            && FileManager.default.isReadableFile(atPath: tweaksDirectory)
            && FileManager.default.isWritableFile(atPath: tweaksDirectory)
    }

    private static func isTweakSafe(path: String) -> Bool {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: path),
              let uid = attrs[.ownerAccountID] as? NSNumber
        else {
            return false
        }
        if uid.intValue != 0 { return false }
        if let perms = attrs[.posixPermissions] as? NSNumber {
            let mode = perms.intValue
            if mode & 0o022 != 0 { return false }
        }
        return true
    }

    private static func isSafeTweakName(_ name: String) -> Bool {
        guard !name.isEmpty, name.count <= 255 else { return false }
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: ".-_"))
        return name.unicodeScalars.allSatisfy { allowed.contains($0) }
    }

    private static func copyFile(from src: String, to dest: String) -> Bool {
        do {
            if FileManager.default.fileExists(atPath: dest) {
                try FileManager.default.removeItem(atPath: dest)
            }
            try FileManager.default.copyItem(atPath: src, toPath: dest)
            return true
        } catch {
            return false
        }
    }

    private static func readSidecarLines(path: String) -> [String] {
        guard let data = try? String(contentsOfFile: path, encoding: .utf8) else { return [] }
        return data
            .split(whereSeparator: \.isNewline)
            .map { String($0).trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty && !$0.hasPrefix("#") }
    }

    @discardableResult
    private static func writeSidecarLines(path: String, lines: [String]) -> Bool {
        if lines.isEmpty {
            try? FileManager.default.removeItem(atPath: path)
            return true
        }
        let tmp = path + ".tmp"
        let body = lines.joined(separator: "\n") + "\n"
        do {
            try body.write(toFile: tmp, atomically: true, encoding: .utf8)
            if FileManager.default.fileExists(atPath: path) {
                try FileManager.default.removeItem(atPath: path)
            }
            try FileManager.default.moveItem(atPath: tmp, toPath: path)
            return true
        } catch {
            try? FileManager.default.removeItem(atPath: tmp)
            return false
        }
    }
}
