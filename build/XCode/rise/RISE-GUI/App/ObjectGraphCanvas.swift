//
//  ObjectGraphCanvas.swift
//  RISE-GUI
//
//  S2 (Mac shell): the "Object Graph" left-panel tab, sibling of the
//  Painter/Material canvas (`NodeGraphCanvas.swift`, now labeled
//  "Material Graph" in the tab strip). Shows the OBJECT hierarchy down to
//  geometry -- SceneEditController::ReadObjectGraph/ReadObjectGraphLaidOut/
//  ReadObjectGraphLaidOutFocused (src/Library/SceneEditor/SceneEditController.h,
//  the S1 core slice), surfaced through `-[RISEViewportBridge objectGraph]`
//  / `-objectGraphFocused(forObject:)`.
//
//  PHASE 1 SCOPE, READ-ONLY -- deliberately NOT a copy of every
//  NodeGraphCanvas feature:
//    - pan/zoom, node cards (name, chunk keyword, a "×N" repeat badge
//      when `repeatCount >= 1` -- see `ObjGraphNodeBoxView.repeatBadge`),
//      ports/edges drawn the SAME way the Material canvas draws them
//      (a Canvas-drawn bezier wire layer under real SwiftUI node boxes,
//      one coordinate space, no separate bookkeeping).
//    - All/Focused view-scope toggle with the SAME STICKY-OBJECT policy
//      NodeGraphCanvas.swift's own `updateStickyFocusObject`/
//      `currentFocusTarget` establish: the focus target is the last
//      EXTERNALLY-selected Object (viewport/outliner, via
//      `selectionRowName`), sticky across a canvas click that does not
//      itself select an Object, reset on a scene switch (detected via
//      the bridge's `ObjectIdentifier`, same verified reasoning that
//      file's own `stickyFocusBridgeID` comment documents), degraded
//      resolves keep last-good content and retry on a bounded timer.
//      Ported near-verbatim rather than re-derived, on purpose -- two
//      independently-invented sticky-memo implementations would be two
//      places for the SAME review-round bug class to recur.
//    - a spotlight for the externally-picked object's OWN node -- NOT a
//      multi-node closure the way the Material canvas's spotlight is
//      (that canvas's "All" view shows every painter/material in the
//      scene regardless of which object is selected, so its spotlight
//      needs to pull in a whole appearance closure to mean anything;
//      THIS canvas's "All" view already shows every object, so the
//      spotlight only needs to say "you are looking at THIS one" --
//      Focused mode is what pulls in the wider neighborhood, on demand).
//    - click-to-select routes to the shared selection (`-setSelection`),
//      so the properties panel follows: category Object for an
//      Object-category node, Geometry for a Geometry-category node (both
//      addressable via `RISEViewportCategory` -- `.geometry` is already
//      a real, selectable outliner section, `OutlinerView.swift`'s own
//      category table confirms).
//
//  EXPLICITLY OUT OF SCOPE THIS PHASE (documented, not an oversight):
//    - Double-click does nothing -- no def-focus/edit affordance exists
//      for an object or geometry node yet.
//    - NO node dragging at all -- layout is ALWAYS transient
//      (`ReadObjectGraphLaidOut`'s own header comment: this graph has no
//      `.risegraph.json` sidecar partition to write into safely), so
//      there is nothing a drag could usefully persist; rather than wire
//      up a drag gesture that silently discards its own result, this
//      phase omits node dragging entirely. A future slice that wants
//      persisted Object Graph layout needs a sidecar format revision
//      first (see the C++ header comment).
//    - No add/rewire/delete/duplicate -- this is a READ surface only.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//

import SwiftUI
import AppKit

// MARK: - Snapshot model (Swift-native copy of one -objectGraph /
// -objectGraphFocused(forObject:) call -- same decode-once-per-fetch
// discipline NodeGraphCanvas.swift's own GraphCanvasNode/Port documents,
// so the rest of this file never re-touches the bridge's NSArray/NSObject
// wrappers)

private struct ObjGraphCanvasPort {
    let paramName: String
    let occurrence: Int
    /// -1 for a node-less (dangling, or out-of-modeled-category) port.
    let otherNodeIndex: Int
    let otherName: String
}

private struct ObjGraphCanvasNode: Identifiable {
    var id: UInt64 { handle }
    let handle: UInt64
    /// This node's position in the snapshot's `nodes` array -- what
    /// `ObjGraphCanvasPort.otherNodeIndex` indexes into.
    let index: Int
    let name: String
    let chunkKeyword: String
    /// RISE::ChunkCategory ordinal -- Geometry=5, Object=8 (ChunkDescriptor.h;
    /// a `rect_light`/`shape_light` node already arrives re-categorized as
    /// Object=8 by `BuildObjectGraphSeedsLocked_`, so this file never needs
    /// to special-case those keywords itself).
    let category: Int
    /// A `standard_object`'s `count_u * count_v` repeat sugar; 0 when this
    /// chunk carries no counts at all. See `SceneEditController::
    /// GraphNode::repeatCount`'s own header comment for the accepted
    /// `count_u 0` ambiguity (both publish 0 here).
    let repeatCount: Int
    let position: CGPoint
    let outEdges: [ObjGraphCanvasPort]
    let inEdges: [ObjGraphCanvasPort]
}

private struct ObjGraphCanvasSnapshot {
    var nodes: [ObjGraphCanvasNode] = []
    var generation: UInt64 = 0
}

private enum ObjGraphMetrics {
    static let nodeWidth: CGFloat = 160
    static let nodeHeight: CGFloat = 64
    static let headerHeight: CGFloat = 22
    static let padding: CGFloat = 80
    /// Below this zoom level, port labels are dropped -- same rationale
    /// (and same numeric threshold, for visual parity across the two
    /// canvases) as NodeGraphCanvas.swift's own `labelZoomThreshold`.
    static let labelZoomThreshold: CGFloat = 0.55
    static let minScale: CGFloat = 0.2
    static let maxScale: CGFloat = 2.5
}

/// User-facing labels "All"/"Focused" -- same segmented-Picker idiom
/// NodeGraphCanvas.swift's own `GraphViewScope` uses. A SEPARATE type
/// (not a reuse of that one) because both are `private` to their own
/// file -- Swift `private` is file-scoped, so there is nothing to share
/// even if the two enums are structurally identical.
private enum ObjGraphViewScope: String, CaseIterable, Identifiable {
    case all, focused
    var id: String { rawValue }
    var label: String { self == .all ? "All" : "Focused" }
}

private extension Comparable {
    func clamped(to range: ClosedRange<Self>) -> Self {
        min(max(self, range.lowerBound), range.upperBound)
    }
}

/// Category-tinted header color. Geometry gets `Theme.teal` (otherwise
/// unused in this canvas, unlike the Material canvas which reserves it
/// for Painter); Object (and any re-categorized light-sugar node) gets
/// `Theme.catObject` -- the SAME identity color `OutlinerView.swift`
/// already uses for the Objects section, so a node here and its row in
/// the outliner read as the same category at a glance.
private func objGraphCategoryTint(_ category: Int) -> Color {
    category == 5 ? Theme.teal : Theme.catObject   // 5 = Geometry
}
private func objGraphCategoryLabel(_ category: Int) -> String {
    category == 5 ? "Geometry" : "Object"
}

private func objGraphInputPortAnchors(nodePosition: CGPoint, outEdgeCount: Int) -> [CGFloat] {
    let count = max(outEdgeCount, 1)
    let top = nodePosition.y + ObjGraphMetrics.headerHeight + 6
    let bottom = nodePosition.y + ObjGraphMetrics.nodeHeight - 6
    guard count > 1 else { return [nodePosition.y + ObjGraphMetrics.nodeHeight / 2] }
    let step = (bottom - top) / CGFloat(count - 1)
    return (0..<count).map { top + CGFloat($0) * step }
}

// MARK: - Canvas

struct ObjectGraphCanvas: View {
    /// Weak — same lifetime discipline as NodeGraphCanvas/OutlinerView/
    /// PropertiesPanel: this view must never keep a torn-down bridge
    /// alive past its scene.
    weak var bridge: RISEViewportBridge?
    @Binding var refreshTrigger: Int

    @State private var snapshot = ObjGraphCanvasSnapshot()
    @State private var lastFetchedEpoch: Int = -1
    @State private var reloadWorkItem: DispatchWorkItem?

    @State private var viewScope: ObjGraphViewScope = .all
    /// Bounded retry for a DEGRADED focused-view fetch (a render owns the
    /// scene) -- same cancel-and-reschedule idiom
    /// NodeGraphCanvas.focusedRetryWorkItem uses.
    @State private var focusedRetryWorkItem: DispatchWorkItem?
    /// Sticky focus memo -- see this file's own header comment and
    /// NodeGraphCanvas.swift's `stickyFocusObjectName`/`stickyFocusBridgeID`
    /// for the full rationale this ports.
    @State private var stickyFocusBridgeID: ObjectIdentifier? = nil
    @State private var stickyFocusObjectName: String? = nil

    @State private var scale: CGFloat = 1.0
    @State private var offset: CGSize = .zero
    @GestureState private var dragDelta: CGSize = .zero
    @GestureState private var magnifyDelta: CGFloat = 1.0
    @State private var hasCentered = false
    @State private var lastViewportSize: CGSize = .zero

    @State private var selectedHandle: UInt64? = nil
    @State private var hoveredHandle: UInt64? = nil
    /// The SINGLE node matching the current external Object selection --
    /// not a closure (see this file's header comment for why this
    /// canvas's spotlight is a one-node affair, unlike the Material
    /// canvas's appearance-closure spotlight).
    @State private var spotlightHandle: UInt64? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
            GeometryReader { geo in
                ZStack(alignment: .topLeading) {
                    Theme.bgWell
                    canvasContent
                        .scaleEffect(effectiveScale, anchor: .topLeading)
                        .offset(effectiveOffset)
                }
                .clipped()
                .contentShape(Rectangle())
                .gesture(panGesture)
                .simultaneousGesture(zoomGesture)
                .onAppear {
                    centerIfNeeded(in: geo.size)
                    lastViewportSize = geo.size
                }
                .onChange(of: snapshot.generation) { _, _ in centerIfNeeded(in: geo.size) }
                .onChange(of: geo.size) { _, newSize in lastViewportSize = newSize }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .clipShape(RoundedRectangle(cornerRadius: 0))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .onAppear { scheduleReload(debounced: false) }
        .onChange(of: refreshTrigger) { _, _ in
            scheduleReload(debounced: true)
            // Same immediate-clear reasoning NodeGraphCanvas.swift's own
            // `.onChange(of: refreshTrigger)` documents: the debounced
            // reload above lags ~250ms, but a plain selection change
            // needs the spotlight (and, in Focused mode, the whole
            // subgraph) to update immediately.
            refreshSpotlight()
            if viewScope == .focused { performReload(force: true) }
        }
        .onChange(of: viewScope) { _, _ in
            scheduleReload(debounced: false, force: true)
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 8) {
            Text("Object Graph").font(Theme.sans(12.5, .semibold)).foregroundColor(Theme.textPrimary)
            Text("\(snapshot.nodes.count) node\(snapshot.nodes.count == 1 ? "" : "s"), \(edgeCount) edge\(edgeCount == 1 ? "" : "s")")
                .font(Theme.mono(10))
                .foregroundColor(Theme.textDim)
            Spacer(minLength: 4)
            Picker("", selection: $viewScope) {
                ForEach(ObjGraphViewScope.allCases) { scope in
                    Text(scope.label).tag(scope)
                }
            }
            .pickerStyle(.segmented)
            .frame(width: 140)
            .help("All: every object and geometry chunk. Focused: the selected object's parents, children, and geometry only.")
            Button {
                scheduleReload(debounced: false, force: true)
            } label: {
                Image(systemName: "arrow.clockwise").font(.system(size: 11))
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.textDim)
            .help("Refresh graph")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    private var edgeCount: Int {
        snapshot.nodes.reduce(0) { $0 + $1.outEdges.filter { $0.otherNodeIndex >= 0 }.count }
    }

    // MARK: - Canvas content

    private var contentSize: CGSize {
        guard !snapshot.nodes.isEmpty else { return CGSize(width: 400, height: 300) }
        let maxX = snapshot.nodes.map { $0.position.x + ObjGraphMetrics.nodeWidth }.max() ?? 400
        let maxY = snapshot.nodes.map { $0.position.y + ObjGraphMetrics.nodeHeight }.max() ?? 300
        return CGSize(width: maxX + ObjGraphMetrics.padding, height: maxY + ObjGraphMetrics.padding)
    }

    private var canvasContent: some View {
        ZStack(alignment: .topLeading) {
            ObjGraphWiresLayer(nodes: snapshot.nodes, showLabels: scale >= ObjGraphMetrics.labelZoomThreshold)
                .frame(width: contentSize.width, height: contentSize.height)

            ForEach(snapshot.nodes) { node in
                ObjGraphNodeBoxView(
                    node: node,
                    isSelected: selectedHandle == node.handle,
                    isSpotlit: spotlightHandle == node.handle,
                    isHovered: hoveredHandle == node.handle,
                    onSelect: { selectNode(node) },
                    onHover: { hovering in hoveredHandle = hovering ? node.handle : (hoveredHandle == node.handle ? nil : hoveredHandle) }
                )
                .position(x: node.position.x + ObjGraphMetrics.nodeWidth / 2,
                          y: node.position.y + ObjGraphMetrics.nodeHeight / 2)
            }
        }
        .frame(width: contentSize.width, height: contentSize.height, alignment: .topLeading)
    }

    // MARK: - Pan / zoom (identical shape to NodeGraphCanvas.swift's own --
    // no node-drag gesture exists in THIS canvas to contend with the pan
    // gesture, so there is nothing analogous to that file's "a drag that
    // starts ON a node is consumed by that node's own gesture first" note)

    private var effectiveScale: CGFloat {
        (scale * magnifyDelta).clamped(to: ObjGraphMetrics.minScale...ObjGraphMetrics.maxScale)
    }
    private var effectiveOffset: CGSize {
        CGSize(width: offset.width + dragDelta.width, height: offset.height + dragDelta.height)
    }
    private var panGesture: some Gesture {
        DragGesture(minimumDistance: 2)
            .updating($dragDelta) { value, state, _ in state = value.translation }
            .onEnded { value in
                offset.width += value.translation.width
                offset.height += value.translation.height
            }
    }
    private var zoomGesture: some Gesture {
        MagnificationGesture()
            .updating($magnifyDelta) { value, state, _ in state = value }
            .onEnded { value in
                scale = (scale * value).clamped(to: ObjGraphMetrics.minScale...ObjGraphMetrics.maxScale)
            }
    }
    private func centerIfNeeded(in size: CGSize) {
        guard !hasCentered, !snapshot.nodes.isEmpty, size.width > 1, size.height > 1 else { return }
        hasCentered = true
        let cs = contentSize
        if cs.width <= size.width && cs.height <= size.height {
            offset = CGSize(width: (size.width - cs.width) / 2, height: (size.height - cs.height) / 2)
        } else {
            offset = .zero
        }
    }

    // MARK: - Selection (click-to-select only -- no double-click, no drag,
    // per this file's own "explicitly out of scope this phase" note)

    /// RISE::ChunkCategory ordinals (ChunkDescriptor.h) -- the SAME "cast
    /// the parser's enum" convention `ObjGraphCanvasNode.category` already
    /// uses.
    private static let kChunkCategoryGeometry = 5
    private static let kChunkCategoryObject = 8

    private func selectNode(_ node: ObjGraphCanvasNode) {
        guard let bridge else { return }
        let cat: RISEViewportCategory = (node.category == Self.kChunkCategoryGeometry) ? .geometry : .object
        _ = bridge.setSelection(cat, name: node.name)
        selectedHandle = node.handle
        refreshTrigger &+= 1
    }

    // MARK: - Spotlight (one node, not a closure -- see this file's own
    // header comment)

    private func refreshSpotlight() {
        guard let bridge, bridge.selectionCategory == .object else {
            spotlightHandle = nil
            return
        }
        // selectionRowName, NOT selectionName: a viewport/outliner pick on
        // a count_u/count_v repeated or subtree-cloned instance names a
        // SYNTHESIZED per-repetition/per-member entity, which resolves
        // back to the AUTHORING chunk's own row name here (the same
        // "resolve back to the instancing chunk" NodeGraphCanvas.swift's
        // identical spotlight comment documents).
        let rowName = bridge.selectionRowName
        guard !rowName.isEmpty else { spotlightHandle = nil; return }
        spotlightHandle = snapshot.nodes.first(where: { $0.name == rowName && $0.category == Self.kChunkCategoryObject })?.handle
    }

    // MARK: - Reload

    private func scheduleReload(debounced: Bool, force: Bool = false) {
        reloadWorkItem?.cancel()
        guard debounced else {
            performReload(force: force)
            return
        }
        let item = DispatchWorkItem { performReload(force: force) }
        reloadWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: item)
    }

    private func nodesFrom(_ g: RISEPainterMaterialGraph) -> [ObjGraphCanvasNode] {
        g.nodes.enumerated().map { idx, n in
            ObjGraphCanvasNode(
                handle: n.handle,
                index: idx,
                name: n.name,
                chunkKeyword: n.chunkKeyword,
                category: n.category,
                repeatCount: n.repeatCount,
                position: CGPoint(x: n.x, y: n.y),
                outEdges: n.outEdges.map {
                    ObjGraphCanvasPort(paramName: $0.paramName, occurrence: $0.occurrence,
                                        otherNodeIndex: $0.otherNodeIndex, otherName: $0.otherName)
                },
                inEdges: n.inEdges.map {
                    ObjGraphCanvasPort(paramName: $0.paramName, occurrence: $0.occurrence,
                                        otherNodeIndex: $0.otherNodeIndex, otherName: $0.otherName)
                }
            )
        }
    }

    /// Folds a freshly fetched node array into `snapshot`, dropping a
    /// vanished selection and re-deriving the spotlight -- the common tail
    /// every fetch path shares. `snapshot` is updated BEFORE
    /// `refreshSpotlight()` runs, so that call reads the fresh array
    /// directly off `snapshot.nodes` -- no separate `freshNodes` parameter
    /// needed the way NodeGraphCanvas.swift's own two-tier gate requires
    /// (this canvas has no per-frame poll to guard against -- see the
    /// sticky-memo comment on that established Mac/Qt asymmetry).
    private func applyFetchedNodes(_ nodes: [ObjGraphCanvasNode], generation: UInt64) {
        if let sel = selectedHandle, !nodes.contains(where: { $0.handle == sel }) {
            selectedHandle = nil
        }
        snapshot = ObjGraphCanvasSnapshot(nodes: nodes, generation: generation)
        refreshSpotlight()
    }

    private func performReload(force: Bool) {
        guard let bridge else { return }
        // MUST run before anything below consults the sticky focus memo.
        updateStickyFocusObject(bridge: bridge)
        let epoch = Int(bridge.sceneEpoch)

        if viewScope == .focused {
            // Focused mode NEVER epoch-gates -- a pure selection change
            // needs a fresh focused fetch, and a selection change never
            // bumps sceneEpoch (only a structural mutation does). Same
            // reasoning NodeGraphCanvas.performReload documents.
            lastFetchedEpoch = epoch
            performFocusedReload(bridge: bridge)
            return
        }

        guard force || epoch != lastFetchedEpoch else { return }
        lastFetchedEpoch = epoch
        let g = bridge.objectGraph()
        applyFetchedNodes(nodesFrom(g), generation: g.generation)
    }

    /// Identifies which SCENE the sticky memo belongs to, so a scene
    /// (re)load while the user stays on the Object Graph tab can't leak a
    /// stale object name from the PREVIOUS scene into the new one -- same
    /// verified reasoning NodeGraphCanvas.swift's own `stickyFocusBridgeID`
    /// documents (`ChatViewModel.sceneOpened` mints a brand-new
    /// `RISEViewportBridge` per scene; this view's own `@State` can
    /// survive a scene switch while staying on this tab, since only the
    /// `bridge` parameter's VALUE changes).
    private func updateStickyFocusObject(bridge: RISEViewportBridge) {
        let bridgeID = ObjectIdentifier(bridge)
        if stickyFocusBridgeID != bridgeID {
            stickyFocusBridgeID = bridgeID
            stickyFocusObjectName = nil
        }
        // Updates ONLY when the CURRENT shared selection is actually an
        // Object -- a canvas-node click sets the shared selection to
        // Object OR Geometry (`selectNode` above); a Geometry pick is
        // deliberately ignored here (leaves the memo untouched, i.e.
        // sticky), while an Object pick IS honored -- clicking an object
        // node in this canvas is exactly "focus on this object", the
        // same gesture picking it in the outliner would be. That
        // asymmetry falls out of this one guard with no extra code: it
        // is the natural analogue of NodeGraphCanvas's own "a
        // Painter/Material click never retargets Focused mode" rule,
        // adapted to a canvas whose OWN nodes can themselves be valid
        // focus roots.
        guard bridge.selectionCategory == .object else { return }
        let n = bridge.selectionRowName
        stickyFocusObjectName = n.isEmpty ? nil : n
    }

    private func currentFocusTarget() -> String? { stickyFocusObjectName }

    private func performFocusedReload(bridge: RISEViewportBridge) {
        guard let target = currentFocusTarget() else {
            // No sticky object -- fall back to showing ALL nodes rather
            // than an empty canvas (design decision, distinct from the
            // degraded case below, which must NOT fall back).
            focusedRetryWorkItem?.cancel()
            focusedRetryWorkItem = nil
            let g = bridge.objectGraph()
            applyFetchedNodes(nodesFrom(g), generation: g.generation)
            return
        }
        guard let g = bridge.objectGraphFocused(forObject: target) else {
            // DEGRADED (a render owns the scene): keep showing whatever is
            // CURRENTLY on screen, untouched, and retry once ~0.5s out --
            // same bounded cancel-and-reschedule idiom NodeGraphCanvas's
            // own focusedRetryWorkItem uses.
            focusedRetryWorkItem?.cancel()
            let retry = DispatchWorkItem { performReload(force: true) }
            focusedRetryWorkItem = retry
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: retry)
            return
        }
        focusedRetryWorkItem?.cancel()
        focusedRetryWorkItem = nil
        // A genuinely empty result (reached here, so NOT degraded) means
        // the sticky object no longer resolves -- deleted, renamed, or
        // (rarer) has no material bound... actually for THIS graph, an
        // Object always resolves to at least itself unless it was
        // deleted/renamed -- fall back to All and CLEAR the sticky memo
        // so a dead name can't keep silently re-resolving to nothing.
        // Same design NodeGraphCanvas.performFocusedReload's own
        // empty-result branch documents.
        if g.nodes.isEmpty {
            stickyFocusObjectName = nil
            let all = bridge.objectGraph()
            applyFetchedNodes(nodesFrom(all), generation: all.generation)
            return
        }
        applyFetchedNodes(nodesFrom(g), generation: g.generation)
    }
}

// MARK: - Node box (READ-ONLY: no drag, no double-click -- see this
// file's own "explicitly out of scope this phase" note)

private struct ObjGraphNodeBoxView: View {
    let node: ObjGraphCanvasNode
    let isSelected: Bool
    /// This node matches the CURRENT external Object selection -- see
    /// ObjectGraphCanvas.refreshSpotlight's own comment.
    let isSpotlit: Bool
    let isHovered: Bool
    let onSelect: () -> Void
    let onHover: (Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            content
        }
        .frame(width: ObjGraphMetrics.nodeWidth, height: ObjGraphMetrics.nodeHeight, alignment: .top)
        .background(Theme.bgCard)
        .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMedium)
                .stroke(borderColor, lineWidth: (isSelected || isSpotlit) ? 2 : 1)
        )
        .shadow(color: .black.opacity(isHovered ? 0.28 : 0.14), radius: isHovered ? 7 : 3, y: 2)
        .shadow(color: isSpotlit ? Theme.gold.opacity(0.6) : .clear, radius: isSpotlit ? 8 : 0)
        .contentShape(Rectangle())
        // A plain tap only -- no drag gesture on this view at all (unlike
        // NodeGraphCanvas's GraphNodeBoxView), so there is no
        // `exclusively(before:)` priority dance needed: SwiftUI still
        // resolves this against the ancestor canvas's own pan `DragGesture`
        // by trying the most specific (deepest) view first, and a
        // `TapGesture` simply never recognizes a drag, so a real pan drag
        // that happens to start on a node box falls through to the
        // container's pan gesture exactly as it would for empty canvas
        // space.
        .gesture(TapGesture(count: 1).onEnded { onSelect() })
        .onHover(perform: onHover)
        .help(node.name)
    }

    private var borderColor: Color {
        if isSelected { return Theme.accent }
        if isSpotlit { return Theme.gold }
        if isHovered { return Theme.borderHover }
        return Theme.borderLight
    }
    private var tint: Color { objGraphCategoryTint(node.category) }

    private var header: some View {
        HStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 0) {
                Text(node.name)
                    .font(Theme.sans(10.5, .semibold))
                    .foregroundColor(Theme.textOnAccent)
                    .lineLimit(1)
                    .truncationMode(.tail)
                Text(node.chunkKeyword)
                    .font(Theme.mono(8.5))
                    .foregroundColor(Theme.textOnAccent.opacity(0.75))
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            Spacer(minLength: 2)
            if node.repeatCount >= 1 {
                repeatBadge
            }
        }
        .padding(.horizontal, 8)
        .frame(height: ObjGraphMetrics.headerHeight)
        .background(tint.opacity(0.85))
    }

    /// Shown for ANY `repeatCount >= 1`, including the `count_u 1` case
    /// ("×1") -- PRESENCE selects the repeated form (the param's own
    /// descriptor comment), so a `count_u 1` chunk genuinely synthesizes a
    /// `[0,0]`-named entry rather than the plain chunk name, and the badge
    /// says so honestly rather than hiding a real (if trivial) repeat.
    /// `repeatCount == 0` (uncounted, OR the accepted `count_u 0`
    /// ambiguity -- see `ObjGraphCanvasNode.repeatCount`'s own comment)
    /// shows no badge at all.
    private var repeatBadge: some View {
        Text("×\(node.repeatCount)")
            .font(Theme.mono(8, .semibold))
            .foregroundColor(Theme.textOnAccent)
            .padding(.horizontal, 4)
            .padding(.vertical, 1)
            .background(Color.black.opacity(0.28))
            .clipShape(Capsule())
            .help("This chunk synthesizes \(node.repeatCount) repetition\(node.repeatCount == 1 ? "" : "s") (count_u * count_v)")
    }

    private var content: some View {
        HStack {
            Text(objGraphCategoryLabel(node.category))
                .font(Theme.mono(8.5))
                .foregroundColor(Theme.textDim)
            Spacer(minLength: 2)
        }
        .padding(8)
    }
}

// MARK: - Wires layer (same Canvas-drawn bezier approach as
// NodeGraphCanvas.swift's own GraphWiresLayer -- ported, not re-derived,
// since both files' `private` scope means nothing here can literally
// reuse that type)

private struct ObjGraphWiresLayer: View {
    let nodes: [ObjGraphCanvasNode]
    let showLabels: Bool

    var body: some View {
        Canvas { context, _ in
            for node in nodes {
                let destPortYs = objGraphInputPortAnchors(nodePosition: node.position, outEdgeCount: node.outEdges.count)
                for (portIdx, port) in node.outEdges.enumerated() {
                    let destPoint = CGPoint(x: node.position.x, y: destPortYs[portIdx])
                    if port.otherNodeIndex >= 0, port.otherNodeIndex < nodes.count {
                        let source = nodes[port.otherNodeIndex]
                        let sourcePoint = CGPoint(x: source.position.x + ObjGraphMetrics.nodeWidth,
                                                    y: source.position.y + ObjGraphMetrics.nodeHeight / 2)
                        drawWire(context: context, from: sourcePoint, to: destPoint,
                                 color: objGraphCategoryTint(source.category).opacity(0.75))
                        if showLabels {
                            drawLabel(context: context, text: port.paramName, near: destPoint)
                        }
                    } else {
                        // Dangling / out-of-modeled-category reference: a
                        // short warning-tinted stub, no destination box.
                        let stubStart = CGPoint(x: destPoint.x - 22, y: destPoint.y)
                        drawWire(context: context, from: stubStart, to: destPoint,
                                 color: Theme.warn.opacity(0.85), dashed: true)
                        if showLabels {
                            drawLabel(context: context, text: port.paramName, near: destPoint, color: Theme.warn)
                        }
                    }
                }
            }
        }
        .allowsHitTesting(false)
    }

    private func drawWire(context: GraphicsContext, from: CGPoint, to: CGPoint, color: Color, dashed: Bool = false) {
        var path = Path()
        path.move(to: from)
        let dx = max(abs(to.x - from.x) * 0.5, 24)
        let c1 = CGPoint(x: from.x + dx, y: from.y)
        let c2 = CGPoint(x: to.x - dx, y: to.y)
        path.addCurve(to: to, control1: c1, control2: c2)
        if dashed {
            context.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
        } else {
            context.stroke(path, with: .color(color), lineWidth: 1.5)
        }
    }

    private func drawLabel(context: GraphicsContext, text: String, near point: CGPoint, color: Color = Theme.textFaint) {
        let resolved = context.resolve(Text(text).font(Theme.mono(8)).foregroundColor(color))
        context.draw(resolved, at: CGPoint(x: point.x - 4, y: point.y - 7), anchor: .trailing)
    }
}
