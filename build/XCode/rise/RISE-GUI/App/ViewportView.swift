//////////////////////////////////////////////////////////////////////
//
//  ViewportView.swift - Interactive 3D viewport for the macOS app.
//
//    Composition: render image + gizmo overlay + region-of-interest
//    overlay + selection chip + refinement pill + timeline.  All
//    edit logic lives in C++ (SceneEditController); this view is
//    purely a thin sink + event router.  The tool-category toolbar
//    itself moved OUT of this view in the UI redesign center-column
//    slice — it's now ContentView's 40pt row above the viewport;
//    `selectedTool` is therefore an inbound Binding, not owned state,
//    but this view still reacts to it (bridge.currentTool sync +
//    gizmo refresh) since that side-effect has nothing to do with
//    where the buttons live.
//
//    Phase 3 wires events through to the controller but the
//    controller is in skeleton mode (no interactive rasterizer):
//    the user mutates the in-memory scene, presses "Render" to see
//    the production result.  Phase 5 wires up live preview.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import AppKit

struct ViewportView: View {
    @EnvironmentObject var viewModel: RenderViewModel

    let bridge: RISEViewportBridge
    @Binding var image: NSImage?
    let timelineVisible: Bool
    @Binding var sceneTime: Double
    let timelineRange: ClosedRange<Double>
    /// True while the user can interact (drag, scrub, edit).  False
    /// while a production render is in flight — the toolbar greys
    /// out and the canvas ignores pointer events so the production
    /// rasterizer doesn't race with edits.
    var interactionEnabled: Bool = true
    /// True while the production rasterizer is actively running.
    /// Narrower than `!interactionEnabled` (which also covers
    /// `.loading` and `.cancelling` transitions) — used to hide the
    /// gizmo overlay specifically during production renders, so
    /// staleness between the interactive-thread's cached handles
    /// and the production rasterizer's output doesn't surface as
    /// "partial widget overlay on a finished image".
    var isProductionRendering: Bool = false
    /// Called whenever the active tool changes or a pointer-down
    /// event fires (which may have updated picking).  ContentView
    /// uses this to bump the property-panel refresh trigger so the
    /// panel re-reads bridge.panelMode + propertySnapshot; this view
    /// also uses it as the cue to re-pull its own selection-chip text.
    var onSelectionMayHaveChanged: () -> Void = {}

    /// Looping preview-play state + controls for the Play button by the
    /// timeline slider.  `isPreviewPlaying` drives the play/stop icon;
    /// `onPlayToggle` starts/stops the loop; `onUserScrubBegan` lets a
    /// manual drag interrupt playback.
    var isPreviewPlaying: Bool = false
    var onPlayToggle: () -> Void = {}
    var onUserScrubBegan: () -> Void = {}

    /// L5a round-5 — TWO EDR renderers passed through from
    /// RenderViewModel.  Production handles the full-quality
    /// render output (opaque base layer); interactive handles
    /// the live-preview overlay (transparent layer above
    /// production).  Both nil → legacy NSImage path.
    /// `edrEnabled` gates BOTH layers (when off, neither
    /// renderer is attached and the legacy path runs).
    var productionEDRRenderer:  MetalEDRRenderer? = nil
    var interactiveEDRRenderer: MetalEDRRenderer? = nil
    var edrEnabled: Bool = false

    /// UI redesign: owned by ContentView (the toolbar row that now
    /// hosts the tool-category buttons needs it too), passed down so
    /// this view's cursor/gizmo/pointer-routing logic still reacts to
    /// tool changes exactly as before the toolbar moved out.
    @Binding var selectedTool: ViewportTool
    /// UI redesign design brief A4: true while the region chip is
    /// "armed" — the next mouse-drag in the viewport draws a region
    /// box instead of routing to the active tool.  Owned by
    /// ContentView (the toolbar's REGION chip toggles it); this view
    /// consumes it to branch pointer routing and disarms it once a
    /// drag completes (or Esc is pressed).
    @Binding var regionArmed: Bool
    /// P1-4 fix: bumped by ContentView (the SAME `propertyRefresh`
    /// counter OutlinerView / PropertiesPanel already key off of)
    /// whenever selection may have changed through a surface OTHER
    /// than this view's own pointer/tool events — most importantly
    /// the outliner's click-to-select.  Before this existed, the
    /// top-left selection chip only refreshed from
    /// `onPointerDown`/tool-change/bridge-attach, so selecting an
    /// object in the outliner left the viewport's chip showing the
    /// previous (or no) selection until the next viewport click.
    @Binding var refreshTrigger: Int

    /// Bumps each time the gizmo overlay should re-pull its handle
    /// snapshot.  Tied to pointer-down / pointer-up / tool change
    /// (which are the events that can change which gizmo is shown)
    /// and to `image` changes (camera motion shifts the projected
    /// handle positions).
    @State private var gizmoRefreshTrigger: Int = 0

    /// Region-of-interest drag-in-progress corners, in full-res
    /// FILM-PIXEL space — the SAME space `onPointerDown` /
    /// `onPointerMove` / `onPointerUp` already receive their `p`
    /// argument in (see `ViewportNSView.surfacePoint(from:)`, which
    /// maps through `bridge.cameraSurfaceDimensions`).  nil when no
    /// drag is in progress.
    @State private var regionDragStart: CGPoint? = nil
    @State private var regionDragCurrent: CGPoint? = nil
    /// Set when Esc cancels a region drag WHILE the mouse button is
    /// still down.  AppKit keeps delivering `mouseDragged`/`mouseUp`
    /// for the rest of that physical gesture even after we've reset
    /// `regionDragStart`/`regionArmed` — without this flag, those
    /// trailing events would fall through to the normal-tool branch
    /// and forward a `pointerMove`/`pointerUp` to the bridge with NO
    /// matching `pointerDown` (it was intercepted by the
    /// region-armed branch at pointer-down time), corrupting the
    /// controller's drag-anchor state.  Cleared on the next
    /// `pointerUp`, which ends the physical gesture.
    @State private var suppressPointerUntilUp: Bool = false

    /// Local snapshot of the bridge's current selection, refreshed
    /// alongside `onSelectionMayHaveChanged` — drives the top-left
    /// selection chip.  Mirrors the same (selectionCategory,
    /// selectionName) pair OutlinerView / PropertiesPanel read.
    @State private var selectionCategory: RISEViewportCategory = .none
    @State private var selectionNameText: String = ""

    var body: some View {
        VStack(spacing: 0) {
            ZStack(alignment: .top) {
                ViewportCanvas(
                    image: $image,
                    cursor: interactionEnabled ? (regionArmed ? .crosshair : selectedTool.nsCursor) : .arrow,
                    productionEDRRenderer:  edrEnabled ? productionEDRRenderer  : nil,
                    interactiveEDRRenderer: edrEnabled ? interactiveEDRRenderer : nil,
                    surfaceDimensionsProvider: { [weak bridge] in
                        bridge?.cameraSurfaceDimensions ?? .zero
                    },
                    onSurfacePixelSizeChanged: { size in
                        guard size.width > 0, size.height > 0 else { return false }
                        let accepted = bridge.setPaneSurfaceDims(
                            0,
                            width: UInt(max(1, size.width)),
                            height: UInt(max(1, size.height)))
                        if accepted {
                            // The NSView can report synchronously from
                            // updateNSView/image.didSet. Defer SwiftUI state
                            // mutation until that update transaction finishes.
                            DispatchQueue.main.async { gizmoRefreshTrigger &+= 1 }
                        }
                        return accepted
                    },
                    onPointerDown: { p in
                        guard interactionEnabled else { return }
                        // Every pointer-down starts a NEW physical
                        // gesture — any suppress flag left over from
                        // an earlier interrupted gesture (e.g. the
                        // mouse-up that should have cleared it never
                        // reached us, such as a system modal
                        // stealing the drag) is stale by definition.
                        // Clearing here — rather than only on the
                        // matching pointer-up — bounds the flag's
                        // lifetime to at most one gesture instead of
                        // "forever" if that pointer-up is ever lost.
                        suppressPointerUntilUp = false
                        if regionArmed {
                            regionDragStart = p
                            regionDragCurrent = p
                            return
                        }
                        bridge.pointerDown(x: Double(p.x), y: Double(p.y))
                        // Pointer-down on Select tool may have just
                        // picked an object; pointer-down on motion
                        // tools doesn't change selection but the
                        // panel refresh is cheap so we always notify.
                        onSelectionMayHaveChanged()
                        refreshSelectionChip()
                        gizmoRefreshTrigger &+= 1
                    },
                    onPointerMove: { p in
                        guard interactionEnabled else { return }
                        if suppressPointerUntilUp { return }
                        if regionArmed {
                            if regionDragStart != nil { regionDragCurrent = p }
                            return
                        }
                        bridge.pointerMove(x: Double(p.x), y: Double(p.y))
                        if bridge.gizmoDragActive {
                            gizmoRefreshTrigger &+= 1
                        }
                    },
                    onPointerUp: { p in
                        guard interactionEnabled else { return }
                        if suppressPointerUntilUp {
                            // Last leg of a gesture Esc cancelled
                            // mid-drag (see `suppressPointerUntilUp`'s
                            // doc) — swallow it and reset for the
                            // next physical gesture.
                            suppressPointerUntilUp = false
                            return
                        }
                        if regionArmed, let start = regionDragStart {
                            commitRegionDrag(from: start, to: p)
                            regionDragStart = nil
                            regionDragCurrent = nil
                            regionArmed = false
                            return
                        }
                        bridge.pointerUp(x: Double(p.x), y: Double(p.y))
                        gizmoRefreshTrigger &+= 1
                    },
                    onEscape: {
                        // Design brief A4: Esc while armed disarms;
                        // Esc with an active region clears it.  A
                        // drag already in progress cancels first
                        // (without committing a region) and arms
                        // `suppressPointerUntilUp` so the rest of
                        // this physical mouse-down gesture doesn't
                        // leak a dangling pointerMove/pointerUp to
                        // the bridge with no matching pointerDown.
                        if regionDragStart != nil {
                            regionDragStart = nil
                            regionDragCurrent = nil
                            regionArmed = false
                            suppressPointerUntilUp = true
                        } else if regionArmed {
                            regionArmed = false
                        } else if viewModel.activeRegion != nil, interactionEnabled {
                            // clearInteractiveRegion -> KickRender takes the
                            // controller mutex a chat/production render holds
                            // for its whole duration; skip the clear while
                            // render-owned (the local disarm/cancel branches
                            // above stay live — they touch no mutex).
                            bridge.clearInteractiveRegion()
                            viewModel.activeRegion = nil
                        }
                    }
                )

                // Gizmo overlay — drawn on top of the rendered frame
                // when an Object-transform tool is active AND the
                // production rasterizer is NOT in flight.  Hidden
                // during production renders because the cached
                // handles were computed against the interactive
                // thread's last camera state, which may not match
                // the production rasterizer's frame state — drawing
                // them would surface as a partial / misaligned
                // overlay on the finished image.  Hit testing is
                // OFF so pointer events continue to flow through to
                // ViewportCanvas; the controller's `OnPointerDown`
                // is what consults the handle array via hit-test.
                if selectedTool.category == .objectTransform && !isProductionRendering {
                    ViewportGizmoOverlay(
                        bridge: bridge,
                        refreshTrigger: gizmoRefreshTrigger,
                        surfaceDimensionsProvider: { [weak bridge] in
                            bridge?.cameraSurfaceDimensions ?? .zero
                        }
                    )
                    .allowsHitTesting(false)
                }

                // Design brief A4 — region-of-interest overlay: the
                // live drag-preview box while armed + dragging, or
                // the persistent active-region box (with a clickable
                // clear badge) once `viewModel.activeRegion` is set.
                RegionOverlay(
                    activeRegion: viewModel.activeRegion,
                    dragStart: regionDragStart,
                    dragCurrent: regionDragCurrent,
                    surfaceDimensionsProvider: { [weak bridge] in
                        bridge?.cameraSurfaceDimensions ?? .zero
                    },
                    interactionEnabled: interactionEnabled && !regionArmed,
                    onCommitActiveRegion: { region in
                        guard interactionEnabled else { return }
                        let left = UInt32(max(0, Int(region.minX)))
                        let top = UInt32(max(0, Int(region.minY)))
                        let right = UInt32(max(Int(region.minX), Int(region.maxX) - 1))
                        let bottom = UInt32(max(Int(region.minY), Int(region.maxY) - 1))
                        bridge.setInteractiveRegionLeft(left, top: top,
                                                        right: right, bottom: bottom)
                        viewModel.activeRegion = CGRect(
                            x: CGFloat(left), y: CGFloat(top),
                            width: CGFloat(right - left + 1),
                            height: CGFloat(bottom - top + 1))
                    },
                    onClearActiveRegion: {
                        // The clear badge stays hit-testable during a chat
                        // render; clearInteractiveRegion -> KickRender takes the
                        // controller mutex that render holds, so gate on the
                        // chat-inclusive interactionEnabled.
                        guard interactionEnabled else { return }
                        bridge.clearInteractiveRegion()
                        viewModel.activeRegion = nil
                    }
                )
            }
            .overlay(alignment: .topTrailing) {
                // Navigation axis-ball (Tier 2 §4): always available (nav is
                // not tool-gated), but hidden during a production render since
                // the interactive camera state may not match the production
                // frame.  Hit-testable — a nub click snaps the view.
                if !isProductionRendering {
                    ViewportNavOverlay(bridge: bridge, refreshTrigger: gizmoRefreshTrigger,
                                       sceneEditable: interactionEnabled)
                        .padding(12)
                }
            }
            .overlay(alignment: .topLeading) {
                if selectionCategory != .none, !selectionNameText.isEmpty {
                    selectionChip.padding(12)
                }
            }
            .overlay(alignment: .top) {
                if regionArmed && regionDragStart == nil && interactionEnabled {
                    Text("Drag to choose an area  ·  Esc to cancel")
                        .font(Theme.sans(11, .semibold))
                        .foregroundColor(Theme.textPrimary)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 7)
                        .background(Theme.bgBase.opacity(0.9),
                                    in: RoundedRectangle(cornerRadius: 6))
                        .overlay(RoundedRectangle(cornerRadius: 6)
                            .stroke(Theme.accent.opacity(0.55), lineWidth: 1))
                        .padding(.top, 12)
                        .allowsHitTesting(false)
                }
            }
            .overlay(alignment: .bottomLeading) {
                refinementPill.padding(12)
            }
            .onChange(of: selectedTool) { _, newValue in
                bridge.currentTool = newValue.bridgeValue
                // Panel mode is derived from the current tool;
                // bump the refresh so the panel switches between
                // empty / camera / object as the user toggles.
                onSelectionMayHaveChanged()
                refreshSelectionChip()
                // Tool change can flip the gizmo shape (Translate
                // → Rotate switches between arrows and rings); a
                // refresh-trigger bump forces the overlay to
                // re-pull the handle array.
                gizmoRefreshTrigger &+= 1
            }
            .onChange(of: image) { _, _ in
                // Camera motion / preview-scale changes shift the
                // projected handle positions; rebuild the snapshot
                // each time a new frame arrives so the gizmo
                // tracks the rendered image.
                gizmoRefreshTrigger &+= 1
            }
            .onChange(of: interactionEnabled) { _, newValue in
                // A production render can start mid-drag, or right
                // after the user arms REGION but before they've
                // started dragging (the Render menu / ⌘R aren't
                // gated on the viewport's own drag/arm state).  Once
                // interaction is disabled, pointer events stop
                // reaching the region-drag branch above, so without
                // this an in-progress drag's dashed preview box would
                // be left stranded on screen with no way to clear it
                // short of Esc, and the "Cancel Draw" chip would sit lit
                // with no drag able to ever consume it.  Clear both
                // proactively — this only touches the UNCOMMITTED
                // drag/arm state, not the persisted
                // `viewModel.activeRegion`, which deliberately survives
                // production so interactive refinement can resume in the
                // same box afterward.
                if !newValue {
                    _ = bridge.finalizeOpenInteractions()
                    regionDragStart = nil
                    regionDragCurrent = nil
                    suppressPointerUntilUp = false
                    regionArmed = false
                }
            }
            .onChange(of: regionArmed) { _, armed in
                // A workspace-level Escape can disarm while this view has an
                // in-flight mouse gesture. Retire its uncommitted draft and
                // swallow the matching physical mouse-up rather than routing
                // an unmatched pointer event into the scene controller.
                if !armed && regionDragStart != nil {
                    regionDragStart = nil
                    regionDragCurrent = nil
                    suppressPointerUntilUp = true
                }
            }
            // Re-sync the toolbar's selection to the underlying
            // controller whenever the bridge identity changes (a new
            // scene was loaded, or the bridge was rebuilt for any
            // other reason).  The new controller defaults to .select
            // internally; without this the toolbar's persisted
            // selection would still highlight (say) Orbit while
            // pointer events go to the Select tool.  `.task(id:)`
            // runs on appear AND on id change, so it covers both
            // initial attach and subsequent scene loads.
            .task(id: ObjectIdentifier(bridge)) {
                bridge.currentTool = selectedTool.bridgeValue
                onSelectionMayHaveChanged()
                refreshSelectionChip()
            }
            // P1-4 fix: re-pull the selection snapshot whenever
            // something OTHER than this view's own pointer/tool events
            // may have changed it (e.g. an outliner click) — see
            // `refreshTrigger`'s doc.
            .onChange(of: refreshTrigger) { _, _ in refreshSelectionChip() }

            if timelineVisible {
                TimelineSlider(
                    time: $sceneTime,
                    range: timelineRange,
                    isPlaying: isPreviewPlaying,
                    onPlayToggle: onPlayToggle,
                    onUserScrubBegan: {
                        guard interactionEnabled else { return }
                        onUserScrubBegan()
                    },
                    onScrubBegin: {
                        guard interactionEnabled else { return }
                        _ = bridge.scrubTimeBegin()
                    },
                    onScrubEnd: {
                        guard interactionEnabled else { return }
                        _ = bridge.scrubTimeEnd()
                    }
                )
                .disabled(!interactionEnabled)
                .opacity(interactionEnabled ? 1.0 : 0.5)
                .onChange(of: sceneTime) { _, newValue in
                    if viewModel.consumePreappliedSceneTime(newValue) { return }
                    guard interactionEnabled else { return }
                    // The render gate can win after the slider's enabled
                    // state was sampled. Keep the binding honest when the
                    // controller refuses rather than displaying a time the
                    // scene never reached.
                    if !bridge.scrubTime(newValue) {
                        sceneTime = bridge.lastSceneTime()
                    }
                }
            }
        }
        // Bridge lifetime is owned by RenderViewModel — it runs from
        // scene-load until clearScene.  This view's appear/disappear
        // intentionally does NOT start/stop the bridge.
    }

    // MARK: - Region-of-interest commit

    /// Convert the two drag corners (already in full-res film-pixel
    /// space) into an inclusive box, clamp to the camera's surface
    /// dimensions, and hand it to the bridge.  A near-zero drag (a
    /// stray click while armed) is treated as "cancel arm" rather
    /// than setting a degenerate 0-1px region.
    private func commitRegionDrag(from a: CGPoint, to b: CGPoint) {
        guard abs(a.x - b.x) >= 2 || abs(a.y - b.y) >= 2 else { return }
        let dims = bridge.cameraSurfaceDimensions
        guard dims.width > 1, dims.height > 1 else { return }
        let maxX = dims.width - 1
        let maxY = dims.height - 1
        let x0 = min(max(min(a.x, b.x), 0), maxX)
        let x1 = min(max(max(a.x, b.x), 0), maxX)
        let y0 = min(max(min(a.y, b.y), 0), maxY)
        let y1 = min(max(max(a.y, b.y), 0), maxY)
        bridge.setInteractiveRegionLeft(UInt32(x0), top: UInt32(y0),
                                         right: UInt32(x1), bottom: UInt32(y1))
        // Immediate UI feedback rather than waiting up to 0.5s for
        // RenderViewModel's next refinement-status poll to pick it up.
        // `right`/`bottom` are INCLUSIVE (see
        // RISEViewportBridge.h's setInteractiveRegionLeft:top:right:bottom:
        // doc) — +1 so the drawn box's width/height matches the pixel
        // COUNT actually restricted, not the coordinate delta.
        viewModel.activeRegion = CGRect(x: x0, y: y0, width: x1 - x0 + 1, height: y1 - y0 + 1)
    }

    // MARK: - Selection chip

    private func refreshSelectionChip() {
        selectionCategory = bridge.selectionCategory
        selectionNameText = bridge.selectionName
    }

    private var selectionChip: some View {
        Text(selectionNameText)
            .font(Theme.mono(10.5))
            .foregroundColor(Theme.accentLight)
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(Theme.bgBase.opacity(0.72))
            .clipShape(Capsule())
            .overlay(Capsule().stroke(Theme.accent.opacity(0.3), lineWidth: 1))
    }

    // MARK: - Refinement pill

    private var refinementPill: some View {
        let isProduction = viewModel.renderState == .rendering
        let isCancelling = viewModel.renderState == .cancelling
        let status = RefinementStatusFormatter.status(
            phase: viewModel.refinementPhase,
            scaleDivisor: viewModel.refinementScaleDivisor,
            isProduction: isProduction,
            isCancelling: isCancelling,
            productionProgress: viewModel.progress,
            isProductionPaused: viewModel.isProductionRenderPaused,
            viewportRenderModeWantsDenoise: viewModel.viewportRenderModeWantsDenoise)
        return HStack(spacing: 9) {
            Text(status.text)
                .font(Theme.mono(10.5))
                .foregroundColor(Theme.textSecondary)
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(Theme.fillTrough)
                    .frame(width: 90, height: 3)
                RoundedRectangle(cornerRadius: 2)
                    .fill(Theme.spectralGradient)
                    .frame(width: max(0, 90 * status.fraction), height: 3)
            }
            if !status.label.isEmpty {
                Text(status.label)
                    .font(Theme.sans(9))
                    .tracking(0.6)
                    .foregroundColor(Theme.success)
            }
            if let region = viewModel.activeRegion {
                let dims = bridge.cameraSurfaceDimensions
                let framePixels = max(1.0, dims.width * dims.height)
                let percent = Int((100.0 * region.width * region.height / framePixels).rounded())
                Text(viewModel.refinementScaleDivisor == 1
                     ? "REGION \(percent)%"
                     : "FULL PREVIEW → REGION")
                    .font(Theme.sans(9, .semibold))
                    .tracking(0.4)
                    .foregroundColor(Theme.warn)
            }
        }
        .padding(.horizontal, 11)
        .padding(.vertical, 7)
        .background(Theme.bgBase.opacity(0.78))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.borderLight, lineWidth: 1))
    }
}

// MARK: - Region-of-interest overlay

/// Draws the design brief A4 region box: while a drag is in progress
/// (`dragStart`/`dragCurrent` both non-nil), a live dashed preview
/// following the pointer; otherwise, when `activeRegion` is set, the
/// persistent dashed box with a clickable "REGION" clear badge.  Uses
/// `ViewportLetterbox` (shared with `ViewportGizmoOverlay`) so the box
/// lines up with the rendered image using the SAME aspect-fit formula
/// `ViewportNSView.currentImageDrawRect()` uses to lay out the image.
private struct RegionOverlay: View {
    /// Full-res film-pixel space, inclusive box (matches
    /// `RISEViewportBridge.getInteractiveRegionLeft:top:right:bottom:`).
    let activeRegion: CGRect?
    let dragStart: CGPoint?
    let dragCurrent: CGPoint?
    let surfaceDimensionsProvider: () -> CGSize
    let interactionEnabled: Bool
    let onCommitActiveRegion: (CGRect) -> Void
    let onClearActiveRegion: () -> Void
    @State private var draftRegion: CGRect? = nil
    @State private var editStartRegion: CGRect? = nil

    private enum Handle: CaseIterable {
        case topLeft, top, topRight, right, bottomRight, bottom, bottomLeft, left
    }

    var body: some View {
        GeometryReader { geom in
            let surface = surfaceDimensionsProvider()
            if let fit = ViewportLetterbox.fit(surface: surface, in: geom.size) {
                if let start = dragStart, let current = dragCurrent {
                    dashedBox(widgetRect(filmRect(start, current), fit: fit))
                        .allowsHitTesting(false)
                } else if let region = draftRegion ?? activeRegion {
                    let rect = widgetRect(region, fit: fit)
                    ZStack(alignment: .topLeading) {
                        outsideMask(viewportSize: geom.size, hole: rect)
                            .allowsHitTesting(false)
                        dashedBox(rect).allowsHitTesting(false)
                        Rectangle()
                            .fill(Color.clear)
                            .contentShape(Rectangle())
                            .frame(width: rect.width, height: rect.height)
                            .position(x: rect.midX, y: rect.midY)
                            .gesture(moveGesture(region: region, fit: fit, surface: surface))
                            .allowsHitTesting(interactionEnabled)
                            .accessibilityElement()
                            .accessibilityLabel("Active render region")
                            .accessibilityValue(regionLabel(region: region, surface: surface))
                            .accessibilityAction(named: "Move left") {
                                move(region: region, dx: -8, dy: 0, surface: surface)
                            }
                            .accessibilityAction(named: "Move right") {
                                move(region: region, dx: 8, dy: 0, surface: surface)
                            }
                            .accessibilityAction(named: "Move up") {
                                move(region: region, dx: 0, dy: -8, surface: surface)
                            }
                            .accessibilityAction(named: "Move down") {
                                move(region: region, dx: 0, dy: 8, surface: surface)
                            }
                            .accessibilityHidden(!interactionEnabled)
                        let badgeW = badgeWidth(region: region, surface: surface)
                        Button(action: onClearActiveRegion) {
                            regionBadge(region: region, surface: surface)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Clear active render region")
                        .accessibilityValue(regionLabel(region: region, surface: surface))
                        .position(badgePosition(rect: rect, badgeWidth: badgeW,
                                                viewportSize: geom.size))
                        .allowsHitTesting(interactionEnabled)
                        .accessibilityHidden(!interactionEnabled)
                        // Handles are intentionally later in the Z-stack so
                        // their 24pt targets win if a tiny/full-frame region
                        // leaves no truly exterior space for the badge.
                        ForEach(Handle.allCases, id: \.self) { handle in
                            ZStack {
                                Circle()
                                    .fill(Theme.warn)
                                    .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 1))
                                    .frame(width: 9, height: 9)
                            }
                                .frame(width: 24, height: 24)
                                .contentShape(Circle())
                                .position(handlePoint(handle, in: rect))
                                .gesture(resizeGesture(handle: handle, region: region,
                                                       fit: fit, surface: surface))
                                .allowsHitTesting(interactionEnabled)
                                .accessibilityLabel(accessibilityLabel(for: handle))
                                .accessibilityHint("Swipe up to expand or down to contract")
                                .accessibilityAdjustableAction { direction in
                                    accessibilityResize(handle: handle, direction: direction,
                                                        region: region, surface: surface)
                                }
                                .accessibilityHidden(!interactionEnabled)
                        }
                    }
                }
            }
        }
        .onChange(of: activeRegion) { _, _ in
            draftRegion = nil
            editStartRegion = nil
        }
        .onChange(of: interactionEnabled) { _, enabled in
            if !enabled {
                draftRegion = nil
                editStartRegion = nil
            }
        }
    }

    private func regionBadge(region: CGRect, surface: CGSize) -> some View {
        Text(regionLabel(region: region, surface: surface))
            .font(Theme.mono(9, .semibold))
            .foregroundColor(.black)
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .background(Theme.warn)
            .clipShape(RoundedRectangle(cornerRadius: 3))
    }

    private func regionLabel(region: CGRect, surface: CGSize) -> String {
        let pixels = max(0, region.width) * max(0, region.height)
        let framePixels = max(1, surface.width * surface.height)
        let percent = Int((100.0 * pixels / framePixels).rounded())
        return "REGION  \(Int(region.width))×\(Int(region.height))  ·  \(percent)%"
    }

    private func badgeWidth(region: CGRect, surface: CGSize) -> CGFloat {
        let label = regionLabel(region: region, surface: surface)
        return CGFloat(label.count) * 5.8 + 14
    }

    private func outsideMask(viewportSize: CGSize, hole: CGRect) -> some View {
        Path { path in
            path.addRect(CGRect(origin: .zero, size: viewportSize))
            path.addRect(hole)
        }
        .fill(Color.black.opacity(0.16), style: FillStyle(eoFill: true))
    }

    private func handlePoint(_ handle: Handle, in rect: CGRect) -> CGPoint {
        switch handle {
        case .topLeft: return CGPoint(x: rect.minX, y: rect.minY)
        case .top: return CGPoint(x: rect.midX, y: rect.minY)
        case .topRight: return CGPoint(x: rect.maxX, y: rect.minY)
        case .right: return CGPoint(x: rect.maxX, y: rect.midY)
        case .bottom: return CGPoint(x: rect.midX, y: rect.maxY)
        case .bottomLeft: return CGPoint(x: rect.minX, y: rect.maxY)
        case .bottomRight: return CGPoint(x: rect.maxX, y: rect.maxY)
        case .left: return CGPoint(x: rect.minX, y: rect.midY)
        }
    }

    private func accessibilityLabel(for handle: Handle) -> String {
        switch handle {
        case .topLeft: return "Resize region from top left"
        case .top: return "Resize region top edge"
        case .topRight: return "Resize region from top right"
        case .right: return "Resize region right edge"
        case .bottomRight: return "Resize region from bottom right"
        case .bottom: return "Resize region bottom edge"
        case .bottomLeft: return "Resize region from bottom left"
        case .left: return "Resize region left edge"
        }
    }

    private func badgePosition(rect: CGRect, badgeWidth: CGFloat,
                               viewportSize: CGSize) -> CGPoint {
        let half = badgeWidth / 2
        let x = min(max(half + 4, rect.midX), max(half + 4, viewportSize.width - half - 4))
        let above = rect.minY - 24
        let below = rect.maxY + 24
        let y: CGFloat
        if above >= 10 {
            y = above
        } else if below <= viewportSize.height - 10 {
            y = below
        } else {
            // Full-height/tiny viewports have no exterior vertical space.
            // Keep clear of the top-edge handle; handle hit-testing still
            // wins because it is layered above the badge.
            y = min(viewportSize.height - 10, rect.minY + 28)
        }
        return CGPoint(x: x, y: y)
    }

    private func clamped(_ region: CGRect, to surface: CGSize) -> CGRect {
        let width = min(max(3, region.width), surface.width)
        let height = min(max(3, region.height), surface.height)
        let x = min(max(0, region.minX), max(0, surface.width - width))
        let y = min(max(0, region.minY), max(0, surface.height - height))
        return CGRect(x: x.rounded(), y: y.rounded(),
                      width: width.rounded(), height: height.rounded())
    }

    private func move(region: CGRect, dx: CGFloat, dy: CGFloat, surface: CGSize) {
        onCommitActiveRegion(clamped(region.offsetBy(dx: dx, dy: dy), to: surface))
    }

    private func moveGesture(region: CGRect, fit: ViewportLetterbox.Fit,
                             surface: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 2)
            .onChanged { value in
                let start = editStartRegion ?? region
                if editStartRegion == nil { editStartRegion = region }
                draftRegion = clamped(start.offsetBy(
                    dx: value.translation.width / fit.scale,
                    dy: value.translation.height / fit.scale), to: surface)
            }
            .onEnded { value in
                let start = editStartRegion ?? region
                let final = clamped(start.offsetBy(
                    dx: value.translation.width / fit.scale,
                    dy: value.translation.height / fit.scale), to: surface)
                draftRegion = nil
                editStartRegion = nil
                onCommitActiveRegion(final)
            }
    }

    private func resizeGesture(handle: Handle, region: CGRect,
                               fit: ViewportLetterbox.Fit,
                               surface: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { value in
                let start = editStartRegion ?? region
                if editStartRegion == nil { editStartRegion = region }
                draftRegion = resized(start, handle: handle,
                                      translation: value.translation,
                                      scale: fit.scale, surface: surface)
            }
            .onEnded { value in
                let start = editStartRegion ?? region
                let final = resized(start, handle: handle,
                                    translation: value.translation,
                                    scale: fit.scale, surface: surface)
                draftRegion = nil
                editStartRegion = nil
                onCommitActiveRegion(final)
            }
    }

    private func resized(_ region: CGRect, handle: Handle,
                         translation: CGSize, scale: CGFloat,
                         surface: CGSize) -> CGRect {
        let dx = translation.width / scale
        let dy = translation.height / scale
        var minX = region.minX, maxX = region.maxX
        var minY = region.minY, maxY = region.maxY
        switch handle {
        case .topLeft: minX += dx; minY += dy
        case .top: minY += dy
        case .topRight: maxX += dx; minY += dy
        case .right: maxX += dx
        case .bottom: maxY += dy
        case .bottomLeft: minX += dx; maxY += dy
        case .bottomRight: maxX += dx; maxY += dy
        case .left: minX += dx
        }
        if handle == .topLeft || handle == .bottomLeft || handle == .left {
            minX = min(max(0, minX), region.maxX - 3)
        } else if handle == .topRight || handle == .bottomRight || handle == .right {
            maxX = max(region.minX + 3, min(surface.width, maxX))
        }
        if handle == .topLeft || handle == .top || handle == .topRight {
            minY = min(max(0, minY), region.maxY - 3)
        } else if handle == .bottomLeft || handle == .bottom || handle == .bottomRight {
            maxY = max(region.minY + 3, min(surface.height, maxY))
        }
        return CGRect(x: minX.rounded(), y: minY.rounded(),
                      width: (maxX - minX).rounded(),
                      height: (maxY - minY).rounded())
    }

    private func accessibilityResize(handle: Handle, direction: AccessibilityAdjustmentDirection,
                                     region: CGRect, surface: CGSize) {
        let outward = direction == .increment ? CGFloat(8) : CGFloat(-8)
        var translation = CGSize.zero
        switch handle {
        case .topLeft: translation = CGSize(width: -outward, height: -outward)
        case .top: translation.height = -outward
        case .topRight: translation = CGSize(width: outward, height: -outward)
        case .right: translation.width = outward
        case .bottomRight: translation = CGSize(width: outward, height: outward)
        case .bottom: translation.height = outward
        case .bottomLeft: translation = CGSize(width: -outward, height: outward)
        case .left: translation.width = -outward
        }
        onCommitActiveRegion(resized(region, handle: handle, translation: translation,
                                     scale: 1, surface: surface))
    }

    private func filmRect(_ a: CGPoint, _ b: CGPoint) -> CGRect {
        CGRect(x: min(a.x, b.x), y: min(a.y, b.y),
              width: abs(a.x - b.x), height: abs(a.y - b.y))
    }

    private func widgetRect(_ rect: CGRect, fit: ViewportLetterbox.Fit) -> CGRect {
        let origin = ViewportLetterbox.toWidget(rect.origin, fit: fit)
        return CGRect(x: origin.x, y: origin.y,
                      width: rect.width * fit.scale, height: rect.height * fit.scale)
    }

    private func dashedBox(_ rect: CGRect) -> some View {
        RoundedRectangle(cornerRadius: 2)
            .stroke(Theme.warn, style: StrokeStyle(lineWidth: 1.5, dash: [5, 3]))
            .frame(width: max(0, rect.width), height: max(0, rect.height))
            .position(x: rect.midX, y: rect.midY)
            .allowsHitTesting(false)
    }
}

/// SwiftUI host for an NSImage with NSResponder-style mouse events
/// translated into the viewport surface coordinate system.
private struct ViewportCanvas: NSViewRepresentable {
    @Binding var image: NSImage?
    let cursor: NSCursor
    /// L5a round-5 — production + interactive EDR renderers, each
    /// driving its own CAMetalLayer sublayer in ViewportNSView.
    /// Production layer is opaque base (full-quality renders);
    /// interactive layer composites on top with transparent
    /// background (live preview during editing).  Either or both
    /// may be nil (LDR fallback / Metal-incapable host).
    let productionEDRRenderer:  MetalEDRRenderer?
    let interactiveEDRRenderer: MetalEDRRenderer?
    /// Closure that returns the camera's stable full-resolution
    /// dimensions for surface-point math.  Closure (rather than a
    /// captured value) so each pointer event reads the freshest
    /// value without us having to invalidate the SwiftUI view tree
    /// on every dim refresh — the bridge updates the underlying
    /// values on its own schedule (scene reload, etc.).
    let surfaceDimensionsProvider: () -> NSSize
    /// Aspect-fitted image surface in physical pixels. Returning false keeps
    /// the measurement pending so a render-admission refusal is retried.
    let onSurfacePixelSizeChanged: (CGSize) -> Bool
    let onPointerDown: (CGPoint) -> Void
    let onPointerMove: (CGPoint) -> Void
    let onPointerUp:   (CGPoint) -> Void
    /// UI redesign design brief A4 — fires on Esc key-down while the
    /// viewport has keyboard focus.  nil is a valid no-op default for
    /// call sites that don't need it.
    var onEscape: (() -> Void)? = nil

    func makeNSView(context: Context) -> ViewportNSView {
        let v = ViewportNSView()
        v.onPointerDown = onPointerDown
        v.onPointerMove = onPointerMove
        v.onPointerUp   = onPointerUp
        v.onEscape      = onEscape
        v.toolCursor    = cursor
        v.surfaceDimensionsProvider = surfaceDimensionsProvider
        v.onSurfacePixelSizeChanged = onSurfacePixelSizeChanged
        // Order matters: setting production first adds its sublayer
        // BENEATH the interactive sublayer (CALayer.addSublayer
        // appends to the end of `sublayers`, painting on top).
        v.productionEDRRenderer  = productionEDRRenderer
        v.interactiveEDRRenderer = interactiveEDRRenderer
        return v
    }

    func updateNSView(_ nsView: ViewportNSView, context: Context) {
        nsView.image = image
        nsView.onPointerDown = onPointerDown
        nsView.onPointerMove = onPointerMove
        nsView.onPointerUp   = onPointerUp
        nsView.onEscape      = onEscape
        nsView.toolCursor = cursor
        nsView.surfaceDimensionsProvider = surfaceDimensionsProvider
        nsView.onSurfacePixelSizeChanged = onSurfacePixelSizeChanged
        nsView.productionEDRRenderer  = productionEDRRenderer
        nsView.interactiveEDRRenderer = interactiveEDRRenderer
        // Force AppKit to recompute the cursor rect so the new tool's
        // cursor takes effect immediately even if the pointer is still
        // hovering over the view.
        nsView.window?.invalidateCursorRects(for: nsView)
        nsView.needsDisplay = true
    }

    static func dismantleNSView(_ nsView: ViewportNSView, coordinator: ()) {
        // Drop EDR attachments cleanly so renderer layer-refs
        // nil-resolve before SwiftUI tears down our NSView.
        nsView.productionEDRRenderer  = nil
        nsView.interactiveEDRRenderer = nil
        nsView.onSurfacePixelSizeChanged = nil
    }
}

/// NSView that displays an NSImage and forwards pointer events.
final class ViewportNSView: NSView {
    var image: NSImage? {
        didSet {
            // Image dimensions / aspect changed → cursor rect needs to
            // be recomputed (the rect tracks the aspect-fit draw area,
            // not the full view bounds).
            window?.invalidateCursorRects(for: self)
            // L5a round-5: re-layout EDR sublayers at the new
            // image's aspect-fit drawRect.
            if hasAnyEDRLayer {
                needsLayout = true
                needsDisplay = true
            }
            reportPixelSizeIfNeeded()
        }
    }
    var onPointerDown: ((CGPoint) -> Void)?
    var onPointerMove: ((CGPoint) -> Void)?
    var onPointerUp:   ((CGPoint) -> Void)?
    var onSurfacePixelSizeChanged: ((CGSize) -> Bool)? {
        didSet {
            if onSurfacePixelSizeChanged == nil { stopSurfaceRetryTimer() }
            reportPixelSizeIfNeeded()
        }
    }
    private var lastReportedPixelSize: CGSize = .zero
    private var surfaceRetryTimer: Timer?
    /// UI redesign design brief A4 — Esc key-down while this view is
    /// (or becomes) first responder.  See `keyDown(with:)`.
    var onEscape: (() -> Void)?

    /// L5a round-6 — production + interactive EDR renderers BOTH
    /// drive a single shared CAMetalLayer ('m_edrLayer').  The
    /// architectural separation (two VFSes with independent
    /// lifecycles) lives in the bridge layer; on screen we show
    /// ONE unified surface (latest-renderer-wins, matching the
    /// legacy NSImage path's UX).  The earlier round-5 design
    /// stacked two layers and showed the interactive overlay's
    /// alpha=0 regions revealing production underneath — visually
    /// the user saw the interactive layer "punching through" to
    /// production.  Round-6 reverts to a single layer with opaque
    /// composition; both renderers configure the layer
    /// idempotently (same settings) and present into successive
    /// drawables, with `cmd.present(drawable)` ordering naturally
    /// giving "latest write wins".
    var productionEDRRenderer: MetalEDRRenderer? {
        didSet {
            if oldValue !== productionEDRRenderer {
                updateEDRLayer()
            }
        }
    }
    var interactiveEDRRenderer: MetalEDRRenderer? {
        didSet {
            if oldValue !== interactiveEDRRenderer {
                updateEDRLayer()
            }
        }
    }
    private var edrLayer: CAMetalLayer? = nil

    private func updateEDRLayer() {
        let active = productionEDRRenderer != nil
                     || interactiveEDRRenderer != nil
        if active {
            if !wantsLayer {
                wantsLayer = true
            }
            if edrLayer == nil {
                let m = CAMetalLayer()
                edrLayer = m
                layer?.addSublayer(m)
            }
            if let m = edrLayer {
                layoutEDRLayer(m)
                // Both renderers attach to the same layer.  Layer
                // config is idempotent (rgba16Float, extended-
                // linear-sRGB, EDR-aware, opaque); the second
                // attach() applies the same settings without harm.
                productionEDRRenderer?.attach(layer: m)
                interactiveEDRRenderer?.attach(layer: m)
            }
            needsDisplay = true
        } else {
            // Both renderers are nil → drop the layer.
            edrLayer?.removeFromSuperlayer()
            edrLayer = nil
            needsDisplay = true
        }
    }

    private var hasAnyEDRLayer: Bool {
        edrLayer != nil
    }

    private func layoutEDRLayer(_ m: CAMetalLayer) {
        // Disable implicit animations so the layer snaps to the
        // new frame rather than tweening — important during scene
        // load when the image dims (and therefore drawRect) change.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let target = currentImageDrawRect() ?? bounds
        m.frame = target
        // Drawable size in physical pixels (Retina backing).
        let scale = window?.backingScaleFactor ?? 1.0
        let drawableSize = CGSize(width: max(1, target.width * scale),
                                  height: max(1, target.height * scale))
        if m.drawableSize != drawableSize {
            m.drawableSize = drawableSize
        }
        CATransaction.commit()
    }

    override func layout() {
        super.layout()
        if let m = edrLayer { layoutEDRLayer(m) }
        reportPixelSizeIfNeeded()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        reportPixelSizeIfNeeded()
    }

    /// Returns the camera's stable full-resolution dimensions so
    /// `surfacePoint` converts to a coord space that doesn't drift
    /// with the preview-scale state machine.  When the closure
    /// returns (0,0) (e.g. before the bridge is wired) we fall back
    /// to `image.size` — same as the old behaviour, harmless because
    /// no rendering / dragging is yet underway.
    var surfaceDimensionsProvider: (() -> NSSize)? {
        didSet { reportPixelSizeIfNeeded() }
    }

    /// Cursor displayed when the pointer is over the rendered image
    /// area of this view.  Outside the image (the empty surround
    /// produced by aspect-fit), AppKit reverts to whatever cursor
    /// the parent / window provides — typically the system arrow.
    /// Set by ViewportCanvas.updateNSView whenever the toolbar's
    /// selection changes; the view invalidates its cursor rects on
    /// tool change so the new cursor takes effect immediately.
    var toolCursor: NSCursor = .arrow

    override var isFlipped: Bool { true }   // top-left origin like UIKit / Metal

    /// Pick the stable source dims used for aspect-fit math. Prefer the
    /// camera/Film dimensions: adaptive preview divisors round W/H
    /// independently, so transient NSImage sizes can have a slightly
    /// different aspect and must never ratchet the reported surface away
    /// from the stable Film. Fall back to NSImage only before the bridge is
    /// ready.
    ///
    /// Why the fallback matters with EDR on: the production HDR
    /// block fires when EDR Preview is enabled, and the production
    /// LDR block (which would have set `renderedImage` →
    /// ViewportNSView.image) is suppressed at the bridge.  Until
    /// the interactive viewport fires its first NSImage block
    /// after a render starts/finishes, `image` is nil — and the
    /// pre-round-7 code returned nil from this helper, which
    /// (a) caused `surfacePoint(from:)` to drop every pointer
    /// event silently (so the toolbar's tools couldn't engage in
    /// edit mode after cancel), and (b) caused `layoutEDRLayer`
    /// to size the Metal layer at full bounds (no aspect-fit) →
    /// stretched preview.  The camera's dims survive the EDR
    /// toggle; using them as a fallback keeps both flows working.
    private func currentSourceDims() -> NSSize? {
        if let dims = surfaceDimensionsProvider?(),
           dims.width > 0,
           dims.height > 0 {
            return dims
        }
        if let image = image,
           image.size.width > 0,
           image.size.height > 0 {
            return image.size
        }
        return nil
    }

    /// Compute the aspect-fit rect that `draw(_:)` will use for the
    /// current source-dims inside the current bounds.  Returns nil
    /// when there are no source dims — the cursor rect collapses
    /// to nothing and the default system cursor wins.
    private func currentImageDrawRect() -> NSRect? {
        guard let imgSize = currentSourceDims() else { return nil }
        let scale = min(bounds.width / imgSize.width, bounds.height / imgSize.height)
        let drawW = imgSize.width * scale
        let drawH = imgSize.height * scale
        return NSRect(x: (bounds.width - drawW) / 2,
                      y: (bounds.height - drawH) / 2,
                      width: drawW, height: drawH)
    }

    private func reportPixelSizeIfNeeded() {
        guard let drawRect = currentImageDrawRect(), drawRect.width > 0, drawRect.height > 0 else { return }
        let backingScale = window?.backingScaleFactor ?? 2.0
        let size = CGSize(
            width: max(1, (drawRect.width * backingScale).rounded()),
            height: max(1, (drawRect.height * backingScale).rounded()))
        guard size != lastReportedPixelSize else { return }
        if onSurfacePixelSizeChanged?(size) == true {
            lastReportedPixelSize = size
            stopSurfaceRetryTimer()
        } else if onSurfacePixelSizeChanged != nil {
            startSurfaceRetryTimer()
        }
    }

    private func startSurfaceRetryTimer() {
        guard surfaceRetryTimer == nil else { return }
        let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.reportPixelSizeIfNeeded()
        }
        surfaceRetryTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func stopSurfaceRetryTimer() {
        surfaceRetryTimer?.invalidate()
        surfaceRetryTimer = nil
    }

    deinit {
        stopSurfaceRetryTimer()
    }

    override func resetCursorRects() {
        // Cursor rect tracks the aspect-fit draw area only.  The
        // empty letterbox / pillarbox surround inherits the parent's
        // cursor (system arrow) — that's the "moves outside the
        // manipulation area" behaviour the user expects.
        if let drawRect = currentImageDrawRect() {
            addCursorRect(drawRect, cursor: toolCursor)
        }
    }

    override func resize(withOldSuperviewSize oldSize: NSSize) {
        super.resize(withOldSuperviewSize: oldSize)
        // The aspect-fit rect depends on view bounds; invalidate so
        // resetCursorRects re-runs with the new geometry.
        window?.invalidateCursorRects(for: self)
        reportPixelSizeIfNeeded()
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.windowBackgroundColor.setFill()
        bounds.fill()

        // L5a round-5: when ANY EDR layer is active (production OR
        // interactive), skip the NSImage draw — the Metal layer(s)
        // cover the aspect-fit drawRect.  Drawing NSImage underneath
        // would either be invisible (production opaque) or flash
        // briefly before the GPU pass overlays it.
        if productionEDRRenderer != nil || interactiveEDRRenderer != nil {
            return
        }

        guard let image = image else {
            // Placeholder — Phase 3 has no live preview yet.
            let para = NSMutableParagraphStyle()
            para.alignment = .center
            let attrs: [NSAttributedString.Key: Any] = [
                .foregroundColor: NSColor.secondaryLabelColor,
                .font: NSFont.systemFont(ofSize: 13),
                .paragraphStyle: para,
            ]
            let str = "Render to see the scene"
            let size = (str as NSString).size(withAttributes: attrs)
            let r = NSRect(x: bounds.midX - size.width/2,
                           y: bounds.midY - size.height/2,
                           width: size.width, height: size.height)
            (str as NSString).draw(in: r, withAttributes: attrs)
            return
        }

        // Aspect-fit the image into bounds (kept in sync with
        // currentImageDrawRect() above — both must produce the same
        // rect or the cursor will land somewhere the image isn't).
        guard let drawRect = currentImageDrawRect() else { return }
        image.draw(in: drawRect,
                   from: .zero,
                   operation: .sourceOver,
                   fraction: 1.0,
                   respectFlipped: true,
                   hints: [.interpolation: NSImageInterpolation.none.rawValue])
    }

    /// Convert a window-coordinate event to image-pixel space — i.e.
    /// the same coord system the rasterizer / camera use internally
    /// (0..GetWidth() × 0..GetHeight()).  Two adjustments map view
    /// points to image pixels:
    ///
    ///   1. Aspect-fit offset: the image is drawn centred in the view
    ///      with letterbox / pillarbox margins.  Subtract drawRect's
    ///      origin so view-relative coords become image-relative.
    ///   2. Pixel-density rescale: the view's draw-rect width is in
    ///      view points; the image's width is in rasterizer pixels.
    ///      Divide by drawRect.size and multiply by image.size to
    ///      land in pixel space.
    ///
    /// HiDPI is handled implicitly because the rescale step uses
    /// drawRect (in points) and image.size (in pixels), not the
    /// backing-scale factor — both ratios bake in the right number.
    /// Returns nil when there's no image to map against.  Coords may
    /// land outside [0, image.size] if the user drags past the
    /// image's edge — the controller treats that as "no hit" for
    /// picking and as a clamped delta for orbit / pan / zoom.  This
    /// SAME conversion is what ViewportView's region-of-interest
    /// drag consumes (via onPointerDown/Move/Up's `p` argument) to
    /// land in full-res film-pixel space — see ViewportView's
    /// `commitRegionDrag`, which deliberately does NOT re-derive a
    /// second mapping.
    private func surfacePoint(from event: NSEvent) -> CGPoint? {
        let p = self.convert(event.locationInWindow, from: nil)
        guard let drawRect = currentImageDrawRect() else { return nil }
        guard drawRect.width > 0, drawRect.height > 0 else { return nil }

        // Use the camera's STABLE full-resolution dimensions — not
        // image.size — as the conversion target.  During a fast
        // drag, image.size shrinks to the preview-scale subsample
        // dims (e.g. 200×200 from a 800×800 camera at scale=4) and
        // grows back to full-res when motion adapts down.  If
        // surfacePoint reported coords in image.size space, mLastPx
        // captured at one scale level would be in a different coord
        // system from the next event's px — the controller's
        // (px - mLastPx) delta would mix scale levels and produce
        // 4×–32× pan/orbit jumps whenever the state machine steps.
        // The bridge's cameraSurfaceDimensions reads from the
        // controller's mFullResW/H which DO NOT flicker, so the
        // delta is stable and the camera tunings (×0.0087 rad/px
        // etc.) behave consistently independent of subsample state.
        //
        // L5a round-7 — fall back to image.size THEN to a generic
        // bounds-based mapping if neither provider nor image are
        // available.  The provider is the right answer for almost
        // every real flow; image-fallback covers the immediate-
        // post-init window before the bridge wires the provider;
        // the final bounds fallback (just normalised 0..1 coords)
        // ensures clicks are at least propagated rather than
        // silently dropped.  With EDR on, image may be nil while
        // the production rasterizer's HDR-only block is firing
        // (production LDR block is suppressed → no NSImage update);
        // pre-round-7 the early-return on nil image silently
        // discarded every pointer event in that window.
        let providedDims = surfaceDimensionsProvider?() ?? .zero
        let surfaceSize: NSSize
        if providedDims.width > 0 && providedDims.height > 0 {
            surfaceSize = providedDims
        } else if let image = image,
                  image.size.width > 0, image.size.height > 0 {
            surfaceSize = image.size
        } else {
            return nil
        }

        let nx = (p.x - drawRect.minX) / drawRect.width
        let ny = (p.y - drawRect.minY) / drawRect.height
        return CGPoint(x: nx * surfaceSize.width, y: ny * surfaceSize.height)
    }

    override func mouseDown(with event: NSEvent) {
        // Claim first responder so keyDown (Esc, design brief A4)
        // reaches this view for the remainder of the interaction —
        // AppKit doesn't do this automatically on click.
        window?.makeFirstResponder(self)
        guard let p = surfacePoint(from: event) else { return }
        onPointerDown?(p)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let p = surfacePoint(from: event) else { return }
        onPointerMove?(p)
    }

    override func mouseUp(with event: NSEvent) {
        guard let p = surfacePoint(from: event) else { return }
        onPointerUp?(p)
    }

    override func keyDown(with event: NSEvent) {
        // 53 = kVK_Escape.  Anything else falls through to the
        // default responder chain unchanged.
        if event.keyCode == 53, let onEscape {
            onEscape()
            return
        }
        super.keyDown(with: event)
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var acceptsFirstResponder: Bool { true }
}
