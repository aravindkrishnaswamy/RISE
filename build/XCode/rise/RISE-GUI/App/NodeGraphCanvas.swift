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
//  SCOPE (S15 shipped Phase A, read-only; THIS FILE now also carries S21,
//  Phase B -- docs/gui/NODE_GRAPH_CANVAS.md §5/§6): drag-to-reposition
//  (live during the drag, persisted via -writeGraphNodeLayoutPosition on
//  release only), drag-to-wire (an output-handle drag onto an existing
//  input port, live-validated via -checkConnection during the drag,
//  committed via -rewireConnection on drop, with a refusal panel + the
//  Duplicate-then-retry escape hatch for a SharedTarget refusal), an
//  add-node search palette (-paletteKeywords / -chunkNodeRequirements /
//  -createChunkNode), and keyboard delete/duplicate
//  (-deleteGraphNode / -duplicateGraphNode). Click selects (wired to the
//  same setSelection the outliner uses); double-click on an
//  expression-family node asks PropertiesPanel to reveal its `def[i]`
//  rows (RenderViewModel.focusPainterDefRows). Every mutation goes
//  through these same S18-S20 bridge verbs -- no direct document write
//  from this file, ever.
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
import AppKit
import UniformTypeIdentifiers

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
    /// rowSlot*rowSpacing grid; both always >= 0 by construction). `var`
    /// (S21) so `repositioned(to:)` below can hand back a same-identity
    /// copy with a live/committed drag position -- this is the only
    /// field S21 ever overrides; nothing else about a node changes
    /// without a fresh `-painterMaterialGraph` fetch.
    var position: CGPoint
    let outEdges: [GraphCanvasPort]
    let inEdges: [GraphCanvasPort]

    var isRampPainter: Bool { chunkKeyword == "ramp_painter" }
    /// §5 item: "a fan-out badge when a node has >1 in-edges."
    var isShared: Bool { inEdges.count > 1 }
    /// S21 review round 1 P2-1: zero in-edges on a Painter/Function node --
    /// nothing on the canvas references it any more. This is the SNAPSHOT-
    /// derived answer to the same question `RISERewireOutcome.nowUnreferenced`
    /// reports at the moment of a rewire, deliberately computed here instead
    /// of by threading that one-shot event through canvas state: a plain
    /// per-node predicate over the CURRENT snapshot is self-healing (wiring a
    /// new reference into the node on the next reload just makes this false
    /// again, no separate "clear the badge" bookkeeping to forget) and reads
    /// the same for an orphan made by a delete, an undo, or an agent edit as
    /// for one made by this canvas's own rewire -- `nowUnreferenced` only
    /// ever covers the last case. See `commitRewire`'s comment for why the
    /// rewire outcome itself needs no further plumbing than the refresh it
    /// already triggers. Materials are excluded: a Material is this graph's
    /// natural ROOT (nothing points a reference chunk AT a material -- it's
    /// the consumer, not the consumed), so zero in-edges there is the normal
    /// case, not an orphan.
    var isOrphaned: Bool { inEdges.isEmpty && category != 2 }

    /// S21 drag-to-reposition: a copy of this node at a new position,
    /// everything else byte-identical. Used both for the LIVE preview
    /// (a throwaway copy fed to the wires layer + this node's own box
    /// while a drag is in flight) and to fold a just-committed drag back
    /// into `snapshot` without a round-trip re-fetch.
    func repositioned(to point: CGPoint) -> GraphCanvasNode {
        var copy = self
        copy.position = point
        return copy
    }

    /// S21: this node's OUTPUT handle anchor -- the drag-to-wire source
    /// hotspot, right-edge center. The mirror of `GraphWiresLayer`'s own
    /// wire-start point for an out-edge, so the visible wire and the
    /// drag handle sit at the exact same pixel.
    var outputHandlePoint: CGPoint {
        CGPoint(x: position.x + GraphMetrics.nodeWidth, y: position.y + GraphMetrics.nodeHeight / 2)
    }
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

    // S21 additions -- drag-to-wire geometry.
    /// Visible radius of the output-handle circle drawn at a node's
    /// right edge.
    static let outputHandleRadius: CGFloat = 6
    /// How close a wire-drag's current point must be to an input-port
    /// anchor to register as a hover/drop candidate, in SCREEN points --
    /// i.e. this constant is independent of zoom, even though `point`
    /// itself (from the `"graphSpace"` coordinate space) is CONTENT-space
    /// and shrinks on screen as the canvas zooms out (screenDist =
    /// contentDist * scale). The hit-test site (`findPortTarget`)
    /// therefore divides this by the current `effectiveScale` to get the
    /// content-space threshold that maps back to a constant ~16pt target
    /// on screen. Corrected S21 review round 1 P2-2 -- the previous
    /// version compared this constant directly against a content-space
    /// distance with no scale correction, so the effective on-screen hit
    /// radius actually SHRANK at low zoom (down to ~3pt at
    /// `minScale` 0.2), the opposite of the old comment's claim that a
    /// content-space radius is easier, not harder, to hit when zoomed
    /// out.
    static let portHitRadius: CGFloat = 16
}

/// S21: the LEFT-edge input-port anchors for a node with `outEdgeCount`
/// reference slots, spread evenly top-to-bottom in declaration order --
/// the SAME layout `GraphWiresLayer` draws wire destinations at (moved
/// here, out of that view, so drag-to-wire hit-testing in
/// `NodeGraphCanvas` and the visible wire endpoints can never drift
/// apart; both call this one function). `nodePosition` is the node's
/// CURRENT (possibly live-drag-overridden) top-left.
private func graphInputPortAnchors(nodePosition: CGPoint, outEdgeCount: Int) -> [CGFloat] {
    let count = max(outEdgeCount, 1)
    let top = nodePosition.y + GraphMetrics.headerHeight + 6
    let bottom = nodePosition.y + GraphMetrics.nodeHeight - 6
    guard count > 1 else { return [nodePosition.y + GraphMetrics.nodeHeight / 2] }
    let step = (bottom - top) / CGFloat(count - 1)
    return (0..<count).map { top + CGFloat($0) * step }
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

// MARK: - S21 drag-to-wire model

/// One existing input-port slot a wire-drag can be dropped onto:
/// `targetHandle`.`targetName`.`param`[occurrence] -- exactly the
/// `(targetCategory, targetName, param, occurrence)` addressing
/// `-rewireConnection` takes. Resolved by hit-testing every node's
/// `graphInputPortAnchors` against the drag's current point.
private struct WirePortTarget: Equatable {
    let nodeHandle: UInt64
    let nodeName: String
    let nodeCategory: Int
    let paramName: String
    let occurrence: Int
    let anchor: CGPoint

    static func == (l: WirePortTarget, r: WirePortTarget) -> Bool {
        l.nodeHandle == r.nodeHandle && l.paramName == r.paramName && l.occurrence == r.occurrence
    }
}

/// Live state for an in-flight output-handle drag. `verdictLegal == nil`
/// means "no candidate hovered yet, or the legality read hasn't landed" --
/// drawn as a neutral wire; `true`/`false` drive the green/red preview
/// (Theme.success / Theme.error) plus the status line.
private struct WireDragState {
    let sourceHandle: UInt64
    let sourceName: String
    let sourceCategory: Int
    var currentPoint: CGPoint
    var target: WirePortTarget? = nil
    var verdictLegal: Bool? = nil
    var verdictMessage: String = ""
}

/// The post-drop refusal the canvas needs to explain AND, for a
/// SharedTarget refusal, unblock via Duplicate-then-retry (the S19/S20
/// escape hatch, made interactive here per S21's brief).
private struct WireRefusalState: Identifiable {
    let id = UUID()
    let outcome: RISERewireOutcome
    let target: WirePortTarget
    let sourceHandle: UInt64
    let sourceName: String
    let sourceCategory: Int
}

/// S21: the modal-ish center panel a rewire refusal shows -- the
/// structured `RISERewireOutcome` explained in prose, with the
/// Duplicate-then-retry escape hatch surfaced as an actual button only
/// when the refusal is the one case it unblocks (`closure == .sharedTarget`,
/// S19/S20's own rule -- see `RISERewireOutcome.closure`'s header doc).
private struct WireRefusalPanel: View {
    let state: WireRefusalState
    let onDismiss: () -> Void
    let onDuplicateAndRetry: () -> Void

    private var outcome: RISERewireOutcome { state.outcome }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: "exclamationmark.triangle.fill").foregroundColor(Theme.warn)
                Text("Can't wire \u{201c}\(state.sourceName)\u{201d} in")
                    .font(Theme.sans(12, .semibold))
                    .foregroundColor(Theme.textPrimary)
            }
            Text("Target: \(state.target.nodeName).\(state.target.paramName)")
                .font(Theme.mono(9.5))
                .foregroundColor(Theme.textFaint)
            Text(outcome.message.isEmpty ? "The wire was refused." : outcome.message)
                .font(Theme.mono(10.5))
                .foregroundColor(Theme.textDim)
                .fixedSize(horizontal: false, vertical: true)

            if !outcome.sharedChunks.isEmpty {
                refusalList(title: "Not solely owned:", items: outcome.sharedChunks)
            }
            if !outcome.outOfClosureReferrers.isEmpty {
                refusalList(title: "Referenced from outside:", items: outcome.outOfClosureReferrers)
            }

            HStack {
                Spacer()
                Button("Cancel", action: onDismiss)
                    .buttonStyle(.plain)
                    .foregroundColor(Theme.textDim)
                    .padding(.horizontal, 10).padding(.vertical, 5)
                if outcome.closure == .sharedTarget {
                    Button {
                        onDuplicateAndRetry()
                    } label: {
                        Text("Duplicate \u{201c}\(state.sourceName)\u{201d} & Retry")
                            .foregroundColor(Theme.textOnAccent)
                            .padding(.horizontal, 10).padding(.vertical, 5)
                            .background(Theme.accent)
                            .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .padding(14)
        .frame(width: 360)
        .background(Theme.bgCard)
        .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
        .overlay(RoundedRectangle(cornerRadius: Theme.radiusMedium).stroke(Theme.borderLight, lineWidth: 1))
        .shadow(color: .black.opacity(0.35), radius: 16, y: 6)
    }

    private func refusalList(title: String, items: [String]) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(Theme.mono(9, .semibold)).foregroundColor(Theme.textFaint)
            ForEach(items, id: \.self) { item in
                Text("\u{2022} \(item)").font(Theme.mono(9.5)).foregroundColor(Theme.textDim)
            }
        }
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

    // -------- Object-pick spotlight (viewport/outliner -> canvas) --------
    /// Every node currently spotlit -- the object's bound material plus its
    /// transitive Painter/Function/Material closure (`-appearanceClosure
    /// (forObject:)`). Set from the CURRENT shared selection, never by this
    /// canvas selecting anything itself: see `refreshSpotlight`'s own
    /// comment for why this must NEVER call `bridge.setSelection`.
    @State private var spotlightHandles: Set<UInt64> = []
    /// The object name (`selectionRowName`) the spotlight was last computed
    /// for -- `nil` when nothing is currently spotlit. Lets
    /// `refreshSpotlight` tell "the same object is still selected, just
    /// recomputing the closure" apart from "the selection actually moved,"
    /// which is what gates the auto-scroll (see that function's comment).
    @State private var lastSpotlightObjectName: String? = nil
    /// Review-round P2-1 fix: unlike the Qt canvas (which re-derives the
    /// spotlight on every preview frame via `performReload`'s
    /// `imageUpdated`-driven poll), this canvas only reacts to DISCRETE
    /// triggers (a `refreshTrigger` bump or `performReload`'s own tail) --
    /// there is no frame-tick to naturally retry a resolve that degraded
    /// to empty because a render currently owns the commit lock
    /// (`AppearanceClosureForObject`'s try_to_lock contract). Holds the
    /// single bounded retry `refreshSpotlight` schedules on a degrade; see
    /// that function's own comment.
    @State private var spotlightRetryWorkItem: DispatchWorkItem?

    // -------- S21: drag-to-reposition --------
    /// The single node currently mid-drag, at its LIVE (uncommitted)
    /// position. nil whenever no reposition drag is in flight. Kept as
    /// ONE optional override (not a dictionary) because only one node
    /// can be dragged at a time (multi-select drag is explicitly out of
    /// scope this slice -- see the file header).
    @State private var liveDragOverride: (handle: UInt64, point: CGPoint)? = nil

    // -------- S21: drag-to-wire --------
    @State private var wireDrag: WireDragState? = nil
    @State private var wireRefusal: WireRefusalState? = nil
    /// Debounces the legality re-check so a fast drag across several
    /// ports doesn't fire a `-checkConnection` C++ call every pixel --
    /// only when the hovered PORT actually changes.
    @State private var lastCheckedTarget: WirePortTarget? = nil

    // -------- S21: add-node palette --------
    @State private var showPalette = false
    /// Where a just-created node should be positioned -- recomputed to
    /// the current viewport CENTER (in content-space) every time the
    /// palette opens, so "near drop point" tracks wherever the user is
    /// currently looking rather than a fixed canvas-space default.
    @State private var paletteDropPoint = CGPoint(x: 60, y: 60)
    @State private var lastViewportSize: CGSize = .zero

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
                    wireStatusLine
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
        // review-round P3 fix: no separate `refreshSpotlight()` call here.
        // `scheduleReload(debounced: false)` runs `performReload`
        // SYNCHRONOUSLY, and `performReload`'s own tail already calls
        // `refreshSpotlight(freshNodes:)` unconditionally -- an explicit
        // call right after would be a full duplicate bridge round-trip on
        // every appearance for no additional effect.
        .onAppear { scheduleReload(debounced: false) }
        .onChange(of: refreshTrigger) { _, _ in
            scheduleReload(debounced: true)
            // LOAD-BEARING, do not remove even though it looks like the
            // same "duplicate" shape as the .onAppear call just above:
            // `scheduleReload(debounced: true)` only runs `performReload`
            // (and therefore its tail `refreshSpotlight`) after a 250ms
            // GCD debounce, but a plain SELECTION change (the overwhelming
            // common trigger for a `refreshTrigger` bump) needs the
            // spotlight to update IMMEDIATELY, not 250ms later -- this
            // explicit, undebounced call is what delivers that.
            refreshSpotlight()
        }
        .background(keyboardShortcuts)
        .sheet(isPresented: $showPalette) {
            NodeGraphAddNodeSheet(
                bridge: bridge,
                existingNodes: paletteCandidateNodes,
                onCreated: { name, _, category in didCreateNode(named: name, category: category) },
                onDismiss: { showPalette = false }
            )
        }
        .overlay(alignment: .center) {
            if let refusal = wireRefusal {
                WireRefusalPanel(
                    state: refusal,
                    onDismiss: { wireRefusal = nil },
                    onDuplicateAndRetry: { retryWithDuplicate(refusal) }
                )
            }
        }
    }

    /// Hidden buttons carrying the app's ONLY keyboard-shortcut idiom for
    /// "act on the current selection" (no `.onDeleteCommand`/NSEvent
    /// monitor precedent exists elsewhere in this app -- OutlinerView's
    /// delete/duplicate are context-menu-only; see RenderViewModel
    /// .removeEntity/.duplicateSelectedOrNamed). `.opacity(0)` rather
    /// than `EmptyView` because a shortcut needs a real controlValue to
    /// bind to; `.disabled` when nothing is selected keeps the shortcut
    /// inert rather than silently no-op'ing on a nil.
    private var keyboardShortcuts: some View {
        Group {
            Button("Delete Node") { deleteSelected() }
                .keyboardShortcut(.delete, modifiers: [])
                .disabled(selectedHandle == nil)
            Button("Delete Node (forward)") { deleteSelected() }
                .keyboardShortcut(.deleteForward, modifiers: [])
                .disabled(selectedHandle == nil)
            Button("Duplicate Node") { duplicateSelected() }
                .keyboardShortcut("d", modifiers: .command)
                .disabled(selectedHandle == nil)
        }
        .opacity(0)
        .frame(width: 0, height: 0)
    }

    /// The drag-to-wire live status line -- S17's diagnostic surfaced as
    /// text, not just the preview wire's color, so a colorblind user (or
    /// anyone at a glance) can read WHY a drop would be refused before
    /// releasing.
    @ViewBuilder
    private var wireStatusLine: some View {
        if let wd = wireDrag {
            VStack {
                Spacer()
                HStack(spacing: 6) {
                    Circle()
                        .fill(wd.verdictLegal == false ? Theme.errorStrong : (wd.verdictLegal == true ? Theme.success : Theme.textFaint))
                        .frame(width: 7, height: 7)
                    Text(wireStatusText(wd))
                        .font(Theme.mono(10.5))
                        .foregroundColor(Theme.textPrimary)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(Theme.bgCard.opacity(0.95))
                .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
                .padding(.bottom, 10)
            }
            .allowsHitTesting(false)
        }
    }

    private func wireStatusText(_ wd: WireDragState) -> String {
        guard let target = wd.target else { return "Drag onto an input port to wire \"\(wd.sourceName)\" in" }
        if wd.verdictLegal == false { return "\(target.nodeName).\(target.paramName): \(wd.verdictMessage)" }
        return "Wire into \(target.nodeName).\(target.paramName)"
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
                openPalette(atViewportCenterOf: lastViewportSize)
            } label: {
                Image(systemName: "plus").font(.system(size: 11, weight: .semibold))
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.textDim)
            .help("Add node")
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

    /// `snapshot.nodes` with the in-flight drag's node (if any) swapped
    /// for its LIVE position -- the one array both the wires layer and
    /// the node boxes render from, so a dragged node's own box AND every
    /// wire touching it move in lockstep with no separate bookkeeping
    /// (the same "one coordinate space, one source of positions"
    /// discipline the file header already commits to for S15's
    /// read-only wires).
    private var displayedNodes: [GraphCanvasNode] {
        guard let o = liveDragOverride else { return snapshot.nodes }
        return snapshot.nodes.map { $0.handle == o.handle ? $0.repositioned(to: o.point) : $0 }
    }

    private var canvasContent: some View {
        ZStack(alignment: .topLeading) {
            GraphWiresLayer(nodes: displayedNodes, showLabels: scale >= GraphMetrics.labelZoomThreshold,
                             wireDraftFrom: wireDrag.flatMap { wd in
                                 displayedNodes.first(where: { $0.handle == wd.sourceHandle })?.outputHandlePoint
                             },
                             wireDraftTo: wireDrag?.currentPoint,
                             wireDraftLegal: wireDrag?.verdictLegal,
                             hoveredPort: wireDrag?.target?.anchor)
                .frame(width: contentSize.width, height: contentSize.height)

            ForEach(displayedNodes) { node in
                GraphNodeBoxView(
                    node: node,
                    bridge: bridge,
                    isSelected: selectedHandle == node.handle,
                    isSpotlit: spotlightHandles.contains(node.handle),
                    isHovered: hoveredHandle == node.handle,
                    onSelect: { selectNode(node) },
                    onOpenDefs: { openDefs(node) },
                    onHover: { hovering in hoveredHandle = hovering ? node.handle : (hoveredHandle == node.handle ? nil : hoveredHandle) },
                    onDragChanged: { pt in handleNodeDragChanged(node, to: pt) },
                    onDragEnded: { pt in handleNodeDragEnded(node, to: pt) }
                )
                .position(x: node.position.x + GraphMetrics.nodeWidth / 2,
                          y: node.position.y + GraphMetrics.nodeHeight / 2)
            }

            // S21 output-handle circles -- SIBLINGS of the node boxes
            // (not children of GraphNodeBoxView) so a wire-drag's own
            // DragGesture never contends with the node body's
            // reposition-drag/tap gesture stack for the same touch.
            ForEach(displayedNodes) { node in
                // A 24x24 `Color.clear` establishes a generous hit target
                // (ZStack centers its children by default) independent of
                // the smaller VISIBLE circle drawn on top -- simpler and
                // less anchor-fragile than a `.contentShape` with an
                // explicit `Shape.size(...)` override.
                ZStack {
                    Color.clear.frame(width: 24, height: 24)
                    Circle()
                        .fill(graphCategoryTint(node.category))
                        .overlay(Circle().stroke(Theme.bgWell, lineWidth: 1.5))
                        .frame(width: GraphMetrics.outputHandleRadius * 2, height: GraphMetrics.outputHandleRadius * 2)
                }
                .contentShape(Rectangle())
                .position(node.outputHandlePoint)
                .gesture(
                    DragGesture(minimumDistance: 2, coordinateSpace: .named("graphSpace"))
                        .onChanged { value in handleWireDragChanged(source: node, point: value.location) }
                        .onEnded { value in handleWireDragEnded(source: node, point: value.location) }
                )
                .help("Drag to wire \"\(node.name)\" into another node's reference")
            }
        }
        .frame(width: contentSize.width, height: contentSize.height, alignment: .topLeading)
        .coordinateSpace(name: "graphSpace")
    }

    // MARK: - Pan / zoom

    private var effectiveScale: CGFloat {
        (scale * magnifyDelta).clamped(to: GraphMetrics.minScale...GraphMetrics.maxScale)
    }

    private var effectiveOffset: CGSize {
        CGSize(width: offset.width + dragDelta.width, height: offset.height + dragDelta.height)
    }

    /// Drag on EMPTY canvas space pans. A drag that starts ON a node is
    /// consumed by that node's own reposition-drag gesture (S21) before
    /// it ever reaches this one -- SwiftUI resolves a touch against the
    /// most specific (deepest) view with a matching `.gesture()` first,
    /// and `GraphNodeBoxView`'s drag gesture sits strictly inside this
    /// container's view tree, so pan never fires for a node-body drag.
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

    // MARK: - Object-pick spotlight

    /// Re-derive `spotlightHandles` from the CURRENT shared (viewport/
    /// outliner) selection. Runs on every `refreshTrigger` bump
    /// independently of `performReload`'s epoch gate above: a plain
    /// Object pick does NOT bump `bridge.sceneEpoch` (only a structural
    /// mutation does -- see `SceneEditController::SceneEpoch`'s own
    /// comment), so this cannot piggyback on that early-return the way the
    /// structural graph refetch does; it must always re-read the current
    /// selection and act on it.
    ///
    /// - Parameter freshNodes: when `performReload` just rebuilt the node
    ///   list, pass that LOCAL array directly rather than letting this read
    ///   back through `snapshot` (`@State`) -- review-round P2 fix: don't
    ///   make the one caller with a fresher answer already in hand take a
    ///   dependency on `@State`'s own write-then-read timing at all.
    ///   `nil` (the two independent call sites, `.onAppear`/
    ///   `.onChange(of: refreshTrigger)`) falls back to the current
    ///   `snapshot.nodes`.
    ///
    /// CRITICAL (per NODE_GRAPH_CANVAS.md's own interaction contract):
    /// never calls `bridge.setSelection`. The shared selection stays on
    /// the object the properties panel is inspecting; this canvas only
    /// LOOKS at it, it never claims it.
    private func refreshSpotlight(freshNodes: [GraphCanvasNode]? = nil) {
        // Any call here supersedes a still-pending degrade retry (see the
        // degrade branch below) -- a fresh, real trigger is always a
        // better answer than a stale scheduled one.
        spotlightRetryWorkItem?.cancel()
        spotlightRetryWorkItem = nil

        let nodes = freshNodes ?? snapshot.nodes
        guard let bridge, bridge.selectionCategory == .object else {
            spotlightHandles = []
            lastSpotlightObjectName = nil
            return
        }
        // `selectionRowName`, NOT `selectionName`: a viewport/outliner pick
        // on a `count_u`/`count_v` repeated or subtree-copied instance
        // names a SYNTHESIZED per-repetition/per-member entity
        // (`I[1,0]`/`I.child`) that is never itself an addressable chunk --
        // `selectionRowName` is the same "resolve back to the instancing
        // chunk's own row" the outliner already performs (OutlinerView's
        // own `selectionRowName` comment), which is what
        // `-appearanceClosureForObject:` needs.
        let objectName = bridge.selectionRowName
        guard !objectName.isEmpty else {
            spotlightHandles = []
            lastSpotlightObjectName = nil
            return
        }
        // (category, name) matching, NOT name alone (review-round P1 fix):
        // a Painter and a Material chunk may legally share a name, so
        // matching this closure's entries against `nodes` by name only can
        // silently spotlight the wrong node on a cross-category collision.
        let closureEntries = bridge.appearanceClosure(forObject: objectName)
        guard !closureEntries.isEmpty else {
            // DEGRADED (or genuinely empty) resolve (review-round P2-1/
            // P2-2 fix): CLEAR the memo rather than leaving it untouched.
            // "Leave untouched" was itself a bug: object A resolves and is
            // spotlit, the user picks object B while a render degrades B's
            // resolve, then picks A again -- with the memo left at "A"
            // from the earlier resolve, re-selecting A would read as
            // "unchanged" and skip the auto-scroll entirely. Clearing
            // makes every post-degrade re-selection (including of the
            // SAME object) a fresh change.
            spotlightHandles = []
            lastSpotlightObjectName = nil

            // Unlike the Qt canvas (re-derives every preview frame), this
            // canvas has no frame-tick to naturally retry once a
            // contended render finishes -- refreshSpotlight only runs on
            // discrete triggers. Schedule exactly ONE bounded retry
            // ~0.5s out, via the SAME DispatchWorkItem cancel-and-
            // reschedule idiom `scheduleReload`'s own debounce already
            // uses. Harmless for a genuinely material-less object (the
            // retry fires once, finds still-empty, and stops -- no
            // pile-up, since every call to this function cancels any
            // pending retry first).
            let retry = DispatchWorkItem { refreshSpotlight() }
            spotlightRetryWorkItem = retry
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: retry)
            return
        }
        // Stamp the memo ONLY on a successful (non-empty) resolve -- see
        // the guard above. Auto-scroll must only fire when the OBJECT
        // selection itself changed since the last SUCCESSFUL resolve, not
        // on every reload -- otherwise a drag/edit while an object stays
        // selected would re-trigger `scrollToNodeIfNeeded` and yank the
        // view out from under an in-progress canvas edit.
        let selectionChanged = (objectName != lastSpotlightObjectName)
        lastSpotlightObjectName = objectName

        var handles: Set<UInt64> = []
        var primaryNode: GraphCanvasNode? = nil
        for entry in closureEntries {
            guard let node = nodes.first(where: { $0.category == entry.category && $0.name == entry.name }) else { continue }
            handles.insert(node.handle)
            if primaryNode == nil { primaryNode = node }   // first entry = the object's material, per the bridge's own contract
        }
        spotlightHandles = handles
        if selectionChanged, let primaryNode { scrollToNodeIfNeeded(primaryNode) }
    }

    /// Auto-scroll (never re-zoom) so `node`'s box is visible within the
    /// current viewport -- the mirror, in reverse, of `openPalette`'s own
    /// screen<->content transform math. A no-op when the node is already
    /// fully visible, so a spotlight on an already-visible node never
    /// fights a manual pan mid-inspection (the same "don't move the view
    /// out from under the user" posture `centerIfNeeded` documents).
    private func scrollToNodeIfNeeded(_ node: GraphCanvasNode) {
        guard lastViewportSize.width > 1, lastViewportSize.height > 1 else { return }
        let s = scale
        let nodeRectScreen = CGRect(
            x: node.position.x * s + offset.width, y: node.position.y * s + offset.height,
            width: GraphMetrics.nodeWidth * s, height: GraphMetrics.nodeHeight * s)
        let viewportRect = CGRect(origin: .zero, size: lastViewportSize)
        guard !viewportRect.contains(nodeRectScreen) else { return }

        let nodeCenterContent = CGPoint(x: node.position.x + GraphMetrics.nodeWidth / 2,
                                         y: node.position.y + GraphMetrics.nodeHeight / 2)
        let screenCenter = CGPoint(x: lastViewportSize.width / 2, y: lastViewportSize.height / 2)
        withAnimation(.easeInOut(duration: 0.25)) {
            offset = CGSize(width: screenCenter.x - nodeCenterContent.x * s,
                             height: screenCenter.y - nodeCenterContent.y * s)
        }
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
        // A structural refresh can change which nodes exist -- re-derive
        // the spotlight against the fresh snapshot rather than pruning the
        // stale handle set in place, since the closure itself may have
        // grown or shrunk (a rewire adding/removing a painter reference),
        // not just lost a node outright. Pass `nodes` directly (see
        // `refreshSpotlight`'s own `freshNodes` parameter comment).
        refreshSpotlight(freshNodes: nodes)
    }

    // MARK: - S21: drag-to-reposition

    private func handleNodeDragChanged(_ node: GraphCanvasNode, to point: CGPoint) {
        liveDragOverride = (node.handle, point)
    }

    /// Commit path: ONE `-writeGraphNodeLayoutPosition` call, on release
    /// only (the file header's "no mid-drag writes" contract). On
    /// success the moved node's position is folded straight into
    /// `snapshot` (no re-fetch -- the sidecar write does not advance
    /// `sceneEpoch`, by design, so nothing would trigger one anyway).
    /// On failure the override simply clears, snapping the node back to
    /// its last-known-good position with a status alert explaining why.
    private func handleNodeDragEnded(_ node: GraphCanvasNode, to point: CGPoint) {
        liveDragOverride = nil
        guard let bridge else { return }
        var outError: NSString? = nil
        let ok = bridge.writeGraphNodeLayoutPosition(name: node.name, x: Double(point.x), y: Double(point.y), outError: &outError)
        if ok {
            if let idx = snapshot.nodes.firstIndex(where: { $0.handle == node.handle }) {
                snapshot.nodes[idx] = snapshot.nodes[idx].repositioned(to: point)
            }
        } else {
            // A GENUINE failure (the write itself failed -- I/O, an
            // unwritable sidecar path), not a legality/ownership refusal --
            // see `presentGraphFailureAlert`'s comment for the distinction
            // this file now draws.
            presentGraphFailureAlert(title: "Couldn't save position", message: (outError as String?) ?? "The position was not saved.")
        }
    }

    // MARK: - S21: drag-to-wire

    /// Hit-test every OTHER node's input-port anchors against `point`
    /// (content-space, already coordinate-space-resolved by the caller's
    /// `DragGesture(coordinateSpace: .named("graphSpace"))`), returning
    /// the nearest one within `GraphMetrics.portHitRadius`. Self-wiring
    /// is not specially excluded here -- a self-referencing drop is
    /// simply refused downstream by the real cycle check
    /// (`ConnectionLegality::WouldCycle` treats `from == to` as a
    /// cycle), so there is exactly one place that rule lives.
    private func findPortTarget(near point: CGPoint) -> WirePortTarget? {
        var best: (WirePortTarget, CGFloat)? = nil
        // Content-space threshold that maps back to a constant ~16pt
        // ON-SCREEN radius regardless of zoom -- see `portHitRadius`'s own
        // comment. `effectiveScale` is clamped to [minScale, maxScale] by
        // construction, so this never divides by zero.
        let hitRadius = GraphMetrics.portHitRadius / effectiveScale
        for node in displayedNodes {
            let anchors = graphInputPortAnchors(nodePosition: node.position, outEdgeCount: node.outEdges.count)
            for (idx, y) in anchors.enumerated() where idx < node.outEdges.count {
                let anchor = CGPoint(x: node.position.x, y: y)
                let dist = hypot(anchor.x - point.x, anchor.y - point.y)
                guard dist <= hitRadius else { continue }
                if best == nil || dist < best!.1 {
                    let port = node.outEdges[idx]
                    best = (WirePortTarget(nodeHandle: node.handle, nodeName: node.name, nodeCategory: node.category,
                                            paramName: port.paramName, occurrence: port.occurrence, anchor: anchor), dist)
                }
            }
        }
        return best?.0
    }

    private func handleWireDragChanged(source: GraphCanvasNode, point: CGPoint) {
        var state = wireDrag ?? WireDragState(sourceHandle: source.handle, sourceName: source.name, sourceCategory: source.category, currentPoint: point)
        state.currentPoint = point
        let target = findPortTarget(near: point)
        if target != state.target {
            state.target = target
            state.verdictLegal = nil
            state.verdictMessage = ""
        }
        wireDrag = state
        guard let target, target != lastCheckedTarget else { return }
        lastCheckedTarget = target
        updateWireVerdict(source: source, target: target)
    }

    /// S17 live pre-check -- `-checkConnection` is a fast, lock-free
    /// descriptor+document read (no commit mutex), so calling it
    /// synchronously per hovered-port-change (NOT per pixel -- see the
    /// `lastCheckedTarget` debounce above) is cheap enough for a drag
    /// gesture's cadence.
    private func updateWireVerdict(source: GraphCanvasNode, target: WirePortTarget) {
        guard let bridge else { return }
        var outDiag: NSString? = nil
        let legal = bridge.checkConnection(
            targetCategory: target.nodeCategory, targetName: target.nodeName, param: target.paramName,
            candidateCategory: source.category, candidateName: source.name, outDiagnostic: &outDiag)
        // The drag may have moved to a different (or no) target while this
        // call was in flight -- discard a stale answer rather than paint
        // a verdict for a port the cursor already left.
        guard wireDrag?.target == target else { return }
        wireDrag?.verdictLegal = legal
        wireDrag?.verdictMessage = legal ? "" : ((outDiag as String?) ?? "Not a legal connection")
    }

    private func handleWireDragEnded(source: GraphCanvasNode, point: CGPoint) {
        let target = wireDrag?.target
        wireDrag = nil
        lastCheckedTarget = nil
        guard let bridge, let target else { return }
        guard viewModel.isSceneEditableForAgents else { return }
        commitRewire(sourceHandle: source.handle, sourceName: source.name, sourceCategory: source.category, target: target, bridge: bridge)
    }

    private func commitRewire(sourceHandle: UInt64, sourceName: String, sourceCategory: Int, target: WirePortTarget, bridge: RISEViewportBridge) {
        guard let outcome = bridge.rewireConnection(
            targetCategory: target.nodeCategory, targetName: target.nodeName, param: target.paramName,
            occurrence: target.occurrence, newRefCategory: sourceCategory, newRefName: sourceName) else { return }
        if outcome.applied {
            // P2-1 (S21 review round 1): `outcome.nowUnreferenced` names
            // exactly what just got orphaned, but this success branch does
            // not read it -- and does not need to. `GraphCanvasNode.isOrphaned`
            // (see its comment) recomputes the SAME fact from the snapshot on
            // every fetch, so all this event needs to do is trigger the one
            // it already triggers below: `scheduleReload(force: true)` pulls
            // a fresh snapshot, and the orphan badge falls out of that for
            // free. Consuming `nowUnreferenced` here would mean carrying a
            // second, event-sourced "which nodes are orphaned" state that
            // could drift from the snapshot (e.g. after a later undo) --
            // strictly worse than the self-healing snapshot read.
            _ = bridge.setSelection(target.nodeCategory == 2 ? .material : .painter, name: target.nodeName)
            selectedHandle = target.nodeHandle
            refreshTrigger &+= 1
            scheduleReload(debounced: false, force: true)
        } else {
            wireRefusal = WireRefusalState(outcome: outcome, target: target, sourceHandle: sourceHandle, sourceName: sourceName, sourceCategory: sourceCategory)
        }
    }

    /// The SharedTarget escape hatch (S19/S20), made interactive: fork
    /// the dragged SOURCE node via `-duplicateGraphNode`, then retry the
    /// exact same rewire against the fork's deduped name -- the fork
    /// solely owns whatever the original shared, so the retry lands
    /// Clean.
    private func retryWithDuplicate(_ refusal: WireRefusalState) {
        guard let bridge else { return }
        let dup = bridge.duplicateGraphNode(category: refusal.sourceCategory, name: refusal.sourceName)
        guard let dup, dup.applied, !dup.newName.isEmpty else {
            presentGraphAlert(title: "Couldn't duplicate \"\(refusal.sourceName)\"",
                               message: dup?.message.isEmpty == false ? dup!.message : "The duplicate was refused.")
            return
        }
        wireRefusal = nil
        refreshTrigger &+= 1
        commitRewire(sourceHandle: refusal.sourceHandle, sourceName: dup.newName, sourceCategory: refusal.sourceCategory, target: refusal.target, bridge: bridge)
    }

    // MARK: - S21: delete / duplicate

    private func selectedNode() -> GraphCanvasNode? {
        guard let h = selectedHandle else { return nil }
        return snapshot.nodes.first(where: { $0.handle == h })
    }

    /// The confirm dialog below is what offers cascade explicitly, per
    /// the file header's "cascade ONLY behind an explicit confirm" rule
    /// -- this function takes no cascade flag of its own; the alert's
    /// button choice is the sole source of truth.
    private func deleteSelected() {
        guard let node = selectedNode(), let bridge, viewModel.isSceneEditableForAgents else { return }

        let confirm = NSAlert()
        confirm.messageText = "Delete \"\(node.name)\"?"
        confirm.informativeText = "Only this node is removed. Any painter it solely owned that becomes unreferenced is left in place as an orphan, unless you choose Delete + Remove Unused Below. This can be undone with Edit > Undo."
        confirm.alertStyle = .warning
        confirm.addButton(withTitle: "Delete")
        confirm.addButton(withTitle: "Delete + Remove Unused Below")
        confirm.addButton(withTitle: "Cancel")
        let choice = confirm.runModal()
        guard choice == .alertFirstButtonReturn || choice == .alertSecondButtonReturn else { return }
        let doCascade = (choice == .alertSecondButtonReturn)

        // Re-check the edit gate after the modal returns -- the confirm
        // blocks the main thread, and a render can start/finish while
        // it's up (same re-check RenderViewModel.removeEntity performs).
        guard viewModel.isSceneEditableForAgents else { return }

        guard let outcome = bridge.deleteGraphNode(category: node.category, name: node.name, cascade: doCascade) else { return }
        if outcome.applied {
            if selectedHandle == node.handle { selectedHandle = nil }
            refreshTrigger &+= 1
            scheduleReload(debounced: false, force: true)
            // "the RESULT shows what was removed -- honest, no fake
            // preview" (file header): only worth a follow-up when a
            // cascade actually swept more than the named target.
            if outcome.removed.count > 1 {
                presentGraphAlert(title: "Removed \(outcome.removed.count) chunks",
                                   message: outcome.removed.joined(separator: ", "))
            }
        } else {
            presentGraphAlert(title: "Couldn't delete \"\(node.name)\"", message: outcome.message.isEmpty ? "The delete was refused." : outcome.message)
        }
    }

    private func duplicateSelected() {
        guard let node = selectedNode(), let bridge, viewModel.isSceneEditableForAgents else { return }
        guard let outcome = bridge.duplicateGraphNode(category: node.category, name: node.name) else { return }
        if outcome.applied {
            refreshTrigger &+= 1
            scheduleReload(debounced: false, force: true)
            // Select the fork once the next fetch lands it -- the
            // fork's handle doesn't exist yet in `snapshot`, so this
            // just primes the name-based bridge selection immediately
            // (the outliner/panel reflect it right away even before the
            // graph refetch resolves a handle for it).
            _ = bridge.setSelection(node.category == 2 ? .material : .painter, name: outcome.newName)
        } else {
            presentGraphAlert(title: "Couldn't duplicate \"\(node.name)\"", message: outcome.message.isEmpty ? "The duplicate was refused." : outcome.message)
        }
    }

    /// A REFUSAL -- the edit was legal to attempt but the engine declined
    /// it (ownership, reference-safety, legality). Matches RenderViewModel's
    /// own `presentEntityEditAlert` house style (`.warning`, no explicit
    /// button -- NSAlert's implicit single button already reads "OK").
    private func presentGraphAlert(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.runModal()
    }

    /// P3 (S21 review round 1): a GENUINE failure -- something the canvas
    /// expected to just work (a sidecar write) came back with an
    /// unexpected error, as opposed to `presentGraphAlert`'s ordinary
    /// business-rule refusal above. Matches RenderViewModel's OWN
    /// distinction for this same split: its I/O-failure alerts
    /// ("Failed to save file", `saveEditorFile`/`saveAndReloadScene`/
    /// `showSaveAlert`) use `.critical`, while its refusal alerts
    /// (`presentEntityEditAlert`, "Couldn't delete...") use `.warning` --
    /// this file's `presentGraphAlert` was using `.warning` for BOTH
    /// cases before this fix. `addButton` is explicit rather than relying
    /// on NSAlert's implicit single button, matching `showSaveAlert`'s own
    /// explicit-OK style for this alert class.
    private func presentGraphFailureAlert(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .critical
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    // MARK: - S21: add-node palette

    /// The candidate pool the palette's required-reference picker
    /// filters -- every node currently on the canvas, Painter/Function
    /// candidates only (a Material can never legally fill a painter
    /// reference slot; the picker still runs each one through
    /// `-checkConnectionByKeyword` for the SPECIFIC param, this is just
    /// the pre-filter that keeps that O(n) sweep from also querying
    /// obviously-wrong candidates).
    private var paletteCandidateNodes: [PaletteCandidateNode] {
        snapshot.nodes.filter { $0.category != 2 }.map {
            PaletteCandidateNode(name: $0.name, keyword: $0.chunkKeyword, category: $0.category)
        }
    }

    private func openPalette(atViewportCenterOf size: CGSize) {
        if size.width > 1 && size.height > 1 {
            // Convert the viewport's on-screen center into content-space
            // (invert the pan/zoom transform `canvasContent` renders
            // under) so a newly created node lands where the user is
            // actually looking, not at a fixed canvas-space point that
            // could be far outside the current scroll position.
            let screenCenter = CGPoint(x: size.width / 2, y: size.height / 2)
            paletteDropPoint = CGPoint(
                x: (screenCenter.x - offset.width) / max(scale, 0.0001) - GraphMetrics.nodeWidth / 2,
                y: (screenCenter.y - offset.height) / max(scale, 0.0001) - GraphMetrics.nodeHeight / 2)
        }
        showPalette = true
    }

    /// Called after `NodeGraphAddNodeSheet` reports a successful create.
    /// The sheet already committed via `-createChunkNode`; this only
    /// (a) persists the drop-point position (create-time auto-layout
    /// would otherwise place it by rank, ignoring where the user asked
    /// for it) and (b) selects + refreshes.
    private func didCreateNode(named name: String, category: Int) {
        showPalette = false
        guard let bridge else { return }
        var outError: NSString? = nil
        _ = bridge.writeGraphNodeLayoutPosition(name: name, x: Double(paletteDropPoint.x), y: Double(paletteDropPoint.y), outError: &outError)
        _ = bridge.setSelection(category == 2 ? .material : .painter, name: name)
        refreshTrigger &+= 1
        scheduleReload(debounced: false, force: true)
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
    /// This node is part of the CURRENT object-pick spotlight closure
    /// (`NodeGraphCanvas.spotlightHandles`) -- a visually DISTINCT state
    /// from `isSelected` on purpose: `isSelected` is this canvas's own
    /// click-to-select, while a spotlight is driven by the shared
    /// selection sitting on an OBJECT elsewhere (viewport/outliner), which
    /// this canvas must never claim as ITS OWN selection (no
    /// `bridge.setSelection` call from a spotlight -- see
    /// `NodeGraphCanvas.refreshSpotlight`). Several nodes can be spotlit
    /// at once; at most one is ever `isSelected`.
    let isSpotlit: Bool
    let isHovered: Bool
    let onSelect: () -> Void
    let onOpenDefs: () -> Void
    let onHover: (Bool) -> Void
    /// S21 drag-to-reposition: live position (content-space, absolute --
    /// NOT a delta) on every gesture update, and once more on release.
    let onDragChanged: (CGPoint) -> Void
    let onDragEnded: (CGPoint) -> Void

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
                .stroke(borderColor, lineWidth: (isSelected || isSpotlit) ? 2 : 1)
        )
        .shadow(color: .black.opacity(isHovered ? 0.28 : 0.14), radius: isHovered ? 7 : 3, y: 2)
        // A SECOND, warm-gold glow layered on top of the normal hover
        // shadow -- opacity/radius both 0 when not spotlit, so this is a
        // pure no-op for every node outside the current closure. Kept as
        // its own `.shadow` (SwiftUI stacks them) rather than folded into
        // the one above so the ordinary hover shadow is untouched.
        .shadow(color: isSpotlit ? Theme.gold.opacity(0.6) : .clear, radius: isSpotlit ? 8 : 0)
        .contentShape(Rectangle())
        // S21: the reposition-drag gesture is tried FIRST via
        // `exclusively(before:)` -- `DragGesture(minimumDistance: 3)`
        // never recognizes a plain click (no movement past 3pt), so a
        // tap/double-tap with no meaningful drag falls straight through
        // to the tap combo below exactly as before; a real drag is
        // consumed here and the tap combo never fires for it. Attaching
        // this to a NAMED coordinate space (`"graphSpace"`, declared by
        // the canvas content container this box is a child of) is what
        // makes `value.location`/`.startLocation` report true
        // content-space coordinates regardless of the ancestor
        // `.scaleEffect`/`.offset` the whole canvas pans/zooms under --
        // the standard, ambiguity-free fix for "drag inside a zoomed
        // container" (a plain `.translation` would need manual
        // `/ scale` correction and silently drift at non-1.0 zoom).
        .gesture(
            DragGesture(minimumDistance: 3, coordinateSpace: .named("graphSpace"))
                .onChanged { value in
                    let delta = CGSize(width: value.location.x - value.startLocation.x,
                                        height: value.location.y - value.startLocation.y)
                    onDragChanged(CGPoint(x: node.position.x + delta.width, y: node.position.y + delta.height))
                }
                .onEnded { value in
                    let delta = CGSize(width: value.location.x - value.startLocation.x,
                                        height: value.location.y - value.startLocation.y)
                    onDragEnded(CGPoint(x: node.position.x + delta.width, y: node.position.y + delta.height))
                }
                .exclusively(before:
                    // `exclusively(before:)` is the documented SwiftUI combinator for
                    // "try the double-tap first; if it doesn't complete within the
                    // system double-click interval, fall through to the single-tap
                    // handler" — the correct way to layer two `TapGesture`s of
                    // different counts on ONE view (plain sequential
                    // `.onTapGesture(count:)` modifiers do not reliably disambiguate
                    // against each other the way count-vs-drag priority does
                    // elsewhere in this app, e.g. ExposureSliderRow's double-tap-to-
                    // reset over the Slider's own drag gesture).
                    TapGesture(count: 2).onEnded { onOpenDefs() }
                        .exclusively(before: TapGesture(count: 1).onEnded { onSelect() })
                )
        )
        .onHover(perform: onHover)
        .help(node.name)
    }

    private var borderColor: Color {
        if isSelected { return Theme.accent }
        if isSpotlit { return Theme.gold }
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
            // Mutually exclusive by construction (isShared needs > 1
            // in-edges, isOrphaned needs 0), so there is never a layout
            // conflict over which badge shows.
            if node.isShared {
                fanOutBadge
            } else if node.isOrphaned {
                orphanBadge
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

    /// S21 review round 1 P2-1: the same fan-out-badge idiom, for the
    /// opposite case -- nothing on the canvas references this node any
    /// more (a rewire, delete, or undo stepped away from it and left it
    /// in the document; NODE_GRAPH_CANVAS.md sect. 5's "this slice does
    /// NOT auto-delete newly-unreferenced chunks" scoping). Recomputed
    /// from `node.isOrphaned` every snapshot -- see that property's own
    /// comment for why this needs no event plumbing.
    private var orphanBadge: some View {
        HStack(spacing: 2) {
            Image(systemName: "link.badge.minus").font(.system(size: 7, weight: .bold))
            Text("orphan").font(Theme.mono(8, .semibold))
        }
        .foregroundColor(Theme.warn)
        .padding(.horizontal, 4)
        .padding(.vertical, 1)
        .background(Color.black.opacity(0.28))
        .clipShape(Capsule())
        .help("No other node on the canvas references \"\(node.name)\" -- left in place by a rewire, delete, or undo; Delete removes it if it is no longer needed")
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
    /// S21 drag-to-wire LIVE PREVIEW -- non-nil `wireDraftFrom`/`To`
    /// draws one extra curve from the dragged node's output handle to
    /// the current drag point, colored by `wireDraftLegal` (nil =
    /// neutral, not yet checked; true/false = Theme.success/.error,
    /// matching the status line's dot). `hoveredPort`, when set, draws a
    /// highlight ring around the candidate input port so the drop
    /// target is unambiguous even before the legality read lands.
    var wireDraftFrom: CGPoint? = nil
    var wireDraftTo: CGPoint? = nil
    var wireDraftLegal: Bool? = nil
    var hoveredPort: CGPoint? = nil

    var body: some View {
        Canvas { context, _ in
            for node in nodes {
                let destPortYs = graphInputPortAnchors(nodePosition: node.position, outEdgeCount: node.outEdges.count)
                for (portIdx, port) in node.outEdges.enumerated() {
                    let destPoint = CGPoint(x: node.position.x, y: destPortYs[portIdx])
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

            if let from = wireDraftFrom, let to = wireDraftTo {
                let color: Color = wireDraftLegal == false ? Theme.errorStrong : (wireDraftLegal == true ? Theme.success : Theme.textFaint)
                drawWire(context: context, from: from, to: to, color: color, dashed: wireDraftLegal != true)
            }
            if let hover = hoveredPort {
                let ringColor: Color = wireDraftLegal == false ? Theme.errorStrong : Theme.success
                var ring = Path()
                ring.addEllipse(in: CGRect(x: hover.x - 9, y: hover.y - 9, width: 18, height: 18))
                context.stroke(ring, with: .color(ringColor), lineWidth: 2)
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
        // Right-align so the label sits just left of the input port it
        // labels, matching "param label drawn at the target end."
        context.draw(resolved, at: CGPoint(x: point.x - 4, y: point.y - 7), anchor: .trailing)
    }
}
