//
//  NamedViewsPanel.swift
//
//  B1 Named Views (Tier 2 §3): a right-panel section listing session/UI view
//  bookmarks.  Capture the current view, click a row to restore it into the
//  transient free-fly pose (non-destructive — no scene write), and per-row
//  Update / Promote-to-camera / Delete.  All state lives in the shared C++
//  NamedViewStore via the bridge; this view only lists + routes actions.
//

import SwiftUI

struct NamedViewsPanel: View {
    let bridge: RISEViewportBridge
    @Binding var refreshTrigger: Int
    @EnvironmentObject var viewModel: RenderViewModel

    @State private var names: [String] = []
    @State private var expanded: Bool = true
    @State private var captureText: String = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            if expanded {
                content
                    .padding(.horizontal, 14)
                    .padding(.bottom, 12)
            }
        }
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
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
                Text("Named Views")
                    .font(Theme.sans(12.5, .semibold))
                    .foregroundColor(Theme.textPrimary)
                Spacer(minLength: 0)
                Text(names.isEmpty ? "" : "\(names.count)")
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

    // MARK: - Content

    private var content: some View {
        VStack(alignment: .leading, spacing: 6) {
            captureRow
            if names.isEmpty {
                Text("No saved views. Frame a shot, then Capture.")
                    .font(Theme.sans(11))
                    .foregroundColor(Theme.textDim)
                    .padding(.vertical, 2)
            } else {
                ForEach(Array(names.enumerated()), id: \.offset) { idx, name in
                    viewRow(idx: idx, name: name)
                }
            }
        }
    }

    private var captureRow: some View {
        HStack(spacing: 6) {
            TextField("View name", text: $captureText)
                .textFieldStyle(.roundedBorder)
                .font(Theme.sans(11))
                .frame(maxWidth: .infinity)
            Button {
                let proposed = captureText.trimmingCharacters(in: .whitespaces)
                let name = proposed.isEmpty ? "View \(names.count + 1)" : proposed
                if bridge.captureNamedView(name) {
                    captureText = ""
                    reload()
                }
            } label: {
                Text("Capture").font(Theme.sans(11, .semibold))
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.small)
        }
    }

    private func viewRow(idx: Int, name: String) -> some View {
        HStack(spacing: 6) {
            // Restore on the label tap (the primary, non-destructive action).
            Button {
                _ = bridge.restoreNamedView(idx)
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: "eye")
                        .font(.system(size: 10))
                        .foregroundColor(Theme.textDim)
                    Text(name)
                        .font(Theme.sans(11.5))
                        .foregroundColor(Theme.textPrimary)
                        .lineLimit(1)
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("Restore this view (non-destructive)")

            iconButton("arrow.clockwise", help: "Update to current view") {
                _ = bridge.updateNamedView(idx); reload()
            }
            iconButton("camera.badge.plus", help: "Promote to a scene camera") {
                _ = bridge.promoteNamedView(idx, name: name)
                // The new active camera surfaces in the outliner via its poll.
                viewModel.entityListEpoch &+= 1
            }
            iconButton("trash", help: "Delete this view") {
                _ = bridge.deleteNamedView(idx); reload()
            }
        }
        .padding(.vertical, 2)
    }

    private func iconButton(_ system: String, help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: system)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(Theme.textDim)
                .frame(width: 20, height: 18)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
    }

    // MARK: - State pull

    private func reload() {
        names = bridge.namedViewNames
    }
}
