//////////////////////////////////////////////////////////////////////
//
//  StartView.swift - The no-scene launch screen (docs/gui/START_SCREEN.md).
//
//    Replaces the old "Open a .RISEscene file to begin" placeholder with
//    the three real entry intents, on screen simultaneously:
//      1. Resume  — a one-click list of the last 10 scenes (with missing-
//                   file states and per-row remove).
//      2. Locate  — the native file picker (also ⌘O), plus drag-and-drop
//                   of a .RISEscene anywhere on this view.
//      3. Create  — a prompt the agent builds a scene from: loads the
//                   bundled EMPTY starter template as an untitled scene,
//                   switches to the Agent tab, and auto-sends the prompt
//                   (ratified decisions #2/#3: truly-empty starter,
//                   auto-send).
//
//    Grouped 2-column layout (decision #1): the two "open existing"
//    paths share the left column; the agent path owns the right column
//    (it is the one that grows — presets land there later, P3).
//
//    The Create column mirrors the chat panel's real provider readiness
//    (spec §6) so path 3 never dead-ends: unconfigured shows a
//    "connect the agent" state whose Set up… button opens the SAME
//    ChatSettingsView the chat panel uses.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import UniformTypeIdentifiers

struct StartView: View {
    @EnvironmentObject var viewModel: RenderViewModel

    /// The Create prompt draft.  Start-screen-local: it is handed to the
    /// chat (via loadStarterScene) only on Create.
    @State private var prompt: String = ""
    /// Guards against a double-activation of Create while the starter
    /// scene load + handoff is in flight (spec §8).
    @State private var createInFlight = false
    /// Popover for the unconfigured state's Set up… button — the same
    /// ChatSettingsView the chat panel's gear opens.
    @State private var showAgentSettings = false
    /// Popover for the always-visible header gear (separate state from
    /// `showAgentSettings` so the two presenters can't fight when both
    /// affordances are on screen in the unconfigured state).
    @State private var showHeaderSettings = false
    /// Focus for "click Create with an empty prompt focuses the box".
    @FocusState private var promptFocused: Bool
    /// Hovered recent-row path — drives the clickable-affordance highlight
    /// (user feedback 2026-07-16: rows didn't read as clickable).
    @State private var hoveredRecentPath: String? = nil

    var body: some View {
        GeometryReader { geo in
            ScrollView {
                VStack(spacing: 0) {
                    if case .error(let message) = viewModel.renderState {
                        errorBanner(message)
                            .padding(.bottom, 18)
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Text("Start a session")
                            .font(Theme.sans(20, .medium))
                            .foregroundColor(Theme.textPrimary)
                        Text("Open a scene you've worked on, browse for a file, or describe one for the agent to build.")
                            .font(Theme.sans(12.5))
                            .foregroundColor(Theme.textSecondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.bottom, 22)

                    HStack(alignment: .top, spacing: 22) {
                        openColumn
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                        createColumn
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                    }
                }
                .frame(maxWidth: 860)
                .padding(32)
                .frame(minHeight: geo.size.height)
                .frame(maxWidth: .infinity)
            }
        }
        .background(Theme.bgCenter)
        // A create whose starter-scene load fails ASYNCHRONOUSLY lands back
        // here in the .error state — re-enable Create (the sync
        // template-missing failure resets in createScene() itself).
        .onChange(of: viewModel.renderState) { _, state in
            if case .error = state { createInFlight = false }
        }
        // Drag-and-drop of a .RISEscene anywhere on the start screen
        // opens it (spec §4.1) — the drop target is registered on this
        // view only, so a loaded scene's viewport is unaffected.
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                guard let url, url.pathExtension.lowercased() == "risescene" else { return }
                Task { @MainActor in
                    viewModel.loadScene(at: url.path)
                }
            }
            return true
        }
    }

    // MARK: - Left column: open an existing scene

    private var openColumn: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Recent scenes")
                .font(Theme.sans(11, .medium))
                .foregroundColor(Theme.textDim)

            if viewModel.recentFiles.isEmpty {
                Text("Scenes you open appear here.")
                    .font(Theme.sans(12))
                    .foregroundColor(Theme.textMuted)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.vertical, 26)
                    .overlay(
                        RoundedRectangle(cornerRadius: 8)
                            .stroke(Theme.borderHairline, lineWidth: 1)
                    )
            } else {
                VStack(spacing: 0) {
                    ForEach(viewModel.recentFiles, id: \.self) { path in
                        recentRow(path)
                        if path != viewModel.recentFiles.last {
                            Rectangle().fill(Theme.borderHairline).frame(height: 1)
                        }
                    }
                }
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(Theme.borderHairline, lineWidth: 1)
                )
            }

            Button {
                viewModel.openScene()
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: "folder")
                        .font(.system(size: 12))
                    Text("Browse files…")
                        .font(Theme.sans(12.5, .medium))
                }
                .foregroundColor(Theme.textPrimary)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
                .overlay(
                    RoundedRectangle(cornerRadius: 7)
                        .stroke(Theme.borderLight, lineWidth: 1)
                )
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            Text("or drop a .RISEscene here")
                .font(Theme.sans(10.5))
                .foregroundColor(Theme.textMuted)
                .frame(maxWidth: .infinity, alignment: .center)
        }
    }

    @ViewBuilder
    private func recentRow(_ path: String) -> some View {
        // Existence checked per render of the screen — cheap at ≤10 rows,
        // and it means an entry deleted/moved outside RISE shows honestly
        // as "file not found" instead of failing on click (spec §4.1).
        let exists = FileManager.default.fileExists(atPath: path)
        let name = ((path as NSString).lastPathComponent as NSString).deletingPathExtension
        let folder = (path as NSString).deletingLastPathComponent

        // Clickable-affordance styling (user feedback 2026-07-16): hover
        // highlight + accent name + trailing chevron + pointing-hand cursor,
        // so the rows read as buttons rather than a static list.
        let hovered = hoveredRecentPath == path && exists

        HStack(spacing: 10) {
            Image(systemName: exists ? "photo" : "photo.slash" /* missing */)
                .font(.system(size: 13))
                .foregroundColor(exists ? Theme.accentLight : Theme.textMuted)
                .frame(width: 18)

            VStack(alignment: .leading, spacing: 1) {
                Text(name)
                    .font(Theme.sans(12.5, .medium))
                    .foregroundColor(exists
                        ? (hovered ? Theme.accentLight : Theme.textPrimary)
                        : Theme.textSecondary)
                    .lineLimit(1)
                if exists {
                    Text((folder as NSString).abbreviatingWithTildeInPath)
                        .font(Theme.sans(10.5))
                        .foregroundColor(Theme.textMuted)
                        .lineLimit(1)
                        .truncationMode(.head)
                } else {
                    Text("file not found")
                        .font(Theme.sans(10.5))
                        .foregroundColor(Theme.error)
                }
            }
            Spacer(minLength: 8)

            if exists {
                Text(relativeTime(for: path))
                    .font(Theme.sans(10.5))
                    .foregroundColor(Theme.textMuted)
                    .lineLimit(1)
                Image(systemName: "chevron.right")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundColor(hovered ? Theme.accentLight : Theme.textMuted)
            } else {
                Button {
                    viewModel.removeRecentFile(path)
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(Theme.textMuted)
                }
                .buttonStyle(.plain)
                .help("Remove from recent scenes")
            }
        }
        .padding(.horizontal, 11)
        .padding(.vertical, 8)
        .background(hovered ? Theme.fillHover : Color.clear)
        .contentShape(Rectangle())
        .opacity(exists ? 1.0 : 0.6)
        .onHover { inside in
            // Stateless NSCursor.set() (review P1): push()/pop() leaked a
            // pushed pointing-hand when clicking a row — loadScene swaps
            // StartView out SYNCHRONOUSLY, the exit-hover never fires, and
            // the orphaned push left the hand cursor stuck over the
            // workspace.  set() has no stack to unbalance.
            if inside {
                hoveredRecentPath = path
                if exists { NSCursor.pointingHand.set() }
            } else {
                if hoveredRecentPath == path { hoveredRecentPath = nil }
                if exists { NSCursor.arrow.set() }
            }
        }
        .help(exists ? "Open \(name)" : "")
        .onTapGesture {
            // A missing row does nothing on click (no error dialog) —
            // its ✕ is the affordance.
            guard exists else { return }
            // The click unmounts this view synchronously — restore the
            // cursor NOW (the exit-hover event will never arrive).
            NSCursor.arrow.set()
            viewModel.loadScene(at: path)
        }
    }

    /// Shared formatter — hoisted so it isn't re-allocated per row per
    /// render (review P3).
    private static let relativeFormatter: RelativeDateTimeFormatter = {
        let f = RelativeDateTimeFormatter()
        f.unitsStyle = .abbreviated
        return f
    }()

    private func relativeTime(for path: String) -> String {
        guard let date = viewModel.recentFileTimes[path] else { return "" }
        return Self.relativeFormatter.localizedString(for: date, relativeTo: Date())
    }

    // MARK: - Right column: create with the agent

    private var createColumn: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Text("Create with the agent")
                    .font(Theme.sans(11, .medium))
                    .foregroundColor(Theme.textDim)
                Spacer()
                if viewModel.chat.agentConfigured {
                    Text(viewModel.chat.provider.displayName)
                        .font(Theme.sans(10.5))
                        .foregroundColor(Theme.accentLight)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 2)
                        .background(Capsule().fill(Theme.accent.opacity(0.12)))
                }
                // Always-visible settings affordance (user feedback
                // 2026-07-16): switch models / add keys from the start
                // screen even when already configured — same
                // ChatSettingsView the chat panel's gear opens.
                Button {
                    showHeaderSettings = true
                } label: {
                    Image(systemName: "gearshape")
                        .font(.system(size: 11))
                        .foregroundColor(Theme.textDim)
                }
                .buttonStyle(.plain)
                .help("Provider, model, and API-key settings")
                .popover(isPresented: $showHeaderSettings, arrowEdge: .trailing) {
                    ChatSettingsView(chat: viewModel.chat)
                }
            }

            ZStack(alignment: .topLeading) {
                TextEditor(text: $prompt)
                    .font(Theme.sans(12.5))
                    .foregroundColor(Theme.textPrimary)
                    .scrollContentBackground(.hidden)
                    .padding(6)
                    .frame(height: 96)
                    .background(Theme.bgWell)
                    .clipShape(RoundedRectangle(cornerRadius: 7))
                    .overlay(
                        RoundedRectangle(cornerRadius: 7)
                            .stroke(Theme.borderHairline, lineWidth: 1)
                    )
                    .focused($promptFocused)
                    .disabled(!viewModel.chat.agentConfigured)
                    .opacity(viewModel.chat.agentConfigured ? 1.0 : 0.5)
                if prompt.isEmpty {
                    Text("A brushed-gold watch dial on black velvet, dramatic side light, shallow depth of field.")
                        .font(Theme.sans(12.5))
                        .foregroundColor(Theme.textMuted)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 8)
                        .allowsHitTesting(false)
                }
            }

            if viewModel.chat.agentConfigured {
                Button {
                    createScene()
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: "sparkles")
                            .font(.system(size: 12))
                        Text(createInFlight ? "Creating…" : "Create scene")
                            .font(Theme.sans(12.5, .medium))
                    }
                    .foregroundColor(Theme.accentLight)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
                    .overlay(
                        RoundedRectangle(cornerRadius: 7)
                            .stroke(Theme.accent.opacity(0.55), lineWidth: 1)
                    )
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(createInFlight)

                Text("Loads a blank stage, then the agent builds your scene from the description.")
                    .font(Theme.sans(10.5))
                    .foregroundColor(Theme.textMuted)
            } else {
                // Unconfigured state (spec §6): path 3 must never dead-end.
                VStack(alignment: .leading, spacing: 8) {
                    Text("Connect an agent to create scenes from a prompt.")
                        .font(Theme.sans(11.5))
                        .foregroundColor(Theme.textSecondary)
                    Button {
                        showAgentSettings = true
                    } label: {
                        Text("Set up…")
                            .font(Theme.sans(12, .medium))
                            .foregroundColor(Theme.textPrimary)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 6)
                            .overlay(
                                RoundedRectangle(cornerRadius: 7)
                                    .stroke(Theme.borderLight, lineWidth: 1)
                            )
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .popover(isPresented: $showAgentSettings, arrowEdge: .trailing) {
                        ChatSettingsView(chat: viewModel.chat)
                    }
                }
                .padding(.top, 2)
            }
        }
    }

    private func createScene() {
        let text = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else {
            // Enabled-with-feedback (spec §4.3): an empty prompt focuses
            // the box rather than silently ignoring the click.
            promptFocused = true
            return
        }
        createInFlight = true
        // loadStarterScene stashes the prompt for the chat's sceneOpened
        // to consume exactly once; the start screen unmounts when the
        // scene finishes loading (viewportBridge becomes non-nil), so
        // createInFlight needs no reset on success.  On a template-missing
        // failure the view stays up — re-enable Create.
        if !viewModel.loadStarterScene(withPrompt: text) {
            createInFlight = false
        }
    }

    // MARK: - Error banner (a failed load lands back here, spec §8)

    private func errorBanner(_ message: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle")
                .font(.system(size: 12))
                .foregroundColor(Theme.error)
            Text(message)
                .font(Theme.sans(12))
                .foregroundColor(Theme.textSecondary)
                .lineLimit(2)
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Theme.error.opacity(0.08))
        .clipShape(RoundedRectangle(cornerRadius: 7))
        .overlay(
            RoundedRectangle(cornerRadius: 7)
                .stroke(Theme.error.opacity(0.35), lineWidth: 1)
        )
    }
}
