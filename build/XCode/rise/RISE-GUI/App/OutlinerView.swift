//////////////////////////////////////////////////////////////////////
//
//  OutlinerView.swift - Top of the right panel: a per-category tree
//    of the scene's entities (Rasterizer / Cameras / Lights / Objects
//    / Materials / Media / Output Settings / Animation / Variants).
//
//    Replaces the old stacked nine-section accordion's navigation
//    role.  Clicking a category header toggles that category's
//    expansion; clicking a child row selects it.  Both actions route
//    through the SAME RISEViewportBridge selection/expansion calls
//    the pre-redesign accordion used (RISEViewportBridge is the
//    single source of truth for "what's expanded" and "what's
//    selected" so viewport click-to-pick and outliner clicks stay in
//    sync automatically) — see PropertiesPanel.swift, which reads the
//    bridge's PRIMARY selection (selectionCategory / selectionName)
//    to drive the inspector below this view.
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
    OutlinerCategoryDef(title: "Output Settings", category: .film,        tag: "FLM", tagColor: Theme.catFilm),
    OutlinerCategoryDef(title: "Animation",       category: .animation,   tag: "ANM", tagColor: Theme.catAnimation),
    OutlinerCategoryDef(title: "Variants",        category: .sceneVariant, tag: "VAR", tagColor: Theme.catVariant),
]

// MARK: - OutlinerView

struct OutlinerView: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int
    // "Reveal in scene file" context-menu item routes through the shared
    // view model (same bridge call PropertiesPanel's ⌗ chip uses) —
    // available via the environment the same way PropertiesPanel picks
    // it up, since both live under ContentView's rightPanel.
    @EnvironmentObject var viewModel: RenderViewModel

    @State private var entitiesByCategory: [Int: [String]] = [:]
    @State private var activeByCategory: [Int: String] = [:]
    @State private var expandedByCategory: [Int: Bool] = [:]
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
                            let children = entitiesByCategory[cat.category.rawValue] ?? []
                            // Rasterizer/Film rows have no chunk address
                            // (registry/preset names, not chunk names) —
                            // the SAME guard that gates "Reveal in scene
                            // file" also gates Duplicate/Delete, since
                            // both routes need chunk-name addressing.
                            let canMutate = viewModel.isSceneEditableForAgents
                                && cat.category != .rasterizer
                                && cat.category != .film
                            ForEach(children, id: \.self) { child in
                                OutlinerChildRow(
                                    name: child,
                                    isSelected: selectionCategory == cat.category && selectionName == child,
                                    isActive: isActiveEntity(cat: cat, name: child),
                                    canReveal: canMutate,
                                    canMutate: canMutate,
                                    onSelect: { selectChild(cat: cat, name: child) },
                                    onReveal: { viewModel.revealEntityInSceneText(category: cat.category, name: child) },
                                    onDuplicate: { viewModel.duplicateSelectedOrNamed(category: cat.category, name: child) },
                                    onDelete: { viewModel.removeEntity(category: cat.category, name: child) }
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

    private var totalEntityCount: Int {
        entitiesByCategory.values.reduce(0) { $0 + $1.count }
    }

    private func categoryRow(_ cat: OutlinerCategoryDef) -> some View {
        let expanded = expandedByCategory[cat.category.rawValue] ?? false
        let count = entitiesByCategory[cat.category.rawValue]?.count ?? 0
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

        // Entity lists only need re-pulling when the scene structure
        // actually changed (add/remove) — cheap epoch check mirrors
        // the pre-redesign accordion's caching.
        let epoch = UInt(bridge.sceneEpoch)
        if force || epoch != lastEpoch {
            lastEpoch = epoch
            var fresh: [Int: [String]] = [:]
            for cat in kOutlinerCategories {
                fresh[cat.category.rawValue] = bridge.categoryEntities(cat.category)
            }
            entitiesByCategory = fresh
        }
    }
}

// MARK: - Child row

private struct OutlinerChildRow: View {
    let name: String
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
    let onSelect: () -> Void
    let onReveal: () -> Void
    let onDuplicate: () -> Void
    let onDelete: () -> Void

    @State private var hovering = false

    var body: some View {
        HStack(spacing: 7) {
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
        .padding(.leading, 30)
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
