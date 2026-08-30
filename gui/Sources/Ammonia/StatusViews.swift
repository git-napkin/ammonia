import AppKit
import SwiftUI

struct TweakRowView: View {
    let tweak: TweakItem
    let showPackage: Bool
    let onEdit: () -> Void
    let onToggle: () -> Void
    let onPackage: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            TweakIconView(name: tweak.name)
                .frame(width: 28, height: 28)

            VStack(alignment: .leading, spacing: 2) {
                Text(tweak.name)
                    .lineLimit(1)
                Text(tweak.isDisabled ? "Disabled" : "Enabled")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer(minLength: 8)

            Menu {
                Button("Edit…", action: onEdit)
                Button(tweak.isDisabled ? "Enable" : "Disable", action: onToggle)
                if showPackage {
                    Button("Package…", action: onPackage)
                }
            } label: {
                Image(systemName: "ellipsis.circle")
                    .imageScale(.large)
                    .foregroundStyle(.secondary)
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
        }
    }
}

struct TweakIconView: View {
    let name: String

    var body: some View {
        Group {
            if let url = TweakService.iconURL(for: name), let image = NSImage(contentsOf: url) {
                Image(nsImage: image)
                    .resizable()
                    .scaledToFit()
            } else if let url = TweakService.defaultIconURL(), let image = NSImage(contentsOf: url) {
                Image(nsImage: image)
                    .resizable()
                    .scaledToFit()
            } else {
                Image(systemName: "puzzlepiece.extension")
                    .symbolRenderingMode(.hierarchical)
                    .foregroundStyle(.secondary)
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
    }
}
