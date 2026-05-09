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
                        // L5a round-2 P1-3 fix — register the host
                        // window with the view model so EDR
                        // headroom can be probed against the
                        // window's actual screen (vs `mainScreen`,
                        // which follows keyboard focus).  Triggers
                        // a refreshEDRAvailability via the didSet
                        // hook + subscribes to the window's own
                        // didChangeScreenNotification.
                        viewModel.hostWindow = window
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
