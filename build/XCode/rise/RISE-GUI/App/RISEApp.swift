import SwiftUI

@main
struct RISEApp: App {
    @StateObject private var viewModel = RenderViewModel()

    /// Mirrors ThemeState's persisted mode so the File > Theme menu
    /// checkmarks stay live (ThemeState.setMode writes this same
    /// UserDefaults key, which also drives ContentView's root .id()
    /// rebuild).
    @AppStorage(ThemeState.userDefaultsKey)
    private var themeModeRaw: String = ThemeMode.dark.rawValue

    init() {
        // Register the bundled IBM Plex faces before any view renders —
        // Theme.sans/mono fall back to system fonts if this fails.
        FontBootstrap.registerBundledFonts()
        // Theme: load the persisted light/dark choice, then pin the
        // AppKit appearance to match so native controls (NSAlert,
        // NSSavePanel, menu rendering) follow the workspace theme
        // rather than System Settings.
        //
        // MUST go through NSApplication.shared, NOT the NSApp global:
        // SwiftUI runs App.init() BEFORE it creates the shared
        // application object, so the implicitly-unwrapped NSApp is
        // still nil here — unwrapping it crashed at launch
        // (EXC_BREAKPOINT in RISEApp.init, field crash 2026-07-11).
        // Accessing .shared instantiates the application first.
        ThemeState.loadPersisted()
        NSApplication.shared.appearance = NSAppearance(
            named: ThemeState.mode == .dark ? .darkAqua : .aqua)
    }

    private var pauseMenuTitle: String {
        viewModel.isProductionRenderPaused ? "Resume Render" : "Pause Render"
    }

    /// One Insert-menu submenu for `category`, or nothing at all when
    /// the category has no registered templates (Camera/Rasterizer/
    /// Film/Animation/SceneVariant always; every category before a
    /// scene is loaded).  `CommandMenu`'s content closure is a plain
    /// `@ViewBuilder` (Button/Menu/Toggle/Divider are Views, adapted to
    /// menu items by the Commands machinery) — NOT `@CommandsBuilder`,
    /// which is only the type `.commands { }`'s own top-level closure
    /// uses to compose CommandGroup/CommandMenu themselves.
    @ViewBuilder
    private func insertCategoryMenu(title: String, category: RISEViewportCategory) -> some View {
        let templates = viewModel.entityTemplates(for: category)
        if !templates.isEmpty {
            Menu(title) {
                ForEach(templates, id: \.index) { t in
                    Button(t.label) {
                        viewModel.addEntity(category: category, templateIndex: t.index)
                    }
                }
            }
            .disabled(!viewModel.canUseSceneTransport)
        }
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(viewModel)
                .onAppear {
                    // Set a fixed initial window size on launch
                    if let window = NSApplication.shared.windows.first {
                        // Item 2: launch maximized — fill the screen's
                        // visible frame (menu bar + Dock respected), not
                        // macOS fullscreen.  Falls back to 1440x900 when
                        // no screen is reported (headless edge case).
                        let frame = window.screen?.visibleFrame
                            ?? NSScreen.main?.visibleFrame
                            ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
                        window.setFrame(frame, display: true)
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

                // Explicit-save-only (user decision 2026-07-12): UI edits
                // never write the .RISEscene to disk automatically — the
                // CST <-> editor live mirror keeps the Scene-file tab
                // following live edits, but a disk write happens ONLY
                // here (or via the TopBar's Save button, which calls the
                // same `saveScene()`).  Disabled while there's nothing
                // unsaved; no suspension/retry state to fold in anymore.
                Button("Save Scene") {
                    viewModel.saveScene()
                }
                .keyboardShortcut("s", modifiers: [.command, .option])
                .disabled(!viewModel.sceneEditsDirty)

                // UI redesign: rehouses the retired controls panel's
                // "Clear" button.  ⇧⌘W rather than the system-owned ⌘W
                // (window close).
                Button("Clear Scene") {
                    viewModel.clearScene()
                }
                .keyboardShortcut("w", modifiers: [.command, .shift])
                .disabled(!viewModel.canCloseScene)

                Divider()

                // UI refinement item 3: light/dark theme switch (user
                // asked for it in the File menu).  ThemeState.setMode
                // persists the choice, flips the AppKit appearance, and
                // (via the shared UserDefaults key ContentView's root
                // .id() watches) rebuilds the whole view tree with the
                // other palette.
                Menu("Theme") {
                    Toggle("Dark", isOn: Binding(
                        get: { themeModeRaw == ThemeMode.dark.rawValue },
                        set: { if $0 { ThemeState.setMode(.dark) } }))
                    Toggle("Light", isOn: Binding(
                        get: { themeModeRaw == ThemeMode.light.rawValue },
                        set: { if $0 { ThemeState.setMode(.light) } }))
                }
            }

            // UI redesign: the retired controls panel exposed Undo/Redo
            // nowhere explicit (only via the interactive viewport's own
            // key handling); the redesign surfaces them as first-class
            // Edit-menu items with the C++ controller's human-readable
            // step label ("Translate", "Agent Edit", ...).  Empty label
            // means the corresponding stack is empty — falls back to a
            // bare "Undo" / "Redo" title rather than a trailing space.
            //
            // P1-1 / P2 fix: this menu used to gate on
            // `viewportBridge == nil || !canOpenScene` — a THIRD,
            // independently hand-copied version of the "is a production
            // render occupying the controller" case list (the other two
            // being `isProductionRenderRunning` and TopBar's old
            // `refinementControlsDisabled`), and one that also missed
            // the chat-driven-agent-render case those two now share via
            // `canUseSceneTransport`.  Single-sourced on
            // `canUseSceneTransport` (which already folds in the
            // bridge-nil check), same as `undo()`/`redo()` themselves
            // guard on.
            CommandGroup(replacing: .undoRedo) {
                Button(viewModel.undoLabel.isEmpty ? "Undo" : "Undo \(viewModel.undoLabel)") {
                    viewModel.undo()
                }
                .keyboardShortcut("z", modifiers: .command)
                .disabled(!viewModel.canUseSceneTransport)

                Button(viewModel.redoLabel.isEmpty ? "Redo" : "Redo \(viewModel.redoLabel)") {
                    viewModel.redo()
                }
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .disabled(!viewModel.canUseSceneTransport)
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
                // Also note: macOS's default Input Source switcher
                // shortcut is bound to ⌃Space out of the box on many
                // systems, so it may intercept this keystroke before it
                // reaches the app — the menu item itself remains fully
                // clickable regardless.
                //
                // P1-1 fix: disabled predicate is now `canUseSceneTransport`
                // Production-only now: refinement pause/resume was
                // removed by user request (refinement restarts on every
                // edit anyway), so this item pauses/resumes THE RENDER
                // and is enabled only while one is in flight.
                Button(pauseMenuTitle) {
                    viewModel.toggleProductionRenderPause()
                }
                .keyboardShortcut(.space, modifiers: [.control])
                .disabled(viewModel.renderState != .rendering)

                Button("Restart Refinement") {
                    viewModel.restartRefinement()
                }
                .disabled(!viewModel.canUseSceneTransport)

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

            // Entity-creation slice: "Add Entity" templates, surfaced as
            // an Insert menu alongside the outliner's per-category "+"
            // affordance and context menu (same `RenderViewModel.addEntity`
            // call). Only categories with at least one registered template
            // get a submenu (`entityTemplates(for:)` returns `[]` for
            // Camera/Rasterizer/Film/Animation/SceneVariant, and for every
            // category before a scene is loaded) — each submenu is
            // individually disabled with the SAME `canUseSceneTransport`
            // gate the Render menu's transport items use per-item (folds
            // in both "no scene loaded" via `viewportBridge == nil` and
            // "a render/agent edit currently owns the scene"). `CommandMenu`
            // itself has no `.disabled` — mirroring the Render menu means
            // per-item disabling, not a single top-level toggle.
            CommandMenu("Insert") {
                insertCategoryMenu(title: "Object", category: .object)
                insertCategoryMenu(title: "Light", category: .light)
                insertCategoryMenu(title: "Material", category: .material)
                insertCategoryMenu(title: "Painter", category: .painter)
                insertCategoryMenu(title: "Medium", category: .medium)
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
