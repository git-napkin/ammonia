import AppKit
import Foundation
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class AppModel: ObservableObject {
    @Published var disablePAC = false
    @Published var pauseInjection = false
    @Published var tweaks: [TweakItem] = []
    @Published var sipStatus: SipStatus = .unknown
    @Published var daemonStatus: DaemonStatus = .notInstalled
    @Published var devToolsAvailable = false
    @Published var statusMessage = ""
    @Published var tweakEditor: TweakNameItem?
    @Published var editorOptions = TweakSidecarOptions()

    func load() {
        guard TweakService.ensurePermissions() else {
            NSApplication.shared.terminate(nil)
            return
        }
        let opts = OptionsService.load()
        disablePAC = opts.disablePAC
        pauseInjection = opts.pauseInjection
        refreshTweaks()
        refreshProbes()
    }

    func refreshTweaks() {
        tweaks = TweakService.scanTweaks()
    }

    func refreshProbes() {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let tools = TweakService.hasDeveloperTools()
            let sip = TweakService.checkSipStatus()
            let daemon = DaemonService.status()
            DispatchQueue.main.async {
                self?.devToolsAvailable = tools
                self?.sipStatus = sip
                self?.daemonStatus = daemon
            }
        }
    }

    func saveSettings() {
        var opts = OptionsService.load()
        opts.disablePAC = disablePAC
        opts.pauseInjection = pauseInjection
        if OptionsService.save(opts) {
            statusMessage = "Settings saved."
        } else {
            statusMessage = "Error: Cannot write to /private/var/ammonia/core/current.options."
        }
    }

    func settingsTogglesChanged() {
        saveSettings()
    }

    func toggleTweak(at index: Int) {
        guard tweaks.indices.contains(index) else { return }
        let name = tweaks[index].name
        if TweakService.toggleTweak(named: name) {
            tweaks[index].isDisabled.toggle()
        } else {
            statusMessage = "Error: Cannot toggle \(name)."
        }
    }

    func installTweak() {
        let panel = NSOpenPanel()
        panel.title = "Choose a tweak dylib"
        if let dylib = UTType(filenameExtension: "dylib") {
            panel.allowedContentTypes = [dylib]
        }
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else {
            statusMessage = "Install canceled or failed."
            return
        }
        if TweakService.installTweak(from: url) {
            refreshTweaks()
            statusMessage = "Tweak installed."
        } else {
            statusMessage = "Install canceled or failed."
        }
    }

    func packageTweak(named name: String) {
        if TweakService.packageTweak(named: name) {
            statusMessage = "Packaged: /tmp/\(name).pkg"
        } else {
            statusMessage = "Package failed for \(name)."
        }
    }

    func installDaemon() {
        if DaemonService.install() {
            statusMessage = "Launch daemon installed."
        } else {
            statusMessage = "Error: Failed to install launch daemon."
        }
        daemonStatus = DaemonService.status()
    }

    func uninstallDaemon() {
        if DaemonService.uninstall() {
            statusMessage = "Launch daemon uninstalled."
        } else {
            statusMessage = "Error: Failed to uninstall launch daemon."
        }
        daemonStatus = DaemonService.status()
    }

    func beginEditingTweak(_ name: String) {
        editorOptions = TweakService.loadSidecarOptions(for: name)
        tweakEditor = TweakNameItem(name: name)
    }

    func saveEditor() {
        guard let name = tweakEditor?.name else { return }
        if TweakService.saveSidecarOptions(for: name, options: editorOptions) {
            tweakEditor = nil
        }
    }

    func cancelEditor() {
        tweakEditor = nil
    }
}

struct TweakNameItem: Identifiable {
    let name: String
    var id: String { name }
}
