//////////////////////////////////////////////////////////////////////
//
//  EnvironmentPanel.swift - The right-panel "Environment" section: the
//    scene's image-based-lighting (IBL) environment as a first-class,
//    always-present control group (NOT a per-selection inspector, since
//    the environment is a scene-level singleton — an hdr/exr painter
//    bound via radiance_* on the active rasterizer, invisible to the
//    Lights category).
//
//    States:
//      • no scene / no active rasterizer  -> the section hides itself
//      • procedural sky (hosek) installed -> read-only note
//      • no environment                   -> "Add HDRI…" (when editable)
//      • environment bound                -> file (Swap…), intensity,
//        rotation X/Y/Z (degrees), "Show in background", Remove
//
//    Every edit routes through RISEViewportBridge -> SceneEditController,
//    which applies it LIVE (viewport re-renders) and mirrors it into the
//    CST so a save keeps it.  See docs/gui/ENVIRONMENT_SECTION.md.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import UniformTypeIdentifiers

struct EnvironmentPanel: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int
    @EnvironmentObject var viewModel: RenderViewModel

    @State private var env: RISEEnvironmentInfo? = nil
    @State private var expanded: Bool = true

    // Local edit buffers so a field shows the user's in-progress text
    // without fighting the reloaded snapshot until they commit.
    @State private var scaleText: String = ""
    @State private var orientXText: String = ""
    @State private var orientYText: String = ""
    @State private var orientZText: String = ""
    @FocusState private var focusedField: Field?

    private enum Field: Hashable { case scale, ox, oy, oz }

    /// Editable only when the active rasterizer supports a live radiance-map
    /// edit AND the scene isn't wedged by an in-flight production / agent render.
    private var canEdit: Bool {
        (env?.editable ?? false) && viewModel.isSceneEditableForAgents
    }

    var body: some View {
        // The environment is meaningful only once a scene with an active
        // rasterizer is loaded; hide the whole section otherwise so an
        // empty app window isn't cluttered.
        Group {
            if let e = env {
                VStack(alignment: .leading, spacing: 0) {
                    header
                    if expanded {
                        content(e)
                            .padding(.horizontal, 14)
                            .padding(.bottom, 12)
                    }
                }
                .overlay(alignment: .bottom) {
                    Rectangle().fill(Theme.borderHairline).frame(height: 1)
                }
            }
        }
        .onAppear { reload() }
        .onChange(of: refreshTrigger) { _, _ in reload() }
        .onChange(of: viewModel.entityListEpoch) { _, _ in reload() }
    }

    // MARK: - Header

    private var header: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.12)) { expanded.toggle() }
        } label: {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(expanded ? "▾" : "▸")
                    .font(.system(size: 8))
                    .foregroundColor(Theme.textDim)
                    .frame(width: 8, alignment: .center)
                Text("Environment")
                    .font(Theme.sans(12.5, .semibold))
                    .foregroundColor(Theme.textPrimary)
                Spacer(minLength: 0)
                Text(statusChip)
                    .font(Theme.mono(9.5))
                    .foregroundColor(Theme.textDim)
                    .lineLimit(1)
            }
            .padding(.horizontal, 14)
            .padding(.top, 12)
            .padding(.bottom, expanded ? 8 : 12)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private var statusChip: String {
        guard let e = env else { return "" }
        if e.proceduralSky { return "procedural sky" }
        if e.hasEnvironment { return (e.file as NSString).lastPathComponent }
        return "none"
    }

    // MARK: - Content

    @ViewBuilder
    private func content(_ e: RISEEnvironmentInfo) -> some View {
        if e.proceduralSky {
            infoNote("Procedural sky (Hosek–Wilkie) is providing the environment. Edit it in the scene file.")
        } else if !e.hasEnvironment {
            emptyState
        } else {
            boundState(e)
        }
    }

    private var emptyState: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("No environment")
                .font(Theme.sans(11))
                .foregroundColor(Theme.textDim)
            if canEdit {
                actionButton(title: "Add HDRI…", systemImage: "photo.on.rectangle") {
                    pickFile { path in
                        var outName: NSString?
                        var outMsg: NSString?
                        let ok = bridge.addEnvironment(path, outName: &outName, outMessage: &outMsg)
                        finishEdit(ok: ok, failureMessage: outMsg as String?)
                    }
                }
            } else {
                disabledReasonNote
            }
        }
    }

    private func boundState(_ e: RISEEnvironmentInfo) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            // File row
            fieldLabel("Image")
            HStack(spacing: 6) {
                Text((e.file as NSString).lastPathComponent.isEmpty
                     ? e.painterName : (e.file as NSString).lastPathComponent)
                    .font(Theme.mono(11))
                    .foregroundColor(Theme.textTertiary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(e.file.isEmpty ? e.painterName : e.file)
                Spacer(minLength: 4)
                if canEdit {
                    smallButton("Swap…") {
                        pickFile { path in
                            finishEdit(ok: bridge.setEnvironmentFile(path), failureMessage: nil)
                        }
                    }
                }
            }

            // Intensity
            fieldLabel("Intensity")
            numericField(text: $scaleText, field: .scale, enabled: canEdit) { v in
                finishEdit(ok: bridge.setEnvironmentScale(v), failureMessage: nil)
            }

            // Rotation (degrees)
            fieldLabel("Rotation (°)")
            HStack(spacing: 6) {
                axisField(text: $orientXText, field: .ox, axis: "X")
                axisField(text: $orientYText, field: .oy, axis: "Y")
                axisField(text: $orientZText, field: .oz, axis: "Z")
            }

            // Background toggle
            Toggle(isOn: Binding(
                get: { env?.background ?? true },
                set: { newVal in finishEdit(ok: bridge.setEnvironmentBackground(newVal), failureMessage: nil) }
            )) {
                Text("Show in background")
                    .font(Theme.sans(11))
                    .foregroundColor(Theme.textTertiary)
            }
            .toggleStyle(.switch)
            .controlSize(.mini)
            .disabled(!canEdit)

            HStack {
                Spacer(minLength: 0)
                if canEdit {
                    smallButton("Remove", destructive: true) {
                        finishEdit(ok: bridge.removeEnvironment(), failureMessage: nil)
                    }
                }
            }

            if !canEdit {
                disabledReasonNote
            }
        }
    }

    // MARK: - Small building blocks

    private func fieldLabel(_ s: String) -> some View {
        Text(s)
            .font(Theme.mono(9.5))
            .foregroundColor(Theme.textDim)
    }

    private func infoNote(_ s: String) -> some View {
        Text(s)
            .font(Theme.sans(10.5))
            .foregroundColor(Theme.textDim)
            .fixedSize(horizontal: false, vertical: true)
    }

    private var disabledReasonNote: some View {
        infoNote(
            (env?.editable ?? false)
            ? "Editing is paused while a render is in flight."
            : "Environment editing needs a Path Tracing / BDPT / VCM rasterizer."
        )
    }

    private func numericField(text: Binding<String>, field: Field, enabled: Bool,
                              commit: @escaping (Double) -> Void) -> some View {
        TextField("", text: text)
            .textFieldStyle(.plain)
            .font(Theme.mono(11.5))
            .foregroundColor(enabled ? Theme.textPrimary : Theme.textDisabled)
            .focused($focusedField, equals: field)
            .disabled(!enabled)
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 5).fill(Theme.bgWell))
            .overlay(RoundedRectangle(cornerRadius: 5).stroke(Theme.borderHairline, lineWidth: 1))
            .onSubmit { commitNumeric(text.wrappedValue, commit) }
            .onChange(of: focusedField) { _, now in
                // Commit on blur (focus left this field).
                if now != field, let v = Double(text.wrappedValue) { commit(v) }
            }
    }

    private func axisField(text: Binding<String>, field: Field, axis: String) -> some View {
        HStack(spacing: 3) {
            Text(axis)
                .font(Theme.mono(9))
                .foregroundColor(Theme.textDisabled)
            numericField(text: text, field: field, enabled: canEdit) { _ in
                commitRotation()
            }
        }
    }

    private func commitNumeric(_ s: String, _ commit: @escaping (Double) -> Void) {
        if let v = Double(s) { commit(v) }
    }

    /// Rotation commits all three axes together (radiance_orient is one vec3).
    private func commitRotation() {
        let x = Double(orientXText) ?? env?.orientX ?? 0
        let y = Double(orientYText) ?? env?.orientY ?? 0
        let z = Double(orientZText) ?? env?.orientZ ?? 0
        finishEdit(ok: bridge.setEnvironmentOrient(x: x, y: y, z: z), failureMessage: nil)
    }

    private func actionButton(title: String, systemImage: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 5) {
                Image(systemName: systemImage).font(.system(size: 10))
                Text(title).font(Theme.sans(11, .medium))
            }
            .foregroundColor(Theme.accentLight)
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.accent.opacity(0.14)))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.accent.opacity(0.3), lineWidth: 1))
        }
        .buttonStyle(.plain)
    }

    private func smallButton(_ title: String, destructive: Bool = false, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.sans(10.5, .medium))
                .foregroundColor(destructive ? Theme.error : Theme.accentLight)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 5)
                    .fill((destructive ? Theme.error : Theme.accent).opacity(0.12)))
        }
        .buttonStyle(.plain)
    }

    // MARK: - Actions

    private func reload() {
        let e = bridge.environmentInfo()
        env = e
        if let e = e, !focusedFieldIsActive {
            scaleText = trimNumber(e.scale)
            orientXText = trimNumber(e.orientX)
            orientYText = trimNumber(e.orientY)
            orientZText = trimNumber(e.orientZ)
        }
    }

    private var focusedFieldIsActive: Bool { focusedField != nil }

    private func finishEdit(ok: Bool, failureMessage: String?) {
        if !ok, let msg = failureMessage, !msg.isEmpty {
            viewModel.presentEnvironmentNotice(msg)
        }
        reload()
        refreshTrigger &+= 1
    }

    /// Present an NSOpenPanel restricted to HDR / EXR images; call `chosen`
    /// with the picked path.  The picker is the guard against a non-existent
    /// file (the core registers a black texture for a missing path).
    private func pickFile(_ chosen: @escaping (String) -> Void) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.title = "Choose an HDRI environment image"
        var types: [UTType] = []
        if let hdr = UTType(filenameExtension: "hdr") { types.append(hdr) }
        if let exr = UTType(filenameExtension: "exr") { types.append(exr) }
        if !types.isEmpty { panel.allowedContentTypes = types }
        if panel.runModal() == .OK, let url = panel.url {
            chosen(url.path)
        }
    }

    private func trimNumber(_ v: Double) -> String {
        // Whole numbers show without a trailing ".0"; others keep up to 4 dp.
        if v == v.rounded() { return String(Int(v)) }
        return String(format: "%g", v)
    }
}
