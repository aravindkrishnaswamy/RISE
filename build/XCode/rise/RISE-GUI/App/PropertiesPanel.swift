//////////////////////////////////////////////////////////////////////
//
//  PropertiesPanel.swift - Right-side accordion for the interactive
//    viewport.  Four sections (Cameras / Rasterizer / Objects /
//    Lights) each list the scene's entities for that category;
//    clicking a row activates it on the C++ side and shows the
//    selected entity's read-only/edit properties below.
//
//    Single selection across the whole panel: picking a row in any
//    section clears whichever was picked before, and auto-expands
//    that section while collapsing the others.  Click-on-image
//    object picking routes through the same machinery so the
//    Objects section auto-expands and the picked row is highlighted.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI

/// Mirrors RISE::ValueKind in ChunkDescriptor.h.  Used to pick a
/// suitable input control (single-line text for now; future phases
/// can specialise for Bool / Enum / Reference).
enum PropertyKind: Int {
    case bool       = 0
    case uint       = 1
    case double     = 2
    case doubleVec3 = 3
    case string     = 4
    case filename   = 5
    case enumKind   = 6
    case reference  = 7
}

/// Single quick-pick preset option, mirrors RISEViewportPropertyPreset.
struct PropertyPreset: Identifiable, Hashable {
    let id: String
    let label: String
    let value: String
}

/// Properties displayed in the right panel.  Built from
/// RISEViewportProperty instances via `from(_:)`.
struct PropertyRow: Identifiable {
    let id: String                // parameter name (unique per entity)
    let name: String
    let initialValue: String
    let description: String
    let kind: PropertyKind
    let editable: Bool
    let presets: [PropertyPreset] // empty when descriptor declared no presets
    let unitLabel: String         // empty for dimensionless / unlabelled fields

    nonisolated static func from(_ src: RISEViewportProperty) -> PropertyRow {
        let presets: [PropertyPreset] = src.presets.enumerated().map { (idx, p) in
            PropertyPreset(id: "\(src.name).preset.\(idx)", label: p.label, value: p.value)
        }
        return PropertyRow(
            id: src.name,
            name: src.name,
            initialValue: src.value,
            description: src.describing,
            kind: PropertyKind(rawValue: src.kind) ?? .string,
            editable: src.editable,
            presets: presets,
            unitLabel: src.unitLabel
        )
    }
}

/// One section of the accordion — Cameras, Rasterizer, Objects, or
/// Lights.  Wraps the bridge's RISEViewportCategory so the SwiftUI
/// view can drive its DisclosureGroup binding cleanly.
struct AccordionSection: Identifiable, Hashable {
    let id: String                // stable identifier (matches the C-API name)
    let title: String             // user-visible title
    let category: RISEViewportCategory
}

private let kAccordionSections: [AccordionSection] = [
    AccordionSection(id: "cameras",     title: "Cameras",    category: .camera),
    AccordionSection(id: "rasterizer",  title: "Rasterizer", category: .rasterizer),
    AccordionSection(id: "objects",     title: "Objects",    category: .object),
    AccordionSection(id: "lights",      title: "Lights",     category: .light),
]

struct PropertiesPanel: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int          // increment to force a snapshot reload

    @State private var rows: [PropertyRow] = []
    @State private var header: String = ""
    @State private var mode: RISEViewportPanelMode = .none
    @State private var selectionCategory: RISEViewportCategory = .none
    @State private var selectionName: String = ""
    @State private var entitiesByCategory: [Int: [String]] = [:]
    @State private var activeNameByCategory: [Int: String] = [:]
    @State private var lastEpoch: UInt = 0

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(header.isEmpty ? "Scene" : header)
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                Spacer()
                Button {
                    reload()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .help("Refresh from the live scene")
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            ScrollView(.vertical) {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(kAccordionSections) { section in
                        // Prefer the user's explicit pick when present;
                        // otherwise fall back to the scene's active
                        // entity (Camera = active camera, Rasterizer =
                        // active rasterizer, Object/Light = empty).
                        // This way the dropdown shows the active
                        // entity on first load instead of "(pick one)".
                        let resolvedName: String =
                            (section.category == selectionCategory && !selectionName.isEmpty)
                                ? selectionName
                                : (activeNameByCategory[section.category.rawValue] ?? "")
                        AccordionSectionView(
                            section: section,
                            entities: entitiesByCategory[section.category.rawValue] ?? [],
                            isExpanded: section.category == selectionCategory,
                            selectedName: resolvedName,
                            onSelectRow: { name in
                                bridge.setSelection(section.category, name: name)
                                reload()
                            },
                            onToggle: { newExpanded in
                                if newExpanded {
                                    // Open this section: empty-name selection
                                    // means "expand without picking a row".
                                    bridge.setSelection(section.category, name: "")
                                } else if selectionCategory == section.category {
                                    // Collapse the currently-open section.
                                    bridge.setSelection(.none, name: "")
                                }
                                reload()
                            }
                        )
                        // Property rows render directly under the
                        // expanded section — keeps the cause/effect
                        // visually obvious.
                        if section.category == selectionCategory && mode != .none {
                            PropertyList(rows: rows, bridge: bridge, onCommitted: reload)
                                .padding(.leading, 12)
                        }
                    }
                }
                .padding(8)
            }
        }
        .frame(minWidth: 240, idealWidth: 280)
        .onAppear { reload() }
        .onChange(of: refreshTrigger) { _, _ in reload() }
    }

    private func reload() {
        bridge.refreshProperties()
        mode = bridge.panelMode
        header = bridge.panelHeader
        selectionCategory = bridge.selectionCategory
        selectionName = bridge.selectionName
        rows = bridge.propertySnapshot().map(PropertyRow.from)

        // Re-pull per-section entity lists when the scene epoch
        // advances (scene reload, structural mutation).  Cheap to
        // pull on every refresh too — the lists are small — but the
        // epoch gate keeps the JNI-style chatter down on busy frames.
        let epoch = UInt(bridge.sceneEpoch)
        if epoch != lastEpoch {
            lastEpoch = epoch
            var fresh: [Int: [String]] = [:]
            for section in kAccordionSections {
                fresh[section.category.rawValue] = bridge.categoryEntities(section.category)
            }
            entitiesByCategory = fresh
        }

        // Active-name lookup is cheap (one C-API hop per category) and
        // can change between epochs (a SetActiveCamera can happen
        // without adding/removing cameras).  Re-pull on every reload.
        var freshActive: [Int: String] = [:]
        for section in kAccordionSections {
            freshActive[section.category.rawValue] = bridge.activeName(for: section.category)
        }
        activeNameByCategory = freshActive
    }
}

/// One accordion section: header + dropdown picker for the section's
/// entity list.  All four sections use the dropdown for visual
/// consistency and to handle worst-case entity counts (Objects can
/// run into the hundreds).
private struct AccordionSectionView: View {
    let section: AccordionSection
    let entities: [String]
    let isExpanded: Bool
    let selectedName: String
    let onSelectRow: (String) -> Void
    let onToggle: (Bool) -> Void

    var body: some View {
        DisclosureGroup(
            isExpanded: Binding(
                get: { isExpanded },
                set: { newValue in
                    // Guard spurious set callbacks during the
                    // animated disclosure transition — SwiftUI can
                    // re-issue `set` with the value it just got from
                    // `get`, which would re-route a no-op
                    // setSelection.  Only forward real changes.
                    if newValue != isExpanded { onToggle(newValue) }
                }
            ),
            content: {
                if entities.isEmpty {
                    Text("(none)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .padding(.vertical, 4)
                        .padding(.leading, 8)
                } else {
                    EntityDropdown(
                        entities: entities,
                        selectedName: selectedName,
                        onSelect: onSelectRow
                    )
                }
            },
            label: {
                Text(section.title)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.primary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .contentShape(Rectangle())
                    .onTapGesture { onToggle(!isExpanded) }
            }
        )
        .padding(.vertical, 2)
    }
}

/// Dropdown picker for sections (Rasterizer / Object) that can hold
/// many entries.  Uses a `Menu` so the selection UX matches the
/// per-property presets dropdowns the rest of the panel uses.
/// Picking an item routes through `onSelect` exactly as a list-row
/// click did, so the rest of the selection plumbing is untouched.
private struct EntityDropdown: View {
    let entities: [String]
    let selectedName: String
    let onSelect: (String) -> Void

    var body: some View {
        HStack {
            Menu {
                ForEach(entities, id: \.self) { name in
                    Button(name) {
                        if name != selectedName { onSelect(name) }
                    }
                }
            } label: {
                HStack(spacing: 4) {
                    Text(selectedName.isEmpty ? "(pick one)" : selectedName)
                        .font(.system(size: 11, design: .monospaced))
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .foregroundColor(selectedName.isEmpty ? .secondary : .primary)
                    Spacer()
                    Image(systemName: "chevron.up.chevron.down")
                        .font(.system(size: 9))
                        .foregroundColor(.secondary)
                }
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(Color.secondary.opacity(0.3), lineWidth: 1)
                )
            }
            .menuStyle(.borderlessButton)
        }
        .padding(.horizontal, 4)
        .padding(.vertical, 4)
    }
}

/// The property-row list shown under whichever accordion section is
/// currently expanded.  Same descriptor-driven rendering as before;
/// kept as a separate view so each section's rows render in-place.
private struct PropertyList: View {
    let rows: [PropertyRow]
    let bridge: RISEViewportBridge
    let onCommitted: () -> Void

    var body: some View {
        if rows.isEmpty {
            Text("No properties available.")
                .font(.caption)
                .foregroundColor(.secondary)
                .padding(.vertical, 4)
        } else {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(rows) { row in
                    PropertyRowView(
                        row: row,
                        onCommit: { newValue in
                            _ = bridge.setPropertyName(row.name, value: newValue)
                            onCommitted()
                        },
                        onScrubBegin: { bridge.beginPropertyScrub() },
                        onScrubEnd:   { bridge.endPropertyScrub()   }
                    )
                }
            }
            .padding(.vertical, 4)
        }
    }
}

/// One row: label on top, editable text field below (or read-only text).
private struct PropertyRowView: View {
    let row: PropertyRow
    let onCommit: (String) -> Void
    let onScrubBegin: () -> Void
    let onScrubEnd:   () -> Void

    @State private var text: String = ""
    @FocusState private var isFocused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(row.name)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(.secondary)
                Spacer()
                if !row.editable {
                    Text("read-only")
                        .font(.system(size: 9))
                        .foregroundColor(.tertiaryLabelColor)
                }
            }

            if row.editable {
                HStack(spacing: 4) {
                    if isScrubbable(kind: row.kind) {
                        ScrubHandle(
                            text: $text,
                            name: row.name,
                            kind: row.kind,
                            onScrubBegin: onScrubBegin,
                            onScrub: { newValue in
                                onCommit(newValue)
                            },
                            onScrubEnd: onScrubEnd
                        )
                    }
                    TextField("", text: $text)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.caption, design: .monospaced))
                        .focused($isFocused)
                        .onSubmit { onCommit(text) }
                        .onChange(of: isFocused) { _, focused in
                            if !focused && text != row.initialValue {
                                onCommit(text)
                            }
                        }
                    if !row.unitLabel.isEmpty {
                        Text(row.unitLabel)
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                            .fixedSize()
                    }
                    if !row.presets.isEmpty {
                        Menu {
                            ForEach(row.presets) { preset in
                                Button(preset.label) {
                                    text = preset.value
                                    onCommit(preset.value)
                                }
                            }
                        } label: {
                            Image(systemName: "list.bullet")
                                .font(.system(size: 10))
                        }
                        .menuStyle(.borderlessButton)
                        .fixedSize()
                        .help("Quick-pick presets")
                    }
                }
            } else {
                HStack(spacing: 4) {
                    Text(text.isEmpty ? row.initialValue : text)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundColor(.secondary)
                        .padding(.horizontal, 4)
                        .padding(.vertical, 2)
                    if !row.unitLabel.isEmpty {
                        Text(row.unitLabel)
                            .font(.system(size: 10))
                            .foregroundColor(.tertiaryLabelColor)
                    }
                }
            }

            if !row.description.isEmpty {
                Text(row.description)
                    .font(.system(size: 10))
                    .foregroundColor(.tertiaryLabelColor)
                    .lineLimit(2)
            }
        }
        .onAppear { text = row.initialValue }
        .onChange(of: row.initialValue) { _, newValue in
            if !isFocused { text = newValue }
        }
    }
}

/// Single-numeric kinds get the scrub handle.  Multi-component (Vec3)
/// and string-like fields don't — scrubbing a vector is ambiguous, and
/// keyboard entry is the natural input for strings.
private func isScrubbable(kind: PropertyKind) -> Bool {
    switch kind {
    case .double, .uint: return true
    default:             return false
    }
}

/// The chevron handle that drives click-and-drag value scrubbing on
/// numeric fields.  Drag up = increase, drag down = decrease.
private struct ScrubHandle: View {
    @Binding var text: String
    let name: String
    let kind: PropertyKind
    let onScrubBegin: () -> Void
    let onScrub: (String) -> Void
    let onScrubEnd:   () -> Void

    @State private var dragStart: ScrubStart? = nil

    private struct ScrubStart {
        let value: Double
        let yOrigin: CGFloat
    }

    var body: some View {
        Image(systemName: "chevron.up.chevron.down")
            .font(.system(size: 9, weight: .medium))
            .foregroundColor(.secondary.opacity(dragStart == nil ? 0.7 : 1.0))
            .frame(width: 16, height: 18)
            .contentShape(Rectangle())
            .onHover { entered in
                if entered { NSCursor.resizeUpDown.push() }
                else       { NSCursor.pop() }
            }
            .help("Drag up/down to change.  Shift = fine, Option = coarse.")
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .local)
                    .onChanged { gesture in
                        if dragStart == nil {
                            let v = Double(text) ?? 0
                            dragStart = ScrubStart(
                                value: v,
                                yOrigin: gesture.startLocation.y
                            )
                            NSCursor.resizeUpDown.push()
                            onScrubBegin()
                        }
                        guard let start = dragStart else { return }

                        let dy = start.yOrigin - gesture.location.y

                        var rate = scrubRate(name: name, value: start.value)
                        let mods = NSEvent.modifierFlags
                        if mods.contains(.shift)  { rate *= 0.25 }
                        if mods.contains(.option) { rate *= 4.0 }

                        let newValue = start.value + Double(dy) * rate
                        let formatted = formatValue(newValue, kind: kind)
                        text = formatted
                        onScrub(formatted)
                    }
                    .onEnded { _ in
                        if dragStart != nil {
                            NSCursor.pop()
                            dragStart = nil
                            onScrubEnd()
                        }
                    }
            )
    }
}

private func isAngularField(_ name: String) -> Bool {
    switch name {
    case "theta", "phi", "fov", "pitch", "yaw", "roll",
         "aperture_rotation", "tilt_x", "tilt_y":
        return true
    default:
        return false
    }
}

private func scrubRate(name: String, value: Double) -> Double {
    if isAngularField(name) { return 0.5 }
    return max(abs(value), 1e-3) * 0.005
}

private func formatValue(_ v: Double, kind: PropertyKind) -> String {
    switch kind {
    case .uint:
        let n = max(0, Int(v.rounded()))
        return String(n)
    case .double:
        return String(format: "%.6g", v)
    default:
        return String(format: "%g", v)
    }
}

private extension Color {
    static var tertiaryLabelColor: Color { Color(nsColor: .tertiaryLabelColor) }
}
