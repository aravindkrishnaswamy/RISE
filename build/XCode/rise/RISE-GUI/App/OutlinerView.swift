//////////////////////////////////////////////////////////////////////
//
//  OutlinerView.swift - Top of the right panel: a per-category
//    RECURSIVE TREE over the scene's AUTHORED graph (Rasterizer /
//    Cameras / Lights / Objects / Materials / Painters / Media /
//    Geometry / Output Settings / Animation / Variants).
//
//    87 §5 step 4b.  This used to be a hand-rolled TWO-LEVEL list
//    (category header + one flat row per entity).  It now draws
//    whatever depth the authored graph has, over the generic
//    node-children API 4a added to SceneEditController: an Object with
//    parts has its parts nested under it, and a category whose entities
//    have no hierarchy is N ROOTS WITH NO CHILDREN, so there is no
//    per-category branch anywhere below — the flat categories render
//    through the identical code path they always did.
//
//    Clicking a category header toggles that category's expansion;
//    clicking a child row selects it; clicking a child row's disclosure
//    glyph expands that node.  The first two route through the SAME
//    RISEViewportBridge selection/expansion calls the pre-redesign
//    accordion used (the bridge is the single source of truth for
//    "what's expanded" and "what's selected" so viewport click-to-pick
//    and outliner clicks stay in sync automatically) — see
//    PropertiesPanel.swift, which reads the bridge's PRIMARY selection
//    (selectionCategory / selectionName) to drive the inspector below
//    this view.  Per-NODE expansion is the one piece of state the
//    bridge does not model, and is held here (see `expandedPaths`).
//
//////////////////////////////////////////////////////////////////////

import SwiftUI

// MARK: - Local category tag colors

// Theme.swift (owned by a parallel workstream this slice must not
// touch) already defines catCamera/catLight/catObject/catMaterial/
// catRender/catAnimation/catMedia for the seven category tags that
// existed pre-outliner.  The outliner surfaces two more categories
// (Output Settings / Film and scene_variant overlays) that don't have
// a token yet — added here as a same-module extension rather than by
// editing Theme.swift directly.
// MARK: - Category metadata

private struct OutlinerCategoryDef {
    let title: String
    let category: RISEViewportCategory
    let tag: String
    let tagColor: Color
}

/// Order mirrors the pre-redesign accordion's grouping (render surface
/// first, then the entities that feed it) rather than the design
/// comp's cosmetic order — an intentional continuity choice since both
/// are equally defensible and this one matches the rest of the app.
private let kOutlinerCategories: [OutlinerCategoryDef] = [
    OutlinerCategoryDef(title: "Rasterizer",      category: .rasterizer,   tag: "RND", tagColor: Theme.catRender),
    OutlinerCategoryDef(title: "Cameras",         category: .camera,      tag: "CAM", tagColor: Theme.catCamera),
    OutlinerCategoryDef(title: "Lights",          category: .light,       tag: "LGT", tagColor: Theme.catLight),
    OutlinerCategoryDef(title: "Objects",         category: .object,      tag: "OBJ", tagColor: Theme.catObject),
    OutlinerCategoryDef(title: "Materials",       category: .material,    tag: "MAT", tagColor: Theme.catMaterial),
    // Painters feed materials/media -- listed right after Materials.
    // Theme.swift is owned by a parallel workstream (see the note
    // above) and has no dedicated painter token; catMaterial's
    // lilac is the closest conceptual match and is explicitly the
    // first-choice fallback for this slice.
    OutlinerCategoryDef(title: "Painters",        category: .painter,     tag: "PNT", tagColor: Theme.catMaterial),
    OutlinerCategoryDef(title: "Media",           category: .medium,      tag: "MED", tagColor: Theme.catMedia),
    // Geometry (GUI redesign 2026-07-22): every "*_geometry" chunk, the
    // shapes objects reference by name.  Listed after Media, before the
    // singleton rows.  No dedicated Theme token yet -- catObject's hue is
    // the conceptual neighbour (geometry IS what objects instantiate).
    OutlinerCategoryDef(title: "Geometry",        category: .geometry,    tag: "GEO", tagColor: Theme.catObject),
    OutlinerCategoryDef(title: "Output Settings", category: .film,        tag: "FLM", tagColor: Theme.catFilm),
    OutlinerCategoryDef(title: "Animation",       category: .animation,   tag: "ANM", tagColor: Theme.catAnimation),
    OutlinerCategoryDef(title: "Variants",        category: .sceneVariant, tag: "VAR", tagColor: Theme.catVariant),
]

// MARK: - Tree model

/// One row of a category's authored tree, already resolved to the two
/// things the view needs that the bridge does not carry: the row's
/// DEPTH and its expand-state PATH KEY.
///
/// WHY A SWIFT-SIDE VALUE COPY PER REFRESH.  SceneEditController hands
/// out generation-tagged node HANDLES that are valid only WITHIN ONE
/// WALK against the generation observed — an incremental edit
/// republishes a whole category's tree and refuses every outstanding
/// handle, and for Geometry that happens on every tick of a radius drag
/// (SceneEditController.h, "HANDLE CHURN").  So no handle, and no index
/// into the bridge's array, is ever parked across an event-loop turn:
/// one `categoryTree(_:)` call rebuilds this whole array and the view
/// draws only that.  Selection is BY NAME, which survives a republish.
private struct OutlinerNode {
    /// The entity name — the row's label AND the identity passed to
    /// `setSelection`, `revealEntityInSceneText`, `duplicateSelectedOrNamed`
    /// and `removeEntity`.  The bridge applies the same `NamedViewDisplayName`
    /// decode the flat `categoryEntities(_:)` path applies, and that decode
    /// is a UTF-8→String conversion rather than a cosmetic transform, so
    /// there is no display/raw split to keep apart here.
    let name: String
    /// Expand-state key: this node's ANCESTRY within its category.  See
    /// `outlinerPathKey` for the encoding and for why a name key is wrong.
    let path: String
    /// 0 for a root; the nesting level the row is drawn at.
    let depth: Int
    /// Whether the node has children — drives the disclosure glyph.  A
    /// childless node draws no glyph at all, which is exactly what every
    /// row of a flat category is.
    let hasChildren: Bool
}

/// Build the expand-state key for a node from its ANCESTRY.
///
/// 87 §5 step 4 keys expand state on the tree PATH, NOT on the node
/// name, and both halves of that matter:
///
///  - A NAME KEY WOULD COLLIDE.  A name is unique only within one
///    manager, and this view draws eleven categories side by side — a
///    Material and an Object may both be called `glass`, and
///    `RISEViewportCategoryPainter` is itself a UNION of two managers
///    with independent contents (SceneEditController.h, TreeNodeRow::serial).
///    One `Set<String>` keyed by bare name would tie unrelated rows'
///    disclosure state together, so the key is category-qualified and
///    then ancestry-qualified.
///  - A PATH KEY IS WHAT SURVIVES A REFRESH.  The array is rebuilt from
///    scratch on every epoch bump, so any key derived from a position —
///    a node index, a row number — names a different node afterwards.
///    Ancestry is stable across a rebuild for exactly the same reason
///    selection is: it is made of names.
///
/// The encoding is LENGTH-PREFIXED (`<byteCount>:<name>` per component)
/// rather than separator-joined, because names may legitimately contain
/// any separator one might pick — 87 §5 step 3 records that an author
/// may write `name my.object`, and step 3's instanced descendants are
/// named `I.X` by construction.  A length prefix makes the composition
/// injective whatever the bytes are, so two distinct paths can never
/// produce one key.
private func outlinerPathKey(category: RISEViewportCategory, ancestry: [String]) -> String {
    var key = "\(category.rawValue)"
    for component in ancestry {
        key += "/\(component.utf8.count):\(component)"
    }
    return key
}

/// Flatten one category's tree into DEPTH-FIRST display order.
///
/// Iterative, with an explicit stack, for the same reason
/// `SceneEditController::BuildAuthoredTree` is: a pathologically deep
/// parent chain must not be able to overflow the stack in the UI layer
/// after the core went to the trouble of not overflowing in its own.
///
/// Roots and each child list are consumed IN THE ORDER THE BRIDGE GIVES
/// THEM — that order is 87 §2's "child order for display comes from
/// declaration order" and is not re-sorted here.
private func outlinerFlatten(_ tree: RISESceneTree,
                             category: RISEViewportCategory) -> [OutlinerNode] {
    let bridgeNodes = tree.nodes
    var out: [OutlinerNode] = []
    out.reserveCapacity(bridgeNodes.count)

    // (index, depth, ancestry-so-far).  Reversed pushes so the stack pops
    // siblings in declaration order.
    var stack: [(index: Int, depth: Int, ancestry: [String])] = []
    for root in tree.roots.reversed() {
        stack.append((Int(truncating: root), 0, []))
    }
    var visited = Set<Int>()

    while let top = stack.popLast() {
        guard top.index >= 0 && top.index < bridgeNodes.count else { continue }
        // The core guarantees every node is reached exactly once (pinned by
        // SceneGraphNodeApiTest case Y); this only stops a hypothetical
        // malformed tree from spinning here rather than failing visibly.
        guard visited.insert(top.index).inserted else { continue }

        let node = bridgeNodes[top.index]
        let ancestry = top.ancestry + [node.name]
        out.append(OutlinerNode(name: node.name,
                                path: outlinerPathKey(category: category, ancestry: ancestry),
                                depth: top.depth,
                                hasChildren: !node.children.isEmpty))
        for child in node.children.reversed() {
            stack.append((Int(truncating: child), top.depth + 1, ancestry))
        }
    }

    // TOTALITY, defensively.  `BuildAuthoredTree`'s contract is that nothing
    // is ever dropped — a dangling parent is rooted and a cycle is broken so
    // that every node stays visible — and case Y pins that the roots+children
    // walk really does reach all of them.  If that ever stopped holding, an
    // entity would silently VANISH from the outliner, which reads as a
    // deleted object rather than as a bug.  So anything the walk missed is
    // shown as a root instead of being lost.
    if visited.count != bridgeNodes.count {
        for index in bridgeNodes.indices where !visited.contains(index) {
            let node = bridgeNodes[index]
            out.append(OutlinerNode(name: node.name,
                                    path: outlinerPathKey(category: category, ancestry: [node.name]),
                                    depth: 0,
                                    hasChildren: false))
        }
    }
    return out
}

/// The rows actually drawn: every node whose ancestors are all expanded.
///
/// A single forward pass over the depth-first array — when a collapsed
/// node is emitted, everything deeper than it is skipped until the depth
/// comes back down.  No recursion, and no per-row ancestor walk.
private func outlinerVisibleRows(_ nodes: [OutlinerNode],
                                 expanded: Set<String>) -> [OutlinerNode] {
    var out: [OutlinerNode] = []
    out.reserveCapacity(nodes.count)
    var hiddenBelowDepth: Int?
    for node in nodes {
        if let depth = hiddenBelowDepth {
            if node.depth > depth { continue }
            hiddenBelowDepth = nil
        }
        out.append(node)
        if node.hasChildren && !expanded.contains(node.path) {
            hiddenBelowDepth = node.depth
        }
    }
    return out
}

// MARK: - OutlinerView

struct OutlinerView: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int
    // "Reveal in scene file" context-menu item routes through the shared
    // view model (same bridge call PropertiesPanel's ⌗ chip uses) —
    // available via the environment the same way PropertiesPanel picks
    // it up, since both live under ContentView's rightPanel.
    @EnvironmentObject var viewModel: RenderViewModel

    /// Per-category authored tree, depth-first, rebuilt on every epoch bump.
    @State private var nodesByCategory: [Int: [OutlinerNode]] = [:]
    @State private var activeByCategory: [Int: String] = [:]
    @State private var expandedByCategory: [Int: Bool] = [:]
    /// Per-NODE disclosure state, keyed by tree PATH (87 §5 step 4).
    ///
    /// Held here rather than in the bridge because the bridge models
    /// expansion per CATEGORY only (`isSectionExpanded(for:)` /
    /// `collapseSection(for:)`, which PropertiesPanel also reads) and has no
    /// per-node concept.  Keeping it as view state is what makes it survive a
    /// refresh: the node array is thrown away and rebuilt, this is not.
    ///
    /// Not pruned when a node disappears.  A stale key costs one string and
    /// re-creating the same node under the same parent restores its
    /// disclosure state, which is the behaviour a user expects from an undo.
    @State private var expandedPaths: Set<String> = []
    @State private var selectionCategory: RISEViewportCategory = .none
    @State private var selectionName: String = ""
    @State private var lastEpoch: UInt = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(kOutlinerCategories, id: \.category.rawValue) { cat in
                        categoryRow(cat)
                        if expandedByCategory[cat.category.rawValue] == true {
                            // Rasterizer/Film rows have no chunk address
                            // (registry/preset names, not chunk names) —
                            // the SAME guard that gates "Reveal in scene
                            // file" also gates Duplicate/Delete, since
                            // both routes need chunk-name addressing.
                            let canMutate = viewModel.isSceneEditableForAgents
                                && cat.category != .rasterizer
                                && cat.category != .film
                            let rows = outlinerVisibleRows(nodesByCategory[cat.category.rawValue] ?? [],
                                                           expanded: expandedPaths)
                            ForEach(rows, id: \.path) { node in
                                OutlinerChildRow(
                                    name: node.name,
                                    depth: node.depth,
                                    hasChildren: node.hasChildren,
                                    isExpanded: expandedPaths.contains(node.path),
                                    isSelected: selectionCategory == cat.category && selectionName == node.name,
                                    isActive: isActiveEntity(cat: cat, name: node.name),
                                    canReveal: canMutate,
                                    canMutate: canMutate,
                                    onToggle: { toggleNode(path: node.path) },
                                    onSelect: { selectChild(cat: cat, name: node.name) },
                                    onReveal: { viewModel.revealEntityInSceneText(category: cat.category, name: node.name) },
                                    onDuplicate: { viewModel.duplicateSelectedOrNamed(category: cat.category, name: node.name) },
                                    onDelete: { viewModel.removeEntity(category: cat.category, name: node.name) }
                                )
                            }
                        }
                    }
                }
                .padding(.horizontal, 8)
                .padding(.bottom, 10)
            }
            .frame(maxHeight: 305)
        }
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
        }
        .onAppear { reload() }
        .onChange(of: refreshTrigger) { _, _ in reload() }
        // Entity-creation slice: `viewModel.entityListEpoch` bumps the
        // instant a GUI-initiated Add/Duplicate/Delete commits, giving
        // an IMMEDIATE, deterministic outliner refresh (force: true
        // bypasses the epoch-cache short-circuit below).  The core ALSO
        // bumps the bridge's own `sceneEpoch` on every landed chunk CRUD
        // now (ApplyAgentChunkCrud_), so a non-forced `reload()` catches
        // structural changes too — importantly including AGENT-driven
        // adds/removes, which never touch `entityListEpoch`.
        .onChange(of: viewModel.entityListEpoch) { _, _ in reload(force: true) }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text("Scene")
                .font(Theme.sans(12.5, .semibold))
                .foregroundColor(Theme.textPrimary)
            Text("\(totalEntityCount) entities")
                .font(Theme.mono(10))
                .foregroundColor(Theme.textDim)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.top, 12)
        .padding(.bottom, 6)
    }

    /// Every node at every depth, across every category.
    ///
    /// Counts AUTHORED nodes, which for Objects is no longer the same as the
    /// flat entity count: 87 step 3's instancing expansions fold into the
    /// chunk that produced them, so an 8×8 array counts 1 here and 64 in the
    /// render list.  That is the point of drawing the authored graph — the
    /// count now matches what the author can actually edit.
    private var totalEntityCount: Int {
        nodesByCategory.values.reduce(0) { $0 + $1.count }
    }

    private func categoryRow(_ cat: OutlinerCategoryDef) -> some View {
        let expanded = expandedByCategory[cat.category.rawValue] ?? false
        let count = nodesByCategory[cat.category.rawValue]?.count ?? 0
        // Entity-creation slice: templates are queried live (cheap —
        // the bridge call is a couple of small C-API round-trips) so
        // the "+" affordance disappears automatically for categories
        // with none (Camera/Rasterizer/Film/Animation/SceneVariant).
        let templates = viewModel.entityTemplates(for: cat.category)
        let canAdd = !templates.isEmpty && viewModel.isSceneEditableForAgents
        return HStack(spacing: 7) {
            Text(expanded && count > 0 ? "▾" : "▸")
                .font(.system(size: 8))
                .foregroundColor(Theme.textDim)
                .frame(width: 8, alignment: .center)
            Text(cat.tag)
                .font(Theme.mono(9.5))
                .foregroundColor(cat.tagColor)
                .frame(width: 26, alignment: .leading)
            Text(cat.title)
                .font(Theme.sans(11.5))
                .foregroundColor(Theme.textTertiary)
                .lineLimit(1)
            Spacer(minLength: 4)
            if canAdd {
                Menu {
                    ForEach(templates, id: \.index) { t in
                        Button(t.label) {
                            viewModel.addEntity(category: cat.category, templateIndex: t.index)
                        }
                    }
                } label: {
                    Image(systemName: "plus.circle")
                        .font(.system(size: 10.5))
                        .foregroundColor(Theme.textDim)
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .help("Add \(cat.title.hasSuffix("s") ? String(cat.title.dropLast()) : cat.title)")
            }
            Text("\(count)")
                .font(Theme.mono(10))
                .foregroundColor(Theme.textDisabled)
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 8)
        .contentShape(Rectangle())
        .onTapGesture { toggleCategory(cat) }
    }

    /// True when `name` is the category's scene-level active entity
    /// (active camera, active rasterizer, the single Film's "default").
    /// Categories with no active-entity concept (Object / Light /
    /// Material / Media / Animation / Variants) always return false —
    /// `activeName(for:)` is documented to return empty for those, so
    /// this never fabricates a marker the bridge doesn't back.
    private func isActiveEntity(cat: OutlinerCategoryDef, name: String) -> Bool {
        guard let active = activeByCategory[cat.category.rawValue], !active.isEmpty else { return false }
        return active == name
    }

    private func toggleCategory(_ cat: OutlinerCategoryDef) {
        let expanded = expandedByCategory[cat.category.rawValue] ?? false
        if expanded {
            bridge.collapseSection(for: cat.category)
        } else {
            // Empty-name selection opens the section without picking a
            // row — same mechanism the pre-redesign accordion used.
            _ = bridge.setSelection(cat.category, name: "")
        }
        refreshTrigger &+= 1
    }

    /// Per-node disclosure.  Purely local — see `expandedPaths` for why the
    /// bridge is not involved, and why nothing needs to be re-read here.
    private func toggleNode(path: String) {
        if expandedPaths.contains(path) {
            expandedPaths.remove(path)
        } else {
            expandedPaths.insert(path)
        }
    }

    private func selectChild(cat: OutlinerCategoryDef, name: String) {
        _ = bridge.setSelection(cat.category, name: name)
        refreshTrigger &+= 1
    }

    /// `force: true` bypasses the epoch-cache short-circuit below —
    /// used by the `entityListEpoch` observer for an immediate refresh
    /// the instant a GUI CRUD commits, without waiting for the next
    /// epoch-driven `reload()`.  (The core now bumps `bridge.sceneEpoch`
    /// on chunk CRUD too, so the non-forced path also catches structural
    /// changes — including agent-driven ones.)
    private func reload(force: Bool = false) {
        selectionCategory = bridge.selectionCategory
        selectionName = bridge.selectionName

        var freshExpanded: [Int: Bool] = [:]
        var freshActive: [Int: String] = [:]
        for cat in kOutlinerCategories {
            freshExpanded[cat.category.rawValue] = bridge.isSectionExpanded(for: cat.category)
            freshActive[cat.category.rawValue] = bridge.activeName(for: cat.category)
        }
        expandedByCategory = freshExpanded
        activeByCategory = freshActive

        // Trees only need re-pulling when the scene structure actually
        // changed (add/remove/re-parent) — cheap epoch check mirrors the
        // pre-redesign accordion's caching.
        //
        // A DRAG-TO-REPARENT gesture, when one is added, must bump the scene
        // epoch itself: re-parenting changes the TREE without changing any
        // category's entity LIST, so nothing else in the pipeline would
        // notice and this short-circuit would keep the old shape on screen.
        // SceneEditController.h says the same thing at ReadTree.
        let epoch = UInt(bridge.sceneEpoch)
        if force || epoch != lastEpoch {
            lastEpoch = epoch
            var fresh: [Int: [OutlinerNode]] = [:]
            for cat in kOutlinerCategories {
                fresh[cat.category.rawValue] = outlinerFlatten(bridge.categoryTree(cat.category),
                                                               category: cat.category)
            }
            nodesByCategory = fresh
        }
    }
}

// MARK: - Child row

private struct OutlinerChildRow: View {
    let name: String
    /// Nesting level; 0 is a root of its category.  Leaf rows at depth 0
    /// land at exactly the x-position the pre-4b flat rows did (the
    /// disclosure gutter is carved out of the old 30pt leading inset, not
    /// added to it), so a flat category looks unchanged.
    let depth: Int
    let hasChildren: Bool
    let isExpanded: Bool
    let isSelected: Bool
    let isActive: Bool
    /// Mirrors `RenderViewModel.isSceneEditableForAgents` — gates the
    /// "Reveal in scene file" context-menu item so it's not offered
    /// while a render owns the scene (the underlying bridge call would
    /// wedge on the controller's commit mutex). Resolvability (a
    /// particular name/category actually having a chunk location) isn't
    /// pre-checked here — an unresolvable target just no-ops, same as
    /// the properties-panel chip when hidden mid-render.
    let canReveal: Bool
    /// Entity-creation slice: gates "Duplicate"/"Delete" the same way
    /// `canReveal` gates "Reveal in scene file" — both need chunk-name
    /// addressing (no Rasterizer/Film) and `isSceneEditableForAgents`.
    /// Kept as a separate field (rather than reusing `canReveal`
    /// directly in the menu) so a future divergence between the two
    /// gates doesn't require touching every call site.
    let canMutate: Bool
    let onToggle: () -> Void
    let onSelect: () -> Void
    let onReveal: () -> Void
    let onDuplicate: () -> Void
    let onDelete: () -> Void

    @State private var hovering = false

    /// 15 + 8 (glyph) + 7 (HStack spacing) puts a depth-0 name at x = 30,
    /// which is where every child row sat before this view became a tree.
    private var leadingInset: CGFloat { 15 + CGFloat(depth) * 13 }

    var body: some View {
        HStack(spacing: 7) {
            // Same ▸/▾ glyph the category header uses, so one visual
            // language covers both levels of disclosure.  Childless rows
            // reserve the width and draw nothing, which keeps names in a
            // column instead of jittering by whether a node has parts.
            Group {
                if hasChildren {
                    Text(isExpanded ? "▾" : "▸")
                        .font(.system(size: 8))
                        .foregroundColor(Theme.textDim)
                } else {
                    Color.clear
                }
            }
            .frame(width: 8, alignment: .center)
            .contentShape(Rectangle())
            // Inner gesture wins over the row-level one below, so hitting
            // the glyph discloses and hitting anywhere else selects.
            .onTapGesture { if hasChildren { onToggle() } }
            Text(name)
                .font(Theme.sans(11.5))
                .foregroundColor(isSelected ? .white : Theme.textMuted)
                .lineLimit(1)
                .truncationMode(.tail)
            Spacer(minLength: 4)
            if isActive {
                Text("ACTIVE")
                    .font(Theme.mono(9.5))
                    .foregroundColor(Theme.accentLight)
            }
        }
        .padding(.vertical, 4)
        .padding(.leading, leadingInset)
        .padding(.trailing, 8)
        .background(
            RoundedRectangle(cornerRadius: Theme.radiusSmall)
                .fill(isSelected ? Theme.accent.opacity(0.14) : (hovering ? Theme.fillHover : Color.clear))
        )
        .contentShape(Rectangle())
        .onTapGesture { onSelect() }
        .onHover { hovering = $0 }
        .contextMenu {
            Button("Reveal in scene file", action: onReveal)
                .disabled(!canReveal)
            Divider()
            Button("Duplicate", action: onDuplicate)
                .disabled(!canMutate)
            Button("Delete", action: onDelete)
                .disabled(!canMutate)
        }
    }
}
