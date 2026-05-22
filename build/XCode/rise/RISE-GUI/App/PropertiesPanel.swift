//////////////////////////////////////////////////////////////////////
//
//  PropertiesPanel.swift - Right-side accordion for the interactive
//    viewport.  Five sections (Cameras / Rasterizer / Objects /
//    Lights / Output Settings — the scene Film) each list the scene's
//    entities for that category; clicking a row activates it on the
//    C++ side and shows the selected entity's read-only/edit
//    properties below.
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
    AccordionSection(id: "cameras",     title: "Cameras",         category: .camera),
    AccordionSection(id: "rasterizer",  title: "Rasterizer",      category: .rasterizer),
    AccordionSection(id: "objects",     title: "Objects",         category: .object),
    AccordionSection(id: "lights",      title: "Lights",          category: .light),
    AccordionSection(id: "materials",   title: "Materials",       category: .material),
    AccordionSection(id: "media",       title: "Media",           category: .medium),
    AccordionSection(id: "film",        title: "Output Settings", category: .film),
]

struct PropertiesPanel: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int          // increment to force a snapshot reload

    // Phase 6.5: the panel's Save / Save-As buttons read these two
    // properties off the shared RenderViewModel — `sceneEditsDirty`
    // (gates the buttons' enable state) and `loadedFilePath` (the
    // default target for in-place Save).  Both are @Published, so
    // SwiftUI re-evaluates the header HStack on every transition.
    @EnvironmentObject var viewModel: RenderViewModel

    @State private var rows: [PropertyRow] = []   // primary-section rows; kept for the header / panel-wide refresh trigger
    @State private var rowsByCategory: [Int: [PropertyRow]] = [:]   // Phase 4b: per-section property rows
    @State private var selectionByCategory: [Int: String] = [:]      // Phase 4b: per-section picked entity
    @State private var expandedByCategory: [Int: Bool] = [:]         // Phase 4b: per-section accordion expansion (independent of selection)
    @State private var header: String = ""
    @State private var mode: RISEViewportPanelMode = .none
    @State private var selectionCategory: RISEViewportCategory = .none
    @State private var selectionName: String = ""
    @State private var entitiesByCategory: [Int: [String]] = [:]
    @State private var activeNameByCategory: [Int: String] = [:]
    @State private var lastEpoch: UInt = 0
    // Tracks whether we've already shown the "new cameras only live in
    // memory" caveat in this session, so we surface it exactly once.
    @State private var addCameraCaveatShown: Bool = false
    // Re-entry guard against the "+" button firing a second prompt
    // while the first NSAlert is still on screen.  NSAlert.runModal()
    // blocks AppKit's event loop for the alert window but does not
    // disable the parent panel's button — a synthetic Return-key
    // event or rapid double-click could open a second alert behind
    // the first.  Set the flag for the modal's lifetime; the button's
    // .disabled binding consults it.
    @State private var addCameraInFlight: Bool = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text(header.isEmpty ? "Scene" : header)
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                Spacer()

                // Phase 6.5: text-labeled buttons (matches Windows
                // ViewportProperties + standard desktop affordance).
                // SF-Symbol-only buttons disappear visually when
                // .disabled in a .borderless style — text + .bordered
                // stays discoverable even when greyed out.
                //
                // Save: in-place write to the originally-loaded path.
                // Disabled when there's nothing to write OR no path
                // was loaded (which would force Save-As anyway).
                Button("Save") {
                    performSceneSave(useLoadedPath: true)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(!viewModel.sceneEditsDirty
                          || viewModel.loadedFilePath == nil)
                .help(saveButtonHelpText(forSaveAs: false))

                // Save As… — opens NSSavePanel so the user can fork
                // the in-memory edits to a different file.  Enabled
                // whenever there are edits; the destination doesn't
                // have to be the originally-loaded path.
                Button("Save As…") {
                    performSceneSave(useLoadedPath: false)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(!viewModel.sceneEditsDirty)
                .help(saveButtonHelpText(forSaveAs: true))

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
                        // Phase 4b: read expanded state + selection
                        // INDEPENDENTLY from the controller.  A
                        // section header click sends an empty-name
                        // SetSelection — that sets expanded=true with
                        // an empty pick, so the section opens with
                        // its dropdown visible but no entity-specific
                        // rows below.  Auto-sync of Material on an
                        // Object pick is handled controller-side.
                        let perCatPick = selectionByCategory[section.category.rawValue] ?? ""
                        let resolvedName: String =
                            !perCatPick.isEmpty
                                ? perCatPick
                                : (activeNameByCategory[section.category.rawValue] ?? "")
                        let sectionExpanded = expandedByCategory[section.category.rawValue] ?? false
                        let sectionRows = rowsByCategory[section.category.rawValue] ?? []
                        AccordionSectionView(
                            section: section,
                            entities: entitiesByCategory[section.category.rawValue] ?? [],
                            isExpanded: sectionExpanded,
                            selectedName: resolvedName,
                            onSelectRow: { name in
                                bridge.setSelection(section.category, name: name)
                                reload()
                            },
                            onToggle: { newExpanded in
                                if newExpanded {
                                    // Open this section: empty-name selection
                                    // sets the expanded flag without picking
                                    // a specific entity.
                                    bridge.setSelection(section.category, name: "")
                                } else if sectionExpanded {
                                    // Collapse this section ONLY — leave
                                    // other sections alone.  Pre-Phase-4b
                                    // this was a panel-wide collapse via
                                    // setSelection(.none, ...), but the
                                    // multi-section model needs a per-
                                    // section close.
                                    bridge.collapseSection(for: section.category)
                                }
                                reload()
                            },
                            // Only the Cameras section gets the "+" action.
                            // Other categories return nil → button hidden.
                            onAddEntity: section.category == .camera
                                ? { promptForNewCameraName(activeName: activeNameByCategory[section.category.rawValue] ?? "") }
                                : nil
                        )
                        // Property rows render directly under each
                        // expanded section.  Edit routes through the
                        // per-category SetProperty so the Material
                        // section's edits go to the right material
                        // even when Object is the primary selection.
                        if sectionExpanded && !sectionRows.isEmpty {
                            PropertyList(
                                rows: sectionRows,
                                bridge: bridge,
                                category: section.category,
                                onCommitted: reload )
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

    /// Prompt the user for a new camera name, then call the bridge's
    /// `addCameraFromActive` to clone the current camera under that
    /// name.  Default proposal is "<active>_copy".  On first
    /// successful add per session, also surfaces a caveat alert that
    /// the new camera lives in memory only (the SceneEditor's
    // ----------------------------------------------------------------
    // Phase 6.5: scene-file save action.  Both header buttons route
    // here.  `useLoadedPath == true` writes to the originally-loaded
    // .RISEscene; `false` opens an NSSavePanel so the user can fork.
    // Status codes mirror SaveResult::Status:
    //   0 = Saved   — silent success (button greys out via the dirty-
    //                 changed callback's clean→dirty=false transition)
    //   1 = NoOp    — silent (nothing to write).  Shouldn't normally
    //                 fire from a button that's disabled-when-clean,
    //                 but it's safe to ignore.
    //   2 = Refused — engine declined (cross-file target, barrier-
    //                 conflict, external modification).  Modal alert.
    //   3 = Failed  — IO error or file-not-found.  Modal alert.
    private func saveButtonHelpText(forSaveAs: Bool) -> String {
        if !viewModel.sceneEditsDirty {
            return "No changes to save"
        }
        if forSaveAs {
            return "Save scene to a chosen path…"
        }
        if let p = viewModel.loadedFilePath {
            return "Save scene to \(p)"
        }
        return "Use Save As… (no loaded path)"
    }

    private func performSceneSave(useLoadedPath: Bool) {
        // Resolve target path.  If the caller asked for in-place save
        // but no path is known (rare — usually a synthetic scene), fall
        // through to the Save-As panel.
        var target: String? = nil
        if useLoadedPath, let p = viewModel.loadedFilePath {
            target = p
        } else {
            let panel = NSSavePanel()
            panel.allowedContentTypes = []  // accept any extension
            panel.nameFieldStringValue = (viewModel.loadedFilePath as NSString?)?.lastPathComponent
                ?? "untitled.RISEscene"
            if let lp = viewModel.loadedFilePath {
                panel.directoryURL =
                    URL(fileURLWithPath: lp).deletingLastPathComponent()
            }
            panel.title = "Save Scene As"
            panel.message = "Choose a destination for the .RISEscene file."
            if panel.runModal() != .OK { return }
            target = panel.url?.path
        }
        guard let path = target, !path.isEmpty else { return }

        var errMsg: NSString? = nil
        let status = bridge.saveScene(to: path, errorMessage: &errMsg)
        switch status {
        case 0:
            // Saved.  Re-anchor `loadedFilePath` so subsequent
            // in-place Save targets the file we just wrote — matches
            // the library's FileIdentity re-anchor (SaveEngine.cpp
            // post-write block).  Without this, the GUI would still
            // show the ORIGINAL load path as the in-place save
            // target, even though the C++ session is now anchored
            // to the new file.
            if path != viewModel.loadedFilePath {
                viewModel.loadedFilePath = path
            }
        case 1:
            // NoOp — silent success.  The file is unchanged on
            // disk; no re-anchor needed.
            break
        case 2:
            showSaveAlert(
                title: "Save Refused",
                message: (errMsg as String?)
                    ?? "The save engine declined to write this file.")
        case 3:
            showSaveAlert(
                title: "Save Failed",
                message: (errMsg as String?)
                    ?? "An I/O error occurred while saving the file.")
        default:
            showSaveAlert(
                title: "Save Failed",
                message: "Unexpected save result (status \(status)).")
        }
    }

    private func showSaveAlert(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    /// scene-text round-trip is not yet implemented; reloading the
    /// .RISEscene file would drop it).
    private func promptForNewCameraName(activeName: String) {
        if addCameraInFlight { return }
        addCameraInFlight = true
        defer { addCameraInFlight = false }

        let alert = NSAlert()
        alert.messageText = "Add Camera"
        alert.informativeText =
            "Cloning the current camera.  Pick a name for the new camera.\n\n" +
            "Notes:\n" +
            "  • The clone is in-memory only — saving the .RISEscene file\n" +
            "    from the editor does not yet emit added cameras.\n" +
            "  • Duplicate names get a numeric suffix appended."
        alert.addButton(withTitle: "Add")
        alert.addButton(withTitle: "Cancel")
        let proposal = activeName.isEmpty ? "camera_copy" : "\(activeName)_copy"
        let input = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        input.stringValue = proposal
        input.becomeFirstResponder()
        alert.accessoryView = input
        let response = alert.runModal()
        if response != .alertFirstButtonReturn {
            return
        }
        let chosenName = input.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let newName = bridge.addCameraFromActive(proposedName: chosenName), !newName.isEmpty else {
            // Failure cases (no active camera, unclonable type, etc.)
            // Surface a short alert so the user understands why the
            // click had no effect — the controller already logged a
            // warning to RISE_Log.txt with the details.
            let fail = NSAlert()
            fail.messageText = "Couldn't add camera"
            fail.informativeText = "The current camera could not be cloned.  See RISE_Log.txt for details."
            fail.alertStyle = .warning
            fail.runModal()
            return
        }
        // Promote the new camera to the panel's selection so the user
        // sees its properties immediately.
        bridge.setSelection(.camera, name: newName)
        reload()

        // One-shot persistence caveat per session.
        if !addCameraCaveatShown {
            addCameraCaveatShown = true
            let caveat = NSAlert()
            caveat.messageText = "New camera \"\(newName)\" added"
            caveat.informativeText =
                "Heads up — added cameras are kept in memory only until the\n" +
                "scene-text round-trip lands.  Save your scene file from a\n" +
                "text editor to preserve them across reloads."
            caveat.alertStyle = .informational
            caveat.runModal()
        }
    }

    private func reload() {
        bridge.refreshProperties()
        mode = bridge.panelMode
        header = bridge.panelHeader
        selectionCategory = bridge.selectionCategory
        selectionName = bridge.selectionName
        rows = bridge.propertySnapshot().map(PropertyRow.from)

        // Phase 4b: per-category state.  Expansion + selection are
        // tracked separately so a header click (empty-name
        // SetSelection) opens the section without picking a row.
        // The controller auto-fills the Material section's
        // selection + expansion when an Object is picked.
        var freshSelections: [Int: String] = [:]
        var freshExpanded:   [Int: Bool]   = [:]
        var freshRows:       [Int: [PropertyRow]] = [:]
        for section in kAccordionSections {
            let cat = section.category
            let isExpanded = bridge.isSectionExpanded(for: cat)
            freshExpanded[cat.rawValue] = isExpanded
            freshSelections[cat.rawValue] = bridge.selectionName(for: cat)
            if isExpanded {
                // Property rows render for every expanded section,
                // including ones with empty selection (Camera /
                // Rasterizer / Film render their active-entity rows
                // as a sensible default; Object / Light / Material
                // stay empty until a pick).
                freshRows[cat.rawValue] = bridge.propertySnapshot(for: cat).map(PropertyRow.from)
            }
        }
        selectionByCategory = freshSelections
        expandedByCategory  = freshExpanded
        rowsByCategory      = freshRows

        // Re-pull per-section entity lists when the scene epoch
        // advances (scene reload, structural mutation).
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
/// entity list.  All sections use the dropdown for visual consistency
/// and to handle worst-case entity counts (Objects can run into the
/// hundreds).  Output Settings is single-entry but uses the same widget
/// chrome to keep the layout uniform.
private struct AccordionSectionView: View {
    let section: AccordionSection
    let entities: [String]
    let isExpanded: Bool
    let selectedName: String
    let onSelectRow: (String) -> Void
    let onToggle: (Bool) -> Void
    /// Optional "+" button action.  Currently only the Cameras section
    /// supplies this; other sections pass `nil` and the button is
    /// hidden.  When the section is collapsed the button is also
    /// hidden (the only "add" action belongs alongside an open
    /// section's content).
    let onAddEntity: (() -> Void)?

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
                HStack(spacing: 4) {
                    Text(section.title)
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(.primary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .contentShape(Rectangle())
                        .onTapGesture { onToggle(!isExpanded) }
                    if isExpanded, let onAdd = onAddEntity {
                        Button {
                            onAdd()
                        } label: {
                            Image(systemName: "plus.circle")
                                .font(.system(size: 12))
                        }
                        .buttonStyle(.borderless)
                        .help("Add — clone the current entity under a new name")
                    }
                }
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
    let category: RISEViewportCategory      // Phase 4b: per-section edit routing
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
                            // Per-category SetProperty so a Material
                            // section row edits the bound material
                            // even when Object is the primary
                            // selection.
                            _ = bridge.setProperty(for: category, name: row.name, value: newValue)
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
