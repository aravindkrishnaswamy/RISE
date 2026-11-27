//////////////////////////////////////////////////////////////////////
//
//  SimulationsPanel.swift - Simulation M5: the right-panel section
//    listing the scene's declared simulations (cloth_simulation,
//    fire_simulation, simulation_cache) with their library-reported
//    state, fidelity, time support and cache, and the per-row actions:
//    Bake / Stop / Resume (the library bake manager) and, for
//    interactive solvers, Live / Reset / Record.
//
//    Presentation only: SimulationsModel holds the bridge reads and
//    forwards every action; this file draws rows and the trusted-
//    location prompt.  Shown only when the scene declares simulations.
//
//////////////////////////////////////////////////////////////////////

import AppKit
import SwiftUI

struct SimulationsPanel: View {
    @ObservedObject var model: SimulationsModel
    @EnvironmentObject var viewModel: RenderViewModel
    @State private var expanded: Bool = true

    var body: some View {
        if model.hasSimulations {
            VStack(alignment: .leading, spacing: 0) {
                header
                if expanded {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(model.rows) { row in
                            SimulationRowView(model: model, row: row,
                                              selected: model.selected == row.name)
                        }
                        if !model.lastMessage.isEmpty {
                            Text(model.lastMessage)
                                .font(Theme.sans(10.5))
                                .foregroundColor(Theme.error)
                                .fixedSize(horizontal: false, vertical: true)
                                .textSelection(.enabled)
                        }
                    }
                    .padding(.horizontal, 14)
                    .padding(.bottom, 12)
                }
            }
            .overlay(alignment: .bottom) {
                Rectangle().fill(Theme.borderHairline).frame(height: 1)
            }
            .alert(item: $model.trustRequest) { request in
                trustAlert(request)
            }
        }
    }

    private var header: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.12)) { expanded.toggle() }
        } label: {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(expanded ? "▾" : "▸")
                    .font(.system(size: 8))
                    .foregroundColor(Theme.textDim)
                    .frame(width: 8, alignment: .center)
                Text("Simulations")
                    .font(Theme.sans(12.5, .semibold))
                    .foregroundColor(Theme.textPrimary)
                Spacer(minLength: 0)
                Text("\(model.rows.count)")
                    .font(Theme.mono(9.5))
                    .foregroundColor(Theme.textDim)
            }
            .padding(.horizontal, 14)
            .padding(.top, 12)
            .padding(.bottom, expanded ? 8 : 12)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func trustAlert(_ request: CacheTrustRequest) -> Alert {
        let what: String
        switch request.action {
        case .bake: what = "bake"
        case .resume: what = "resume"
        case .record: what = "record"
        }
        if request.suggestedRoot.isEmpty {
            return Alert(title: Text("Cannot \(what) `\(request.simulation)` here"),
                         message: Text(request.reason
                                       + "\n\nChoose a private cache directory in the simulation's `cache` field."),
                         dismissButton: .default(Text("OK")) { model.cancelTrust() })
        }
        let message = request.reason
            + "\n\nRISE refuses to write simulation caches where another user could swap a directory "
            + "between its checks and its writes. Trusting \(request.suggestedRoot) stops that check at this "
            + "directory for every cache below it: the cache directories themselves stay private (owner only), "
            + "but whoever can write \(request.suggestedRoot) or its parent directories could still replace a "
            + "directory below it. Trust it only if everyone who can write there is trusted with your caches."
            + "\n\nThe choice is saved in the app's preferences and applied at every launch."
        return Alert(title: Text("Trust this cache location?"),
                     message: Text(message),
                     primaryButton: .default(Text("Trust this location")) { model.trustAndContinue(request) },
                     secondaryButton: .cancel { model.cancelTrust() })
    }
}

private struct SimulationRowView: View {
    @ObservedObject var model: SimulationsModel
    @EnvironmentObject var viewModel: RenderViewModel
    let row: SimulationRow
    let selected: Bool

    private var live: LiveRunRow? { model.liveRuns[row.name] }
    private var isBusy: Bool { model.busy.contains(row.name) }

    private var stateStyle: SimulationRangeStyle {
        if live != nil { return .live }
        if job?.isActive == true { return .baking }
        switch row.state {
        case .complete: return .complete
        case .baking: return .baking
        case .stale: return .stale
        default: return .partial
        }
    }

    private var stateColor: Color {
        switch row.state {
        case .missing: return live != nil ? stateStyle.color : Theme.textDim
        case .failed: return Theme.errorStrong
        default: return stateStyle.color
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Button {
                model.selected = selected ? nil : row.name
                // M5c: the row also selects the simulation in the scene, so the
                // Inspector shows its solver fields (the Simulation category).
                if !selected { viewModel.jumpToEntity(category: .simulation, name: row.name) }
            } label: {
                HStack(spacing: 6) {
                    Circle().fill(stateColor).frame(width: 7, height: 7)
                    Text(row.name)
                        .font(Theme.sans(12, .semibold))
                        .foregroundColor(Theme.textPrimary)
                        .lineLimit(1)
                    Text(row.solverId.isEmpty ? row.keyword : row.solverId)
                        .font(Theme.mono(9.5))
                        .foregroundColor(Theme.textFaint)
                        .lineLimit(1)
                    Spacer(minLength: 0)
                    Text(live != nil ? "live" : row.state.label)
                        .font(Theme.mono(10))
                        .foregroundColor(stateColor)
                    if isBusy { ProgressView().controlSize(.mini) }
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            HStack(spacing: 8) {
                Text(row.labelText)
                    .help(row.evidenceText.isEmpty ? "The fidelity label of the cache on disk." : row.evidenceText)
                Text("\(row.framesAvailable)/\(row.framesTotal) frames")
                Text(row.timeSupportLabel)
            }
            .font(Theme.mono(9.5))
            .foregroundColor(Theme.textMuted)
            .lineLimit(1)

            if !row.evidenceText.isEmpty {
                Text(row.evidenceText)
                    .font(Theme.mono(9.5))
                    .foregroundColor(Theme.textMuted)
                    .lineLimit(3)
                    .fixedSize(horizontal: false, vertical: true)
                    .help(row.evidenceText)
            }

            if row.state == .stale && !row.staleReason.isEmpty {
                Text("stale: " + row.staleReason)
                    .font(Theme.mono(9.5))
                    .foregroundColor(Theme.dirty)
                    .lineLimit(5)   // a group lists each member's reason: do not truncate it
                    .fixedSize(horizontal: false, vertical: true)
                    .help("stale: " + row.staleReason)
            }

            if !row.estimateSummary.isEmpty && job?.isActive != true && row.isProducer {
                Text("bake estimate " + row.estimateSummary)
                    .font(Theme.mono(9.5))
                    .foregroundColor(Theme.textMuted)
                    .lineLimit(1)
                    .help("The solver's estimate for a whole bake on this machine class: approximate. "
                          + "Memory is the peak it declares; the bake manager counts it against its budget.")
            }

            if !row.cacheDirectory.isEmpty {
                Text(row.cacheDirectory)
                    .font(Theme.mono(9))
                    .foregroundColor(Theme.textDim)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(row.cacheDirectory)
                    .contextMenu {
                        Button("Copy path") {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(row.cacheDirectory, forType: .string)
                        }
                        Button("Reveal in Finder") { revealInFinder(row.cacheDirectory) }
                    }
            }

            if row.isProducer { actions }

            if selected { details }
        }
        .padding(8)
        .background(selected ? Theme.fillActive : Theme.bgCard,
                    in: RoundedRectangle(cornerRadius: 6))
    }

    private var job: BakeJobRow? { model.bakeJobs[row.name] }

    private var actions: some View {
        VStack(alignment: .leading, spacing: 4) {
            if let job, job.state != .finished {
                Text("bake: " + job.summary)
                    .font(Theme.mono(9.5))
                    .foregroundColor(job.state == .failed ? Theme.error : Theme.textMuted)
                    .lineLimit(2)
            }
            HStack(spacing: 6) {
                if job?.isActive == true || row.state == .baking {
                    actionButton("Stop", "Stop the bake at its next checkpoint (it can be resumed)") {
                        model.stopBake(row.name)
                    }
                } else if job?.resumable == true || row.state == .partial {
                    actionButton("Resume", "Continue the bake from the partial cache") { model.resume(row.name) }
                    actionButton("Bake", "Bake (a partial cache of the current configuration is continued)") {
                        model.bake(row.name)
                    }
                } else {
                    actionButton(row.state == .complete ? "Rebake" : "Bake",
                                 row.state == .complete ? "Bake again although the cache is current"
                                                        : "Bake this simulation (and any out-of-date upstream) into its cache") {
                        model.bake(row.name)
                    }
                }
                if row.interactive && !row.consumerGeometry.isEmpty {
                    Divider().frame(height: 14)
                    if live != nil {
                        actionButton("Stop live", "Stop the live run and show the baked cache again") {
                            model.stopLive(row.name)
                        }
                        actionButton("Reset", "Restart the live run from its initial state") {
                            model.resetLive(row.name)
                        }
                        actionButton("Record", "Restart live and also write the run into the cache") {
                            model.record(row.name)
                        }
                    } else {
                        actionButton("Live", "Run the solver in the viewport and play it") {
                            model.goLive(row.name)
                        }
                    }
                }
                Spacer(minLength: 0)
            }
        }
        .disabled(isBusy)
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 2) {
            detail("keyword", row.keyword)
            if !row.backend.isEmpty { detail("backend", row.backend) }
            if !row.consumerGeometry.isEmpty { detail("shown by", row.consumerGeometry) }
            if !row.inputs.isEmpty { detail("scene inputs", row.inputs) }
            if !row.upstream.isEmpty {
                detail(row.isGroup ? "members" : "upstream", row.upstream + (row.upstreamOutOfDate ? " (out of date)" : ""))
            }
            ForEach(Array(row.connections.enumerated()), id: \.offset) { _, line in
                detail("coupling", line)
            }
            if let live {
                detail("live run", "\(live.framesPublished)/\(live.framesTotal) frames published"
                       + (live.playing ? ", stepping" : "") + (live.finished ? ", finished" : "")
                       + (live.failed ? ", failed" : ""))
            }
            if !row.errorMessage.isEmpty {
                Text(row.errorMessage)
                    .font(Theme.sans(10.5))
                    .foregroundColor(Theme.error)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            }
            Text("Parameters: in the Inspector (this row selects `\(row.name)`). An edit marks the cache stale and never starts a bake.")
                .font(Theme.sans(10))
                .foregroundColor(Theme.textDim)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.top, 2)
    }

    private func detail(_ key: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(key)
                .font(Theme.mono(9.5))
                .foregroundColor(Theme.textDim)
                .frame(width: 78, alignment: .leading)
            Text(value)
                .font(Theme.mono(9.5))
                .foregroundColor(Theme.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
                .textSelection(.enabled)
        }
    }

    /// The app's outline pill (PillButtonStyle): Theme colours, legible in
    /// both appearances (the system `.bordered` style drew dark-on-dark
    /// labels against the panel's own dark surface).
    private func actionButton(_ title: String, _ help: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title).lineLimit(1)
        }
        .buttonStyle(PillButtonStyle(filled: false))
        .help(help)
    }

    /// Selects the cache directory in Finder (its parent when it does not exist yet).
    private func revealInFinder(_ path: String) {
        var url = URL(fileURLWithPath: path)
        while !FileManager.default.fileExists(atPath: url.path) && url.pathComponents.count > 1 {
            url = url.deletingLastPathComponent()
        }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
}
