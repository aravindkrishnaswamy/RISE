//
//  NodeGraphCanvas.swift
//  RISE-GUI
//
//  doc-88 Phase 3 S15 (docs/gui/NODE_GRAPH_CANVAS.md sect. 5 interaction
//  minimums + sect. 6 S15): Mac read-only Painter/Material node canvas.
//  One transactional snapshot per refresh (-[RISEViewportBridge
//  painterMaterialGraph]) rendered as real SwiftUI node views (so the
//  ALREADY-SHIPPED PainterThumbnailView/RampStripThumbnailView from
//  PropertiesPanel.swift can be reused verbatim, cache and all) plus a
//  Canvas-drawn wire layer underneath, both living in the SAME
//  pan/zoom-transformed coordinate space so wires track node boxes with
//  no separate bookkeeping.
//
//  SCOPE (S15 is Phase A, read-only — docs/gui/NODE_GRAPH_CANVAS.md §5):
//  no drag-to-reposition (positions render exactly where the bridge lays
//  them out; dragging is S21), no topology editing, no chunk creation.
//  Click selects (wired to the same setSelection the outliner uses);
//  double-click on an expression-family node asks PropertiesPanel to
//  reveal its `def[i]` rows (RenderViewModel.focusPainterDefRows).
//
//  ENTRY POINT: a third left-panel tab ("Graph"), alongside the existing
//  Agent/Scene file tabs — see ContentView.leftPanelTabStrip/leftPanel
//  and RenderViewModel.LeftPanelTab. Chosen over a center-column mode
//  because the center column is a fixed viewport/N-up surface with no
//  existing "swap the whole pane" concept, whereas the left panel
//  already has exactly that idiom (two tabs, about to become three) at
//  zero new chrome cost. A pan/zoom canvas works fine in a resizable
//  360-700pt column — the interaction model doesn't assume a fixed
//  aspect the way the viewport does.
//
//  REFRESH CADENCE (S14 review note: this must never poll per frame —
//  ReadPainterMaterialGraphLaidOut is a full transactional read composing
//  S11's graph + S12's auto-layout + S13's sidecar, not a cheap getter):
//  reload happens on first appearance, on an explicit Refresh click, and
//  on `refreshTrigger` bumps (the SAME shared counter OutlinerView/
//  PropertiesPanel/EnvironmentPanel already reload on — every selection
//  change and every GUI/agent CRUD bumps it). A `refreshTrigger` bump does
//  NOT force a re-fetch by itself: `performReload` first compares
//  `bridge.sceneEpoch` (the same monotonic "structural mutation" counter
//  OutlinerView.reload's `lastEpoch` gate already uses) against the last
//  epoch actually fetched, and skips the round-trip when nothing
//  structural changed — a plain selection-only bump (the overwhelmingly
//  common case) costs one integer compare, not a bridge call. On top of
//  that, every non-forced request is DEBOUNCED ~250ms (DispatchWorkItem
//  cancel-and-reschedule, the same idiom SceneEditorWindow's completion
//  popup debounce already uses) so a burst of rapid structural edits
//  (e.g. an agent creating several chunks in a row, each bumping
//  sceneEpoch) collapses into one fetch after the burst settles, not one
//  fetch per bump.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//

import SwiftUI

// MARK: - Snapshot model (Swift-native copy of one -painterMaterialGraph call)
//
// Decoded once per fetch into plain value types so the rest of this file
// never re-touches the bridge's NSArray/NSObject wrappers. `index` mirrors
// the node's position in the ORIGINAL RISEPainterMaterialGraph.nodes array
// -- the same array RISEGraphPort.otherNodeIndex indexes into (see that
// property's doc: valid only within the ONE call that produced it) -- so
// resolving a port's target is a plain array subscript, never a name
// lookup.

/// One reference-param slot, decoded from `RISEGraphPort`.
private struct GraphCanvasPort {
    let paramName: String
    let occurrence: Int
    /// -1 for a node-less (dangling, or out-of-modeled-category) port.
    let otherNodeIndex: Int
    let otherName: String
}

/// One node, decoded from `RISEGraphNode` + its laid-out position.
private struct GraphCanvasNode: Identifiable {
    /// `RISEGraphNode.handle` -- a real generation-tagged handle (unlike
    /// `index` below), kept only for `Identifiable` conformance and as
    /// the natural "did the user's selection survive a refetch" key.
    var id: UInt64 { handle }
    let handle: UInt64
    /// This node's position in the snapshot's `nodes` array -- what
    /// `GraphCanvasPort.otherNodeIndex` indexes into.
    let index: Int
    let name: String
    let chunkKeyword: String
    /// RISE::ChunkCategory ordinal: 0 Painter, 1 Function, 2 Material
    /// (the only three this graph models -- SceneEditController's own
    /// `BuildPainterMaterialGraphSeedsLocked_` comment).
    let category: Int
    let defCount: Int
    /// Top-left anchor in graph space (GraphLayout's rank*columnSpacing /
    /// rowSlot*rowSpacing grid; both always >= 0 by construction).
    let position: CGPoint
    let outEdges: [GraphCanvasPort]
    let inEdges: [GraphCanvasPort]

    var isRampPainter: Bool { chunkKeyword == "ramp_painter" }
    /// §5 item: "a fan-out badge when a node has >1 in-edges."
    var isShared: Bool { inEdges.count > 1 }
}

private struct GraphCanvasSnapshot {
    var nodes: [GraphCanvasNode] = []
    var generation: UInt64 = 0
}

// MARK: - Layout constants

private enum GraphMetrics {
    static let nodeWidth: CGFloat = 172
    static let nodeHeight: CGFloat = 108
    static let thumbnailSize: CGFloat = 40
    static let headerHeight: CGFloat = 22
    static let padding: CGFloat = 80
    /// Below this zoom level, param labels on wires are dropped -- they
    /// become illegible well before the node text itself does, and
    /// drawing ~1 label per edge at a tiny scale is real Canvas cost for
    /// zero readable value. See §5's port-label requirement: "hidden
    /// below a zoom threshold."
    static let labelZoomThreshold: CGFloat = 0.55
    static let minScale: CGFloat = 0.2
    static let maxScale: CGFloat = 2.5
}

/// Category-tinted header color -- the only per-node color-coding this
/// slice does (§5: "No node color/shape encodes information the
/// descriptor doesn't already carry" -- category IS already carried,
/// directly off `RISEGraphNode.category`). Material reuses the
/// established `Theme.catMaterial` identity color; Painter/Function get
/// distinct existing tokens so the three categories that can actually
/// appear in THIS graph read apart from each other (OutlinerView's own
/// Painters/Materials tags both reuse catMaterial today -- fine there
/// since they're never adjacent nodes on one canvas the way they are
/// here, where telling a painter box from a material box at a glance is
/// the whole point).
private func graphCategoryTint(_ category: Int) -> Color {
    switch category {
    case 2: return Theme.catMaterial   // Material
    case 1: return Theme.purple        // Function
    default: return Theme.teal         // Painter (0), and any unmodeled default
    }
}

private func graphCategoryLabel(_ category: Int) -> String {
    switch category {
    case 2: return "Material"
    case 1: return "Function"
    default: return "Painter"
    }
}

// MARK: - Root view

struct NodeGraphCanvas: View {
    /// Weak — same lifetime discipline as every other right/left-panel
    /// view over the bridge (OutlinerView, PropertiesPanel): the panel
    /// must never keep a torn-down bridge alive past its scene.
    weak var bridge: RISEViewportBridge?
    @Binding var refreshTrigger: Int
    @EnvironmentObject var viewModel: RenderViewModel

    @State private var snapshot = GraphCanvasSnapshot()
    @State private var lastFetchedEpoch: Int = -1
    @State private var reloadWorkItem: DispatchWorkItem?

    @State private var scale: CGFloat = 1.0
    @State private var offset: CGSize = .zero
    @GestureState private var dragDelta: CGSize = .zero
    @GestureState private var magnifyDelta: CGFloat = 1.0
    @State private var hasCentered = false

    @State private var selectedHandle: UInt64? = nil
    @State private var hoveredHandle: UInt64? = nil

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
                .onAppear { centerIfNeeded(in: geo.size) }
                .onChange(of: snapshot.generation) { _, _ in centerIfNeeded(in: geo.size) }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .clipShape(RoundedRectangle(cornerRadius: 0))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .onAppear { scheduleReload(debounced: false) }
        .onChange(of: refreshTrigger) { _, _ in scheduleReload(debounced: true) }
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 8) {
            Text("Graph").font(Theme.sans(12.5, .semibold)).foregroundColor(Theme.textPrimary)
            Text("\(snapshot.nodes.count) node\(snapshot.nodes.count == 1 ? "" : "s"), \(edgeCount) edge\(edgeCount == 1 ? "" : "s")")
                .font(Theme.mono(10))
                .foregroundColor(Theme.textDim)
            Spacer(minLength: 4)
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

    // MARK: - Canvas content (wires layer + node boxes, one coordinate space)

    private var contentSize: CGSize {
        guard !snapshot.nodes.isEmpty else { return CGSize(width: 400, height: 300) }
        let maxX = snapshot.nodes.map { $0.position.x + GraphMetrics.nodeWidth }.max() ?? 400
        let maxY = snapshot.nodes.map { $0.position.y + GraphMetrics.nodeHeight }.max() ?? 300
        return CGSize(width: maxX + GraphMetrics.padding, height: maxY + GraphMetrics.padding)
    }

    private var canvasContent: some View {
        ZStack(alignment: .topLeading) {
            GraphWiresLayer(nodes: snapshot.nodes, showLabels: scale >= GraphMetrics.labelZoomThreshold)
                .frame(width: contentSize.width, height: contentSize.height)

            ForEach(snapshot.nodes) { node in
                GraphNodeBoxView(
                    node: node,
                    bridge: bridge,
                    isSelected: selectedHandle == node.handle,
                    isHovered: hoveredHandle == node.handle,
                    onSelect: { selectNode(node) },
                    onOpenDefs: { openDefs(node) },
                    onHover: { hovering in hoveredHandle = hovering ? node.handle : (hoveredHandle == node.handle ? nil : hoveredHandle) }
                )
                .position(x: node.position.x + GraphMetrics.nodeWidth / 2,
                          y: node.position.y + GraphMetrics.nodeHeight / 2)
            }
        }
        .frame(width: contentSize.width, height: contentSize.height, alignment: .topLeading)
    }

    // MARK: - Pan / zoom

    private var effectiveScale: CGFloat {
        (scale * magnifyDelta).clamped(to: GraphMetrics.minScale...GraphMetrics.maxScale)
    }

    private var effectiveOffset: CGSize {
        CGSize(width: offset.width + dragDelta.width, height: offset.height + dragDelta.height)
    }

    /// Drag on empty canvas space pans. Node boxes install their own tap
    /// gestures but do not consume drags, so a drag that starts on a node
    /// still pans the canvas (no drag-to-reposition this slice — §5).
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
                scale = (scale * value).clamped(to: GraphMetrics.minScale...GraphMetrics.maxScale)
            }
    }

    /// First layout (or a fresh generation with no prior manual pan) gets
    /// a one-shot centering pass so the graph doesn't open pinned to the
    /// top-left corner. Only runs once per bridge lifetime (`hasCentered`)
    /// -- a later structural refresh must NOT recenter under the user's
    /// hands (that would fight a manual pan mid-inspection).
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

    // MARK: - Selection / def-focus (§5 interaction minimums 1 & 2)

    private func selectNode(_ node: GraphCanvasNode) {
        guard let bridge else { return }
        // ChunkCategory::Function(1) has no UI section of its own; the
        // C++ selection-routing convention (UiCategoryForChunkCategory,
        // SceneEditController.cpp) maps it onto Painter because
        // colour-family painters dual-register as functions and a
        // function reference resolves against the painter union. Mirror
        // that here rather than inventing a second mapping.
        let cat: RISEViewportCategory = (node.category == 2) ? .material : .painter
        _ = bridge.setSelection(cat, name: node.name)
        selectedHandle = node.handle
        refreshTrigger &+= 1
    }

    /// A double-click always selects (the `exclusively(before:)` gesture
    /// combinator means the single-tap handler never ALSO fires for a
    /// double-click, so this must not skip selection just because the
    /// node has no defs) — the def-focus request only fires on top of
    /// that when there's something to reveal.
    private func openDefs(_ node: GraphCanvasNode) {
        selectNode(node)
        guard node.defCount > 0 else { return }
        viewModel.focusPainterDefRows(name: node.name)
    }

    // MARK: - Reload

    private func scheduleReload(debounced: Bool, force: Bool = false) {
        reloadWorkItem?.cancel()
        if !debounced {
            performReload(force: force)
            return
        }
        let item = DispatchWorkItem { performReload(force: force) }
        reloadWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: item)
    }

    private func performReload(force: Bool) {
        guard let bridge else { return }
        let epoch = Int(bridge.sceneEpoch)
        guard force || epoch != lastFetchedEpoch else { return }
        lastFetchedEpoch = epoch

        let g = bridge.painterMaterialGraph()
        let nodes: [GraphCanvasNode] = g.nodes.enumerated().map { idx, n in
            GraphCanvasNode(
                handle: n.handle,
                index: idx,
                name: n.name,
                chunkKeyword: n.chunkKeyword,
                category: n.category,
                defCount: n.defCount,
                position: CGPoint(x: n.x, y: n.y),
                outEdges: n.outEdges.map {
                    GraphCanvasPort(paramName: $0.paramName, occurrence: $0.occurrence,
                                     otherNodeIndex: $0.otherNodeIndex, otherName: $0.otherName)
                },
                inEdges: n.inEdges.map {
                    GraphCanvasPort(paramName: $0.paramName, occurrence: $0.occurrence,
                                     otherNodeIndex: $0.otherNodeIndex, otherName: $0.otherName)
                }
            )
        }
        // A structural refresh can retarget/invalidate an outstanding
        // selection (the node could have been deleted); dropping it is
        // the safe default (matches OutlinerView's own stance of not
        // second-guessing a vanished selection).
        if let sel = selectedHandle, !nodes.contains(where: { $0.handle == sel }) {
            selectedHandle = nil
        }
        snapshot = GraphCanvasSnapshot(nodes: nodes, generation: g.generation)
    }
}

private extension Comparable {
    func clamped(to range: ClosedRange<Self>) -> Self {
        min(max(self, range.lowerBound), range.upperBound)
    }
}

// MARK: - Node box

private struct GraphNodeBoxView: View {
    let node: GraphCanvasNode
    weak var bridge: RISEViewportBridge?
    let isSelected: Bool
    let isHovered: Bool
    let onSelect: () -> Void
    let onOpenDefs: () -> Void
    let onHover: (Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            content
        }
        .frame(width: GraphMetrics.nodeWidth, height: GraphMetrics.nodeHeight, alignment: .top)
        .background(Theme.bgCard)
        .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMedium)
                .stroke(borderColor, lineWidth: isSelected ? 2 : 1)
        )
        .shadow(color: .black.opacity(isHovered ? 0.28 : 0.14), radius: isHovered ? 7 : 3, y: 2)
        .contentShape(Rectangle())
        // `exclusively(before:)` is the documented SwiftUI combinator for
        // "try the double-tap first; if it doesn't complete within the
        // system double-click interval, fall through to the single-tap
        // handler" — the correct way to layer two `TapGesture`s of
        // different counts on ONE view (plain sequential
        // `.onTapGesture(count:)` modifiers do not reliably disambiguate
        // against each other the way count-vs-drag priority does
        // elsewhere in this app, e.g. ExposureSliderRow's double-tap-to-
        // reset over the Slider's own drag gesture).
        .gesture(
            TapGesture(count: 2).onEnded { onOpenDefs() }
                .exclusively(before: TapGesture(count: 1).onEnded { onSelect() })
        )
        .onHover(perform: onHover)
        .help(node.name)
    }

    private var borderColor: Color {
        if isSelected { return Theme.accent }
        if isHovered { return Theme.borderHover }
        return Theme.borderLight
    }

    private var tint: Color { graphCategoryTint(node.category) }

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
            if node.isShared {
                fanOutBadge
            }
        }
        .padding(.horizontal, 8)
        .frame(height: GraphMetrics.headerHeight)
        .background(tint.opacity(0.85))
    }

    /// "the shared-painter case" -- §5: a node with >1 in-edges is
    /// referenced by more than one other node (a single link, not a
    /// copy per material). Rendered as one badge, not a duplicated node.
    private var fanOutBadge: some View {
        HStack(spacing: 2) {
            Image(systemName: "arrow.triangle.branch").font(.system(size: 7, weight: .bold))
            Text("\(node.inEdges.count)").font(Theme.mono(8, .semibold))
        }
        .foregroundColor(Theme.textOnAccent)
        .padding(.horizontal, 4)
        .padding(.vertical, 1)
        .background(Color.black.opacity(0.28))
        .clipShape(Capsule())
        .help("Referenced by \(node.inEdges.count) other nodes -- shared, not copied")
    }

    @ViewBuilder
    private var content: some View {
        HStack(alignment: .top, spacing: 8) {
            thumbnail
            VStack(alignment: .leading, spacing: 3) {
                Text(graphCategoryLabel(node.category))
                    .font(Theme.mono(8.5))
                    .foregroundColor(Theme.textDim)
                if node.defCount > 0 {
                    defCountPill
                }
                if hasDanglingPort {
                    danglingBadge
                }
            }
            Spacer(minLength: 2)
        }
        .padding(8)
    }

    /// Materials have no `PainterPreview` evaluation (that engine only
    /// evaluates IPainter/IScalarPainter chunks) -- attempting a fetch
    /// for one would just churn the cache toward a permanent refusal, so
    /// skip it entirely rather than show an always-neutral tile.
    @ViewBuilder
    private var thumbnail: some View {
        if node.category == 2 {
            ZStack {
                RoundedRectangle(cornerRadius: 4).fill(tint.opacity(0.12))
                Text("◐").font(.system(size: 16)).foregroundColor(tint)
            }
            .frame(width: GraphMetrics.thumbnailSize, height: GraphMetrics.thumbnailSize)
        } else if node.isRampPainter {
            RampStripThumbnailView(bridge: bridge, painterName: node.name,
                                    width: GraphMetrics.thumbnailSize, height: GraphMetrics.thumbnailSize * 0.45)
        } else {
            PainterThumbnailView(bridge: bridge, painterName: node.name, defIndex: -1, size: GraphMetrics.thumbnailSize)
        }
    }

    private var defCountPill: some View {
        Text("\(node.defCount) def\(node.defCount == 1 ? "" : "s")")
            .font(Theme.mono(8))
            .foregroundColor(Theme.textDim)
            .padding(.horizontal, 5)
            .padding(.vertical, 1)
            .background(Theme.fillTrough)
            .clipShape(Capsule())
    }

    private var hasDanglingPort: Bool {
        node.outEdges.contains { $0.otherNodeIndex < 0 }
    }

    private var danglingBadge: some View {
        HStack(spacing: 2) {
            Image(systemName: "exclamationmark.triangle.fill").font(.system(size: 7))
            Text("dangling ref").font(Theme.mono(7.5))
        }
        .foregroundColor(Theme.warn)
    }
}

// MARK: - Wires layer

/// Draws every real edge exactly once (walked via each node's `outEdges`
/// -- the mirror `inEdges` entries on the target are never separately
/// drawn, only used for the fan-out badge) plus a short warning-tinted
/// stub for every dangling port. Pure `GraphicsContext` drawing, no live
/// subviews, so this redraws cheaply on every pan/zoom frame.
private struct GraphWiresLayer: View {
    let nodes: [GraphCanvasNode]
    let showLabels: Bool

    var body: some View {
        Canvas { context, _ in
            for node in nodes {
                let destPortXs = inputPortAnchors(for: node)
                for (portIdx, port) in node.outEdges.enumerated() {
                    let destPoint = CGPoint(x: node.position.x, y: destPortXs[portIdx])
                    if port.otherNodeIndex >= 0, port.otherNodeIndex < nodes.count {
                        let source = nodes[port.otherNodeIndex]
                        let sourcePoint = CGPoint(x: source.position.x + GraphMetrics.nodeWidth,
                                                   y: source.position.y + GraphMetrics.nodeHeight / 2)
                        drawWire(context: context, from: sourcePoint, to: destPoint,
                                 color: graphCategoryTint(source.category).opacity(0.75))
                        if showLabels {
                            drawLabel(context: context, text: port.paramName, near: destPoint)
                        }
                    } else {
                        // Dangling / out-of-modeled-category reference:
                        // a short warning-tinted stub, no destination box.
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

    /// Spreads a node's input ports evenly along its left edge, in
    /// declaration order — mirrors the row-thumbnail idiom (stage order
    /// in PropertiesPanel is descriptor/occurrence order, not sorted).
    private func inputPortAnchors(for node: GraphCanvasNode) -> [CGFloat] {
        let count = max(node.outEdges.count, 1)
        let top = node.position.y + GraphMetrics.headerHeight + 6
        let bottom = node.position.y + GraphMetrics.nodeHeight - 6
        guard count > 1 else { return [node.position.y + GraphMetrics.nodeHeight / 2] }
        let step = (bottom - top) / CGFloat(count - 1)
        return (0..<count).map { top + CGFloat($0) * step }
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
        // Right-align so the label sits just left of the input port it
        // labels, matching "param label drawn at the target end."
        context.draw(resolved, at: CGPoint(x: point.x - 4, y: point.y - 7), anchor: .trailing)
    }
}
