//////////////////////////////////////////////////////////////////////
//
//  PropertiesPanel.swift - Right-panel inspector for the interactive
//    viewport.  Shows the SCENE's currently selected entity (whatever
//    RISEViewportBridge's PRIMARY selection is — set either by
//    clicking a row in OutlinerView.swift above this panel, or by
//    picking an object directly in the 3D viewport) with a
//    descriptor-driven property list.
//
//    Pre-redesign this file also owned the nine-section stacked
//    accordion (Cameras / Rasterizer / Objects / ... each listing its
//    own entities).  That navigation role moved to OutlinerView.swift;
//    this file keeps exactly the property-row rendering / editing
//    machinery — kind-specific value cells, scrub-to-change numeric
//    fields, preset quick-picks, focused-field edit protection — and
//    restyles it to the redesign's token set (Theme.swift).
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import CoreGraphics
import AppKit

/// Mirrors RISE::ValueKind in ChunkDescriptor.h.  Selects which value
/// cell renders for a property row.
enum PropertyKind: Int {
    // Raw values MUST match the parser's ValueKind wire enum exactly
    // (src/Library/Parsers/ChunkDescriptor.h) — the bridge sends
    // `ValueKind` cast to int.  A prior version of this enum omitted
    // DoubleVec4/DoubleMat4 and packed the tail as 4..7, silently
    // misrouting every String/Filename/Enum/Reference row (a
    // DoubleMat4 even rendered a filename browse button).  Found by
    // the Windows-port slice mirroring the real wire values.
    case bool       = 0
    case uint       = 1
    case double     = 2
    case doubleVec3 = 3
    case doubleVec4 = 4
    case doubleMat4 = 5
    case string     = 6
    case filename   = 7
    case enumKind   = 8
    case reference  = 9
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
    let id: String                // parameter name -- unique per entity: the synthetic
                                   // leading identity row is "chunk_type" (never a real
                                   // descriptor param name), so it can't collide with a
                                   // chunk that happens to declare a real `type` param
                                   // (e.g. sdf3d_painter, polynomial_function2d_painter)
    let name: String
    let initialValue: String
    let description: String
    let kind: PropertyKind
    let editable: Bool
    let presets: [PropertyPreset] // empty when descriptor declared no presets
    let unitLabel: String         // empty for dimensionless / unlabelled fields
    /// doc 88 S4b (Tier-1 param sliders): the row's authored numeric
    /// range.  `hasRange` is true only when BOTH bounds are known; the
    /// text field stays either way, because the range is a presentation
    /// hint and the scene language still accepts values outside it.
    /// `rangeStep` is 0 for "continuous".
    let hasRange: Bool
    let rangeMin: Double
    let rangeMax: Double
    let rangeStep: Double
    /// Snapshot position -- the C-ABI property index this row was built
    /// from (jump-to-definition queries the core by index).
    let index: Int

    nonisolated static func from(_ src: RISEViewportProperty, index: Int) -> PropertyRow {
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
            unitLabel: src.unitLabel,
            hasRange: src.hasRange,
            rangeMin: src.rangeMin,
            rangeMax: src.rangeMax,
            rangeStep: src.rangeStep,
            index: index
        )
    }
}

// MARK: - Category display metadata (Inspector header)

/// Plural section title, mirrors the wording OutlinerView.swift uses
/// for the same category so the header's meta line reads consistently
/// with the tree above it.
private func categoryTitle(_ cat: RISEViewportCategory) -> String {
    switch cat {
    case .camera:       return "Cameras"
    case .rasterizer:   return "Rasterizer"
    case .object:       return "Objects"
    case .light:        return "Lights"
    case .film:         return "Output Settings"
    case .material:     return "Materials"
    case .medium:       return "Media"
    case .animation:    return "Animation"
    case .sceneVariant: return "Variants"
    case .painter:      return "Painters"
    case .geometry:     return "Geometry"
    case .none:         return "Scene"
    default:            return "Scene"
    }
}

/// Plain-text glyph for the entity-header icon chip.  Design brief A6
/// specifies these for the eight core categories; scene_variant and
/// painter aren't in the brief (both newer categories than the design
/// comp) so they get a reasonable same-family glyph rather than being
/// left blank.
private func categoryGlyph(_ cat: RISEViewportCategory) -> String {
    switch cat {
    case .camera:       return "◉"
    case .object:       return "◆"
    case .light:        return "☀"
    case .material:     return "◐"
    case .rasterizer:   return "⚙"
    case .film:         return "▦"
    case .medium:       return "≈"
    case .animation:    return "▶"
    case .sceneVariant: return "⧉"
    case .painter:      return "▧"
    case .geometry:     return "◇"
    case .none:         return "•"
    default:            return "•"
    }
}

// MARK: - Painter preview thumbnails (doc 88 S10, Tier 2)
//
// Minimal Mac shell for the headless painter-preview engine
// (src/Library/SceneEditor/PainterPreview.h): a small async, cached
// swatch for a painter's own output, and one per expression `def[i]`
// row.  This is deliberately thin -- the engine (domain conventions,
// display encode, scalar auto-range normalization) is fully specified
// and tested on the C++ side (PainterPreviewTest.cpp); this file only
// turns the bridge's RGBA8 bytes into an NSImage and caches it.
//
// P2-2 (S10 review round 1): NO blanket `extension RISEViewportBridge:
// @unchecked Sendable {}` here.  RISEViewportBridge is an Objective-C
// class wrapping a live C++ SceneEditController*, and it exposes plenty
// of MUTABLE, main-actor-only state elsewhere in the app (e.g.
// currentTool/primaryPane/agentAutonomyLevel, read/written from
// RenderViewModel.swift) -- a whole-type `@unchecked Sendable` would
// disarm the concurrency checker for every one of those module-wide,
// not just the two read-only preview calls this file actually sends
// across an actor boundary.  `PainterPreviewFetcher` below is the
// narrowest sound shape instead: a tiny wrapper that holds ONLY a weak
// bridge reference and exposes ONLY
// painterPreview(for:defIndex:width:height:wasScalar:rangeMin:rangeMax:)
// and rampStripPreview(for:width:height:) -- the two calls the
// background-queue fetch closures below actually need.  Both are sound
// to call off the main actor: SceneEditController::GetPainterPreview /
// GetRampStripPreview take the controller's own try_lock +
// mRenderOwnsScene guard and REFUSE (return nil) rather than block or
// race on contention (PainterPreview.h's CONCURRENCY note) -- neither
// call touches a bridge ivar, only a straight pass-through to the
// C-ABI shim (RISE_API_SceneEditController_PainterPreview /
// RISE_API_SceneEditController_RampStripPreview).
final class PainterPreviewFetcher: @unchecked Sendable {
    private weak var bridge: RISEViewportBridge?

    init(bridge: RISEViewportBridge) {
        self.bridge = bridge
    }

    func painterPreview(for painter: String, defIndex: Int, width: UInt, height: UInt,
                         wasScalar: inout ObjCBool, rangeMin: inout Double, rangeMax: inout Double) -> Data? {
        bridge?.painterPreview(for: painter, defIndex: defIndex, width: width, height: height,
                                wasScalar: &wasScalar, rangeMin: &rangeMin, rangeMax: &rangeMax)
    }

    func rampStripPreview(for painter: String, width: UInt, height: UInt) -> Data? {
        bridge?.rampStripPreview(for: painter, width: width, height: height)
    }
}

/// Cache key includes the retained CST Document's HEAD REVISION
/// (`RISEViewportBridge.getSceneTextVersionUuid(_:revision:)`, the
/// same "uuid fresh per load, revision bumps iff content changed"
/// signal `serializedSceneText`'s neighbours already use) -- so ANY
/// scene edit invalidates every cached thumbnail.  Coarser than a
/// per-painter dirty signal (which doesn't exist yet), but never
/// serves a stale swatch: a revision bump is the bridge's own
/// definition of "the document changed."
@MainActor
final class PainterThumbnailCache {
    static let shared = PainterThumbnailCache()
    private var images: [String: NSImage] = [:]
    private var inFlight: Set<String> = []

    private func key(_ painter: String, _ defIndex: Int, _ w: Int, _ h: Int, _ revision: UInt64) -> String {
        "\(painter)|\(defIndex)|\(w)x\(h)|\(revision)"
    }

    /// Returns a cached image synchronously on a hit.  On a miss, kicks
    /// off an async fetch (off the main thread -- the preview evaluates
    /// the painter over a w*h grid, cheap but not free at 60fps-body-
    /// evaluation cadence) and calls `onReady` back on the main actor
    /// once it lands; returns nil for this call (the view redraws when
    /// `onReady` sets its @State).  A fetch already in flight for the
    /// same key is not duplicated.
    func image(fetcher: PainterPreviewFetcher, painter: String, defIndex: Int,
               width: Int, height: Int, revision: UInt64,
               onReady: @escaping (NSImage) -> Void) -> NSImage? {
        let k = key(painter, defIndex, width, height, revision)
        if let cached = images[k] { return cached }
        if inFlight.contains(k) { return nil }
        inFlight.insert(k)
        DispatchQueue.global(qos: .userInitiated).async {
            var wasScalar: ObjCBool = false
            var rangeMin: Double = 0
            var rangeMax: Double = 0
            let data = fetcher.painterPreview(
                for: painter, defIndex: defIndex,
                width: UInt(width), height: UInt(height),
                wasScalar: &wasScalar, rangeMin: &rangeMin, rangeMax: &rangeMax)
            let image = data.flatMap { Self.image(fromRGBA8: $0, width: width, height: height) }
            DispatchQueue.main.async {
                PainterThumbnailCache.shared.inFlight.remove(k)
                guard let image else { return }
                PainterThumbnailCache.shared.images[k] = image
                onReady(image)
            }
        }
        return nil
    }

    /// Same fetch/cache contract as `image(fetcher:painter:defIndex:...)`
    /// above, for the ramp gradient strip (Deliverable 2).  Uses the
    /// SAME cache dictionary under the `defIndex == -2` sentinel key so
    /// a ramp's own -1 painter preview (if ever requested) and its strip
    /// never collide.
    func rampStrip(fetcher: PainterPreviewFetcher, painter: String,
                    width: Int, height: Int, revision: UInt64,
                    onReady: @escaping (NSImage) -> Void) -> NSImage? {
        let k = key(painter, -2, width, height, revision)
        if let cached = images[k] { return cached }
        if inFlight.contains(k) { return nil }
        inFlight.insert(k)
        DispatchQueue.global(qos: .userInitiated).async {
            let data = fetcher.rampStripPreview(for: painter, width: UInt(width), height: UInt(height))
            let image = data.flatMap { Self.image(fromRGBA8: $0, width: width, height: height) }
            DispatchQueue.main.async {
                PainterThumbnailCache.shared.inFlight.remove(k)
                guard let image else { return }
                PainterThumbnailCache.shared.images[k] = image
                onReady(image)
            }
        }
        return nil
    }

    /// Same recipe RenderViewModel.handleOutput uses for the viewport's
    /// own RGBA8->NSImage conversion (premultipliedLast, no decode
    /// array, no interpolation at build time).  `nonisolated`: a pure
    /// function of its arguments (no access to the cache's own state),
    /// called from the background-queue fetch closures above -- which
    /// run off the main actor by design (MATERIAL_EDITOR.md §3.3), so
    /// this must not inherit the class's @MainActor isolation.
    private nonisolated static func image(fromRGBA8 data: Data, width: Int, height: Int) -> NSImage? {
        guard width > 0, height > 0, data.count == width * height * 4 else { return nil }
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
        guard let provider = CGDataProvider(data: data as CFData),
              let cgImage = CGImage(
                  width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                  bytesPerRow: width * 4, space: colorSpace, bitmapInfo: bitmapInfo,
                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent
              ) else { return nil }
        return NSImage(cgImage: cgImage, size: NSSize(width: width, height: height))
    }
}

/// A small preview swatch for a painter (`defIndex == -1`) or one of its
/// expression `def` stages (`defIndex >= 0`).  Renders a neutral filled
/// tile until the async fetch lands, never blocking the row it sits in.
private struct PainterThumbnailView: View {
    let bridge: RISEViewportBridge
    let painterName: String
    let defIndex: Int
    var size: CGFloat = 48

    @State private var image: NSImage? = nil

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 4).fill(Theme.textDim.opacity(0.08))
            if let image {
                Image(nsImage: image)
                    .resizable()
                    .interpolation(.none)
                    .aspectRatio(contentMode: .fill)
                    .clipShape(RoundedRectangle(cornerRadius: 4))
            }
        }
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.borderHairline, lineWidth: 1))
        .frame(width: size, height: size)
        .onAppear { refresh() }
        .onChange(of: painterName) { _, _ in image = nil; refresh() }
        .onChange(of: defIndex) { _, _ in image = nil; refresh() }
    }

    private func refresh() {
        var uuid: UInt64 = 0
        var revision: UInt64 = 0
        _ = bridge.getSceneTextVersionUuid(&uuid, revision: &revision)
        // Oversample a little past the on-screen point size for a crisp
        // swatch on Retina displays; the engine's kMaxDim cap (2048) is
        // far above anything this UI ever requests.
        let px = max(8, Int((size * 2).rounded(.up)))
        let fetcher = PainterPreviewFetcher(bridge: bridge)
        if let cached = PainterThumbnailCache.shared.image(
            fetcher: fetcher, painter: painterName, defIndex: defIndex,
            width: px, height: px, revision: revision,
            onReady: { img in self.image = img }
        ) {
            image = cached
        }
    }
}

/// Deliverable 2: a ramp_painter's gradient strip -- its own colour
/// interpolation over its authored stop domain, via
/// `RISEViewportBridge.rampStripPreview(for:width:height:)`.  Reuses
/// PainterThumbnailCache under a distinct `defIndex == -2` key (a ramp
/// strip is a different evaluation than the painter's own -1 preview:
/// RampPainter::EvalAt sweeps `t` directly, bypassing `input` -- see
/// PainterPreview::RenderRampStripPreview's doc comment), so the two
/// never collide or overwrite each other in the cache.
private struct RampStripThumbnailView: View {
    let bridge: RISEViewportBridge
    let painterName: String
    var width: CGFloat = 220
    var height: CGFloat = 20

    @State private var image: NSImage? = nil

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 4).fill(Theme.textDim.opacity(0.08))
            if let image {
                Image(nsImage: image)
                    .resizable()
                    .interpolation(.none)
                    .clipShape(RoundedRectangle(cornerRadius: 4))
            }
        }
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.borderHairline, lineWidth: 1))
        .frame(width: width, height: height)
        .onAppear { refresh() }
        .onChange(of: painterName) { _, _ in image = nil; refresh() }
    }

    private func refresh() {
        var uuid: UInt64 = 0
        var revision: UInt64 = 0
        _ = bridge.getSceneTextVersionUuid(&uuid, revision: &revision)
        let pxW = max(8, Int((width * 2).rounded(.up)))
        let pxH = max(1, Int((height * 2).rounded(.up)))
        let fetcher = PainterPreviewFetcher(bridge: bridge)
        if let cached = PainterThumbnailCache.shared.rampStrip(
            fetcher: fetcher, painter: painterName, width: pxW, height: pxH, revision: revision,
            onReady: { img in self.image = img }
        ) {
            image = cached
        }
    }
}

/// True iff `rowName` has PainterIntrospection's synthetic occurrence-row
/// shape for the `def` role specifically ("def[2]") -- mirrors
/// PainterIntrospection::ParseOccurrenceRowName's contract on the Swift
/// side (name-string parsing only; this file has no CST access to
/// confirm the role is actually `def` on THIS chunk kind, which is fine:
/// a false positive here only means an extra thumbnail fetch that
/// resolves to a def-index-out-of-range refusal and shows the neutral
/// placeholder tile, never a wrong value).
private func defStageIndex(rowName: String) -> Int? {
    guard rowName.hasPrefix("def["), rowName.hasSuffix("]") else { return nil }
    let inner = rowName.dropFirst(4).dropLast(1)
    guard !inner.isEmpty else { return nil }
    return Int(inner)
}

struct PropertiesPanel: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int          // increment to force a snapshot reload

    // Phase 6.5: the panel's Save / Save-As buttons read these two
    // properties off the shared RenderViewModel — `sceneEditsDirty`
    // (gates the buttons' enable state) and `loadedFilePath` (the
    // default target for in-place Save).  Both are @Published, so
    // SwiftUI re-evaluates the header HStack on every transition.
    @EnvironmentObject var viewModel: RenderViewModel

    @State private var rows: [PropertyRow] = []
    @State private var selectionCategory: RISEViewportCategory = .none
    @State private var selectionName: String = ""
    @State private var showAdvanced: Bool = false
    @State private var lastEntityKey: String = ""
    // "Reveal in scene file" (design comp ⌗ affordance): the selected
    // entity's 1-based line in the scene text, or nil when unavailable
    // (no CST document, unresolvable/ambiguous name, or a category with
    // no chunk-name addressing scheme).  Refetched only on an entity-
    // IDENTITY change (see `reload`'s `lastEntityKey` gate) — NOT on
    // every reload — so it costs one bridge call per selection, not one
    // per frame / per property edit.
    @State private var sourceLine: UInt32? = nil
    // Tracks whether we've already shown the "new cameras only live in
    // memory" caveat in this session, so we surface it exactly once.
    @State private var addCameraCaveatShown: Bool = false
    // Re-entry guard against a second "Add Camera" prompt firing while
    // the first NSAlert is still on screen.
    @State private var addCameraInFlight: Bool = false

    private static let maxBasicRows = 8

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 13) {
                    entityHeader
                    propertyBody
                }
                .padding(14)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)

            Rectangle().fill(Theme.borderHairline).frame(height: 1)
            footer
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .onAppear { reload() }
        .onChange(of: refreshTrigger) { _, _ in reload() }
    }

    // MARK: - Entity header

    private var entityHeader: some View {
        HStack(spacing: 9) {
            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(Theme.accent.opacity(0.15))
                    .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.accent.opacity(0.3), lineWidth: 1))
                Text(categoryGlyph(selectionCategory))
                    .font(.system(size: 12))
                    .foregroundColor(Theme.accentLight)
            }
            .frame(width: 26, height: 26)

            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text(headerTitle)
                        .font(Theme.sans(12.5, .semibold))
                        .foregroundColor(Theme.textPrimary)
                        .lineLimit(1)
                    if let line = sourceLine {
                        SourceLineChip(line: line) {
                            viewModel.revealEntityInSceneText(category: selectionCategory, name: selectionName)
                        }
                    }
                }
                Text(headerMeta)
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.textDim)
                    .lineLimit(1)
            }
            Spacer(minLength: 4)
        }
    }

    private var headerTitle: String {
        if !selectionName.isEmpty { return selectionName }
        if selectionCategory != .none { return categoryTitle(selectionCategory) }
        return "Scene"
    }

    private var headerMeta: String {
        if selectionCategory == .none { return "Nothing selected" }
        if !selectionName.isEmpty { return categoryTitle(selectionCategory) }
        return "No entity selected"
    }

    // MARK: - Property body

    /// doc 88 S10: a painter's own preview swatch, at the top of its row
    /// group -- the C1 "material swatch at panel top" idiom
    /// (MATERIAL_EDITOR.md §2.1), reused for painter nodes.
    @ViewBuilder
    private var painterThumbnailHeader: some View {
        if selectionCategory == .painter && !selectionName.isEmpty {
            HStack(spacing: 10) {
                PainterThumbnailView(bridge: bridge, painterName: selectionName, defIndex: -1, size: 56)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Preview").font(Theme.sans(10, .medium)).foregroundColor(Theme.textDim)
                    Text("Evaluated over a synthetic UV/world patch, not a scene render.")
                        .font(Theme.sans(9))
                        .foregroundColor(Theme.textDim)
                        .lineLimit(2)
                }
                Spacer(minLength: 4)
            }
            // Deliverable 2: the ramp's own gradient, above its `stop[i]`
            // rows -- detected off the generic "type" row's chunk keyword
            // rather than a bespoke category, since a ramp_painter is
            // still Category::Painter like every other painter kind.
            if isRampPainter {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Gradient").font(Theme.sans(10, .medium)).foregroundColor(Theme.textDim)
                    RampStripThumbnailView(bridge: bridge, painterName: selectionName)
                }
            }
        }
    }

    private var isRampPainter: Bool {
        rows.first(where: { $0.name == "type" })?.initialValue == "ramp_painter"
    }

    @ViewBuilder
    private var propertyBody: some View {
        VStack(alignment: .leading, spacing: 10) {
            if selectionCategory == .camera {
                cameraAffordances
            }
            painterThumbnailHeader
            if rows.isEmpty {
                Text(emptyRowsMessage)
                    .font(Theme.sans(11))
                    .foregroundColor(Theme.textDim)
            } else {
                let split = Self.splitBasicAdvanced(rows)
                VStack(alignment: .leading, spacing: 9) {
                    ForEach(split.basic) { row in makeRow(row) }
                }
                if !split.advanced.isEmpty {
                    advancedDisclosure(split.advanced)
                }
            }
        }
    }

    private var emptyRowsMessage: String {
        switch selectionCategory {
        case .animation:    return "Selecting activates this animation path."
        case .sceneVariant: return "Selecting activates this scene variant — the scene re-derives with it active."
        case .none:         return "Select an item in the outliner."
        default:            return "Select an entity in the outliner to see its properties."
        }
    }

    private func makeRow(_ row: PropertyRow) -> some View {
        HStack(alignment: .top, spacing: 8) {
            // doc 88 S10: a def[i] row gets a small stage thumbnail --
            // "most of a node graph's explanatory value with none of the
            // canvas machinery" (doc 88 §P5 Tier 2).  Only meaningful on
            // an expression painter's `def` occurrences; a false-positive
            // name match on another chunk kind just fetches a refusal and
            // shows the neutral placeholder (see defStageIndex's doc
            // comment), never a wrong value.
            if selectionCategory == .painter, let stageIdx = defStageIndex(rowName: row.name) {
                PainterThumbnailView(bridge: bridge, painterName: selectionName, defIndex: stageIdx, size: 28)
            }
            propertyRowCell(row)
        }
    }

    private func propertyRowCell(_ row: PropertyRow) -> some View {
        PropertyRowView(
            row: row,
            onCommit: { newValue in
                let accepted = bridge.setProperty(
                    for: selectionCategory, name: row.name, value: newValue)
                reload()
                return accepted
            },
            onScrubBegin: { bridge.beginPropertyScrub() },
            onScrubEnd:   { bridge.endPropertyScrub()   }
        )
        // Source traceability: reveal THIS param's exact span in the Scene-file
        // editor.  Shown only when the entity resolves to a scene-file chunk
        // (sourceLine != nil) — the same gate as the whole-entity ⌗ chip.
        .contextMenu {
            if sourceLine != nil {
                // The synthetic leading identity row ("chunk_type") has no
                // param span of its own -- it's the chunk's keyword, not a
                // descriptor parameter -- so reveal the WHOLE chunk (empty
                // param, per ResolveSourceSpan's documented convention)
                // rather than name-matching "chunk_type" against a real
                // descriptor param.  Without this, right-clicking the
                // identity row on a chunk that also declares a genuine
                // `type` parameter (sdf3d_painter, polynomial_function2d_painter)
                // used to jump to that param's line instead of the chunk.
                let isIdentityRow = row.id == "chunk_type"
                let revealParam = isIdentityRow ? "" : row.name
                Button {
                    viewModel.revealSourceSpan(category: selectionCategory, name: selectionName, param: revealParam)
                } label: {
                    // Label matches the action: the identity row reveals the
                    // whole chunk, not a param named "chunk_type".
                    Label(isIdentityRow ? "Reveal chunk in Scene File"
                                        : "Reveal “\(row.name)” in Scene File",
                          systemImage: "text.magnifyingglass")
                }
            }
            // Jump-to-definition (GUI redesign 2026-07-22): a Reference
            // row whose value names a live element gets a direct jump to
            // that element's own panel.  Resolution happens at menu-build
            // time (right-click), so the item reflects the CURRENT scene
            // -- a dangling reference simply shows no item.
            if row.kind == .reference {
                var jumpCat: RISEViewportCategory = .none
                var jumpName: NSString? = nil
                if bridge.propertyJumpTarget(atIndex: UInt(row.index),
                                             outCategory: &jumpCat, outName: &jumpName),
                   let name = jumpName as String? {
                    Button {
                        viewModel.jumpToEntity(category: jumpCat, name: name)
                    } label: {
                        Label("Jump to Definition of “\(name)”", systemImage: "arrow.uturn.right")
                    }
                }
            }
        }
    }

    private func advancedDisclosure(_ advanced: [PropertyRow]) -> some View {
        AdvancedDisclosure(count: advanced.count, isOpen: $showAdvanced) {
            VStack(alignment: .leading, spacing: 9) {
                ForEach(advanced) { row in makeRow(row) }
            }
        }
    }

    /// Progressive disclosure (design brief A6, two levels max): the
    /// first ~8 rows show inline as "Basic"; the rest collapse under
    /// one "Advanced" toggle.  Priority for the Basic 8: editable rows
    /// with quick-pick presets first (the ones most worth a click),
    /// then whichever other rows come next in descriptor order, until
    /// the cap is reached.  Both groups are then rendered back in
    /// their ORIGINAL descriptor order — the priority pass only
    /// decides membership, not display order, so a Basic row doesn't
    /// visually jump ahead of a field that logically precedes it.
    private static func splitBasicAdvanced(_ rows: [PropertyRow]) -> (basic: [PropertyRow], advanced: [PropertyRow]) {
        guard rows.count > maxBasicRows else { return (rows, []) }

        let withPresets = rows.filter { $0.editable && !$0.presets.isEmpty }
        var selectedIDs = Set(withPresets.prefix(maxBasicRows).map { $0.id })
        if selectedIDs.count < maxBasicRows {
            for r in rows where !selectedIDs.contains(r.id) {
                selectedIDs.insert(r.id)
                if selectedIDs.count >= maxBasicRows { break }
            }
        }
        let basic = rows.filter { selectedIDs.contains($0.id) }
        let advanced = rows.filter { !selectedIDs.contains($0.id) }
        return (basic, advanced)
    }

    // MARK: - Camera affordances

    private var cameraAffordances: some View {
        VStack(alignment: .leading, spacing: 6) {
            Button {
                _ = bridge.setSelection(.camera, name: selectionName)
                reload()
            } label: {
                Text("Use in viewport")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(OutlineAccentButtonStyle())
            .disabled(selectionName.isEmpty)

            Button {
                promptForNewCameraName(activeName: bridge.activeName(for: .camera))
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: "plus.circle")
                        .font(.system(size: 10))
                    Text("Add Camera")
                        .font(Theme.mono(10.5))
                }
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.textDim)
            .disabled(addCameraInFlight)
        }
    }

    /// Prompt the user for a new camera name, then call the bridge's
    /// `addCameraFromActive` to clone the current camera under that
    /// name.  Default proposal is "<active>_copy".  On first
    /// successful add per session, also surfaces a caveat alert that
    /// the new camera lives in memory only.
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

    // MARK: - Footer (Save / Save As / Refresh)

    private var footer: some View {
        HStack(spacing: 8) {
            Button("Save") {
                performSceneSave(useLoadedPath: true)
            }
            .buttonStyle(PillButtonStyle(filled: true))
            .disabled(!viewModel.sceneEditsDirty || viewModel.loadedFilePath == nil)
            .help(saveButtonHelpText(forSaveAs: false))

            Button("Save As…") {
                performSceneSave(useLoadedPath: false)
            }
            .buttonStyle(PillButtonStyle(filled: false))
            .disabled(!viewModel.sceneEditsDirty)
            .help(saveButtonHelpText(forSaveAs: true))

            Spacer(minLength: 4)

            Button {
                reload()
            } label: {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 11))
                    .foregroundColor(Theme.textDim)
            }
            .buttonStyle(.plain)
            .help("Refresh from the live scene")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
    }

    // ----------------------------------------------------------------
    // Phase 6.5: scene-file save action.  Both header buttons route
    // here.  `useLoadedPath == true` writes to the originally-loaded
    // .RISEscene; `false` opens an NSSavePanel so the user can fork.
    // Status codes mirror SaveResult::Status:
    //   0 = Saved   — silent success (button greys out via the dirty-
    //                 changed callback's clean→dirty=false transition)
    //   1 = NoOp    — silent success.  A Save-As still re-anchors to
    //                 the chosen byte-identical target.
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
            // Pull the just-written bytes back into the scene-editor
            // pane so it reflects the round-tripped edits (camera
            // moves, property changes, created chunks).  When the user
            // ALSO has unsaved text-editor edits, refreshing would
            // silently discard them — but leaving them unsurfaced has
            // its own hazard (clicking the editor's own Save would
            // overwrite our just-written bytes).  Surface the conflict
            // with a one-shot alert so the user can decide; adversarial
            // -review round 1 P1.
            if !viewModel.isEditorDirty {
                viewModel.refreshEditorContents()
            } else {
                let alert = NSAlert()
                alert.messageText = "Scene editor has unsaved text changes"
                alert.informativeText =
                    "Your interactive edits were saved to \(path).  " +
                    "The scene editor pane still shows the pre-save text " +
                    "plus your unsaved edits.  Clicking Save in the scene " +
                    "editor will overwrite the just-saved interactive " +
                    "changes — use Revert in the scene editor to discard " +
                    "your text edits and pull the new file content."
                alert.alertStyle = .warning
                alert.addButton(withTitle: "OK")
                alert.runModal()
            }
        case 1:
            // NoOp is still a successful Save-As: the chosen file already
            // contains byte-identical CST, and the core re-anchored its file
            // identity to this path.  Keep the shell's next in-place Save
            // aligned with that same target.
            if path != viewModel.loadedFilePath {
                viewModel.loadedFilePath = path
            }
            if !viewModel.isEditorDirty {
                viewModel.refreshEditorContents()
            } else {
                // Same conflict as the Saved arm: Save-As changed which
                // file the editor session targets, but refreshing would
                // discard the scene editor's independent text edits.
                let alert = NSAlert()
                alert.messageText = "Scene editor has unsaved text changes"
                alert.informativeText =
                    "The scene is now anchored to \(path), which already " +
                    "contained the current interactive scene.  The scene " +
                    "editor pane still shows its own unsaved text changes.  " +
                    "Clicking Save in the scene editor will overwrite that " +
                    "file — use Revert in the scene editor to discard your " +
                    "text edits and pull the selected file content."
                alert.alertStyle = .warning
                alert.addButton(withTitle: "OK")
                alert.runModal()
            }
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

    // MARK: - Reload

    private func reload() {
        bridge.refreshProperties()
        selectionCategory = bridge.selectionCategory
        selectionName = bridge.selectionName
        // Read the per-category snapshot just like the Windows inspector.
        // Animation and Scene Variant intentionally have no legacy
        // primary PanelMode, but their accordion sections can still expose
        // descriptor-backed rows.
        rows = bridge.propertySnapshot(for: selectionCategory)
            .enumerated().map { PropertyRow.from($1, index: $0) }

        // Reset the Advanced disclosure whenever the selected entity's
        // identity changes — otherwise it could stay open showing a
        // stale row set as the user clicks between entities.
        let key = "\(selectionCategory.rawValue)|\(selectionName)"
        let keyChanged = key != lastEntityKey
        if keyChanged {
            lastEntityKey = key
            showAdvanced = false
        }

        // "Reveal in scene file": fetch on selection change, and ALSO
        // re-arm when a selection made mid-render becomes fetchable once
        // the render finishes (review P2: the gate below failed then, and
        // nothing re-triggered for the SAME still-selected entity).  The
        // `sourceLine == nil` re-arm never fires per-frame on entities
        // whose line IS known.  Gated on `isSceneEditableForAgents`:
        // calling the bridge while a render owns the scene would wedge on
        // the controller's commit mutex (same caveat as every other
        // scene-text bridge call).
        if keyChanged || (sourceLine == nil && viewModel.isSceneEditableForAgents) {
            if viewModel.isSceneEditableForAgents, !selectionName.isEmpty {
                var offset: UInt64 = 0
                var line: UInt32 = 0
                sourceLine = bridge.getEntitySourceLocation(
                    for: selectionCategory, name: selectionName, byteOffset: &offset, line: &line) ? line : nil
            } else {
                sourceLine = nil
            }
        }
    }
}

// MARK: - Source-line chip ("reveal in scene file")

/// The design comp's "⌗ L<line>" affordance: a small bordered pill next
/// to the entity name that scrolls the Scene-file tab to this entity's
/// chunk.  Hidden entirely by the caller when no location is available
/// (see `PropertiesPanel.sourceLine`), so this view is only ever shown
/// with a resolved line number.
private struct SourceLineChip: View {
    let line: UInt32
    let action: () -> Void
    @State private var isHovered = false

    var body: some View {
        Button(action: action) {
            Text("⌗ L\(line)")
                .font(Theme.mono(10))
                .foregroundColor(isHovered ? Theme.textSecondary : Theme.textDim)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
        }
        .buttonStyle(.plain)
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusSmall)
                .stroke(isHovered ? Theme.borderStrong : Theme.borderHairline, lineWidth: 1)
        )
        .onHover { isHovered = $0 }
        .help("Reveal in scene file")
    }
}

// MARK: - Advanced disclosure

private struct AdvancedDisclosure<Content: View>: View {
    let count: Int
    @Binding var isOpen: Bool
    @ViewBuilder let content: () -> Content

    @State private var hovering = false

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack(spacing: 6) {
                Text("Advanced")
                    .font(Theme.sans(11.5))
                Text(isOpen ? "▾" : "▸")
                    .font(.system(size: 9))
                Spacer(minLength: 4)
                Text("\(count) more")
                    .font(Theme.mono(9.5))
                    .foregroundColor(Theme.textDisabled)
            }
            .foregroundColor(hovering ? Theme.textSecondary : Theme.textFaint)
            .contentShape(Rectangle())
            .onTapGesture { isOpen.toggle() }
            .onHover { hovering = $0 }

            if isOpen {
                content()
            }
        }
    }
}

// MARK: - Property row

/// One label + value-cell row.  The value cell's presentation is
/// picked by `row.kind`; all kinds share the same 82pt label column.
private struct PropertyRowView: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool
    let onScrubBegin: () -> Void
    let onScrubEnd:   () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Text(row.name)
                .font(Theme.sans(11))
                .foregroundColor(Theme.textFaint)
                .frame(width: 82, alignment: .leading)
                .padding(.top, 5)

            VStack(alignment: .leading, spacing: 4) {
                valueCell
                if !row.description.isEmpty {
                    Text(row.description)
                        .font(Theme.mono(9.5))
                        .foregroundColor(Theme.textGhost)
                        .lineLimit(2)
                }
            }
        }
    }

    // doc 88 S4b (Tier-1 param sliders): a row that carries an authored
    // numeric range gets a Slider ABOVE its ordinary cell — never INSTEAD
    // of it.  The range is a presentation hint, not a validation rule: the
    // scene language accepts values outside it, an expression `param` line
    // carries metadata the slider does not touch, and the text field is the
    // only way to reach either.  So: slider for the common gesture, field
    // for everything the slider cannot say.
    @ViewBuilder
    private var valueCell: some View {
        if row.hasRange && row.editable {
            VStack(alignment: .leading, spacing: 4) {
                ParamSliderCell(row: row, onCommit: onCommit)
                defaultValueCell
            }
        } else {
            defaultValueCell
        }
    }

    @ViewBuilder
    private var defaultValueCell: some View {
        switch row.kind {
        case .bool:
            BoolPillCell(row: row, onCommit: onCommit)
        case .doubleVec3:
            Vec3Cell(row: row, onCommit: onCommit)
        case .filename:
            FilenameCell(row: row, onCommit: onCommit)
        case .reference:
            ReferenceChipCell(row: row, onCommit: onCommit)
        case .enumKind:
            EnumChipCell(row: row, onCommit: onCommit)
        case .double, .uint, .string, .doubleVec4, .doubleMat4:
            // Vec4/Mat4 get the plain well: no bespoke multi-field
            // editor yet, and free-text is the honest fallback.
            TextWellCell(row: row, onCommit: onCommit, onScrubBegin: onScrubBegin, onScrubEnd: onScrubEnd)
        }
    }
}

// MARK: - Authored-range slider (doc 88 S4b, Tier 1)

/// A slider over a row's authored `min`/`max`(/`step`) range.
///
/// The row's VALUE is not necessarily a bare number: an expression
/// painter's `param[i]` row reads `ring_scale 4.0 min 0.5 max 20 label
/// "Ring density"`, and a commit must send the WHOLE line back (that is
/// the line the CST write replaces).  So the slider rewrites exactly the
/// first token that parses as a number and leaves every other byte of the
/// line alone — the param's own name, and all of its metadata, survive a
/// scrub untouched.  A bare-number row (`4.0`) is the same rule with the
/// first token being the only token.
///
/// Commits on RELEASE, not per tick: each commit is a real CST edit +
/// re-derive + undo entry, and a continuous drag would push one per frame.
private struct ParamSliderCell: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool

    @State private var value: Double = 0
    @State private var dragging: Bool = false

    var body: some View {
        // (S5) `row` is assembled on the GUI bridge from FOUR separate lock
        // acquisitions (doc 88 S4b's PropertyRow snapshot construction), not
        // one atomic read -- a live scene mutation can swap the underlying
        // snapshot in between two of them, tearing `rangeMin` from one
        // snapshot and `rangeMax` from a different (older or newer) one.
        // When that tear happens to invert the pair, `row.rangeMin...
        // row.rangeMax` is a Swift Range precondition failure (a hard trap,
        // not a thrown error) the instant SwiftUI builds it.  Compute the
        // validity ONCE, here, into a local -- never inline the comparison
        // into the ternary/`in:` argument, where a second torn re-read could
        // disagree with the first -- and skip the Slider entirely on an
        // invalid range rather than let it construct one.  A dropped frame's
        // worth of missing slider (the value text still renders) is a fine
        // outcome for a one-in-a-million lock race; a crash is not.
        let hasValidRange = row.rangeMax > row.rangeMin
        return HStack(spacing: 6) {
            if hasValidRange {
                Slider(
                    value: $value,
                    in: row.rangeMin...row.rangeMax,
                    step: row.rangeStep > 0 ? row.rangeStep : 0.0001,
                    onEditingChanged: { editing in
                        dragging = editing
                        if !editing { commit() }
                    }
                )
                .controlSize(.small)
            }
            Text(formatValue(value, kind: .double))
                .font(Theme.mono(10))
                .foregroundColor(Theme.textDim)
                .frame(width: 46, alignment: .trailing)
        }
        .onAppear { value = Self.numericToken(row.initialValue)?.1 ?? row.rangeMin }
        .onChange(of: row.initialValue) { _, newValue in
            // Don't fight the user's own drag; re-sync from the snapshot
            // once the gesture is over (an Undo, or an edit through the
            // text field, moves the row underneath us).
            if !dragging { value = Self.numericToken(newValue)?.1 ?? row.rangeMin }
        }
    }

    private func commit() {
        guard let (range, _) = Self.numericToken(row.initialValue) else { return }
        var line = row.initialValue
        line.replaceSubrange(range, with: formatValue(value, kind: .double))
        _ = onCommit(line)
    }

    /// The first whitespace-delimited token, SKIPPING token 0, that looks and
    /// parses as a Double, as (its range in `line`, its value).  nil when the
    /// line has no such token — the slider then has nothing to drive and the
    /// caller falls back to the range minimum / skips the commit.
    ///
    /// Two deliberate departures from "just try `Double(token)` on every token":
    ///
    /// 1. Token 0 is SKIPPED.  `hasRange` (the sole gate that puts a row through
    ///    ParamSliderCell rather than TextWellCell) is set only for an
    ///    expression painter's `param[i]` rows.  The CST line is
    ///    `param <paramName> <value> [min ... max ... step ... label ...]`,
    ///    and `initialValue` here is everything AFTER the `param` role token
    ///    -- so its OWN token 0 is `<paramName>` (the expression parameter's
    ///    own identifier, e.g. `wob` or `ring_scale`), never a value the
    ///    slider should scrub.  A parameter literally named `4` (or, see
    ///    point 2, `inf`/`nan`) would otherwise get its name token silently
    ///    overwritten by the slider.
    ///
    /// 2. A token must LOOK numeric (optional sign, then a digit or `.`) before
    ///    `Double(_:)` is even tried.  `Double("inf")`, `Double("nan")`, and
    ///    `Double("0x1p3")` (C99 hex float) all successfully parse in Swift, so
    ///    without this guard a param named exactly `inf`/`nan`/`infinity` (rare,
    ///    but not disallowed by the scene grammar) would still be misread as a
    ///    numeric value by the naive scan — the token-0 skip above only rules
    ///    out the FIRST token, not a same-shaped later one on a multi-token
    ///    value line.
    private static func numericToken(_ line: String) -> (Range<String.Index>, Double)? {
        func looksNumeric(_ s: Substring) -> Bool {
            var s = s
            if let f = s.first, f == "+" || f == "-" { s = s.dropFirst() }
            guard let f = s.first else { return false }
            return f.isNumber || f == "."
        }
        var i = line.startIndex
        var tokenIndex = 0
        while i < line.endIndex {
            while i < line.endIndex, line[i].isWhitespace { i = line.index(after: i) }
            guard i < line.endIndex else { return nil }
            var j = i
            while j < line.endIndex, !line[j].isWhitespace { j = line.index(after: j) }
            let tok = line[i..<j]
            if tokenIndex > 0, looksNumeric(tok) {
                // A token that LOOKS numeric (leading sign/digit/`.`) but does not
                // actually parse (`4.0.1`, a lone `-`, a truncated `1e`) means this
                // line is not shaped the way this scanner assumes -- bail out with
                // nil rather than falling through to scan later tokens.  Falling
                // through would let the scan land on the NEXT numeric-looking
                // token, which for a `param` line is `min`'s own bound
                // (`param ring_scale <bad> min 0.5 max 20 ...`) -- so a malformed
                // value token would silently make the slider scrub and commit
                // OVER the min bound instead of refusing to drive at all.
                guard let d = Double(tok) else { return nil }
                return (i..<j, d)
            }
            tokenIndex += 1
            i = j
        }
        return nil
    }
}

// MARK: - Shared well chrome

private func wellBackground() -> some View {
    RoundedRectangle(cornerRadius: Theme.radiusSmall)
        .fill(Theme.bgWell)
        .overlay(RoundedRectangle(cornerRadius: Theme.radiusSmall).stroke(Theme.borderLight, lineWidth: 1))
}

/// Shared quick-pick presets menu (list-bullet icon button).
private struct PresetMenu: View {
    let row: PropertyRow
    let onPick: (String) -> Void

    var body: some View {
        Menu {
            ForEach(row.presets) { preset in
                Button(preset.label) { onPick(preset.value) }
            }
        } label: {
            Image(systemName: "list.bullet")
                .font(.system(size: 10))
                .foregroundColor(Theme.textDim)
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .help("Quick-pick presets")
    }
}

// MARK: - Text / number well (String, Double, UInt — and the Enum /
// Reference fallback when the descriptor declared no presets)

private struct TextWellCell: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool
    let onScrubBegin: () -> Void
    let onScrubEnd: () -> Void

    @State private var text: String = ""
    @FocusState private var isFocused: Bool

    var body: some View {
        HStack(spacing: 4) {
            if row.editable {
                if isScrubbable(kind: row.kind) {
                    ScrubHandle(
                        text: $text,
                        name: row.name,
                        kind: row.kind,
                        onScrubBegin: onScrubBegin,
                        onScrub: { _ = commit($0) },
                        onScrubEnd: onScrubEnd
                    )
                }
                TextField("", text: $text)
                    .textFieldStyle(.plain)
                    .font(Theme.mono(11))
                    .foregroundColor(Theme.textPrimary)
                    .focused($isFocused)
                    .onSubmit { _ = commit(text) }
                    .onChange(of: isFocused) { _, focused in
                        if !focused && text != row.initialValue {
                            _ = commit(text)
                        }
                    }
                if !row.unitLabel.isEmpty {
                    Text(row.unitLabel)
                        .font(Theme.mono(9.5))
                        .foregroundColor(Theme.textDim)
                        .fixedSize()
                }
                if !row.presets.isEmpty {
                    PresetMenu(row: row) { value in
                        text = value
                        _ = commit(value)
                    }
                }
            } else {
                Text(text.isEmpty ? row.initialValue : text)
                    .font(Theme.mono(11))
                    .foregroundColor(Theme.textMuted)
                if !row.unitLabel.isEmpty {
                    Text(row.unitLabel)
                        .font(Theme.mono(9.5))
                        .foregroundColor(Theme.textDim)
                }
                Spacer(minLength: 4)
                Text("read-only")
                    .font(Theme.mono(9))
                    .foregroundColor(Theme.textGhost)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .background(wellBackground())
        .onAppear { text = row.initialValue }
        .onChange(of: row.initialValue) { _, newValue in
            if !isFocused { text = newValue }
        }
    }

    private func commit(_ value: String) -> Bool {
        let accepted = onCommit(value)
        if !accepted { text = row.initialValue }
        return accepted
    }
}

// MARK: - Bool pill toggle

private struct BoolPillCell: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool

    @State private var isOn: Bool = false

    var body: some View {
        HStack(spacing: 8) {
            Button {
                guard row.editable else { return }
                isOn.toggle()
                if !onCommit(isOn ? "true" : "false") {
                    isOn = Self.parseBool(row.initialValue)
                }
            } label: {
                ZStack(alignment: isOn ? .trailing : .leading) {
                    RoundedRectangle(cornerRadius: 9)
                        .fill(isOn ? Theme.accent : Theme.fillTrough)
                        .frame(width: 32, height: 18)
                    Circle()
                        .fill(Color(hex: 0x0d1116))
                        .frame(width: 14, height: 14)
                        .padding(2)
                }
            }
            .buttonStyle(.plain)
            .disabled(!row.editable)

            if !row.editable {
                Text("read-only")
                    .font(Theme.mono(9))
                    .foregroundColor(Theme.textGhost)
            }
            Spacer(minLength: 0)
        }
        .onAppear { isOn = Self.parseBool(row.initialValue) }
        .onChange(of: row.initialValue) { _, v in isOn = Self.parseBool(v) }
    }

    private static func parseBool(_ s: String) -> Bool {
        let l = s.lowercased()
        return l == "true" || l == "1"
    }
}

// MARK: - DoubleVec3 (three tinted wells)

private struct Vec3Cell: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool

    @State private var comps: [String] = ["0", "0", "0"]
    @FocusState private var focusedIndex: Int?

    var body: some View {
        HStack(spacing: 4) {
            ForEach(0..<3, id: \.self) { i in
                TextField("", text: Binding(
                    get: { i < comps.count ? comps[i] : "0" },
                    set: { newValue in
                        while comps.count <= i { comps.append("0") }
                        comps[i] = newValue
                    }
                ))
                .textFieldStyle(.plain)
                .font(Theme.mono(11))
                .foregroundColor(vecColor(i))
                .disabled(!row.editable)
                .focused($focusedIndex, equals: i)
                .onSubmit { commit() }
                .padding(.horizontal, 6)
                .padding(.vertical, 5)
                .background(wellBackground())
            }
        }
        .onAppear { comps = Self.parseVec3(row.initialValue) }
        .onChange(of: row.initialValue) { _, v in
            if focusedIndex == nil { comps = Self.parseVec3(v) }
        }
        .onChange(of: focusedIndex) { old, new in
            if old != nil && new == nil { commit() }
        }
    }

    private func commit() {
        if !onCommit(comps.joined(separator: " ")) {
            comps = Self.parseVec3(row.initialValue)
        }
    }

    private func vecColor(_ i: Int) -> Color {
        switch i {
        case 0:  return Theme.error
        case 1:  return Theme.successLight
        default: return Theme.accentLight
        }
    }

    private static func parseVec3(_ s: String) -> [String] {
        var parts = s.split(separator: " ").map(String.init)
        while parts.count < 3 { parts.append("0") }
        return Array(parts.prefix(3))
    }
}

// MARK: - Filename (well + browse button)

private struct FilenameCell: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool

    @State private var text: String = ""
    @FocusState private var isFocused: Bool

    var body: some View {
        HStack(spacing: 4) {
            TextField("", text: $text)
                .textFieldStyle(.plain)
                .font(Theme.mono(11))
                .foregroundColor(row.editable ? Theme.textPrimary : Theme.textMuted)
                .disabled(!row.editable)
                .focused($isFocused)
                .onSubmit { _ = commit(text) }
                .onChange(of: isFocused) { _, focused in
                    if !focused && text != row.initialValue { _ = commit(text) }
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 5)
                .background(wellBackground())

            if row.editable {
                Button {
                    browse()
                } label: {
                    Image(systemName: "folder")
                        .font(.system(size: 10))
                        .foregroundColor(Theme.textDim)
                }
                .buttonStyle(.plain)
                .padding(6)
                .background(wellBackground())
                .help("Browse…")
            }
        }
        .onAppear { text = row.initialValue }
        .onChange(of: row.initialValue) { _, v in
            if !isFocused { text = v }
        }
    }

    private func browse() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        if panel.runModal() == .OK, let url = panel.url {
            text = url.path
            _ = commit(url.path)
        }
    }

    private func commit(_ value: String) -> Bool {
        let accepted = onCommit(value)
        if !accepted { text = row.initialValue }
        return accepted
    }
}

// MARK: - Reference (swatch + name chip)

private struct ReferenceChipCell: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool

    @State private var text: String = ""
    @FocusState private var isFocused: Bool

    var body: some View {
        HStack(spacing: 8) {
            // A generic identity dot, not a per-entity color swatch —
            // the bridge doesn't expose the referenced entity's actual
            // appearance (material color, etc.), so a swatch that
            // implied one would be fabricated data.
            RoundedRectangle(cornerRadius: 4)
                .fill(Theme.purple.opacity(0.5))
                .frame(width: 16, height: 16)

            if row.editable {
                TextField("", text: $text)
                    .textFieldStyle(.plain)
                    .font(Theme.mono(11.5))
                    .foregroundColor(Theme.textPrimary)
                    .focused($isFocused)
                    .onSubmit { _ = commit(text) }
                    .onChange(of: isFocused) { _, focused in
                        if !focused && text != row.initialValue { _ = commit(text) }
                    }
            } else {
                Text(text.isEmpty ? row.initialValue : text)
                    .font(Theme.mono(11.5))
                    .foregroundColor(Theme.textMuted)
            }
            Spacer(minLength: 4)
            if !row.presets.isEmpty {
                PresetMenu(row: row) { value in
                    text = value
                    _ = commit(value)
                }
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .background(wellBackground())
        .onAppear { text = row.initialValue }
        .onChange(of: row.initialValue) { _, v in
            if !isFocused { text = v }
        }
    }

    private func commit(_ value: String) -> Bool {
        let accepted = onCommit(value)
        if !accepted { text = row.initialValue }
        return accepted
    }
}

// MARK: - Enum (bordered menu chip when presets exist, else a plain well)

private struct EnumChipCell: View {
    let row: PropertyRow
    let onCommit: (String) -> Bool

    @State private var text: String = ""

    var body: some View {
        if row.editable && !row.presets.isEmpty {
            Menu {
                ForEach(row.presets) { preset in
                    Button(preset.label) {
                        text = preset.value
                        if !onCommit(preset.value) {
                            text = row.initialValue
                        }
                    }
                }
            } label: {
                HStack(spacing: 4) {
                    Text(displayLabel)
                        .font(Theme.mono(11))
                        .foregroundColor(Theme.textPrimary)
                    Spacer(minLength: 2)
                    Image(systemName: "chevron.up.chevron.down")
                        .font(.system(size: 8))
                        .foregroundColor(Theme.textDim)
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 5)
                .background(wellBackground())
            }
            .menuStyle(.borderlessButton)
            .onAppear { text = row.initialValue }
            .onChange(of: row.initialValue) { _, v in text = v }
        } else {
            // No descriptor-declared presets for this enum — fall back
            // to a plain editable well rather than inventing options.
            TextWellCell(row: row, onCommit: onCommit, onScrubBegin: {}, onScrubEnd: {})
        }
    }

    private var displayLabel: String {
        row.presets.first(where: { $0.value == text })?.label ?? (text.isEmpty ? row.initialValue : text)
    }
}

// MARK: - Button styles

private struct PillButtonStyle: ButtonStyle {
    var filled: Bool
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        // Filled variant is accent + textOnAccent (2026-07-24 fix): the
        // previous Theme.textPrimary fill inverts to near-black in Light
        // mode, where the Color.black text made the Save label invisible
        // (~1.1:1). Matches ProposalCard's Apply-button idiom; the
        // Windows client (ViewportProperties.cpp) carries the same fix.
        configuration.label
            .font(Theme.sans(11, .semibold))
            .foregroundColor(filled ? Theme.textOnAccent : Theme.textTertiary)
            .padding(.horizontal, 11)
            .padding(.vertical, 5)
            .background(
                RoundedRectangle(cornerRadius: Theme.radiusMedium)
                    .fill(filled ? Theme.accent : Color.clear)
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusMedium)
                    .stroke(filled ? Color.clear : Theme.borderStrong, lineWidth: 1)
            )
            .opacity(isEnabled ? (configuration.isPressed ? 0.85 : 1.0) : 0.4)
    }
}

private struct OutlineAccentButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Theme.sans(11.5))
            .foregroundColor(Theme.accentLight)
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(
                RoundedRectangle(cornerRadius: Theme.radiusMedium)
                    .fill(configuration.isPressed ? Theme.accent.opacity(0.1) : Color.clear)
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusMedium)
                    .stroke(Theme.accent.opacity(0.35), lineWidth: 1)
            )
            .opacity(isEnabled ? 1.0 : 0.4)
    }
}

// MARK: - Scrub helpers

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
            .foregroundColor(Theme.textDim.opacity(dragStart == nil ? 0.85 : 1.0))
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

/// POSIX locale, fixed regardless of the user's system locale: this string is written
/// straight into scene text (a CST param value), and `String(format:)` without an
/// explicit locale follows the CURRENT locale on Apple platforms -- a comma-decimal
/// locale (e.g. fr_FR) would format `4.0` as `4,000000`, which the scene parser cannot
/// read back.  Shared by every call site that formats a value for the wire: the
/// TextWellCell scrub path and ParamSliderCell's drag/commit path both go through here.
/// Deliberately module-visible (not `private`) -- EnvironmentPanel.swift's own
/// `trimNumber` writes CST param values the same way and reuses THIS instance (S5)
/// rather than carrying a second `Locale(identifier: "en_US_POSIX")` construction.
let kPosixLocale = Locale(identifier: "en_US_POSIX")

private func formatValue(_ v: Double, kind: PropertyKind) -> String {
    switch kind {
    case .uint:
        let n = max(0, Int(v.rounded()))
        return String(n)
    case .double:
        return String(format: "%.6g", locale: kPosixLocale, v)
    default:
        return String(format: "%g", locale: kPosixLocale, v)
    }
}
