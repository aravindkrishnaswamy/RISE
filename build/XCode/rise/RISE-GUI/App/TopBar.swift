import SwiftUI

/// The workspace's persistent 44 pt top bar (design brief "Top bar"):
/// scene identity (left), refinement-status cluster (center), Save
/// (right).  Native menu bar (RISEApp.swift) covers File / Edit /
/// Render / View — this in-window strip is always visible regardless
/// of which menu is open.
struct TopBar: View {
    @EnvironmentObject var viewModel: RenderViewModel

    /// Progress-bar fraction held while refinement is paused, so the
    /// bar freezes instead of collapsing to 0 — updated by
    /// `syncLastFraction()` whenever the state ISN'T paused.
    @State private var lastNonPausedFraction: CGFloat = 0

    private static let logoGradient = LinearGradient(
        gradient: Gradient(stops: Theme.spectralStops),
        startPoint: .topLeading, endPoint: .bottomTrailing)

    var body: some View {
        HStack(spacing: 14) {
            sceneIdentity
            Spacer(minLength: 0)
            renderStatusCluster
            Spacer(minLength: 0)
            saveButton
        }
        .padding(.horizontal, 14)
        .frame(height: 44)
        .background(Theme.bgTopBar)
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
        }
        .onChange(of: viewModel.refinementPhase) { _, _ in syncLastFraction() }
        .onChange(of: viewModel.refinementScaleDivisor) { _, _ in syncLastFraction() }
        .onChange(of: viewModel.progress) { _, _ in syncLastFraction() }
        .onChange(of: viewModel.renderState) { _, _ in syncLastFraction() }
        .onAppear { syncLastFraction() }
    }

    // MARK: - Left: scene identity

    private var sceneIdentity: some View {
        HStack(spacing: 9) {
            RoundedRectangle(cornerRadius: 5)
                .fill(Self.logoGradient)
                .frame(width: 20, height: 20)
            Text("RISE")
                .font(Theme.sans(13, .bold))
                .tracking(13 * 0.12)
                .foregroundColor(Theme.textPrimary)

            Rectangle().fill(Theme.borderLight).frame(width: 1, height: 20)

            if let path = viewModel.loadedFilePath {
                HStack(spacing: 8) {
                    Text((path as NSString).lastPathComponent)
                        .font(Theme.mono(12))
                        .foregroundColor(Theme.textSecondary)
                    if viewModel.sceneEditsDirty {
                        HStack(spacing: 5) {
                            Circle().fill(Theme.dirty).frame(width: 6, height: 6)
                            Text("edited")
                                .font(Theme.sans(10))
                                .foregroundColor(Theme.dirty)
                        }
                    }
                }
            } else {
                Text("No scene")
                    .font(Theme.mono(12))
                    .foregroundColor(Theme.textDim)
            }
        }
    }

    // MARK: - Center: render-status cluster

    private var renderStatusCluster: some View {
        HStack(spacing: 10) {
            pauseResumeButton
            restartButton
            readout
            Rectangle().fill(Theme.borderLight).frame(width: 1, height: 22)
            integratorChip
        }
        .padding(.vertical, 5)
        .padding(.horizontal, 6)
        .background(Theme.bgWell, in: RoundedRectangle(cornerRadius: Theme.radiusLarge))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusLarge)
                .stroke(Theme.borderLight, lineWidth: 1)
        )
    }

    /// True while no bridge is attached, or a production render owns
    /// the scene — the same window `pauseRefinement`/`resumeRefinement`/
    /// `restartRefinement` no-op in on the view-model side; disabling
    /// the buttons here keeps the UI honest about it.
    private var refinementControlsDisabled: Bool {
        viewModel.viewportBridge == nil
            || viewModel.renderState == .rendering
            || viewModel.renderState == .cancelling
    }

    private var pauseResumeButton: some View {
        Button {
            viewModel.togglePauseRefinement()
        } label: {
            Image(systemName: viewModel.isRefinementPaused ? "play.fill" : "pause.fill")
                .font(.system(size: 10))
                .foregroundColor(Theme.textPrimary)
                .frame(width: 26, height: 26)
                .background(Theme.fillActive, in: RoundedRectangle(cornerRadius: 6))
        }
        .buttonStyle(.plain)
        .disabled(refinementControlsDisabled)
        .help("Pause / resume refinement (Space)")
    }

    private var restartButton: some View {
        Button {
            viewModel.restartRefinement()
        } label: {
            Image(systemName: "arrow.clockwise")
                .font(.system(size: 12))
                .foregroundColor(Theme.textMuted)
                .frame(width: 26, height: 26)
        }
        .buttonStyle(.plain)
        .disabled(refinementControlsDisabled)
        .help("Restart refinement")
    }

    private var readout: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(statusRow1)
                    .font(Theme.mono(10.5))
                    .foregroundColor(Theme.textPrimary)
                Text(statusLabel)
                    .font(Theme.sans(9))
                    .tracking(0.6)
                    .foregroundColor(statusLabelColor)
            }
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(Theme.fillTrough)
                    .frame(width: 150, height: 3)
                RoundedRectangle(cornerRadius: 2)
                    .fill(Theme.spectralGradient)
                    .frame(width: max(0, 150 * progressFraction), height: 3)
            }
        }
        .padding(.trailing, 4)
    }

    private var integratorChip: some View {
        Group {
            if let integ = viewModel.resolvedIntegrator {
                Text("AUTO → \(integ.uppercased())")
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.success)
                    .help(viewModel.resolveReason ?? "")
            } else {
                Text("AUTO")
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.textDim)
            }
        }
        .padding(.horizontal, 4)
    }

    // MARK: - Right: Save

    private var saveButton: some View {
        Button {
            viewModel.saveScene()
        } label: {
            Text("Save")
                .font(Theme.sans(12, .semibold))
                .foregroundColor(Color.black.opacity(0.92))
                .padding(.horizontal, 15)
                .padding(.vertical, 6)
                .background(Theme.textPrimary, in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
        }
        .buttonStyle(.plain)
        .opacity(viewModel.sceneEditsDirty ? 1.0 : 0.4)
        .disabled(!viewModel.sceneEditsDirty)
    }

    // MARK: - Status text derivation

    private var isProduction: Bool { viewModel.renderState == .rendering }
    private var isCancelling: Bool { viewModel.renderState == .cancelling }

    /// One shared mapping with the viewport's refinement pill
    /// (RefinementStatusFormatter) so the two readouts can never
    /// disagree at a glance.
    private var status: RefinementStatusFormatter.Status {
        RefinementStatusFormatter.status(
            phase: viewModel.refinementPhase,
            scaleDivisor: viewModel.refinementScaleDivisor,
            isProduction: isProduction,
            isCancelling: isCancelling,
            productionProgress: viewModel.progress)
    }

    private var statusRow1: String { status.text }
    private var statusLabel: String { status.label }

    private var statusLabelColor: Color {
        if isCancelling { return Theme.warn }
        if isProduction { return Theme.success }
        switch viewModel.refinementPhase {
        case 4: return Theme.warn
        case -1: return Theme.textDim
        default: return Theme.success
        }
    }

    /// The "honest" fraction for the current instant — ignores the
    /// paused-freeze rule (see `progressFraction`).
    private var liveFraction: CGFloat { status.fraction }

    /// The fraction actually drawn: while paused (and not mid-production),
    /// freeze at the last live value instead of collapsing.
    private var progressFraction: CGFloat {
        (viewModel.refinementPhase == 4 && !isProduction) ? lastNonPausedFraction : liveFraction
    }

    private func syncLastFraction() {
        if !(viewModel.refinementPhase == 4 && !isProduction) {
            lastNonPausedFraction = liveFraction
        }
    }
}
