import SwiftUI

// RISE UI redesign — left panel, Agent tab, "DIFF CARD".
//
// Renders one `ChatViewModel.ProposalEntry` as the bordered diff card
// from the approved design comp: a header (title + scene file), a
// diff-styled body keyed by proposal `kind`, and a footer that mirrors
// the proposal's `status`.  All actions wire to the EXISTING plumbing —
// `ChatViewModel.resolveProposal(id:approve:)` and the shared
// `RenderViewModel.undo()` — no new RPC surface.
//
// HONESTY NOTE: the comp shows an optional red "− <param>: <current>"
// line above the green "+" line for a param edit, sourced from the
// param's value BEFORE the edit.  `list_proposals` does not echo a
// pre-edit value (AgentRpc.h's wire shape has no such field), so that
// data is not cheaply available here — per the slice brief, the minus
// line is simply omitted for `param_edit` rather than faked.
struct ProposalDiffCard: View {
    @EnvironmentObject var viewModel: RenderViewModel
    @ObservedObject var chat: ChatViewModel
    let proposal: ChatViewModel.ProposalEntry

    /// The scene file the proposal targets — read from the SAME
    /// `loadedFilePath` TopBar/ContentView already use for the window
    /// title, so this is never a fabricated label.
    private var sceneFileName: String {
        guard let path = viewModel.loadedFilePath else { return "" }
        return (path as NSString).lastPathComponent
    }

    /// Non-empty chunk-text lines, for `insert_chunk` bodies and the
    /// "+N" header chip — real counts, not a guess.
    private var chunkLines: [String] {
        proposal.chunkText.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
    }

    private static let maxChunkLinesShown = 12

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
            diffBody
                .padding(.vertical, 6)
            footer
        }
        .background(Theme.bgCardDeep)
        .clipShape(RoundedRectangle(cornerRadius: Theme.radiusCard))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusCard)
                .stroke(Theme.accent.opacity(0.32), lineWidth: 1)
        )
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 8) {
            Text("Proposed edit")
                .font(Theme.sans(11.5, .semibold))
                .foregroundColor(Theme.textPrimary)
            if !sceneFileName.isEmpty {
                Text(sceneFileName)
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.textDim)
            }
            Spacer(minLength: 6)
            HStack(spacing: 5) {
                if minusCount > 0 {
                    countChip("−\(minusCount)", color: Theme.error, bg: Theme.errorStrong.opacity(0.1))
                }
                if plusCount > 0 {
                    countChip("+\(plusCount)", color: Theme.successLight, bg: Theme.success.opacity(0.12))
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
    }

    private var plusCount: Int {
        switch proposal.kind {
        case "insert_chunk": return chunkLines.filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }.count
        case "remove_chunk": return 0
        default: return 1
        }
    }

    private var minusCount: Int {
        proposal.kind == "remove_chunk" ? 1 : 0
    }

    private func countChip(_ text: String, color: Color, bg: Color) -> some View {
        Text(text)
            .font(Theme.mono(9))
            .foregroundColor(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(bg, in: RoundedRectangle(cornerRadius: 4))
    }

    // MARK: - Body (diff-styled, keyed by kind)

    @ViewBuilder
    private var diffBody: some View {
        switch proposal.kind {
        case "insert_chunk":
            insertChunkBody
        case "remove_chunk":
            VStack(alignment: .leading, spacing: 2) {
                diffLine(sign: "−",
                         text: proposal.entityKind.isEmpty
                             ? proposal.target
                             : "\(proposal.target) (\(proposal.entityKind))",
                         isAddition: false)
            }
        default:
            paramEditBody
        }
    }

    private var paramEditBody: some View {
        VStack(alignment: .leading, spacing: 2) {
            blockLabelRow("param edit · \(proposal.target)")
            diffLine(sign: "+", text: "\(proposal.param): \(proposal.value)", isAddition: true)
        }
    }

    private var insertChunkBody: some View {
        let lines = chunkLines
        let label = proposal.target.isEmpty
            ? (lines.first(where: { !$0.trimmingCharacters(in: .whitespaces).isEmpty }) ?? "block")
                .trimmingCharacters(in: .whitespaces)
            : proposal.target
        let shown = lines.prefix(Self.maxChunkLinesShown)
        let remaining = lines.count - shown.count
        return VStack(alignment: .leading, spacing: 2) {
            blockLabelRow("new block · \(label)")
            ForEach(Array(shown.enumerated()), id: \.offset) { _, line in
                diffLine(sign: "+", text: line, isAddition: true)
            }
            if remaining > 0 {
                Text("… \(remaining) more")
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.textDim)
                    .padding(.horizontal, 12)
                    .padding(.top, 2)
            }
        }
    }

    private func blockLabelRow(_ text: String) -> some View {
        HStack(spacing: 7) {
            Circle().fill(Theme.accentSoft).frame(width: 5, height: 5)
            Text(text)
                .font(Theme.mono(10))
                .foregroundColor(Theme.accentSoft)
        }
        .padding(.horizontal, 12)
        .padding(.top, 2)
        .padding(.bottom, 2)
    }

    private func diffLine(sign: String, text: String, isAddition: Bool) -> some View {
        HStack(spacing: 0) {
            Text("\(sign) \(text)")
                .font(Theme.mono(11))
                .foregroundColor(isAddition ? Theme.successLight : Theme.error)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 1.5)
        .background(isAddition ? Theme.success.opacity(0.1) : Theme.errorStrong.opacity(0.1))
    }

    // MARK: - Footer (keyed by status)

    @ViewBuilder
    private var footer: some View {
        Rectangle().fill(Theme.borderHairline).frame(height: 1)
        switch proposal.status {
        case "pending":
            pendingFooter
        case "applied":
            resolvedFooter(
                icon: "✓", text: "Applied · one undo step", color: Theme.successLight,
                trailing: AnyView(undoButton))
        case "rejected":
            // No "restore proposal" affordance: the controller does not
            // expose re-staging a rejected proposal, so — per the "no
            // fake affordances" rule — this state is a plain label only.
            resolvedFooter(
                icon: "✕", text: "Rejected — scene unchanged", color: Theme.textFaint,
                trailing: nil)
        case "conflict":
            resolvedFooter(
                icon: "⚠", text: "Conflict — scene changed since this was proposed",
                color: Theme.warn, trailing: nil)
        default:
            EmptyView()
        }
    }

    /// P1-2 fix: same gate `ChatViewModel.resolveProposal` itself
    /// enforces (`sceneEditable()`, backed by
    /// `RenderViewModel.isSceneEditableForAgents`) — disabling the
    /// buttons here is the honest-UI half of that fix; the view-model
    /// guard is the one that actually matters for correctness.
    private var applyRejectDisabled: Bool {
        !viewModel.isSceneEditableForAgents
    }

    private var pendingFooter: some View {
        HStack(spacing: 8) {
            Button {
                chat.resolveProposal(id: proposal.id, approve: true)
            } label: {
                Text("Apply — restarts refinement")
                    .font(Theme.sans(12, .semibold))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.plain)
            .background(Theme.accent, in: RoundedRectangle(cornerRadius: 7))
            .foregroundColor(Color(hex: 0x0d1116))
            .disabled(applyRejectDisabled)
            .opacity(applyRejectDisabled ? 0.4 : 1.0)
            .help(applyRejectDisabled ? "Unavailable while a render is in flight" : "")

            Button {
                chat.resolveProposal(id: proposal.id, approve: false)
            } label: {
                Text("Reject")
                    .font(Theme.sans(12))
                    .padding(.horizontal, 15)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.textTertiary)
            .overlay(
                RoundedRectangle(cornerRadius: 7).stroke(Theme.borderStrong, lineWidth: 1)
            )
            .disabled(applyRejectDisabled)
            .opacity(applyRejectDisabled ? 0.4 : 1.0)
            .help(applyRejectDisabled ? "Unavailable while a render is in flight" : "")
        }
        .padding(10)
        .background(Theme.bgCardDeep)
    }

    private func resolvedFooter(icon: String, text: String, color: Color, trailing: AnyView?) -> some View {
        HStack(spacing: 8) {
            Text("\(icon) \(text)")
                .font(Theme.sans(11.5))
                .foregroundColor(color)
            Spacer(minLength: 6)
            if let trailing { trailing }
        }
        .padding(10)
        .background(Theme.bgCardDeep)
    }

    /// Joins the shared undo history (RenderViewModel.undo() — the SAME
    /// Edit-menu undo TopBar drives), matching the comp's "one undo
    /// step" note: applying a proposal is a single CST commit.
    private var undoButton: some View {
        Button {
            viewModel.undo()
        } label: {
            Text("↺ Undo")
                .font(Theme.sans(11))
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
        }
        .buttonStyle(.plain)
        .foregroundColor(Theme.textFaint)
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.borderStrong, lineWidth: 1))
    }
}
