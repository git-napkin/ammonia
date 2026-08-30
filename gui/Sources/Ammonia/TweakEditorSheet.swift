import SwiftUI

struct TweakEditorSheet: View {
    let tweakName: String
    @Binding var options: TweakSidecarOptions
    let onSave: () -> Void
    let onCancel: () -> Void

    @State private var blacklistInput = ""
    @State private var frameworkInput = ""
    @State private var whitelistInput = ""

    var body: some View {
        NavigationStack {
            Form {
                stringListSection(
                    title: "Blacklisted applications",
                    emptyHint: "None",
                    items: $options.blacklistedApps,
                    input: $blacklistInput,
                    placeholder: "Executable name"
                )

                stringListSection(
                    title: "Framework dependencies",
                    emptyHint: "None",
                    items: $options.frameworkDependencies,
                    input: $frameworkInput,
                    placeholder: "Framework name"
                )

                stringListSection(
                    title: "Process whitelist",
                    emptyHint: "Empty loads in every process (minus blacklists).",
                    items: $options.processWhitelist,
                    input: $whitelistInput,
                    placeholder: "Executable name"
                )
            }
            .formStyle(.grouped)
            .navigationTitle(tweakName)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", action: onCancel)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save", action: onSave)
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
        .frame(minWidth: 440, minHeight: 480)
    }

    private func stringListSection(
        title: String,
        emptyHint: String,
        items: Binding<[String]>,
        input: Binding<String>,
        placeholder: String
    ) -> some View {
        Section(title) {
            if items.wrappedValue.isEmpty {
                Text(emptyHint)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(items.wrappedValue, id: \.self) { item in
                    HStack {
                        Text(item)
                            .lineLimit(1)
                        Spacer()
                        Button {
                            items.wrappedValue.removeAll { $0 == item }
                        } label: {
                            Image(systemName: "minus.circle")
                                .foregroundStyle(.secondary)
                        }
                        .buttonStyle(.borderless)
                    }
                }
            }

            HStack {
                TextField(placeholder, text: input)
                    .onSubmit { addItem(input: input, to: items) }
                Button("Add") {
                    addItem(input: input, to: items)
                }
                .disabled(input.wrappedValue.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
    }

    private func addItem(input: Binding<String>, to items: Binding<[String]>) {
        let trimmed = input.wrappedValue.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        items.wrappedValue.append(trimmed)
        input.wrappedValue = ""
    }
}
