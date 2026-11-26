import SwiftUI

struct RenderImageView: View {
    @EnvironmentObject var viewModel: RenderViewModel

    /// L5a — pick between the legacy LDR `Image(nsImage:)` path
    /// and the new EDR-aware `MetalEDRView` based on the toggle
    /// + screen capability.  Switching at runtime is supported:
    /// the bridge's setHDREnabled flips which block fires and
    /// triggers a Repaint, so the next observable state is at
    /// the new format.  When the user toggles ON but moves the
    /// window to an SDR display, refreshEDRAvailability flips
    /// edrEnabled back to false automatically.
    private var edrActive: Bool {
        viewModel.edrAvailable && viewModel.edrEnabled
    }

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                Color(nsColor: .controlBackgroundColor)

                if edrActive, let renderer = viewModel.edrRenderer {
                    // EDR display path — half-float pixels through
                    // a CAMetalLayer with extendedLinearSRGB color
                    // space.  Aspect-fit handled inside the
                    // renderer's fragment-shader pipeline.  Used
                    // only when no scene is loaded (no interactive
                    // editor); the interactive path embeds the
                    // EDR layer inside ViewportNSView directly,
                    // see ContentView's main viewport block.
                    MetalEDRSimpleView(renderer: renderer)
                        .frame(maxWidth: geometry.size.width,
                               maxHeight: geometry.size.height)
                } else if let image = viewModel.renderedImage {
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
