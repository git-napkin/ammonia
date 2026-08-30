import SwiftUI

@main
struct AmmoniaApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .onAppear { model.load() }
        }
        .defaultSize(width: 520, height: 540)
        .windowToolbarStyle(.unified)
    }
}
