//////////////////////////////////////////////////////////////////////
//
//  PropertiesPanel.swift - Right-side properties panel for the
//    interactive viewport.  Driven entirely by the ChunkDescriptor
//    of the loaded camera (or, in a future phase, the selected
//    object) — the names, descriptions, and editable / read-only
//    flags all come from the parser's canonical descriptor list.
//    The same descriptor that loads the scene from .RISEscene
//    populates this panel.
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

    // `nonisolated` — this is a pure value-constructor that touches no
    // main-actor state, so it's safe to pass as a function reference
    // from any context (e.g. `array.map(PropertyRow.from)`).  Without
    // this, Swift 6 strict concurrency warns when the call site can't
    // prove it's on the main actor.
    nonisolated static func from(_ src: RISEViewportProperty) -> PropertyRow {
        // Use enumerated indices to derive unique ids without
        // assuming preset labels are unique per parameter (they
        // should be, but defensive against descriptor authoring
        // mistakes).
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

struct PropertiesPanel: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int          // increment to force a snapshot reload

    @State private var rows: [PropertyRow] = []
    @State private var header: String = ""
    @State private var mode: RISEViewportPanelMode = .none

    var body: some View {
        VStack(spacing: 0) {
            // Header is empty when mode == .none; we still render the
            // bar so the layout doesn't jump when the user picks an
            // object or activates a camera tool.  The Refresh button
            // is hidden in .none mode (nothing to refresh).
            HStack {
                Text(header.isEmpty ? " " : header)
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                Spacer()
                if mode != .none {
                    Button {
                        reload()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .buttonStyle(.borderless)
                    .help("Refresh properties from the live entity")
                }
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            ScrollView(.vertical) {
                VStack(alignment: .leading, spacing: 6) {
                    switch mode {
                    case .none:
                        // Empty panel — no tool / selection that has
                        // anything to show.
                        Text("Pick an object or select a camera tool to inspect properties.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .padding(8)
                    case .camera, .object:
                        if rows.isEmpty {
                            Text("No properties available.")
                                .font(.caption)
                                .foregroundColor(.secondary)
                                .padding(8)
                        } else {
                            ForEach(rows) { row in
                                PropertyRowView(
                                    row: row,
                                    onCommit: { newValue in
                                        _ = bridge.setPropertyName(row.name, value: newValue)
                                        reload()
                                    },
                                    onScrubBegin: { bridge.beginPropertyScrub() },
                                    onScrubEnd:   { bridge.endPropertyScrub()   }
                                )
                            }
                        }
                    @unknown default:
                        EmptyView()
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
        rows = bridge.propertySnapshot().map(PropertyRow.from)
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
                                // Live-commit on every drag tick so the
                                // viewport renders the change in real
                                // time.  The same `setProperty` path
                                // that keyboard editing uses, so the
                                // viewport sees these mutations identically.
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
                    // Unit suffix — surfaces "mm" / "°" / "scene
                    // units" next to the field so the user can tell
                    // at a glance what unit a number is in (e.g. a
                    // focal_length of "35" reads as "35 mm" rather
                    // than possibly being misread as "35 metres").
                    if !row.unitLabel.isEmpty {
                        Text(row.unitLabel)
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                            .fixedSize()
                    }
                    // Quick-pick presets: rendered as a Menu next to
                    // the line edit when the descriptor declared any.
                    // Picking a preset writes the parser-acceptable
                    // value through the same SetProperty path the
                    // line edit uses, so undo/redo and re-render work
                    // identically for both inputs.  The line edit
                    // stays usable for arbitrary custom values.
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
            // Reflect external changes (e.g. user dragged the camera in
            // the viewport) without overwriting an active edit.
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
///
/// Two rate regimes:
///   • Angular fields (theta / phi / fov / pitch / yaw / roll):
///     fixed 0.5°/px — matches the Orbit tool's sensitivity so the
///     panel scrub and the viewport drag feel the same.
///   • Everything else: proportional to the value magnitude
///     (|v| × 0.005/px) with a floor of 1e-3 so values near zero can
///     still scrub off zero.
///
/// Modifiers (held while dragging):
///   • Shift  — 0.25× rate (fine control)
///   • Option — 4×    rate (coarse control)
///
/// Cursor: vertical-resize while hovering the chevron and during the drag.
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
            // NSCursor swap on hover so users discover that the icon
            // is interactive.  Pop the pushed cursor on exit so the
            // arrow returns when the pointer leaves the icon.
            .onHover { entered in
                if entered { NSCursor.resizeUpDown.push() }
                else       { NSCursor.pop() }
            }
            .help("Drag up/down to change.  Shift = fine, Option = coarse.")
            .gesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .local)
                    .onChanged { gesture in
                        if dragStart == nil {
                            // First .onChanged callback of this drag;
                            // capture the parsed initial value and Y,
                            // then fire the scrub-begin callback so
                            // the controller bumps preview-scale.
                            let v = Double(text) ?? 0
                            dragStart = ScrubStart(
                                value: v,
                                yOrigin: gesture.startLocation.y
                            )
                            // Lock the resize cursor for the duration
                            // of the drag — the .onHover toggle would
                            // otherwise race with the pointer motion.
                            NSCursor.resizeUpDown.push()
                            onScrubBegin()
                        }
                        guard let start = dragStart else { return }

                        // Drag up (smaller Y in flipped coordinate
                        // space) increases the value.
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

/// Angular fields the camera descriptor surfaces — these get a fixed
/// 0.5°/px rate (matching the Orbit tool's sensitivity).  Other
/// numeric fields use the proportional rate so a 50-unit value and a
/// 0.05-unit value both scrub at sensible speeds with the same
/// gesture, but for angles the proportional rate is too slow at
/// typical magnitudes (theta=30 → 0.15°/px → 100px = 15° feels
/// sluggish).  `aperture_rotation` is a thinlens_camera angular field
/// (polygon rotation in degrees) and uses the same convention.
private func isAngularField(_ name: String) -> Bool {
    switch name {
    case "theta", "phi", "fov", "pitch", "yaw", "roll",
         "aperture_rotation", "tilt_x", "tilt_y":
        return true
    default:
        return false
    }
}

/// Per-pixel scrub rate.  Angular fields use a fixed 0.5/px (matches
/// the Orbit tool's 0.0087 rad/px ≈ 0.5°/px).  Non-angular fields
/// use a proportional rate with a 1e-3 magnitude floor so values at
/// zero can still scrub off zero.
private func scrubRate(name: String, value: Double) -> Double {
    if isAngularField(name) { return 0.5 }
    return max(abs(value), 1e-3) * 0.005
}

/// Format a scrubbed value back into the same textual form the C++
/// FormatDouble / FormatUInt helpers produce (`%.6g` for doubles,
/// integer for UInt clamped at zero).  Keeping the formatting in
/// lockstep avoids round-trip drift across many small drags.
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
