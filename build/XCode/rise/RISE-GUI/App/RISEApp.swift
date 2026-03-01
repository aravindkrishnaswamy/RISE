import SwiftUI

@main
struct RISEApp: App {
    @StateObject private var viewModel = RenderViewModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(viewModel)
                .onAppear {
                    // Set a fixed initial window size on launch
                    if let window = NSApplication.shared.windows.first {
                        let frame = NSRect(x: 0, y: 0, width: 1024, height: 768)
                        window.setFrame(frame, display: true)
                        window.center()
                    }
                }
        }
        .defaultSize(width: 1024, height: 768)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Open Scene...") {
                    viewModel.openScene()
                }
                .keyboardShortcut("o", modifiers: .command)
            }
        }
    }
}
