//////////////////////////////////////////////////////////////////////
//
//  OutlinerView.swift - Top of the right panel: a per-category tree
//    of the scene's entities (Rasterizer / Cameras / Lights / Objects
//    / Materials / Media / Groups / Output Settings / Animation /
//    Variants).
//
//    Two levels deep for every category except Groups, which is three:
//    category header -> group row -> member object rows (arc-86 slice 5,
//    docs/agentic-redesign/86-object-grouping.md).  A member row selects
//    as an ORDINARY OBJECT, so the object property panel and the
//    viewport gizmo keep working on it unchanged; only the group row
//    itself selects as a group.  Members ALSO remain listed under the
//    flat Objects category -- that category is the complete object list
//    by definition, and v1 keeps it complete rather than hiding grouped
//    objects from it.
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
    // Geometry (GUI redesign 2026-07-22): every "*_geometry" chunk, the
    // shapes objects reference by name.  Listed after Media, before the
    // singleton rows.  No dedicated Theme token yet -- catObject's hue is
    // the conceptual neighbour (geometry IS what objects instantiate).
    OutlinerCategoryDef(title: "Geometry",        category: .geometry,    tag: "GEO", tagColor: Theme.catObject),
    // Groups (arc-86 slice 5): `group` chunks, the only category here
    // whose rows expand to a THIRD level (their member objects).
    // Appended at the end of the entity block -- the same place
    // Geometry was added -- rather than next to Objects, so the
    // established ordering of the categories above is undisturbed.
    // No dedicated Theme token; catObject's hue is the conceptual
    // neighbour, since a group is a composition OF objects.
    OutlinerCategoryDef(title: "Groups",          category: .group,       tag: "GRP", tagColor: Theme.catObject),
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

    // arc-86 slice 5: the third tree level.  `membersByGroup` is CLEARED on
    // the SAME epoch cadence as the category entity lists (a group's
    // membership only changes when the scene structure does) but re-primed
    // by a separate loop that runs on EVERY reload() call -- see the P2 fix
    // (F1, GUI-fix-round) comment in reload() for why the clear and the
    // (re)fill are two separate steps rather than one inline rebuild: an
    // inline rebuild caches a groupMembers() pull's result unconditionally,
    // including an EMPTY one, which is always a stale read (a real group
    // can't have zero members), not a real 0-member group -- freezing that
    // group's row at count 0 until the next structural edit.
    // `expandedGroups` is per-GROUP-NAME expansion, held here rather
    // than in the bridge because the core's expansion state is
    // per-CATEGORY (`isSectionExpanded(for:)`) and has no per-row
    // concept.  Keyed by name so it survives the epoch-driven refresh
    // exactly the way `expandedByCategory` does.  Deliberately NOT
    // pruned to the currently-declared groups on each refresh: the member
    // snapshot goes momentarily stale while a render owns the scene, and
    // pruning against a stale list would silently collapse the user's open
    // rows.  They ARE cleared wholesale on a SCENE CHANGE (the `.task(id:
    // ObjectIdentifier(bridge))` modifier below -- P2 fix F2, GUI-fix-round;
    // keyed on the bridge's identity rather than `loadedFilePath` so a
    // same-path reopen clears too) -- mirroring the Qt shell, whose
    // setBridge() clears both hashes so a group name from the old scene
    // cannot leak into the new one's tree or silently pre-expand a
    // same-named group in it.
    @State private var membersByGroup: [String: [String]] = [:]
    @State private var expandedGroups: [String: Bool] = [:]
    /// Name of the group row the pointer is currently over, or nil.  One
    /// view-level slot rather than per-row `@State` — `groupRow` is a method,
    /// not a row struct, and only one row can be hovered at a time anyway.
    @State private var hoveredGroup: String? = nil

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
                            if cat.category == .group {
                                // arc-86 slice 5: the one category with a third
                                // level.  Group rows carry their own disclosure
                                // triangle; their members are rendered beneath
                                // at a deeper indent and select as OBJECTS.
                                ForEach(children, id: \.self) { g in
                                    groupRow(name: g, canReveal: canMutate)
                                    if expandedGroups[g] == true {
                                        let members = membersByGroup[g] ?? []
                                        // Indexed rather than `id: \.self`: a
                                        // `group` chunk may legally repeat a
                                        // `member` line, and duplicate ids break
                                        // ForEach identity.
                                        ForEach(Array(members.enumerated()), id: \.offset) { _, m in
                                            OutlinerChildRow(
                                                name: m,
                                                isSelected: selectionCategory == .object && selectionName == m,
                                                isActive: false,
                                                canReveal: viewModel.isSceneEditableForAgents,
                                                canMutate: viewModel.isSceneEditableForAgents,
                                                // Delete is OFF on a member row, and only here:
                                                // removing a standard_object chunk that a `group`
                                                // still names as a `member` cannot succeed — the
                                                // core's remove dry-run re-derive fails on the
                                                // group's unresolvable member — so the item would
                                                // be a guaranteed confirm-then-fail pair.  It
                                                // stays enabled on the SAME object's row in the
                                                // flat Objects list, where the membership that
                                                // causes the refusal is not visible; this row
                                                // exists BECAUSE of that membership, so this is
                                                // the one place that can explain it.
                                                canDelete: false,
                                                leadingIndent: 48,
                                                onSelect: { selectChild(category: .object, name: m) },
                                                onReveal: { viewModel.revealEntityInSceneText(category: .object, name: m) },
                                                onDuplicate: { viewModel.duplicateSelectedOrNamed(category: .object, name: m) },
                                                onDelete: { viewModel.removeEntity(category: .object, name: m) }
                                            )
                                        }
                                    }
                                }
                            } else {
                                ForEach(children, id: \.self) { child in
                                    OutlinerChildRow(
                                        name: child,
                                        isSelected: selectionCategory == cat.category && selectionName == child,
                                        isActive: isActiveEntity(cat: cat, name: child),
                                        canReveal: canMutate,
                                        canMutate: canMutate,
                                        onSelect: { selectChild(category: cat.category, name: child) },
                                        onReveal: { viewModel.revealEntityInSceneText(category: cat.category, name: child) },
                                        onDuplicate: { viewModel.duplicateSelectedOrNamed(category: cat.category, name: child) },
                                        onDelete: { viewModel.removeEntity(category: cat.category, name: child) }
                                    )
                                }
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
        // arc-86 slice 5: a NEW SCENE invalidates both group-tree caches.
        // `expandedGroups` is keyed by group NAME, so without this a group
        // called "cart" that the user had opened in the previous scene would
        // render pre-expanded in the next one that happens to have a "cart" —
        // the exact hazard Qt's setBridge() names when it clears its two
        // hashes.  `membersByGroup` is rebuilt wholesale by reload() anyway;
        // cleared here too so the two never disagree even for one frame.
        //
        // P2 fix (F2, GUI-fix-round): keyed on the BRIDGE's identity, not
        // `viewModel.loadedFilePath`.  This view's `@State` is scoped to its
        // position in the SwiftUI tree, not to which `bridge` value it was
        // last handed -- a same-path reopen assigns the SAME string to
        // `loadedFilePath` (RenderViewModel.swift's load completion sets
        // `self.loadedFilePath = untitled ? nil : path`, unconditionally, no
        // `!=` guard), so `onChange(of: loadedFilePath)` never fired for it,
        // even though `RenderViewModel` had already swapped in a BRAND NEW
        // `RISEViewportBridge` over the freshly-reloaded Job.  This view's
        // `expandedGroups` / `membersByGroup` then survived pointed at a
        // GROUP NAME that happens to still exist in the reopened scene, but
        // whose row identity, member list, and disclosure state are now
        // read through a retired controller's stale idea of a live one.
        // `.task(id:)` re-runs its body whenever `id` changes (and once on
        // first appearance, same as `.onAppear` above) -- ContentView.swift
        // already uses exactly this pattern (`.task(id:
        // ObjectIdentifier(vb))`) to reset `viewportLayout` on every fresh
        // bridge; this adopts the same idiom rather than inventing a second
        // one.  `RISEViewportBridge` is an `NSObject` subclass (reference
        // type), so `ObjectIdentifier` distinguishes every construction,
        // including a same-path reopen's brand new instance.
        .task(id: ObjectIdentifier(bridge)) {
            expandedGroups.removeAll()
            membersByGroup.removeAll()
            reload(force: true)
        }
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

    private func selectChild(category: RISEViewportCategory, name: String) {
        _ = bridge.setSelection(category, name: name)
        refreshTrigger &+= 1
    }

    /// arc-86 slice 5: a single group's row, between the "Groups"
    /// category header and the member rows.  It has a second tap target
    /// the ordinary child rows don't: the leading triangle toggles this
    /// group's member list, while a tap anywhere else SELECTS the group
    /// (category `.group`), which drives the properties panel to the
    /// group's own position / orientation / scale.
    ///
    /// Its context menu offers "Reveal in scene file" only.  Duplicate /
    /// Delete are deliberately omitted this slice: a `group` chunk is
    /// not a manager entity, so those two routes have no coverage for it
    /// yet, and offering a control that may half-work is worse than
    /// leaving group CRUD to the scene text.
    private func groupRow(name: String, canReveal: Bool) -> some View {
        let expanded = expandedGroups[name] ?? false
        let count = (membersByGroup[name] ?? []).count
        let isSelected = selectionCategory == .group && selectionName == name
        return HStack(spacing: 7) {
            if count > 0 {
                // A Button, not a second .onTapGesture: this row already has a
                // row-wide tap (select the group), and a nested tap gesture's
                // precedence over its parent's is not something to rely on.  A
                // button's own hit region resolves it unambiguously.
                Button {
                    expandedGroups[name] = !expanded
                } label: {
                    Text(expanded ? "▾" : "▸")
                        .font(.system(size: 8))
                        .foregroundColor(Theme.textDim)
                        .frame(width: 10, alignment: .center)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            } else {
                // Empty group: an INERT spacer, matching the Qt twin
                // (buildGroupRow's `else` branch).  A live Button here would
                // flip `expandedGroups[name]` on click and then reveal nothing
                // and change no glyph — a control that does nothing, which is
                // worse than no control.  Same width reserved so the name stays
                // column-aligned with the groups that do have a disclosure.
                Color.clear.frame(width: 10, height: 10)
            }
            Text(name)
                .font(Theme.sans(11.5))
                .foregroundColor(isSelected ? .white : Theme.textMuted)
                .lineLimit(1)
                .truncationMode(.tail)
            Spacer(minLength: 4)
            Text("\(count)")
                .font(Theme.mono(10))
                .foregroundColor(Theme.textDisabled)
        }
        .padding(.vertical, 4)
        .padding(.leading, 30)
        .padding(.trailing, 8)
        .background(
            RoundedRectangle(cornerRadius: Theme.radiusSmall)
                .fill(isSelected ? Theme.accent.opacity(0.14)
                                 : (hoveredGroup == name ? Theme.fillHover : Color.clear))
        )
        .contentShape(Rectangle())
        // Hover feedback, matching OutlinerChildRow (which every other Mac
        // outliner row is).  Tracked as ONE view-level "which name is hovered"
        // rather than a per-row `@State`, because groupRow is a method on this
        // view, not a row struct that could own state of its own.
        .onHover { inside in
            if inside { hoveredGroup = name }
            else if hoveredGroup == name { hoveredGroup = nil }
        }
        .onTapGesture { selectChild(category: .group, name: name) }
        .contextMenu {
            Button("Reveal in scene file") {
                viewModel.revealEntityInSceneText(category: .group, name: name)
            }
            .disabled(!canReveal)
        }
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

            // arc-86 P2 fix (F1, GUI-fix-round): drop the WHOLE group-member
            // cache on a structural change -- a still-declared group needs a
            // FRESH pull too (its OWN membership may be what changed), not
            // just groups that vanished.  This clear does not repopulate
            // anything by itself; the unconditional prime loop below (which
            // runs on EVERY reload() call, not only a structural one) does
            // that, one groupMembers() call per declared group.  Splitting
            // the drop from the (re)fill is what fixes the actual bug: the
            // prime loop only caches a NON-empty pull, so a group whose pull
            // bails right here -- `bridge.groupMembers` reads the
            // controller's snapshot, which is skipped while a render owns
            // the scene or the refresh lost its try_lock -- is left UNPRIMED
            // instead of an empty result getting baked in under this
            // just-stamped epoch.  The OLD code called `bridge.groupMembers`
            // and stored its result unconditionally right in this block, so
            // a bailed pull froze the group at count 0 with no disclosure
            // triangle until the NEXT structural edit bumped the epoch
            // again.  Left unprimed, it simply retries on the very next
            // `reload()` call (every preview frame, via
            // `onChange(of: refreshTrigger)`), self-healing within one
            // frame instead.
            membersByGroup.removeAll()
        }

        // arc-86 P2 fix (F1): prime any group not yet cached, on EVERY
        // reload() call -- independent of the epoch/force gate above.  Only
        // fills in MISSING keys (never overwrites an already-cached,
        // presumed-good entry) and only caches a NON-empty pull -- see the
        // comment above for why an empty pull must never be cached.  An
        // empty groupMembers() result is never a real 0-member group:
        // GroupAsciiChunkParser::Finalize hard-rejects `members.empty()` at
        // parse time, so an empty read here is definitionally stale, not a
        // genuine empty group.
        for g in (entitiesByCategory[RISEViewportCategory.group.rawValue] ?? []) {
            if membersByGroup[g] != nil { continue }
            let members = bridge.groupMembers(g)
            if !members.isEmpty { membersByGroup[g] = members }
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
    /// arc-86 slice 5: a SECOND gate on "Delete", on top of `canMutate`.
    /// Defaults to true (every pre-existing call site keeps its behaviour);
    /// the group MEMBER row lowers it, because the core is guaranteed to
    /// refuse deleting an object a `group` chunk still lists.  The menu item
    /// is kept but disabled and RE-LABELLED with the reason — a SwiftUI
    /// context-menu item cannot carry a hover tooltip, so the label is the
    /// only place the explanation can live.
    var canDelete: Bool = true
    /// arc-86 slice 5: leading indent in points.  30 is the original
    /// (and default) one-level-deep child indent; group MEMBERS pass a
    /// deeper value so the third tree level reads as nested under its
    /// group row rather than as a sibling of it.
    var leadingIndent: CGFloat = 30
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
        .padding(.leading, leadingIndent)
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
            if canDelete {
                Button("Delete", action: onDelete)
                    .disabled(!canMutate)
            } else {
                Button("Delete — remove from its group first", action: {})
                    .disabled(true)
            }
        }
    }
}
