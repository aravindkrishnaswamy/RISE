//////////////////////////////////////////////////////////////////////
//
//  MenuBarState.swift - the menu bar's OWN observable state, decoupled
//    from RenderViewModel's publish traffic.
//
//    THE PROBLEM (menu-flyout bug, 2026-07-16): SwiftUI ObservableObject
//    observation is WRAPPER-based, not read-based — RISEApp held the
//    RenderViewModel via @StateObject, so `.commands { }` rebuilt the
//    entire native menu bar on EVERY objectWillChange, and macOS
//    dismisses an open flyout when its menu rebuilds.  RenderViewModel
//    publishes at render frequency (progressive frames, progress ticks,
//    elapsed-time timer, log lines, chat streaming), so menus were
//    unusable whenever anything was happening.
//
//    THE FIX: menus observe THIS object instead.  It subscribes to the
//    view model, coalesces each publish burst to one next-runloop
//    recompute, snapshots exactly the state menus render, and publishes
//    ONLY when that snapshot actually changed.  Progress ticks, image
//    frames, chat tokens, and log lines no longer touch the menu bar;
//    a real menu-relevant change (a scene loads, a render starts/stops,
//    undo labels move) rebuilds it exactly once.
//
//    Equality-guarding the periodic pollers (publish-on-change in
//    pollRefinementState) remains valuable for general view-tree churn,
//    but this object is what makes menus IMMUNE rather than merely
//    quieter.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import Combine

/// Everything the `.commands` menus render, as one Equatable value.
struct MenuBarSnapshot: Equatable {
    var canOpenScene = false
    var canCloseScene = false
    var canSaveImage = false
    var canUseSceneTransport = false
    var canStartProductionRender = false
    var canDrawRegion = false
    var hasActiveRegion = false
    var productionHonorsRegion = false
    var isRendering = false
    var isProductionRenderPaused = false
    var sceneEditsDirty = false
    var hasAnimation = false
    var undoLabel = ""
    var redoLabel = ""
    var recentFiles: [String] = []
    var edrEnabled = false
    var edrAvailable = false
    var viewToneCurve = 0
    var showLeftPanel = true
    var showLogDrawer = true
    /// Insert-menu template freshness: the templates themselves are read
    /// through at render time (no subscription); these two fields make the
    /// snapshot differ exactly when the template set can have changed.
    var hasBridge = false
    var entityListEpoch = 0
    /// P1 render modes (docs/gui/RENDER_MODES.md §5) — the View > Viewport
    /// Rendering submenu's checkmark target + item list.  Unlike the
    /// Insert templates above, these are cheap enough (a handful of static
    /// strings, changes at most once per scene open) to snapshot directly
    /// rather than re-reading at render time.
    var viewportRenderMode = "preview"
    var viewportRenderModes: [ViewportRenderModeInfo] = []
    /// X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis") — the submenu's
    /// checkmark target.  Applies to EVERY render mode, INCLUDING
    /// "preview" (default ON); the submenu's own `.disabled` gate is the
    /// same scene/bridge availability gate the mode list above uses, not
    /// a per-mode restriction.
    var viewportXray = false
}

@MainActor
final class MenuBarState: ObservableObject {
    @Published private(set) var snap: MenuBarSnapshot

    private weak var model: RenderViewModel?
    private var subscription: AnyCancellable?
    /// Coalesces a burst of objectWillChange fires (one edit can publish
    /// a dozen properties) into ONE recompute on the next runloop turn —
    /// which also matters for correctness: objectWillChange fires BEFORE
    /// the mutation, so an immediate recompute would capture stale values.
    private var refreshScheduled = false

    init(observing model: RenderViewModel) {
        self.model = model
        self.snap = Self.capture(model)
        subscription = model.objectWillChange.sink { [weak self] _ in
            self?.scheduleRefresh()
        }
    }

    private func scheduleRefresh() {
        guard !refreshScheduled else { return }
        refreshScheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.refreshScheduled = false
            guard let model = self.model else { return }
            let fresh = Self.capture(model)
            if fresh != self.snap {
                self.snap = fresh
            }
        }
    }

    private static func capture(_ m: RenderViewModel) -> MenuBarSnapshot {
        var s = MenuBarSnapshot()
        s.canOpenScene = m.canOpenScene
        s.canCloseScene = m.canCloseScene
        s.canSaveImage = m.canSaveImage
        s.canUseSceneTransport = m.canUseSceneTransport
        s.canStartProductionRender = m.canStartProductionRender
        s.canDrawRegion = m.canRequestRegionDraw
        s.hasActiveRegion = (m.activeRegion != nil)
        s.productionHonorsRegion = m.canRenderActiveRegion
        s.isRendering = (m.renderState == .rendering)
        s.isProductionRenderPaused = m.isProductionRenderPaused
        s.sceneEditsDirty = m.sceneEditsDirty
        s.hasAnimation = m.hasAnimation
        s.undoLabel = m.undoLabel
        s.redoLabel = m.redoLabel
        s.recentFiles = m.recentFiles
        s.edrEnabled = m.edrEnabled
        s.edrAvailable = m.edrAvailable
        s.viewToneCurve = m.viewToneCurve
        s.showLeftPanel = m.showLeftPanel
        s.showLogDrawer = m.showLogDrawer
        s.hasBridge = (m.viewportBridge != nil)
        s.entityListEpoch = m.entityListEpoch
        s.viewportRenderMode = m.viewportRenderMode
        s.viewportRenderModes = m.viewportRenderModes
        s.viewportXray = m.viewportXray
        return s
    }
}

/// Owns the app's long-lived model objects WITHOUT subscribing to them —
/// RISEApp holds this via plain @State (a non-ObservableObject class), so
/// the App body (and therefore `.commands`) is NOT invalidated by
/// RenderViewModel's publish traffic.  ContentView subscribes itself via
/// @EnvironmentObject; the menus subscribe to `menuBar` only.
@MainActor
final class AppModel {
    let viewModel: RenderViewModel
    let menuBar: MenuBarState

    init() {
        let vm = RenderViewModel()
        self.viewModel = vm
        self.menuBar = MenuBarState(observing: vm)
    }
}
