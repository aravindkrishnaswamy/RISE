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
    OutlinerCategoryDef(title: "Media",           category: .medium,      tag: "MED", tagColor: Theme.catMedia),
    OutlinerCategoryDef(title: "Output Settings", category: .film,        tag: "FLM", tagColor: Theme.catFilm),
    OutlinerCategoryDef(title: "Animation",       category: .animation,   tag: "ANM", tagColor: Theme.catAnimation),
    OutlinerCategoryDef(title: "Variants",        category: .sceneVariant, tag: "VAR", tagColor: Theme.catVariant),
]

// MARK: - OutlinerView

struct OutlinerView: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int

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
                            ForEach(children, id: \.self) { child in
                                OutlinerChildRow(
                                    name: child,
                                    isSelected: selectionCategory == cat.category && selectionName == child,
                                    isActive: isActiveEntity(cat: cat, name: child),
                                    onSelect: { selectChild(cat: cat, name: child) }
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

    private func reload() {
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
        if epoch != lastEpoch {
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
    let onSelect: () -> Void

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
    }
}
