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
                .disabled(!viewModel.canOpenScene)

                Menu("Open Recent") {
                    if viewModel.recentFiles.isEmpty {
                        Text("No Recent Scenes")
                            .foregroundColor(.secondary)
                    } else {
                        ForEach(viewModel.recentFiles, id: \.self) { path in
                            Button((path as NSString).lastPathComponent) {
                                viewModel.openRecentScene(at: path)
                            }
                        }

                        Divider()

                        Button("Clear Recent") {
                            viewModel.clearRecentFiles()
                        }
                    }
                }
                .disabled(!viewModel.canOpenScene)
            }
        }
    }
}
