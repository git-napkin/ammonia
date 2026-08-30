import Foundation

enum OptionsService {
    private static let path = "/private/var/ammonia/core/current.options"

    static func load() -> AmmoniaOptions {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil),
              let dict = plist as? [String: Any]
        else {
            return AmmoniaOptions()
        }

        var opts = AmmoniaOptions()
        opts.disablePAC = dict["disablePAC"] as? Bool ?? false
        opts.pauseInjection = dict["pauseInjection"] as? Bool ?? false
        if let enabled = dict["enabledTweaks"] as? [String] {
            opts.enabledTweaks = enabled
        }
        return opts
    }

    private static func fixPermissions() -> Bool {
        ProcessRunner.privilegedAppleScript(
            """
            do shell script "mkdir -p /private/var/ammonia/core && \
            touch /private/var/ammonia/core/current.options && \
            chmod 666 /private/var/ammonia/core/current.options" with administrator privileges
            """
        )
    }

    @discardableResult
    static func save(_ opts: AmmoniaOptions) -> Bool {
        let dict: [String: Any] = [
            "disablePAC": opts.disablePAC,
            "pauseInjection": opts.pauseInjection,
            "enabledTweaks": opts.enabledTweaks,
        ]
        guard let data = try? PropertyListSerialization.data(
            fromPropertyList: dict,
            format: .xml,
            options: 0
        ) else {
            return false
        }

        let url = URL(fileURLWithPath: path)
        func write() -> Bool {
            let tmp = url.deletingLastPathComponent().appendingPathComponent("current.options.tmp")
            do {
                try data.write(to: tmp, options: .atomic)
                if FileManager.default.fileExists(atPath: url.path) {
                    try FileManager.default.removeItem(at: url)
                }
                try FileManager.default.moveItem(at: tmp, to: url)
                try FileManager.default.setAttributes([.posixPermissions: 0o666], ofItemAtPath: url.path)
                return true
            } catch {
                try? FileManager.default.removeItem(at: tmp)
                return false
            }
        }

        if write() { return true }
        if fixPermissions() { return write() }
        return false
    }
}
