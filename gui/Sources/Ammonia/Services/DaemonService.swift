import Foundation

enum DaemonService {
    private static let plistPath = "/Library/LaunchDaemons/com.ammonia.inject.plist"
    private static let plistSource = "/private/var/ammonia/core/share/com.ammonia.inject.plist"

    static func status() -> DaemonStatus {
        guard FileManager.default.fileExists(atPath: plistPath) else {
            return .notInstalled
        }
        if ProcessRunner.run(
            executable: "/bin/launchctl",
            arguments: ["print", "system/com.ammonia.inject"]
        ) {
            return .loaded
        }
        return .notLoaded
    }

    @discardableResult
    static func install() -> Bool {
        var src = plistSource
        if !FileManager.default.isReadableFile(atPath: plistSource) {
            let fallback = "/tmp/com.ammonia.inject.plist"
            guard writeFallbackPlist(to: fallback) else { return false }
            src = fallback
        }

        let script = """
        do shell script "mkdir -p /var/log/ammonia && \
        cp '\(src)' '\(plistPath)' && \
        chown root:wheel '\(plistPath)' && \
        chmod 644 '\(plistPath)' && \
        (launchctl bootout system/com.pluginplayground.grant 2>/dev/null || true) && \
        rm -f /Library/LaunchDaemons/com.pluginplayground.grant.plist && \
        (launchctl bootout system/com.ammonia.inject 2>/dev/null || true) && \
        (launchctl bootstrap system '\(plistPath)' || launchctl load '\(plistPath)')" \
        with administrator privileges
        """
        let ok = ProcessRunner.privilegedAppleScript(script)
        if src == "/tmp/com.ammonia.inject.plist" {
            try? FileManager.default.removeItem(atPath: src)
        }
        return ok
    }

    @discardableResult
    static func uninstall() -> Bool {
        ProcessRunner.privilegedAppleScript(
            """
            do shell script "launchctl bootout system/com.ammonia.inject 2>/dev/null; \
            launchctl bootout system/com.pluginplayground.grant 2>/dev/null; \
            launchctl unload /Library/LaunchDaemons/com.ammonia.inject.plist 2>/dev/null; \
            rm -f /Library/LaunchDaemons/com.ammonia.inject.plist \
            /Library/LaunchDaemons/com.pluginplayground.grant.plist" with administrator privileges
            """
        )
    }

    private static func writeFallbackPlist(to path: String) -> Bool {
        let xml = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>Label</key>
            <string>com.ammonia.inject</string>
            <key>ProgramArguments</key>
            <array>
                <string>/private/var/ammonia/core/ammonia</string>
            </array>
            <key>RunAtLoad</key>
            <true/>
            <key>KeepAlive</key>
            <dict>
                <key>SuccessfulExit</key>
                <false/>
            </dict>
            <key>ThrottleInterval</key>
            <integer>10</integer>
            <key>StandardOutPath</key>
            <string>/var/log/ammonia/ammonia.log</string>
            <key>StandardErrorPath</key>
            <string>/var/log/ammonia/ammonia.err</string>
        </dict>
        </plist>
        """
        do {
            try xml.write(toFile: path, atomically: true, encoding: .utf8)
            return true
        } catch {
            return false
        }
    }
}
