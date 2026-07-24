//////////////////////////////////////////////////////////////////////
//
//  MultiPaneViewport.swift - N-up multi-viewport SHELL for the macOS
//    app (docs/gui/RENDER_MODES.md §7).  One window, up to four
//    simultaneous views of the same scene, multiplexed by ONE C++
//    interactive render thread (the core; see SceneEditController's
//    pane scheduler).  This file is purely the SwiftUI/AppKit shell:
//    layout picker, per-pane display + input routing, per-pane chrome
//    (mode dropdown + vantage menu + primary marker), and confining
//    the gizmo/nav overlays + refinement pill to the PRIMARY pane.
//
//  Single layout (docs §7.0/§7.5 "Single layout must behave EXACTLY
//  as today") is deliberately NOT handled by this file at all —
//  ContentView keeps rendering the existing `ViewportView` byte-for-
//  byte unchanged when `viewportLayout == .single`.  Everything here
//  only engages for TwoH / OnePlusTwo / Quad.
//
//  Pane 0's DISPLAY reuses the SAME image stream `ViewportView`
//  already shows (`RenderViewModel.renderedImage`, wired via the
//  legacy un-indexed `-setImageBlock:`) — see
//  RISEViewportBridge.mm's "N-up pane-0 sink" doc comment for why pane
//  0 was NOT migrated onto the new per-pane sink surface.  Panes 1-3
//  use `RenderViewModel.panePreviewImages[pane]`, wired via the new
//  `-setPaneImageBlock:forPane:` at scene load (RenderViewModel.swift).
//
//  Coordinate note: pointer events for every pane are converted to
//  full-resolution film-pixel space using the SAME
//  `bridge.cameraSurfaceDimensions` accessor `ViewportView` uses for
//  pane 0 — RISE's Film pixel dimensions are a scene-level singleton
//  (docs/SCENE_CONVENTIONS.md: cameras are imaging-only, the `film`
//  chunk owns dims), shared by every pane regardless of which
//  camera/vantage that pane is showing, so one accessor is correct for
//  all four cells.  There is no per-pane camera-dims C-ABI accessor —
//  see the file-level "C-ABI friction" note at the bottom.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import AppKit

// MARK: - Layout model

/// Swift mirror of `RISEViewportLayout`.  The four fixed layouts
/// (§7.2 decision 3: no adjustable splitters) plus their visible pane
/// index lists — a static fact about the ratified layout set, not a
/// live controller query (no C-ABI entry exists for "pane count for
/// layout"; see the file-level friction note).
enum ViewportLayoutOption: Int, CaseIterable, Identifiable {
    case single     = 0
    case twoH       = 1
    case onePlusTwo = 2
    case quad       = 3

    var id: Int { rawValue }

    var bridgeValue: RISEViewportLayout {
        RISEViewportLayout(rawValue: rawValue) ?? .single
    }

    static func from(_ v: RISEViewportLayout) -> ViewportLayoutOption {
        ViewportLayoutOption(rawValue: v.rawValue) ?? .single
    }

    /// Visible pane indices, primary-first-in-spirit ordering (matches
    /// §7.2's pane numbering: 0 is always the "big"/first pane).
    var paneIndices: [Int] {
        switch self {
        case .single:     return [0]
        case .twoH:       return [0, 1]
        case .onePlusTwo: return [0, 1, 2]
        case .quad:       return [0, 1, 2, 3]
        }
    }

    var label: String {
        switch self {
        case .single:     return "Single"
        case .twoH:       return "Two Up"
        case .onePlusTwo: return "One + Two"
        case .quad:       return "Quad"
        }
    }
}

/// Fixed-fraction pane-rect math (§7.2: "Pane rects are fixed
/// fractions computed GUI-side; the C++ core only needs per-pane pixel
/// dims").  `gap` is the thin visual seam between cells, in points.
enum NUpLayout {
    static func rects(for layout: ViewportLayoutOption, in size: CGSize, gap: CGFloat = 2) -> [Int: CGRect] {
        guard size.width > 0, size.height > 0 else { return [:] }
        switch layout {
        case .single:
            return [0: CGRect(origin: .zero, size: size)]

        case .twoH:
            let halfW = (size.width - gap) / 2
            return [
                0: CGRect(x: 0, y: 0, width: halfW, height: size.height),
                1: CGRect(x: halfW + gap, y: 0, width: size.width - gap - halfW, height: size.height),
            ]

        case .onePlusTwo:
            let leftW = (size.width - gap) * (2.0 / 3.0)
            let rightX = leftW + gap
            let rightW = size.width - rightX
            let halfH = (size.height - gap) / 2
            return [
                0: CGRect(x: 0, y: 0, width: leftW, height: size.height),
                1: CGRect(x: rightX, y: 0, width: rightW, height: halfH),
                2: CGRect(x: rightX, y: halfH + gap, width: rightW, height: size.height - gap - halfH),
            ]

        case .quad:
            let halfW = (size.width - gap) / 2
            let halfH = (size.height - gap) / 2
            return [
                0: CGRect(x: 0, y: 0, width: halfW, height: halfH),
                1: CGRect(x: halfW + gap, y: 0, width: size.width - gap - halfW, height: halfH),
                2: CGRect(x: 0, y: halfH + gap, width: halfW, height: size.height - gap - halfH),
                3: CGRect(x: halfW + gap, y: halfH + gap, width: size.width - gap - halfW, height: size.height - gap - halfH),
            ]
        }
    }
}

// MARK: - Layout picker (viewport toolbar chip)

/// Four fixed layout icons in the viewport toolbar (§7.5).  Drawn with
/// plain SwiftUI shapes rather than SF Symbols — there is no stock
/// symbol for the asymmetric "one + two" split, and the toolbar's own
/// history (ViewportToolbar.swift's file header) already flags SF
/// Symbol catalog drift as a real past bug source, so a small
/// hand-drawn glyph avoids that risk entirely for all four icons
/// uniformly.
struct ViewportLayoutPicker: View {
    let bridge: RISEViewportBridge
    @Binding var layout: ViewportLayoutOption

    var body: some View {
        HStack(spacing: 2) {
            ForEach(ViewportLayoutOption.allCases) { option in
                Button {
                    // Layout admission is fallible: a coordinated render
                    // can start after the toolbar's enabled-state snapshot.
                    // Publish the SwiftUI mirror only after the controller
                    // accepts the mutation, otherwise ContentView would
                    // construct panes the core never made visible and its
                    // onChange handler would clear region state spuriously.
                    if bridge.setViewportLayout(option.bridgeValue) {
                        layout = option
                    }
                } label: {
                    LayoutGlyph(option: option, isSelected: layout == option)
                        .frame(width: 24, height: 18)
                        .padding(4)
                        .background(
                            layout == option ? Theme.fillActive : Color.clear,
                            in: RoundedRectangle(cornerRadius: Theme.radiusSmall)
                        )
                }
                .buttonStyle(.plain)
                .help(option.label)
            }
        }
        .padding(2)
        .background(Theme.bgPanel, in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMedium)
                .stroke(Theme.borderHairline, lineWidth: 1)
        )
    }
}

private struct LayoutGlyph: View {
    let option: ViewportLayoutOption
    let isSelected: Bool

    var body: some View {
        GeometryReader { geo in
            let cells = NUpLayout.rects(for: option, in: geo.size, gap: 1.5)
            ZStack(alignment: .topLeading) {
                RoundedRectangle(cornerRadius: 1.5)
                    .stroke(isSelected ? Theme.accent : Theme.textTertiary, lineWidth: 1)
                ForEach(option.paneIndices, id: \.self) { i in
                    if let r = cells[i] {
                        Rectangle()
                            .fill(isSelected ? Theme.accent.opacity(0.55) : Theme.textTertiary.opacity(0.35))
                            .frame(width: max(0, r.width - 1), height: max(0, r.height - 1))
                            .position(x: r.midX, y: r.midY)
                    }
                }
            }
        }
    }
}

// MARK: - Multi-pane container

/// The N-up shell.  Engages only when `layout != .single` — the
/// caller (ContentView) keeps using the existing `ViewportView` for
/// Single.  Owns per-pane chrome + primary-pane bookkeeping; the
/// underlying render/refinement state all lives in the C++ controller
/// (§7.3's scheduler) — this view is a thin display + input router.
struct MultiPaneViewportView: View {
    @EnvironmentObject var viewModel: RenderViewModel

    let bridge: RISEViewportBridge
    let layout: ViewportLayoutOption
    /// Same meaning as `ViewportView`'s own property of the same name.
    var interactionEnabled: Bool = true
    var isProductionRendering: Bool = false
    /// The single, controller-global current tool (SetTool/CurrentTool
    /// has no pane index — one tool selection drives every pane, same
    /// as the cursor in the single-viewport case).
    let selectedTool: ViewportTool
    /// user-review P2#4: a pane pick can change the selection, but N-up
    /// never drove the shared Inspector/Outliner refresh the single
    /// viewport does.  ContentView wires this to `propertyRefresh += 1`
    /// (the same counter OutlinerView + the inspector observe), so a pick
    /// in any pane re-snapshots both panels -- matching ViewportView.
    var onSelectionMayHaveChanged: () -> Void = {}

    @State private var primaryPane: Int = 0
    @State private var gizmoRefreshTrigger: Int = 0
    /// Bumped after any per-pane mode/vantage change so the chrome
    /// strips re-read their pane's current mode/vantage from the
    /// bridge (they don't otherwise observe controller state).
    @State private var chromeRefresh: Int = 0
    /// Last surface pixel size reported to the core per pane — guards
    /// against re-issuing an identical `_SetPaneSurfaceDims` call on
    /// every SwiftUI body re-evaluation (the C-ABI already
    /// short-circuits identical dims server-side, but avoiding the
    /// round trip entirely is cheap and keeps this view's intent
    /// obvious).
    @State private var lastSurfacePixelSize: [Int: CGSize] = [:]
    /// Most recently measured desired size, retained separately from the
    /// accepted-size cache. PaneCanvasNSView suppresses duplicate geometry
    /// callbacks, so a controller refusal must be retried explicitly when
    /// interaction/render admission becomes available again.
    @State private var desiredSurfacePixelSize: [Int: CGSize] = [:]
    // user-review P2#2: the "already presetted" guard lives on the view MODEL
    // (viewModel.panePresetApplied), NOT here as @State -- @State resets when
    // this view is torn down on a Single detour (Quad→Single→Quad), which
    // re-clobbered an explicit "preview" choice.  Bridge/scene-scoped there.

    var body: some View {
        GeometryReader { geo in
            let rects = NUpLayout.rects(for: layout, in: geo.size)
            ZStack(alignment: .topLeading) {
                ForEach(layout.paneIndices, id: \.self) { pane in
                    if let rect = rects[pane] {
                        paneCell(pane: pane, rect: rect)
                            .frame(width: max(0, rect.width), height: max(0, rect.height))
                            .position(x: rect.midX, y: rect.midY)
                    }
                }
            }
        }
        .task(id: ObjectIdentifier(bridge)) {
            primaryPane = Int(bridge.primaryPane)
            applyMultiViewModePreset()
            // BUGFIX (2026-07-21): sync the CURRENT tool to the core on
            // appear.  ViewportView does this (its .task(id:) + its
            // onChange(of: selectedTool)); MultiPaneViewport did NOT, so
            // entering multi-view -- or changing the tool while in it --
            // left the core on whatever tool was last set from single
            // view.  Selecting Orbit (or any tool) in a multi-view then
            // did NOTHING: the toolbar highlight moved but pointer events
            // still ran the stale tool in the core (verified live: an
            // Orbit drag logged tool=1 TranslateObject).  Mirror
            // ViewportView's contract exactly.
            bridge.currentTool = selectedTool.bridgeValue
        }
        .onChange(of: selectedTool) { _, newValue in
            // The load-bearing half of the fix above: forward EVERY tool
            // change to the core while multi-view is on screen.  Without
            // this, the single global tool the whole app shares (SetTool
            // has no pane index) only tracked in single view.
            bridge.currentTool = newValue.bridgeValue
            gizmoRefreshTrigger &+= 1
        }
        .onChange(of: layout) { _, _ in
            // A layout switch can hide/reveal panes and change which
            // pane is primary if the previous primary just got hidden
            // (the core falls back to pane 0 per §7.2); re-read rather
            // than assume.
            primaryPane = Int(bridge.primaryPane)
            applyMultiViewModePreset()
        }
        .onChange(of: isProductionRendering) { _, rendering in
            // user-review P2-2: a preset that was refused because a render owned
            // the scene left its pane unmarked; retry now that the render is
            // done so those panes finally get the spread (self-healing, no user
            // action needed).
            if !rendering { applyMultiViewModePreset() }
        }
        .onChange(of: interactionEnabled) { _, enabled in
            if enabled { retryPendingSurfaceSizes() }
        }
    }

    /// Multi-view mode PRESET (user request 2026-07-21).  Entering a
    /// multi-pane layout should reveal a SPREAD of complementary outputs,
    /// not four copies of the beauty preview.  Per-slot roles are stable
    /// across layouts (a pane index means the same thing in TwoH and Quad):
    ///   slot 0 = beauty preview (the editing surface -- left untouched)
    ///   slot 1 = wireframe  (topology)
    ///   slot 2 = normals    (shading)
    ///   slot 3 = depth      (Z range)
    /// -- the classic modeling multi-view.  Applied ONLY to a visible
    /// secondary pane STILL showing "preview", so a pane the user has
    /// already switched keeps its choice across layout toggles.  (A future
    /// "reset" affordance or alternate presets -- e.g. a lighting spread of
    /// clay_lights / direct / indirect -- can layer on this; decision 2
    /// keeps the built-in set fixed for now.)
    private static let presetModeBySlot = ["preview", "wireframe", "normals", "depth"]

    private func applyMultiViewModePreset() {
        var changed = false
        for pane in layout.paneIndices where pane != 0 && pane < Self.presetModeBySlot.count {
            // user-review P2#2: apply the slot preset EXACTLY ONCE per pane
            // -- the first time it becomes visible.  After that the pane's
            // mode is the user's to own (including an explicit "preview",
            // which the old current=="preview" gate wrongly re-clobbered on
            // every layout change).
            guard !viewModel.panePresetApplied.contains(pane) else { continue }
            // A pane the core already persisted to a non-Preview mode is the
            // user's -- mark it applied and leave it alone.
            if bridge.paneRenderMode(UInt(pane)) != "preview" {
                viewModel.panePresetApplied.insert(pane)
                continue
            }
            // user-review P2-2: mark applied ONLY when the setter SUCCEEDS.
            // setPaneRenderMode is refused while a production/agent render owns
            // the scene; marking before checking left the pane stuck at Preview
            // forever (no retry).  On refusal, stay unmarked so the render-end
            // retry below (or the next layout change) re-applies.
            if bridge.setPaneRenderMode(UInt(pane), name: Self.presetModeBySlot[pane]) {
                viewModel.panePresetApplied.insert(pane)
                changed = true
            }
        }
        if changed { chromeRefresh &+= 1 }
    }

    private func applySurfaceSize(pane: Int, size: CGSize) {
        guard lastSurfacePixelSize[pane] != size else { return }
        // Cache only an accepted size. A render can acquire admission after
        // PaneCanvas measured the view; keeping the desired value separate
        // lets interactionEnabled's false->true transition retry even though
        // PaneCanvasNSView suppresses duplicate measurements.
        if bridge.setPaneSurfaceDims(
            UInt(pane),
            width: UInt(max(1, size.width)),
            height: UInt(max(1, size.height))) {
            lastSurfacePixelSize[pane] = size
        }
    }

    private func retryPendingSurfaceSizes() {
        for pane in layout.paneIndices {
            guard let size = desiredSurfacePixelSize[pane] else { continue }
            applySurfaceSize(pane: pane, size: size)
        }
    }

    // MARK: Pane cell

    @ViewBuilder
    private func paneCell(pane: Int, rect: CGRect) -> some View {
        let isPrimary = pane == primaryPane
        let image: NSImage? = pane == 0 ? viewModel.renderedImage : viewModel.panePreviewImages[pane]

        ZStack(alignment: .top) {
            PaneCanvas(
                image: image,
                cursor: interactionEnabled ? selectedTool.nsCursor : .arrow,
                surfaceDimensionsProvider: { [weak bridge] in
                    bridge?.cameraSurfaceDimensions ?? .zero
                },
                onPointerDown: { p in
                    guard interactionEnabled else { return false }
                    let accepted = bridge.onPanePointerDown(UInt(pane), x: Double(p.x), y: Double(p.y))
                    // §7.8 decision 1 / the core's own promotion rule:
                    // re-read rather than assume — a navigation-tool
                    // Down never promotes, so the primary badge must
                    // only move when the core actually moved it.
                    let newPrimary = Int(bridge.primaryPane)
                    if newPrimary != primaryPane { primaryPane = newPrimary }
                    gizmoRefreshTrigger &+= 1
                    // user-review P2#4: a Select-tool Down may have just
                    // picked an object; motion Downs don't change selection
                    // but the panel refresh is cheap, so always notify
                    // (mirrors ViewportView's own pointer-down contract).
                    onSelectionMayHaveChanged()
                    return accepted
                },
                onPointerMove: { p in
                    guard interactionEnabled else { return }
                    bridge.onPanePointerMove(UInt(pane), x: Double(p.x), y: Double(p.y))
                    if bridge.gizmoDragActive { gizmoRefreshTrigger &+= 1 }
                },
                onPointerUp: { p in
                    guard interactionEnabled else { return }
                    bridge.onPanePointerUp(UInt(pane), x: Double(p.x), y: Double(p.y))
                    gizmoRefreshTrigger &+= 1
                    onSelectionMayHaveChanged()   // user-review P2#4
                },
                onSurfacePixelSizeChanged: { pixelSize in
                    guard pixelSize.width > 0, pixelSize.height > 0 else { return }
                    // Render the pane at the FILM's aspect ratio, letterboxed
                    // within the cell -- NOT the raw cell aspect.  The gizmo
                    // overlay and the pointer->film-pixel conversion BOTH map
                    // through cameraSurfaceDimensions (the authored film dims)
                    // and letterbox-fit into the cell (ViewportLetterbox.fit /
                    // currentImageDrawRect).  If a pane rendered at its cell's
                    // OWN aspect, its content would not line up with the drawn
                    // gizmo or with where a click maps -- the "gizmos do
                    // nothing in a pane" bug (2026-07-21).  Constraining to the
                    // film aspect makes every pane behave EXACTLY like the
                    // single viewport (which renders at film dims), so the
                    // shared overlay + pointer math is correct with no change.
                    let film = bridge.cameraSurfaceDimensions
                    let paneSize: CGSize
                    if film.width > 0, film.height > 0 {
                        let filmAspect = film.width / film.height
                        let cellAspect = pixelSize.width / pixelSize.height
                        if cellAspect > filmAspect {
                            let h = pixelSize.height
                            paneSize = CGSize(width: (h * filmAspect).rounded(), height: h)
                        } else {
                            let w = pixelSize.width
                            paneSize = CGSize(width: w, height: (w / filmAspect).rounded())
                        }
                    } else {
                        paneSize = pixelSize
                    }
                    desiredSurfacePixelSize[pane] = paneSize
                    applySurfaceSize(pane: pane, size: paneSize)
                }
            )

            if isPrimary && selectedTool.category == .objectTransform && !isProductionRendering {
                ViewportGizmoOverlay(
                    bridge: bridge,
                    refreshTrigger: gizmoRefreshTrigger,
                    surfaceDimensionsProvider: { [weak bridge] in
                        bridge?.cameraSurfaceDimensions ?? .zero
                    }
                )
                .allowsHitTesting(false)
            }

            PaneChromeStrip(
                bridge: bridge,
                pane: pane,
                isPrimary: isPrimary,
                modes: viewModel.viewportRenderModes,
                refreshToken: chromeRefresh,
                onChanged: { chromeRefresh &+= 1 }
            )
            .disabled(!interactionEnabled)
        }
        .overlay(alignment: .topTrailing) {
            if isPrimary && interactionEnabled && !isProductionRendering {
                // user-review P1-1: this overlay is on the PRIMARY pane cell;
                // target that pane so nav actions move it, not pane 0.
                ViewportNavOverlay(bridge: bridge, pane: UInt(pane),
                                   refreshTrigger: gizmoRefreshTrigger,
                                   sceneEditable: interactionEnabled)
                    .padding(8)
            }
        }
        .overlay(alignment: .bottomLeading) {
            if isPrimary {
                PaneRefinementPill(bridge: bridge, pane: pane, modes: viewModel.viewportRenderModes)
                    .padding(8)
            }
        }
        .overlay(
            Rectangle().stroke(Theme.borderHairline, lineWidth: 1)
        )
        .clipped()
    }
}

// MARK: - Pane chrome strip (mode dropdown + vantage menu + primary marker)

/// Minimal per-pane overlay strip (§7.5).  The mode dropdown reads the
/// SAME registry data (`modes`, sourced from
/// `RISEViewportBridge.viewportRenderModes()`) the single-viewport
/// mode chip (ContentView's `viewportRenderModeChip`) uses, and calls
/// the pane-indexed setter — a compact sibling rendering rather than a
/// literal reuse of that chip's SwiftUI type, which is private to
/// ContentView and sized for the full toolbar row, not a small pane
/// strip; see this task's report for the explicit call-out.  X-ray is
/// deliberately NOT offered here — §7.2 keeps it a single GLOBAL
/// toggle in v1, unchanged by N-up.
private struct PaneChromeStrip: View {
    let bridge: RISEViewportBridge
    let pane: Int
    let isPrimary: Bool
    let modes: [ViewportRenderModeInfo]
    /// Bumped by the parent after any change so this strip re-reads
    /// its pane's mode/vantage — the strip itself has no other signal
    /// that pane state changed (no push notifications from the core).
    let refreshToken: Int
    let onChanged: () -> Void

    @State private var modeName: String = "preview"
    @State private var vantageKind: RISEViewportVantageKind = .sceneCamera
    @State private var vantageNamedView: String = ""

    var body: some View {
        HStack(spacing: 5) {
            if isPrimary {
                Image(systemName: "star.fill")
                    .font(.system(size: 7))
                    .foregroundColor(Theme.accent)
                    .help("Primary pane")
            }
            modeMenu
            vantageMenu
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .background(Theme.bgBase.opacity(0.72))
        .onAppear { refresh() }
        .onChange(of: refreshToken) { _, _ in refresh() }
    }

    private func refresh() {
        modeName = bridge.paneRenderMode(UInt(pane))
        var kind: RISEViewportVantageKind = .sceneCamera
        var namedView: NSString? = nil
        if bridge.getPaneVantage(UInt(pane), kind: &kind, namedView: &namedView) {
            vantageKind = kind
            vantageNamedView = (namedView as String?) ?? ""
        }
    }

    private var activeModeTitle: String {
        modes.first { $0.name == modeName }?.title ?? modeName
    }

    private var modeMenu: some View {
        Menu {
            ForEach(modes) { mode in
                Button {
                    _ = bridge.setPaneRenderMode(UInt(pane), name: mode.name)
                    onChanged()
                } label: {
                    if mode.name == modeName {
                        Label(mode.title, systemImage: "checkmark")
                    } else {
                        Text(mode.title)
                    }
                }
                .help(mode.question)
            }
        } label: {
            Text(activeModeTitle)
                .font(Theme.mono(9.5))
                .lineLimit(1)
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .help("Pane render mode")
    }

    private var vantageLabel: String {
        switch vantageKind {
        case .sceneCamera: return "Scene Camera"
        case .freeFly:     return "Free-Fly"
        case .namedView:   return vantageNamedView.isEmpty ? "Named View" : vantageNamedView
        @unknown default:  return "—"
        }
    }

    private var vantageMenu: some View {
        Menu {
            Button {
                _ = bridge.setPaneVantageSceneCamera(UInt(pane))
                onChanged()
            } label: {
                if vantageKind == .sceneCamera {
                    Label("Scene Camera", systemImage: "checkmark")
                } else {
                    Text("Scene Camera")
                }
            }
            let names = bridge.namedViewNames
            if !names.isEmpty {
                Divider()
                ForEach(names, id: \.self) { name in
                    Button {
                        _ = bridge.setPaneVantageNamedView(UInt(pane), name: name)
                        onChanged()
                    } label: {
                        if vantageKind == .namedView && vantageNamedView == name {
                            Label(name, systemImage: "checkmark")
                        } else {
                            Text(name)
                        }
                    }
                }
            }
            Divider()
            if vantageKind == .freeFly {
                Button("Exit Free-Fly") {
                    _ = bridge.paneExitFreeFly(UInt(pane))
                    onChanged()
                }
            } else {
                Button("Enter Free-Fly") {
                    _ = bridge.paneEnterFreeFly(UInt(pane))
                    onChanged()
                }
            }
        } label: {
            HStack(spacing: 3) {
                Text(vantageLabel).lineLimit(1)
                Image(systemName: "chevron.down").font(.system(size: 6))
            }
            .font(Theme.mono(9.5))
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .help("Pane vantage")
    }
}

// MARK: - Pane refinement pill (primary pane only)

/// Same `RefinementStatusFormatter` the single-viewport pill uses
/// (ViewportView.swift), scoped to `pane` via
/// `-paneRefinementPhase(_:scaleDivisor:)` instead of the un-indexed
/// call.  Production-render text is deliberately omitted here:
/// production renders suspend the WHOLE pane set (§7.6 — "one client,
/// one lease"), so the single global production/EDR status the
/// TopBar/single-viewport pill already shows covers it; duplicating
/// that text per pane would be redundant chrome for no new
/// information.
private struct PaneRefinementPill: View {
    let bridge: RISEViewportBridge
    let pane: Int
    let modes: [ViewportRenderModeInfo]

    var body: some View {
        TimelineView(.periodic(from: Date(), by: 0.5)) { _ in
            let status = currentStatus()
            HStack(spacing: 6) {
                Text(status.text)
                    .font(Theme.mono(9))
                    .foregroundColor(Theme.textSecondary)
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 2)
                        .fill(Theme.fillTrough)
                        .frame(width: 54, height: 2)
                    RoundedRectangle(cornerRadius: 2)
                        .fill(Theme.spectralGradient)
                        .frame(width: max(0, 54 * status.fraction), height: 2)
                }
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 4)
            .background(Theme.bgBase.opacity(0.72))
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
    }

    private func currentStatus() -> RefinementStatusFormatter.Status {
        var divisor: UInt32 = 1
        let phase = bridge.paneRefinementPhase(UInt(pane), scaleDivisor: &divisor)
        let modeName = bridge.paneRenderMode(UInt(pane))
        let wantsDenoise = modes.first { $0.name == modeName }?.wantsDenoise ?? true
        return RefinementStatusFormatter.status(
            phase: Int(phase),
            scaleDivisor: divisor,
            isProduction: false,
            isCancelling: false,
            productionProgress: 0,
            viewportRenderModeWantsDenoise: wantsDenoise)
    }
}

// MARK: - Pane display + input (NSViewRepresentable, no EDR)

/// Per-pane display surface.  Deliberately lighter than
/// `ViewportView`'s `ViewportCanvas`/`ViewportNSView` — no EDR Metal
/// layer, no region-of-interest, no Escape handling.  N-up panes are
/// plain-NSImage-only by design (see RISEViewportBridge.mm's "N-up
/// pane-0 sink" comment); EDR is not in the §7 spec's scope at all.
private struct PaneCanvas: NSViewRepresentable {
    let image: NSImage?
    let cursor: NSCursor
    let surfaceDimensionsProvider: () -> NSSize
    /// Returns whether the core accepted the gesture (§7.4: Down
    /// returns false ⇒ drop the gesture) — the underlying NSView uses
    /// this to decide whether to forward the rest of the physical
    /// mouse-down/dragged/up sequence.
    let onPointerDown: (CGPoint) -> Bool
    let onPointerMove: (CGPoint) -> Void
    let onPointerUp: (CGPoint) -> Void
    /// Pixel size (backing-scale aware) the cell's NSView actually
    /// occupies — fed to `_SetPaneSurfaceDims` by the caller.
    let onSurfacePixelSizeChanged: (CGSize) -> Void

    func makeNSView(context: Context) -> PaneCanvasNSView {
        let v = PaneCanvasNSView()
        configure(v)
        return v
    }

    func updateNSView(_ nsView: PaneCanvasNSView, context: Context) {
        configure(nsView)
        nsView.window?.invalidateCursorRects(for: nsView)
        nsView.needsDisplay = true
    }

    private func configure(_ v: PaneCanvasNSView) {
        v.image = image
        v.onPointerDown = onPointerDown
        v.onPointerMove = onPointerMove
        v.onPointerUp = onPointerUp
        v.onSurfacePixelSizeChanged = onSurfacePixelSizeChanged
        v.toolCursor = cursor
        v.surfaceDimensionsProvider = surfaceDimensionsProvider
    }
}

final class PaneCanvasNSView: NSView {
    var image: NSImage? {
        didSet {
            window?.invalidateCursorRects(for: self)
            needsDisplay = true
        }
    }
    var onPointerDown: ((CGPoint) -> Bool)?
    var onPointerMove: ((CGPoint) -> Void)?
    var onPointerUp: ((CGPoint) -> Void)?
    var onSurfacePixelSizeChanged: ((CGSize) -> Void)?
    var surfaceDimensionsProvider: (() -> NSSize)?
    var toolCursor: NSCursor = .arrow

    /// True for the duration of a gesture the core accepted at Down —
    /// gates forwarding of the matching Move/Up sequence per §7.4's
    /// "Down returns false ⇒ drop the gesture" contract.
    private var gestureArmed = false
    private var lastReportedPixelSize: CGSize = .zero

    override var isFlipped: Bool { true }

    // Same aspect-fit convention as ViewportNSView (kept independent —
    // see this file's header comment on why this is a lean
    // re-implementation, not a shared subclass).
    private func currentSourceDims() -> NSSize? {
        if let image, image.size.width > 0, image.size.height > 0 {
            return image.size
        }
        if let dims = surfaceDimensionsProvider?(), dims.width > 0, dims.height > 0 {
            return dims
        }
        return nil
    }

    private func currentImageDrawRect() -> NSRect? {
        guard let imgSize = currentSourceDims(), bounds.width > 0, bounds.height > 0 else { return nil }
        let scale = min(bounds.width / imgSize.width, bounds.height / imgSize.height)
        let drawW = imgSize.width * scale
        let drawH = imgSize.height * scale
        return NSRect(x: (bounds.width - drawW) / 2, y: (bounds.height - drawH) / 2,
                      width: drawW, height: drawH)
    }

    override func resetCursorRects() {
        if let drawRect = currentImageDrawRect() {
            addCursorRect(drawRect, cursor: toolCursor)
        }
    }

    override func layout() {
        super.layout()
        reportPixelSizeIfNeeded()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        reportPixelSizeIfNeeded()
    }

    override func resize(withOldSuperviewSize oldSize: NSSize) {
        super.resize(withOldSuperviewSize: oldSize)
        window?.invalidateCursorRects(for: self)
        reportPixelSizeIfNeeded()
    }

    private func reportPixelSizeIfNeeded() {
        guard bounds.width > 0, bounds.height > 0 else { return }
        let scale = window?.backingScaleFactor ?? 2.0
        let w = max(1, (bounds.width * scale).rounded())
        let h = max(1, (bounds.height * scale).rounded())
        let size = CGSize(width: w, height: h)
        guard size != lastReportedPixelSize else { return }
        lastReportedPixelSize = size
        onSurfacePixelSizeChanged?(size)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill()
        bounds.fill()
        guard let image, let drawRect = currentImageDrawRect() else { return }
        image.draw(in: drawRect, from: .zero, operation: .sourceOver, fraction: 1.0,
                   respectFlipped: true, hints: [.interpolation: NSImageInterpolation.none.rawValue])
    }

    /// Same conversion as `ViewportNSView.surfacePoint(from:)`, minus
    /// the EDR-specific fallback ordering (N-up panes never carry an
    /// EDR layer).
    private func surfacePoint(from event: NSEvent) -> CGPoint? {
        let p = self.convert(event.locationInWindow, from: nil)
        guard let drawRect = currentImageDrawRect(), drawRect.width > 0, drawRect.height > 0 else { return nil }

        let providedDims = surfaceDimensionsProvider?() ?? .zero
        let surfaceSize: NSSize
        if providedDims.width > 0, providedDims.height > 0 {
            surfaceSize = providedDims
        } else if let image, image.size.width > 0, image.size.height > 0 {
            surfaceSize = image.size
        } else {
            return nil
        }

        let nx = (p.x - drawRect.minX) / drawRect.width
        let ny = (p.y - drawRect.minY) / drawRect.height
        return CGPoint(x: nx * surfaceSize.width, y: ny * surfaceSize.height)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        guard let p = surfacePoint(from: event) else { gestureArmed = false; return }
        gestureArmed = onPointerDown?(p) ?? false
    }

    override func mouseDragged(with event: NSEvent) {
        guard gestureArmed, let p = surfacePoint(from: event) else { return }
        onPointerMove?(p)
    }

    override func mouseUp(with event: NSEvent) {
        defer { gestureArmed = false }
        guard gestureArmed, let p = surfacePoint(from: event) else { return }
        onPointerUp?(p)
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var acceptsFirstResponder: Bool { true }
}
