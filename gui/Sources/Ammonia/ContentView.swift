import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Form {
            Section {
                Toggle("Disable arm64e (PAC)", isOn: $model.disablePAC)
                    .onChange(of: model.disablePAC) { _ in model.settingsTogglesChanged() }
            } footer: {
                Text("Not applied in launchd. Prefer native arm64e plus -arm64e_preview_abi.")
            }

            Section {
                Toggle("Pause tweak injection", isOn: $model.pauseInjection)
                    .onChange(of: model.pauseInjection) { _ in model.settingsTogglesChanged() }
            } footer: {
                Text("Opener skips loading tweaks. New apps still get opener until you restart them.")
            }

            Section("Status") {
                LabeledContent("SIP") {
                    Text(sipValue)
                        .foregroundStyle(sipForeground)
                }
                Text(sipDetail)
                    .font(.caption)
                    .foregroundStyle(.secondary)

                LabeledContent("Launch daemon") {
                    HStack(spacing: 12) {
                        Text(daemonValue)
                            .foregroundStyle(daemonForeground)
                        Spacer(minLength: 8)
                        daemonAction
                    }
                }
                Text(daemonDetail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                if model.tweaks.isEmpty {
                    Text("No tweaks installed.")
                        .foregroundStyle(.secondary)
                    Text("Use Install to add a `.dylib`, or copy one into `/private/var/ammonia/core/tweaks`.")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                } else {
                    ForEach(Array(model.tweaks.enumerated()), id: \.element.id) { index, tweak in
                        TweakRowView(
                            tweak: tweak,
                            showPackage: model.devToolsAvailable,
                            onEdit: { model.beginEditingTweak(tweak.name) },
                            onToggle: { model.toggleTweak(at: index) },
                            onPackage: { model.packageTweak(named: tweak.name) }
                        )
                    }
                }
            } header: {
                HStack {
                    Text("Tweaks")
                    Spacer()
                    Button("Install…", action: model.installTweak)
                        .buttonStyle(.link)
                }
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 460, idealWidth: 520, minHeight: 420)
        .toolbar {
            if !model.statusMessage.isEmpty {
                ToolbarItem(placement: .status) {
                    Text(model.statusMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            ToolbarItem(placement: .primaryAction) {
                Button("Save") { model.saveSettings() }
                    .keyboardShortcut("s", modifiers: .command)
            }
        }
        .sheet(item: $model.tweakEditor) { item in
            TweakEditorSheet(tweakName: item.name, options: $model.editorOptions) {
                model.saveEditor()
            } onCancel: {
                model.cancelEditor()
            }
        }
    }

    @ViewBuilder
    private var daemonAction: some View {
        switch model.daemonStatus {
        case .notInstalled:
            Button("Install") { model.installDaemon() }
        case .loaded, .notLoaded:
            Button("Uninstall") { model.uninstallDaemon() }
        }
    }

    private var sipValue: String {
        switch model.sipStatus {
        case .enabled: "Enabled"
        case .disabled: "Disabled"
        case .partiallyDisabled: "Partially disabled"
        case .unknown: "Unknown"
        }
    }

    private var sipDetail: String {
        switch model.sipStatus {
        case .enabled: "Injection needs SIP disabled or partially disabled."
        case .partiallyDisabled: "Debugging restrictions are off — ready for injection."
        case .disabled: "SIP is off — injection can run."
        case .unknown: "Could not determine SIP status."
        }
    }

    private var sipForeground: Color {
        switch model.sipStatus {
        case .enabled: .red
        case .unknown: .orange
        default: .primary
        }
    }

    private var daemonValue: String {
        switch model.daemonStatus {
        case .notInstalled: "Not installed"
        case .loaded: "Loaded"
        case .notLoaded: "Not loaded"
        }
    }

    private var daemonDetail: String {
        switch model.daemonStatus {
        case .loaded: "Registered with launchd."
        case .notLoaded: "Plist present but the job is not loaded."
        case .notInstalled: "Install to register the inject daemon."
        }
    }

    private var daemonForeground: Color {
        switch model.daemonStatus {
        case .loaded: .primary
        case .notLoaded: .orange
        case .notInstalled: .red
        }
    }
}
