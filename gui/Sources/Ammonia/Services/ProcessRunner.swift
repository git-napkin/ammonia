import Foundation

enum ProcessRunner {
    @discardableResult
    static func run(executable: String, arguments: [String] = []) -> Bool {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }

    static func capture(executable: String, arguments: [String] = []) -> (output: String, status: Int32) {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
            process.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            let text = String(data: data, encoding: .utf8) ?? ""
            return (text, process.terminationStatus)
        } catch {
            return ("", -1)
        }
    }

    @discardableResult
    static func privilegedAppleScript(_ script: String) -> Bool {
        run(executable: "/usr/bin/osascript", arguments: ["-e", script])
    }
}
