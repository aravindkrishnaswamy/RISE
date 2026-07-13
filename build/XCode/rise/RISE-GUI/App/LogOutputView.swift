//////////////////////////////////////////////////////////////////////
//
//  LogOutputView.swift - The log drawer's full content: header
//    (title, severity filter pills, WARN/ERR counts, text filter,
//    follow toggle, clear, collapse chevron) + the filtered scrolling
//    row list.  ContentView.logDrawer wraps this in the drawer's
//    outer frame/border/corner-radius and supplies `onCollapse`;
//    ContentView.collapsedLogStrip is the separate 28pt collapsed
//    state shown when `viewModel.showLogDrawer` is false.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI

/// Severity filter for the log drawer's pill group.  `.info` folds in
/// `.event` (RISE's "narration" log level) since the design comp's
/// pill set is All/Info/Warn/Error, not a 5-way split.
private enum LogSeverityFilter: String, CaseIterable, Identifiable {
    case all = "All"
    case info = "Info"
    case warn = "Warn"
    case error = "Error"

    var id: String { rawValue }

    func matches(_ level: RISELogLevel) -> Bool {
        switch self {
        case .all: return true
        case .info: return level == .info || level == .event
        case .warn: return level == .warning
        case .error: return level == .error || level == .fatal
        }
    }
}

struct LogOutputView: View {
    @EnvironmentObject var viewModel: RenderViewModel

    /// Fires when the user clicks the collapse chevron.  ContentView
    /// wires this to `viewModel.showLogDrawer = false`.
    var onCollapse: () -> Void = {}

    @State private var severityFilter: LogSeverityFilter = .all
    @State private var filterText: String = ""
    /// Auto-scroll-to-newest.  Defaults on, matching the pre-redesign
    /// drawer's unconditional scroll-to-bottom behavior.
    @State private var follow: Bool = true

    var body: some View {
        VStack(spacing: 0) {
            header
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
            logRows
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 10) {
            Text("Log")
                .font(Theme.sans(11, .semibold))
                .foregroundColor(Theme.textPrimary)

            severityPills

            countChip(text: "WARN \(warnCount)", active: warnCount > 0, color: Theme.warn)
            countChip(text: "ERR \(errorCount)", active: errorCount > 0, color: Theme.error)

            filterField

            Spacer(minLength: 4)

            followToggle

            Button {
                viewModel.clearLog()
            } label: {
                Text("clear")
                    .font(Theme.mono(10))
                    .foregroundColor(Theme.textDim)
            }
            .buttonStyle(.plain)
            .disabled(viewModel.logMessages.isEmpty)
            .help("Clear log")

            Button(action: onCollapse) {
                Image(systemName: "chevron.down")
                    .font(.system(size: 11))
                    .foregroundColor(Theme.textDim)
            }
            .buttonStyle(.plain)
            .help("Collapse log (⌥L)")
        }
        .padding(.horizontal, 10)
        .frame(height: 32)
        .background(Theme.bgHeader)
    }

    private var severityPills: some View {
        HStack(spacing: 2) {
            ForEach(LogSeverityFilter.allCases) { f in
                Text(f.rawValue)
                    .font(Theme.mono(10))
                    .foregroundColor(severityFilter == f ? Theme.textPrimary : Theme.textFaint)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(
                        severityFilter == f ? Theme.fillActive : Color.clear,
                        in: RoundedRectangle(cornerRadius: 4)
                    )
                    .contentShape(Rectangle())
                    .onTapGesture { severityFilter = f }
            }
        }
        .padding(2)
        .background(Theme.bgWell, in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.borderHairline, lineWidth: 1))
    }

    private func countChip(text: String, active: Bool, color: Color) -> some View {
        Text(text)
            .font(Theme.mono(9))
            .foregroundColor(active ? color : Theme.textDim)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(active ? color.opacity(0.10) : Color.clear, in: RoundedRectangle(cornerRadius: 4))
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(active ? color.opacity(0.35) : Theme.borderLight, lineWidth: 1)
            )
    }

    private var filterField: some View {
        TextField("⌕ filter…", text: $filterText)
            .textFieldStyle(.plain)
            .font(Theme.mono(10.5))
            .foregroundColor(Theme.textSecondary)
            .padding(.horizontal, 9)
            .padding(.vertical, 4)
            .background(Theme.bgWell, in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.borderHairline, lineWidth: 1))
            .frame(maxWidth: 200)
    }

    private var followToggle: some View {
        Text("follow \(follow ? "●" : "○")")
            .font(Theme.mono(10))
            .foregroundColor(follow ? Theme.accent : Theme.textDim)
            .contentShape(Rectangle())
            .onTapGesture { follow.toggle() }
            .help("Auto-scroll to newest")
    }

    // MARK: - Body

    private var logRows: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(filteredMessages) { entry in
                        row(entry).id(entry.id)
                    }
                }
            }
            .background(Theme.bgWell)
            .onChange(of: viewModel.logMessages.count) { _, _ in
                guard follow, let last = filteredMessages.last else { return }
                proxy.scrollTo(last.id, anchor: .bottom)
            }
        }
    }

    private func row(_ entry: LogMessage) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 7) {
            Text(Self.timeFormatter.string(from: entry.timestamp))
                .foregroundColor(Theme.textGhost)
            Text(streamGlyph(entry.level))
                .foregroundColor(colorForLevel(entry.level))
            Text(entry.text)
                .foregroundColor(colorForLevel(entry.level))
                .textSelection(.enabled)
        }
        .font(Theme.mono(10.5))
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 12)
        .padding(.vertical, 2)
        .background(entry.level == .warning ? Theme.warn.opacity(0.07) : Color.clear)
    }

    private func streamGlyph(_ level: RISELogLevel) -> String {
        switch level {
        case .event:   return "»"
        case .info:    return "·"
        case .warning: return "!"
        case .error, .fatal: return "✕"
        @unknown default: return "·"
        }
    }

    // MARK: - Derived

    private var filteredMessages: [LogMessage] {
        viewModel.logMessages.filter { entry in
            guard severityFilter.matches(entry.level) else { return false }
            guard !filterText.isEmpty else { return true }
            return entry.text.localizedCaseInsensitiveContains(filterText)
        }
    }

    private var warnCount: Int {
        viewModel.logMessages.filter { $0.level == .warning }.count
    }
    private var errorCount: Int {
        viewModel.logMessages.filter { $0.level == .error || $0.level == .fatal }.count
    }

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    private func colorForLevel(_ level: RISELogLevel) -> Color {
        switch level {
        case .event:
            return Theme.textSecondary
        case .info:
            return Theme.textMuted
        case .warning:
            return Theme.warn
        case .error, .fatal:
            return Theme.error
        @unknown default:
            return Theme.textMuted
        }
    }
}
