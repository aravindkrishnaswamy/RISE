import SwiftUI
import Foundation

/// Binds the agent's skills root to the INSTALLATION, once, at launch.
///
/// WHY THIS EXISTS.  `AgentSession::SkillsRoot()` resolves, first hit
/// wins: `$RISE_SKILLS_PATH` → `$RISE_MEDIA_PATH` + "skills/agent/" →
/// "./skills/agent/".  Tier 2 is a trap for a GUI: `RISE_MEDIA_PATH` is
/// re-pointed on EVERY scene load (`RenderViewModel.registerMediaPaths`
/// walks up from the open scene to the nearest `global.options`), so the
/// agent's skills silently became a property of whichever scene was
/// open.  Build a scene from scratch, or open one outside a RISE project
/// tree, and the walk-up found nothing, the media path pointed somewhere
/// with no `skills/agent/`, and all skills vanished — with no signal at
/// any layer.  Skills belong to the app, not to the document.
///
/// Setting tier 1 here is enough: it takes first precedence in the core,
/// so nothing in the library changes and the CLI / MCP paths keep their
/// existing resolution.  `setProjectRoot:` / `RISE_MEDIA_PATH` are left
/// alone — they are doing their own (correct) job for scene media.
enum SkillsRootBootstrap {

    /// Resolve and publish `RISE_SKILLS_PATH`.  Call once, before any
    /// scene can load (`ChatViewModel.sceneOpened` is what reads the
    /// index).  A no-op when nothing resolves: leaving the core's
    /// fallbacks intact beats pointing the variable at a directory that
    /// is not there.
    static func install() {
        // An operator-set value always wins — this is a repair for a
        // default, not an override of a deliberate choice.
        if let set = ProcessInfo.processInfo.environment["RISE_SKILLS_PATH"], !set.isEmpty {
            return
        }
        guard let root = resolve() else { return }
        setenv("RISE_SKILLS_PATH", root, 1)
    }

    /// First existing directory wins:
    ///
    ///  1. `<app>/Contents/Resources/skills/agent` — the repo's `skills/`
    ///     folder, copied in by the RISE-GUI target's Resources build
    ///     phase (a folder reference, so there is no second copy of the
    ///     markdown to drift).  This is the path in BOTH a shipped .app
    ///     and a dev build: the phase runs in every configuration, and a
    ///     dev build's .app lives in DerivedData where no amount of
    ///     walking up reaches the checkout.
    ///  2. `skills/agent` found by walking up from the .app itself — for
    ///     an .app that does sit inside a source tree (a custom SYMROOT,
    ///     or a build product copied into the repo) whose resource copy
    ///     did not run.
    private static func resolve() -> String? {
        var candidates: [URL] = []
        if let resources = Bundle.main.resourceURL {
            candidates.append(resources.appendingPathComponent("skills/agent"))
        }
        // Bounded walk: deep enough for build/XCode/rise/<config>/ inside
        // a checkout, short enough never to wander off to "/".
        var dir = Bundle.main.bundleURL.deletingLastPathComponent()
        for _ in 0 ..< 8 {
            candidates.append(dir.appendingPathComponent("skills/agent"))
            let parent = dir.deletingLastPathComponent()
            if parent.path == dir.path { break }
            dir = parent
        }
        return candidates.first(where: isDirectory)?.path
    }

    private static func isDirectory(_ url: URL) -> Bool {
        var isDir: ObjCBool = false
        return FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir)
            && isDir.boolValue
    }
}

@main
struct RISEApp: App {
    /// Menu-flyout fix (2026-07-16): the model is held through a plain
    /// (non-ObservableObject) holder in @State — NOT @StateObject — so the
    /// App body and its `.commands` are NOT subscribed to RenderViewModel's
    /// render-frequency publish traffic.  ObservableObject observation is
    /// wrapper-based: with @StateObject here, EVERY objectWillChange
    /// rebuilt the whole menu bar, and macOS dismisses an open flyout on
    /// rebuild — menus "flew out and closed immediately" whenever a scene
    /// was loaded.  ContentView still observes the model itself (via
    /// .environmentObject); the menus observe MenuBarState, which
    /// publishes only when menu-rendered state actually changes.
    @State private var app = AppModel()

    /// Mirrors ThemeState's persisted mode so the File > Theme menu
    /// checkmarks stay live (ThemeState.setMode writes this same
    /// UserDefaults key, which also drives ContentView's root .id()
    /// rebuild).
    @AppStorage(ThemeState.userDefaultsKey)
    private var themeModeRaw: String = ThemeMode.dark.rawValue

    init() {
        // Anchor the agent's skills to the INSTALLATION before anything
        // can read the index (see SkillsRootBootstrap for why binding
        // them to RISE_MEDIA_PATH, which follows the open scene, was the
        // bug).  Cheap, and it must precede the first scene load.
        SkillsRootBootstrap.install()
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

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(app.viewModel)
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
                        app.viewModel.hostWindow = window
                    }
                }
        }
        .defaultSize(width: 1440, height: 900)
        .commands {
            AppMenus(menu: app.menuBar, actions: app.viewModel)
        }
    }
}

/// The complete menu bar.  RENDERED state comes exclusively from
/// `menu.snap` (the diffed MenuBarSnapshot — see MenuBarState.swift);
/// ACTIONS call straight into the view model, which is held as a plain
/// `let` so it contributes NO observation.  Bindings bridge the two:
/// read from the snapshot, write to the model.
private struct AppMenus: Commands {
    @ObservedObject var menu: MenuBarState
    let actions: RenderViewModel

    /// Mirrors ThemeState's persisted mode for the Theme checkmarks
    /// (same key RISEApp watches).
    @AppStorage(ThemeState.userDefaultsKey)
    private var themeModeRaw: String = ThemeMode.dark.rawValue

    private var pauseMenuTitle: String {
        menu.snap.isProductionRenderPaused ? "Resume Render" : "Pause Render"
    }

    /// One Insert-menu submenu for `category`, or nothing at all when
    /// the category has no registered templates (Camera/Rasterizer/
    /// Film/Animation/SceneVariant always; every category before a
    /// scene is loaded).  The template LIST is read through `actions`
    /// at render time (a plain method call — no subscription); the
    /// snapshot's hasBridge/entityListEpoch fields are what make the
    /// menu rebuild when the set can have changed.
    @ViewBuilder
    private func insertCategoryMenu(title: String, category: RISEViewportCategory) -> some View {
        let templates = actions.entityTemplates(for: category)
        if !templates.isEmpty {
            Menu(title) {
                ForEach(templates, id: \.index) { t in
                    Button(t.label) {
                        actions.addEntity(category: category, templateIndex: t.index)
                    }
                }
            }
            .disabled(!menu.snap.canUseSceneTransport)
        }
    }

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("Open Scene...") {
                actions.openScene()
            }
            .keyboardShortcut("o", modifiers: .command)
            .disabled(!menu.snap.canOpenScene)

            Menu("Open Recent") {
                if menu.snap.recentFiles.isEmpty {
                    Text("No Recent Scenes")
                        .foregroundColor(.secondary)
                } else {
                    ForEach(menu.snap.recentFiles, id: \.self) { path in
                        Button((path as NSString).lastPathComponent) {
                            actions.openRecentScene(at: path)
                        }
                    }

                    Divider()

                    Button("Clear Recent") {
                        actions.clearRecentFiles()
                    }
                }
            }
            .disabled(!menu.snap.canOpenScene)

            Divider()

            // L5a round-9 — File > Save Rendered Image…  Disabled
            // until the user has started at least one render
            // (`canSaveImage` gates on renderState ∈ {.rendering,
            // .cancelling, .completed, .cancelled} — see
            // RenderViewModel).  Opens an NSSavePanel with HDR
            // EXR as the default, with PNG / TIFF as LDR
            // alternatives in the format dropdown.
            Button("Save Rendered Image...") {
                actions.saveRenderedImage()
            }
            .keyboardShortcut("s", modifiers: .command)
            .disabled(!menu.snap.canSaveImage)

            Divider()

            // Explicit-save-only (user decision 2026-07-12): UI edits
            // never write the .RISEscene to disk automatically — the
            // CST <-> editor live mirror keeps the Scene-file tab
            // following live edits, but a disk write happens ONLY
            // here (or via the TopBar's Save button, which calls the
            // same `saveScene()`).  Disabled while there's nothing
            // unsaved; no suspension/retry state to fold in anymore.
            Button("Save Scene") {
                actions.saveScene()
            }
            .keyboardShortcut("s", modifiers: [.command, .option])
            .disabled(!menu.snap.sceneEditsDirty)

            // UI redesign: rehouses the retired controls panel's
            // "Clear" button.  ⇧⌘W rather than the system-owned ⌘W
            // (window close).
            Button("Clear Scene") {
                actions.clearScene()
            }
            .keyboardShortcut("w", modifiers: [.command, .shift])
            .disabled(!menu.snap.canCloseScene)

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
        // P1-1 / P2 fix: single-sourced on `canUseSceneTransport`,
        // same as `undo()`/`redo()` themselves guard on.
        CommandGroup(replacing: .undoRedo) {
            Button(menu.snap.undoLabel.isEmpty ? "Undo" : "Undo \(menu.snap.undoLabel)") {
                actions.undo()
            }
            .keyboardShortcut("z", modifiers: .command)
            .disabled(!menu.snap.canUseSceneTransport)

            Button(menu.snap.redoLabel.isEmpty ? "Redo" : "Redo \(menu.snap.redoLabel)") {
                actions.redo()
            }
            .keyboardShortcut("z", modifiers: [.command, .shift])
            .disabled(!menu.snap.canUseSceneTransport)
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
            // Production-only now: refinement pause/resume was
            // removed by user request (refinement restarts on every
            // edit anyway), so this item pauses/resumes THE RENDER
            // and is enabled only while one is in flight.
            Button(pauseMenuTitle) {
                actions.toggleProductionRenderPause()
            }
            .keyboardShortcut(.space, modifiers: [.control])
            .disabled(!menu.snap.isRendering)

            Button("Restart Refinement") {
                actions.restartRefinement()
            }
            .disabled(!menu.snap.canUseSceneTransport)

            Button(menu.snap.hasActiveRegion ? "Redraw Render Region" : "Draw Render Region") {
                actions.requestRegionDraw()
            }
            .keyboardShortcut("r", modifiers: [.command, .shift])
            .disabled(!menu.snap.canDrawRegion)

            Divider()

            Button("Production Render") {
                actions.startRender()
            }
            .keyboardShortcut("r", modifiers: .command)
            .disabled(!menu.snap.canStartProductionRender)

            Button("Render Active Region") {
                actions.startActiveRegionRender()
            }
            .disabled(!menu.snap.canStartProductionRender || !menu.snap.hasActiveRegion
                      || !menu.snap.productionHonorsRegion)

            Button("Render Animation…") {
                actions.startAnimationRender()
            }
            .disabled(!menu.snap.canStartProductionRender || !menu.snap.hasAnimation)

            Button("Cancel Render") {
                actions.cancelRender()
            }
            .keyboardShortcut(".", modifiers: .command)
            .disabled(!menu.snap.isRendering)
        }

        // Entity-creation slice: "Add Entity" templates, surfaced as
        // an Insert menu alongside the outliner's per-category "+"
        // affordance and context menu (same `RenderViewModel.addEntity`
        // call). Only categories with at least one registered template
        // get a submenu — each individually disabled with the SAME
        // `canUseSceneTransport` gate the Render menu's transport items
        // use per-item. `CommandMenu` itself has no `.disabled` —
        // mirroring the Render menu means per-item disabling, not a
        // single top-level toggle.
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
            Toggle("EDR Preview", isOn: Binding(
                get: { menu.snap.edrEnabled },
                set: { actions.edrEnabled = $0 }))
                .disabled(!menu.snap.edrAvailable)

            // L5e — Tone Curve submenu.  Driven by the view model's
            // `viewToneCurve` (Int matching RISE's DISPLAY_TRANSFORM
            // enum: 0 None, 1 Reinhard, 2 ACES, 3 AgX, 4 Hable).
            // SwiftUI's `Picker` inside a CommandGroup renders as a
            // submenu with the selected option marked.  Greyed out when
            // EDR is on — HDR display is by-construction tone-curve-
            // free; the OS compositor handles the display map.
            Menu("Tone Curve") {
                Picker("Tone Curve", selection: Binding(
                    get: { menu.snap.viewToneCurve },
                    set: { actions.viewToneCurve = $0 })) {
                    Text("None").tag(0)
                    Text("Reinhard").tag(1)
                    Text("ACES").tag(2)
                    Text("AgX").tag(3)
                    Text("Hable").tag(4)
                }
                .pickerStyle(.inline)
            }
            .disabled(menu.snap.edrEnabled)

            // P1 render modes (docs/gui/RENDER_MODES.md §5): the same
            // registry list + setter the viewport's own dropdown chip
            // uses (ContentView's `viewportRenderModeChip`) — kept as a
            // SEPARATE menu from "Tone Curve" just above on purpose (§4
            // ratified decision 1: render modes and view transforms are
            // different concepts and must never be presented as one
            // control surface). Disabled with no bridge / no modes
            // loaded yet / while the scene isn't editable, same gate as
            // the Insert-menu submenus above.
            Menu("Viewport Rendering") {
                ForEach(menu.snap.viewportRenderModes) { mode in
                    Button {
                        actions.setViewportRenderMode(mode.name)
                    } label: {
                        if mode.name == menu.snap.viewportRenderMode {
                            Label(mode.title, systemImage: "checkmark")
                        } else {
                            Text(mode.title)
                        }
                    }
                }
                // X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): an
                // orthogonal boolean, not a fifth mode — applies to EVERY
                // mode including the shaded preview (default ON; user
                // decision 2026-07-17).  Same toggle the viewport chip
                // offers (ContentView's `viewportRenderModeChip`); menus
                // read only `menu.snap`.
                Divider()
                Toggle("See Through Glass", isOn: Binding(
                    get: { menu.snap.viewportXray },
                    set: { actions.setViewportXray($0) }))
            }
            .disabled(!menu.snap.canUseSceneTransport || menu.snap.viewportRenderModes.isEmpty)

            Divider()

            // UI redesign: show/hide the left panel (Agent / Scene
            // file tabs) and the bottom log drawer.  Both default on;
            // toggling doesn't persist across launches in slice 1.
            Toggle("Left Panel", isOn: Binding(
                get: { menu.snap.showLeftPanel },
                set: { actions.showLeftPanel = $0 }))
            Toggle("Log", isOn: Binding(
                get: { menu.snap.showLogDrawer },
                set: { actions.showLogDrawer = $0 }))
                .keyboardShortcut("l", modifiers: [.option])
        }
    }
}
