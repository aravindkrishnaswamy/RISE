import SwiftUI

struct LogOutputView: View {
    @EnvironmentObject var viewModel: RenderViewModel

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Log Output")
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                Spacer()
                Button {
                    viewModel.clearLog()
                } label: {
                    Image(systemName: "trash")
                        .font(.caption)
                }
                .buttonStyle(.borderless)
                .disabled(viewModel.logMessages.isEmpty)
                .help("Clear log")
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(viewModel.logMessages) { entry in
                            Text(entry.text)
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundColor(colorForLevel(entry.level))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 1)
                                .id(entry.id)
                        }
                    }
                }
                .background(Color(nsColor: .textBackgroundColor))
                .onChange(of: viewModel.logMessages.count) { _, _ in
                    if let last = viewModel.logMessages.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
        }
    }

    private func colorForLevel(_ level: RISELogLevel) -> Color {
        switch level {
        case .event:
            return .primary
        case .info:
            return .secondary
        case .warning:
            return .orange
        case .error, .fatal:
            return .red
        @unknown default:
            return .primary
        }
    }
}
