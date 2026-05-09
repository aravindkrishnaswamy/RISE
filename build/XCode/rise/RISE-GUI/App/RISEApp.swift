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

                Divider()

                // L5a round-9 — File > Save Rendered Image…  Disabled
                // until the user has started at least one render
                // (`canSaveImage` gates on renderState ∈ {.rendering,
                // .cancelling, .completed, .cancelled} — see
                // RenderViewModel).  Opens an NSSavePanel with HDR
                // EXR as the default, with PNG / TIFF as LDR
                // alternatives in the format dropdown.
                Button("Save Rendered Image...") {
                    viewModel.saveRenderedImage()
                }
                .keyboardShortcut("s", modifiers: .command)
                .disabled(!viewModel.canSaveImage)
            }

            // L5a round-9 — EDR Preview menu item, parity with the
            // Windows port's "View > HDR Preview".  EDR is a display
            // option (not a per-render parameter), so it belongs in
            // the menu bar rather than the rendering controls panel.
            //
            // CommandGroup(after: .toolbar) APPENDS to the existing
            // system "View" menu (which contains Enter Full Screen).
            // The wrong choice here — `CommandMenu("View")` — creates
            // a SECOND View menu with our items split off; SwiftUI's
            // menu-merging is by-placement, not by-name.  The
            // `.toolbar` placement exists on every macOS app and is
            // the conventional anchor for app-supplied View items.
            //
            // SwiftUI renders a bound `Toggle` inside a CommandGroup
            // as a checkable menu item with a leading checkmark; the
            // disabled state mirrors `edrAvailable` so the item greys
            // out when the active screen lacks EDR headroom (e.g.
            // window dragged to an external SDR monitor).
            CommandGroup(after: .toolbar) {
                Toggle("EDR Preview", isOn: $viewModel.edrEnabled)
                    .disabled(!viewModel.edrAvailable)
            }
        }
    }
}
