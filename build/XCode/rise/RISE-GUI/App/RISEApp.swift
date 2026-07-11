import SwiftUI

@main
struct RISEApp: App {
    @StateObject private var viewModel = RenderViewModel()

    init() {
        // Register the bundled IBM Plex faces before any view renders —
        // Theme.sans/mono fall back to system fonts if this fails.
        FontBootstrap.registerBundledFonts()
        // UI redesign: the workspace chrome (Theme.swift) is dark-only —
        // force dark appearance so AppKit-native controls (NSAlert,
        // NSSavePanel, the menu bar's own rendering) match rather than
        // following System Settings' light/dark preference.
        NSApp.appearance = NSAppearance(named: .darkAqua)
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(viewModel)
                .onAppear {
                    // Set a fixed initial window size on launch
                    if let window = NSApplication.shared.windows.first {
                        let frame = NSRect(x: 0, y: 0, width: 1440, height: 900)
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
        .defaultSize(width: 1440, height: 900)
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

                Divider()

                // UI redesign: the TopBar's Save button always targets
                // the loaded path (no Save-As affordance there — that
                // stays in the Properties panel); this menu item is the
                // keyboard-driven twin of that same button.
                Button("Save Scene") {
                    viewModel.saveScene()
                }
                .keyboardShortcut("s", modifiers: [.command, .option])
                .disabled(!viewModel.sceneEditsDirty)

                // UI redesign: rehouses the retired controls panel's
                // "Clear" button.  ⇧⌘W rather than the system-owned ⌘W
                // (window close).
                Button("Close Scene") {
                    viewModel.clearScene()
                }
                .keyboardShortcut("w", modifiers: [.command, .shift])
                .disabled(!viewModel.canCloseScene)
            }

            // UI redesign: the retired controls panel exposed Undo/Redo
            // nowhere explicit (only via the interactive viewport's own
            // key handling); the redesign surfaces them as first-class
            // Edit-menu items with the C++ controller's human-readable
            // step label ("Translate", "Agent Edit", ...).  Empty label
            // means the corresponding stack is empty — falls back to a
            // bare "Undo" / "Redo" title rather than a trailing space.
            CommandGroup(replacing: .undoRedo) {
                Button(viewModel.undoLabel.isEmpty ? "Undo" : "Undo \(viewModel.undoLabel)") {
                    viewModel.undo()
                }
                .keyboardShortcut("z", modifiers: .command)
                .disabled(viewModel.viewportBridge == nil || !viewModel.canOpenScene)

                Button(viewModel.redoLabel.isEmpty ? "Redo" : "Redo \(viewModel.redoLabel)") {
                    viewModel.redo()
                }
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .disabled(viewModel.viewportBridge == nil || !viewModel.canOpenScene)
            }

            // UI redesign (design brief A2): refinement transport +
            // production render, rehousing the retired controls panel's
            // Render / Render Animation / Cancel buttons.
            CommandMenu("Render") {
                // NOTE: the design brief's TopBar tooltip advertises a
                // bare Space shortcut ("Pause / resume refinement
                // (Space)"), but binding bare Space as an app-wide
                // SwiftUI menu shortcut would steal the spacebar from
                // every text field (chat input, scene editor, JSON-RPC
                // request box) — Space with no modifiers is not safe to
                // claim globally.  Bound to ⌃Space instead; the TopBar
                // button (no keyboard involved) still reads "(Space)"
                // in its tooltip per the design brief's literal text.
                Button(viewModel.isRefinementPaused ? "Resume Refinement" : "Pause Refinement") {
                    viewModel.togglePauseRefinement()
                }
                .keyboardShortcut(.space, modifiers: [.control])
                .disabled(viewModel.viewportBridge == nil
                          || viewModel.renderState == .rendering
                          || viewModel.renderState == .cancelling)

                Button("Restart Refinement") {
                    viewModel.restartRefinement()
                }
                .disabled(viewModel.viewportBridge == nil
                          || viewModel.renderState == .rendering
                          || viewModel.renderState == .cancelling)

                Divider()

                Button("Production Render") {
                    viewModel.startRender()
                }
                .keyboardShortcut("r", modifiers: .command)
                .disabled(!viewModel.canStartProductionRender)

                Button("Render Animation…") {
                    viewModel.startAnimationRender()
                }
                .disabled(!viewModel.canStartProductionRender || !viewModel.hasAnimation)

                Button("Cancel Render") {
                    viewModel.cancelRender()
                }
                .keyboardShortcut(".", modifiers: .command)
                .disabled(viewModel.renderState != .rendering)
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

                // L5e — Tone Curve submenu.  Driven by
                // `viewModel.viewToneCurve` (Int matching RISE's
                // DISPLAY_TRANSFORM enum: 0 None, 1 Reinhard, 2
                // ACES, 3 AgX, 4 Hable).  SwiftUI's `Picker` inside
                // a CommandGroup renders as a submenu with the
                // selected option marked.  Greyed out when EDR is
                // on — HDR display is by-construction tone-curve-
                // free; the OS compositor handles the display map.
                Menu("Tone Curve") {
                    Picker("Tone Curve", selection: $viewModel.viewToneCurve) {
                        Text("None").tag(0)
                        Text("Reinhard").tag(1)
                        Text("ACES").tag(2)
                        Text("AgX").tag(3)
                        Text("Hable").tag(4)
                    }
                    .pickerStyle(.inline)
                }
                .disabled(viewModel.edrEnabled)

                Divider()

                // UI redesign: show/hide the left panel (Agent / Scene
                // file tabs) and the bottom log drawer.  Both default on;
                // toggling doesn't persist across launches in slice 1.
                Toggle("Left Panel", isOn: $viewModel.showLeftPanel)
                Toggle("Log", isOn: $viewModel.showLogDrawer)
                    .keyboardShortcut("l", modifiers: [.option])
            }
        }
    }
}
