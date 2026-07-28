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
            renderTransportButton
            saveStateChip
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
            } else if viewModel.viewportBridge != nil {
                // Untitled scene (start-screen create path, spec §5.2):
                // loaded from the bundled starter template, not yet on
                // disk.  Gains a real name via Save-As.
                HStack(spacing: 8) {
                    Text("Untitled")
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
            restartButton
            readout
            // P2 fix: when there's no resolved integrator to show (not
            // using the auto-dispatcher, or no render yet), omit the
            // chip AND its divider entirely rather than rendering a
            // misleading static "AUTO" — the scene may not even use
            // the auto-dispatcher.
            if let integ = viewModel.resolvedIntegrator {
                Rectangle().fill(Theme.borderLight).frame(width: 1, height: 22)
                integratorChip(integ)
            }
        }
        .padding(.vertical, 5)
        .padding(.horizontal, 6)
        .background(Theme.bgWell, in: RoundedRectangle(cornerRadius: Theme.radiusLarge))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusLarge)
                .stroke(Theme.borderLight, lineWidth: 1)
        )
    }

    /// P1-1 fix: was `viewModel.viewportBridge == nil || renderState ==
    /// .rendering || .cancelling` — a hand-copy of
    /// `isProductionRenderRunning`'s case list that (like the five
    /// transport methods it gates) missed the "a chat-driven agent
    /// render is outstanding" case.  Single-sourced on
    /// `canUseSceneTransport` now, which the transport methods
    /// themselves guard on — so this predicate and the actual no-op
    /// behavior can never drift apart again.
    private var refinementControlsDisabled: Bool {
        !viewModel.canUseSceneTransport
    }

    // (The refinement pause/resume button was removed by user request —
    // interactive refinement restarts on every edit anyway, so pausing
    // it was a niche control.  Production pause lives on the transport
    // pill; the restart button below remains.)

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
                if !statusLabel.isEmpty {
                    Text(statusLabel)
                        .font(Theme.sans(9))
                        .tracking(0.6)
                        .foregroundColor(statusLabelColor)
                }
                // P1-5 fix: elapsed/remaining time was tracked on the
                // view model (formattedElapsedTime / formattedRemainingTime)
                // but never surfaced anywhere in the redesigned TopBar —
                // restore it as a compact caption alongside the status
                // text during a production render.
                if isProduction || isCancelling {
                    Text(elapsedRemainingText)
                        .font(Theme.mono(9.5))
                        .foregroundColor(Theme.textDim)
                }
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
        // P1-5 / P1-6: surface whichever of "why is this in an error
        // state" (highest priority) or "what stage is production on"
        // is meaningful right now — empty string is a no-op tooltip.
        .help(readoutHelp)
    }

    /// P1-5 fix: `formattedElapsedTime`, plus `· ~<remaining> remaining`
    /// when the ETA has warmed up (`formattedRemainingTime` is nil
    /// while it's still warming up or otherwise unavailable).
    private var elapsedRemainingText: String {
        guard let remaining = viewModel.formattedRemainingTime else {
            return viewModel.formattedElapsedTime
        }
        return "\(viewModel.formattedElapsedTime) · ~\(remaining) remaining"
    }

    private var readoutHelp: String {
        if let msg = errorMessage { return msg }
        if !viewModel.progressTitle.isEmpty { return viewModel.progressTitle }
        return ""
    }

    private func integratorChip(_ integ: String) -> some View {
        Text("AUTO → \(integ.uppercased())")
            .font(Theme.mono(10))
            .foregroundColor(Theme.success)
            .help(viewModel.resolveReason ?? "")
            .padding(.horizontal, 4)
    }

    // MARK: - Right: save-state indicator
    //
    // Explicit-save-only (user decision 2026-07-12): UI edits never
    // write the .RISEscene to disk automatically — the CST <-> editor
    // live mirror (RenderViewModel.pollRefinementState's item 1) keeps
    // the Scene-file tab following live edits, but a disk write happens
    // ONLY when the user explicitly saves.  So this chip is actionable
    // while dirty (an active Save button that calls `viewModel.saveScene()`
    // directly) and passive once clean ("saved").  There is no
    // auto-save-suspended state to surface here anymore — a save that's
    // refused or I/O-fails (e.g. the file changed on disk externally)
    // is reported through `saveScene()`'s own NSAlert at the moment the
    // user clicks Save, not as a standing chip state.

    // MARK: - Right: render transport (Render -> Pause -> Resume)

    /// One slot that morphs with the production render's lifecycle:
    /// idle -> "Render" (kicks a production render); rendering ->
    /// "Pause" (parks the workers); paused -> "Resume".  Cancelling
    /// shows a disabled label.  Any path that mutates or reloads the
    /// scene while paused cancels the render (those paths all cancel
    /// actives first) and this slot honestly falls back to "Render".
    @ViewBuilder
    private var renderTransportButton: some View {
        if isCancelling {
            Text("Cancelling…")
                .font(Theme.sans(12, .semibold))
                .foregroundColor(Theme.textDisabled)
                .padding(.horizontal, 15)
                .padding(.vertical, 6)
        } else if isProduction {
            HStack(spacing: 6) {
                if viewModel.isProductionRenderPaused {
                    transportPill("Resume", filled: true) {
                        viewModel.toggleProductionRenderPause()
                    }
                    .help("Resume the render where it left off")
                } else {
                    transportPill("Pause", filled: false) {
                        viewModel.toggleProductionRenderPause()
                    }
                    .help("Pause the render — workers park, CPU goes quiet")
                }
                cancelPill
            }
        } else {
            HStack(spacing: 4) {
                transportPill("Render", filled: true) {
                    viewModel.startRender()
                }
                .help("Start a full-frame production render (⌘R)")

                if viewModel.activeRegion != nil {
                    Menu {
                        Button("Render Active Region") {
                            viewModel.startActiveRegionRender()
                        }
                    } label: {
                        Image(systemName: "chevron.down")
                            .font(Theme.sans(10, .semibold))
                            .foregroundColor(Color.black.opacity(0.92))
                            .padding(.horizontal, 9)
                            .padding(.vertical, 7)
                            .background(Theme.accent,
                                        in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                    .disabled(!viewModel.canRenderActiveRegion)
                    .opacity(viewModel.canRenderActiveRegion ? 1.0 : 0.4)
                    .help(viewModel.canRenderActiveRegion
                          ? "Render only the active region"
                          : "The active production rasterizer does not support region rendering")
                }
            }
            .disabled(!viewModel.canStartProductionRender)
            .opacity(viewModel.canStartProductionRender ? 1.0 : 0.4)
        }
    }

    /// Cancel, shown beside Pause/Resume while a render is in flight —
    /// error-tinted outline so it reads as the destructive option.
    /// Works while paused too (the pause gate observes the cancel).
    private var cancelPill: some View {
        Button {
            viewModel.cancelRender()
        } label: {
            Text("Cancel")
                .font(Theme.sans(12, .semibold))
                .foregroundColor(Theme.error)
                .padding(.horizontal, 13)
                .padding(.vertical, 6)
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusMedium)
                        .stroke(Theme.error.opacity(0.5), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .help("Cancel the render (⌘.)")
    }

    private func transportPill(_ title: String, filled: Bool,
                               action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.sans(12, .semibold))
                .foregroundColor(filled ? Color.black.opacity(0.92) : Theme.warn)
                .padding(.horizontal, 15)
                .padding(.vertical, 6)
                .background(
                    filled ? Theme.accent : Color.clear,
                    in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusMedium)
                        .stroke(filled ? Color.clear : Theme.warn.opacity(0.5), lineWidth: 1))
        }
        .buttonStyle(.plain)
    }

    @ViewBuilder
    private var saveStateChip: some View {
        // Untitled scenes (loadedFilePath == nil but a bridge exists) get
        // the SAME save affordance — saveScene() routes to Save-As for
        // them (spec §5.2), so the pill never leads to a silent no-op.
        if viewModel.loadedFilePath != nil || viewModel.viewportBridge != nil {
            if viewModel.sceneEditsDirty {
                Button {
                    viewModel.saveScene()
                } label: {
                    Text("Save")
                        .font(Theme.sans(12, .semibold))
                        .foregroundColor(Theme.textPrimary)
                        .padding(.horizontal, 13)
                        .padding(.vertical, 6)
                        .background(
                            Theme.fillHover,
                            in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.radiusMedium)
                                .stroke(Theme.borderStrong, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .help("Write the scene file to disk (⌥⌘S) — edits are never auto-saved")
            } else if viewModel.loadedFilePath != nil {
                // Only a NAMED scene can honestly claim "saved" — an
                // untitled clean scene has never touched disk (review P2:
                // showing "✓ saved" there falsely promises persistence).
                // Untitled + clean = the pristine starter template; show
                // nothing until there is something to save.
                Text("✓ saved")
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.textDim)
            }
        }
    }

    // MARK: - Status text derivation

    private var isProduction: Bool { viewModel.renderState == .rendering }
    private var isCancelling: Bool { viewModel.renderState == .cancelling }

    /// P1-6 fix: `renderState == .error(msg)` used to fall through to
    /// RefinementStatusFormatter, which has no `.error` case in its
    /// (phase, isProduction, isCancelling) input space — it silently
    /// rendered as "Settled" (phase 0's default), hiding a real
    /// failure behind the same look as "everything's fine."  Checked
    /// BEFORE calling into the shared formatter so an error always
    /// wins regardless of whatever refinement phase happens to be
    /// cached from before the failure.
    ///
    /// One shared mapping with the viewport's refinement pill
    /// (RefinementStatusFormatter) so the two readouts can never
    /// disagree at a glance — this is why the error branch is handled
    /// HERE rather than by adding an `.error` case to the formatter's
    /// own state space (TopBar is the only surface that needs it: the
    /// bottom-left refinement pill only shows during interactive
    /// editing, which is unreachable once `renderState` is `.error`).
    private var status: RefinementStatusFormatter.Status {
        if errorMessage != nil {
            return RefinementStatusFormatter.Status(text: "Error", label: "ERROR", fraction: 0)
        }
        return RefinementStatusFormatter.status(
            phase: viewModel.refinementPhase,
            scaleDivisor: viewModel.refinementScaleDivisor,
            isProduction: isProduction,
            isCancelling: isCancelling,
            productionProgress: viewModel.progress,
            isProductionPaused: viewModel.isProductionRenderPaused,
            viewportRenderModeWantsDenoise: viewModel.viewportRenderModeWantsDenoise)
    }

    /// The failure message when `renderState == .error(msg)`, else nil.
    private var errorMessage: String? {
        if case .error(let msg) = viewModel.renderState { return msg }
        return nil
    }

    private var statusRow1: String { status.text }
    private var statusLabel: String {
        guard viewModel.isRegionProductionRender else { return status.label }
        if errorMessage != nil { return "REGION ERROR" }
        if viewModel.renderState == .cancelled || isCancelling { return "REGION CANCELLED" }
        return "REGION FINAL"
    }

    private var statusLabelColor: Color {
        if errorMessage != nil { return Theme.error }
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
