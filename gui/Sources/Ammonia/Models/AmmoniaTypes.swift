import Foundation

struct AmmoniaOptions: Equatable {
    var disablePAC = false
    var pauseInjection = false
    var enabledTweaks: [String] = []
}

struct TweakItem: Identifiable, Equatable {
    var id: String { name }
    let name: String
    var isDisabled: Bool
}

struct TweakSidecarOptions: Equatable {
    var blacklistedApps: [String] = []
    var frameworkDependencies: [String] = []
    var processWhitelist: [String] = []
}

enum SipStatus: Int {
    case unknown = 0
    case enabled = 1
    case disabled = 2
    case partiallyDisabled = 3
}

enum DaemonStatus: Int {
    case notInstalled = 0
    case loaded = 1
    case notLoaded = 2
}
