//
//  NodeGraphPalette.swift
//  RISE-GUI
//
//  doc-88 Phase 3 S21 (docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S21): the
//  node-graph canvas's "add node" search palette, opened from
//  NodeGraphCanvas's toolbar "+" button (the only entry point this
//  slice wires up -- there is no double-click-on-empty-canvas gesture
//  anywhere in NodeGraphCanvas.swift; an earlier draft of this comment
//  claimed one, corrected S21 review round 1 P3).
//
//  TWO-STEP FLOW, mirroring the S18 bridge contract exactly:
//    1. Pick a category (Painter / Function / Material) and search/select
//       a keyword from `-paletteKeywords(forCategory:)` -- the open set
//       `-createChunkNode` can build, straight off the descriptor
//       registry (RISE::AllKeywordsForCategory), not a hand-maintained
//       Swift list.
//    2. If `-chunkNodeRequirements(forKeyword:)` is non-empty, fill each
//       one before creating: a REFERENCE requirement gets a picker of
//       legal candidates (`-checkConnectionByKeyword`, the pure-
//       descriptor S17 form -- the ordinary by-name `-checkConnection`
//       can't run yet, because the node being created doesn't exist to
//       address); a `file` requirement (the raster-image painters'
//       required `file` param -- png_painter, exr_painter, etc.) opens
//       an NSOpenPanel per the S18 pre-flight contract ("file-slot
//       keywords open the file picker... the EXR reader throws rather
//       than returning an error" -- NODE_GRAPH_CANVAS.md sect. 6 S18);
//       any other literal gets a plain text field.
//
//  No fake preview, no private write path: the ONLY mutation here is
//  the single `-createChunkNode` call in `create()`. Positioning near
//  the drop point is NodeGraphCanvas's job (`didCreateNode`), not this
//  sheet's -- this file only picks WHAT to create.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//

import SwiftUI
import AppKit
import UniformTypeIdentifiers

/// A node already on the canvas, reduced to what the requirement
/// candidate picker needs. Built from `snapshot.nodes` by
/// `NodeGraphCanvas.paletteCandidateNodes`.
struct PaletteCandidateNode: Identifiable, Hashable {
    var id: String { name }
    let name: String
    let keyword: String
    let category: Int
}

/// One decoded entry from `-chunkNodeRequirements(forKeyword:)`.
private struct ChunkRequirement: Identifiable {
    var id: String { param }
    let param: String
    let description: String
    let isReference: Bool
}

private enum PaletteCategory: Int, CaseIterable, Hashable {
    case painter = 0
    case function = 1
    case material = 2

    var label: String {
        switch self {
        case .painter: return "Painter"
        case .function: return "Function"
        case .material: return "Material"
        }
    }
}

struct NodeGraphAddNodeSheet: View {
    weak var bridge: RISEViewportBridge?
    let existingNodes: [PaletteCandidateNode]
    /// (name, keyword, category) of the node that landed.
    let onCreated: (String, String, Int) -> Void
    let onDismiss: () -> Void

    @State private var category: PaletteCategory = .painter
    @State private var search: String = ""
    @State private var keywords: [String] = []

    // Step 2 state -- non-nil `selectedKeyword` means "on the
    // requirement-fill step."
    @State private var selectedKeyword: String? = nil
    @State private var requirements: [ChunkRequirement] = []
    @State private var argValues: [String: String] = [:]
    @State private var baseName: String = ""
    @State private var errorMessage: String? = nil
    @State private var isCreating = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            sheetHeader
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
            if let keyword = selectedKeyword {
                requirementForm(keyword: keyword)
            } else {
                keywordPicker
            }
            if let errorMessage {
                Text(errorMessage)
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.errorStrong)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 8)
            }
        }
        .frame(width: 420, height: 460)
        .background(Theme.bgCard)
        .onAppear { reloadKeywords() }
        .onChange(of: category) { _, _ in reloadKeywords() }
    }

    // MARK: - Header

    private var sheetHeader: some View {
        HStack(spacing: 8) {
            if selectedKeyword != nil {
                Button {
                    selectedKeyword = nil
                    errorMessage = nil
                } label: {
                    Image(systemName: "chevron.left").font(.system(size: 11, weight: .semibold))
                }
                .buttonStyle(.plain)
                .foregroundColor(Theme.textDim)
            }
            Text(selectedKeyword ?? "Add Node")
                .font(Theme.sans(12.5, .semibold))
                .foregroundColor(Theme.textPrimary)
            Spacer()
            Button {
                onDismiss()
            } label: {
                Image(systemName: "xmark").font(.system(size: 10, weight: .semibold))
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.textDim)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    // MARK: - Step 1: category + search + keyword list

    private var keywordPicker: some View {
        VStack(alignment: .leading, spacing: 8) {
            Picker("", selection: $category) {
                ForEach(PaletteCategory.allCases, id: \.self) { c in
                    Text(c.label).tag(c)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(.horizontal, 14)
            .padding(.top, 10)

            TextField("Search keywords\u{2026}", text: $search)
                .textFieldStyle(.roundedBorder)
                .font(Theme.mono(11))
                .padding(.horizontal, 14)

            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(filteredKeywords, id: \.self) { kw in
                        Button {
                            selectKeyword(kw)
                        } label: {
                            HStack {
                                Text(kw)
                                    .font(Theme.mono(11.5))
                                    .foregroundColor(Theme.textPrimary)
                                Spacer()
                            }
                            .padding(.horizontal, 14)
                            .padding(.vertical, 6)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                    if filteredKeywords.isEmpty {
                        Text("No \(category.label.lowercased()) keywords match \u{201c}\(search)\u{201d}")
                            .font(Theme.mono(10.5))
                            .foregroundColor(Theme.textFaint)
                            .padding(14)
                    }
                }
            }
        }
    }

    private var filteredKeywords: [String] {
        guard !search.isEmpty else { return keywords }
        let needle = search.lowercased()
        return keywords.filter { $0.lowercased().contains(needle) }
    }

    private func reloadKeywords() {
        guard let bridge else { keywords = []; return }
        keywords = bridge.paletteKeywords(forCategory: category.rawValue)
    }

    private func selectKeyword(_ keyword: String) {
        selectedKeyword = keyword
        errorMessage = nil
        baseName = ""
        argValues = [:]
        guard let bridge else { requirements = []; return }
        let raw = bridge.chunkNodeRequirements(forKeyword: keyword)
        requirements = raw.map { d in
            ChunkRequirement(
                param: (d["param"] as? String) ?? "",
                description: (d["description"] as? String) ?? "",
                isReference: (d["isReference"] as? Bool) ?? false)
        }
        // A zero-requirement keyword creates immediately -- no reason to
        // make the user look at an empty form and press Create.
        if requirements.isEmpty { create() }
    }

    // MARK: - Step 2: required-arg form

    @ViewBuilder
    private func requirementForm(keyword: String) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Name").font(Theme.mono(9, .semibold)).foregroundColor(Theme.textFaint)
                    TextField(keyword, text: $baseName)
                        .textFieldStyle(.roundedBorder)
                        .font(Theme.mono(11))
                }

                ForEach(requirements) { req in
                    requirementRow(keyword: keyword, req: req)
                }
            }
            .padding(14)
        }
        HStack {
            Spacer()
            Button(isCreating ? "Creating\u{2026}" : "Create", action: create)
                .buttonStyle(.plain)
                .foregroundColor(Theme.textOnAccent)
                .padding(.horizontal, 14).padding(.vertical, 6)
                .background(canCreate ? Theme.accent : Theme.accent.opacity(0.4))
                .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
                .disabled(!canCreate || isCreating)
        }
        .padding(.horizontal, 14)
        .padding(.bottom, 12)
    }

    private var canCreate: Bool {
        requirements.allSatisfy { !(argValues[$0.param] ?? "").isEmpty }
    }

    @ViewBuilder
    private func requirementRow(keyword: String, req: ChunkRequirement) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 4) {
                Text(req.param).font(Theme.mono(9, .semibold)).foregroundColor(Theme.textFaint)
                Text("required").font(Theme.mono(8)).foregroundColor(Theme.warn)
            }
            if !req.description.isEmpty {
                Text(req.description).font(Theme.mono(9)).foregroundColor(Theme.textFaint)
            }
            if req.isReference {
                referenceCandidatePicker(keyword: keyword, req: req)
            } else if req.param == "file" {
                fileSlotPicker(req: req)
            } else {
                TextField("Value", text: Binding(
                    get: { argValues[req.param] ?? "" },
                    set: { argValues[req.param] = $0 }))
                    .textFieldStyle(.roundedBorder)
                    .font(Theme.mono(11))
            }
        }
    }

    /// The S17 pure-descriptor candidate filter: every existing node
    /// whose (keyword, category) `-checkConnectionByKeyword` accepts for
    /// THIS specific (new keyword, param) pair -- the same legality rule
    /// a hand-authored scene line would be held to, run BEFORE the new
    /// chunk exists to address by name (see this file's header comment
    /// for why the by-name form can't be used here).
    ///
    /// ADVISORY, NOT AUTHORITATIVE (S21 review round 1 P3, verified against
    /// ConnectionLegality.h's own documented "WHAT THIS DOES NOT DO"):
    /// `-checkConnectionByKeyword` only recognizes the STATICALLY-visible
    /// per-channel `scalar_painter { values <r> <g> <b> }` form; a per-
    /// channel `scalar_painter { expression vec3(...) } }` (HasPerChannel
    /// Variation() == true only once evaluated) is NOT caught here, so this
    /// picker can list a candidate that would violate a `requireSingle`
    /// Scalar-pipe slot. That false-accept is not a hole in practice: the
    /// COMMIT is the real guard. `-createChunkNode` / `-rewireConnection`
    /// both route through `SceneEditController`'s full-derivability dry-run
    /// (`Job::ApplyCstParamEditChecked` -> `DeriveEditedCstDocument_`),
    /// which re-derives the whole document through the real chunk parsers
    /// -- so a per-channel candidate offered here still gets refused
    /// honestly at commit time by `ResolveOrDiagnoseScalar`'s
    /// bound-to-per-channel branch (Job.cpp), the same diagnostic a hand-
    /// authored scene line would earn. Pinned end-to-end (false-accept at
    /// this layer, refusal at commit) by RewireConnectionTest.cpp PART 8.
    private func referenceCandidatePicker(keyword: String, req: ChunkRequirement) -> some View {
        let candidates = existingNodes.filter { candidate in
            guard let bridge else { return false }
            var diag: NSString? = nil
            return bridge.checkConnectionByKeyword(
                targetKeyword: keyword, param: req.param,
                candidateKeyword: candidate.keyword, candidateCategory: candidate.category,
                outDiagnostic: &diag)
        }
        return Menu {
            if candidates.isEmpty {
                Text("No legal candidates on the canvas yet")
            }
            ForEach(candidates) { c in
                Button(c.name) { argValues[req.param] = c.name }
            }
        } label: {
            let chosen = argValues[req.param] ?? ""
            HStack {
                Text(chosen.isEmpty ? "Choose\u{2026}" : chosen)
                    .font(Theme.mono(11))
                    .foregroundColor(chosen.isEmpty ? Theme.textFaint : Theme.textPrimary)
                Spacer()
                Image(systemName: "chevron.up.chevron.down").font(.system(size: 8)).foregroundColor(Theme.textFaint)
            }
            .padding(.horizontal, 8).padding(.vertical, 5)
            .background(Theme.fillTrough)
            .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
        }
        .menuStyle(.borderlessButton)
    }

    /// The S18 file-slot pre-flight: a required `file` param opens a
    /// real NSOpenPanel rather than a free-text field, so a bad path can
    /// never reach the EXR/PNG/TIFF/HDR reader at derive time -- that
    /// reader throws rather than returning an error (S18's own comment;
    /// this UI-level guard is what keeps a canvas-driven create from
    /// ever exercising that path with a typo'd path).
    private func fileSlotPicker(req: ChunkRequirement) -> some View {
        let currentPath = argValues[req.param] ?? ""
        let displayName = currentPath.isEmpty ? "No file chosen" : URL(fileURLWithPath: currentPath).lastPathComponent
        return HStack(spacing: 6) {
            Text(displayName)
                .font(Theme.mono(10.5))
                .foregroundColor(currentPath.isEmpty ? Theme.textFaint : Theme.textPrimary)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
            Button("Choose\u{2026}") {
                pickFile { path in argValues[req.param] = path }
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.accent)
            .font(Theme.mono(10.5))
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(Theme.fillTrough)
        .clipShape(RoundedRectangle(cornerRadius: Theme.radiusMedium))
    }

    private func pickFile(_ chosen: @escaping (String) -> Void) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.title = "Choose an image file"
        var types: [UTType] = []
        for ext in ["png", "jpg", "jpeg", "tif", "tiff", "exr", "hdr"] {
            if let t = UTType(filenameExtension: ext) { types.append(t) }
        }
        if !types.isEmpty { panel.allowedContentTypes = types }
        if panel.runModal() == .OK, let url = panel.url { chosen(url.path) }
    }

    // MARK: - Create

    private func create() {
        guard let bridge, let keyword = selectedKeyword else { return }
        guard canCreate else {
            errorMessage = "Fill every required field first."
            return
        }
        isCreating = true
        errorMessage = nil
        let params = requirements.map { $0.param }
        let values = requirements.map { argValues[$0.param] ?? "" }
        var outName: NSString? = nil
        var outMessage: NSString? = nil
        let applied = bridge.createChunkNode(
            keyword: keyword,
            baseName: baseName.isEmpty ? nil : baseName,
            argParams: params.isEmpty ? nil : params,
            argValues: values.isEmpty ? nil : values,
            outName: &outName, outMessage: &outMessage)
        isCreating = false
        if applied, let name = outName as String?, !name.isEmpty {
            onCreated(name, keyword, category.rawValue)
        } else {
            errorMessage = (outMessage as String?) ?? "The node could not be created."
        }
    }
}
