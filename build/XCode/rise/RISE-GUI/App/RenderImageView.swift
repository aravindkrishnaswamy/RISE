import SwiftUI

struct RenderImageView: View {
    @EnvironmentObject var viewModel: RenderViewModel

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                Color(nsColor: .controlBackgroundColor)

                if let image = viewModel.renderedImage {
                    Image(nsImage: image)
                        .interpolation(.none)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(maxWidth: geometry.size.width,
                               maxHeight: geometry.size.height)
                } else {
                    VStack(spacing: 16) {
                        Image(systemName: "photo")
                            .font(.system(size: 48))
                            .foregroundColor(.secondary)
                        Text(placeholderText)
                            .foregroundColor(.secondary)
                    }
                }

                if viewModel.renderState == .loading {
                    VStack(spacing: 12) {
                        ProgressView()
                            .controlSize(.large)
                        Text("Loading scene...")
                            .font(.headline)
                        if viewModel.progress > 0 {
                            ProgressView(value: viewModel.progress)
                                .progressViewStyle(.linear)
                                .frame(width: 200)
                            Text(String(format: "%.0f%%", viewModel.progress * 100))
                                .font(.caption)
                                .monospacedDigit()
                        }
                    }
                    .padding(24)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                }
            }
        }
    }

    private var placeholderText: String {
        switch viewModel.renderState {
        case .idle:
            return "Open a .RISEscene file to begin"
        case .sceneLoaded:
            return "Press Render to start"
        default:
            return ""
        }
    }
}
