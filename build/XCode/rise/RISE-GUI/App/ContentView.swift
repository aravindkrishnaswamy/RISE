import SwiftUI

/// A layout that arranges subviews horizontally, wrapping to the next line when needed.
struct FlowLayout: Layout {
    var spacing: CGFloat = 8

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let sizes = subviews.map { $0.sizeThatFits(.unspecified) }
        return arrangeSubviews(sizes: sizes, containerWidth: proposal.width ?? .infinity).size
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let sizes = subviews.map { $0.sizeThatFits(.unspecified) }
        let offsets = arrangeSubviews(sizes: sizes, containerWidth: bounds.width).offsets
        for (index, subview) in subviews.enumerated() {
            subview.place(at: CGPoint(x: bounds.minX + offsets[index].x,
                                      y: bounds.minY + offsets[index].y),
                          proposal: .unspecified)
        }
    }

    private func arrangeSubviews(sizes: [CGSize], containerWidth: CGFloat) -> (offsets: [CGPoint], size: CGSize) {
        var offsets: [CGPoint] = []
        var x: CGFloat = 0
        var y: CGFloat = 0
        var rowHeight: CGFloat = 0
        var maxWidth: CGFloat = 0

        for size in sizes {
            if x + size.width > containerWidth && x > 0 {
                x = 0
                y += rowHeight + spacing
                rowHeight = 0
            }
            offsets.append(CGPoint(x: x, y: y))
            rowHeight = max(rowHeight, size.height)
            x += size.width + spacing
            maxWidth = max(maxWidth, x - spacing)
        }

        return (offsets, CGSize(width: maxWidth, height: y + rowHeight))
    }
}

struct ContentView: View {
    @EnvironmentObject var viewModel: RenderViewModel
    @State private var editorWidth: CGFloat = sceneEditorPanelWidth
    @GestureState private var dragStartWidth: CGFloat? = nil
    @State private var suppressEditorAdjust = false

    /// Bumped on every frame update so the right-side properties panel
    /// re-snapshots from the live camera (which may have just been
    /// mutated by a drag).
    @State private var propertyRefresh: Int = 0

    var body: some View {
        HStack(spacing: 0) {
            // Editor sidebar (slides in from left)
            if viewModel.isEditorVisible {
                SceneEditorPanel()
                    .frame(width: editorWidth)
                    .transition(.move(edge: .leading))

                // Draggable resize handle
                Rectangle()
                    .fill(Color(nsColor: .separatorColor))
                    .frame(width: 5)
                    .contentShape(Rectangle())
                    .onHover { hovering in
                        if hovering {
                            NSCursor.resizeLeftRight.push()
                        } else {
                            NSCursor.pop()
                        }
                    }
                    .gesture(
                        DragGesture(minimumDistance: 1)
                            .updating($dragStartWidth) { _, startWidth, _ in
                                if startWidth == nil {
                                    startWidth = editorWidth
                                }
                            }
                            .onChanged { value in
                                let start = dragStartWidth ?? editorWidth
                                let newWidth = start + value.translation.width
                                editorWidth = max(200, min(newWidth, 1200))
                            }
                    )
            }

            // Main content
            VStack(spacing: 0) {
                // Top area: viewport (toolbar + canvas + timeline +
                // properties panel) when a scene is loaded.  Until then,
                // the passive RenderImageView shows the no-scene
                // placeholder.  There's no separate "interact mode" —
                // viewport is always on once a scene exists; clicking
                // Render stops the viewport, runs the production
                // rasterizer, then restarts the viewport.
                if let vb = viewModel.viewportBridge {
                    let interacting = (viewModel.renderState != .rendering
                                       && viewModel.renderState != .cancelling
                                       && viewModel.renderState != .loading)
                    HStack(spacing: 0) {
                        ViewportView(
                            bridge: vb,
                            image: $viewModel.renderedImage,
                            timelineVisible: viewModel.hasAnimation,
                            sceneTime: $viewModel.sceneTime,
                            // Pull the timeline range from the scene's
                            // animation_options chunk via the bridge.
                            // Falls back to 5.0 only if the scene
                            // declares no animation options at all
                            // (animationTimeEnd == 0), so we avoid a
                            // 0-length slider that would clamp every
                            // scrub to t=0.
                            timelineMax: vb.animationTimeEnd > 0 ? vb.animationTimeEnd : 5.0,
                            interactionEnabled: interacting,
                            onSelectionMayHaveChanged: { propertyRefresh += 1 }
                        )
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                        // Right panel — visibility / contents are
                        // controlled by the bridge's panelMode.  None →
                        // empty placeholder; Camera → camera property
                        // table; Object → name + position read-out.  The
                        // panel itself reads bridge.panelMode each refresh.
                        Divider()

                        PropertiesPanel(
                            bridge: vb,
                            refreshTrigger: $propertyRefresh
                        )
                        .frame(width: 280)
                        .disabled(!interacting)
                    }
                    .onChange(of: viewModel.renderedImage) { _, _ in
                        // Each rendered frame implies the camera may
                        // have moved; bump the refresh counter so the
                        // panel re-snapshots.  The panel itself
                        // protects against overwriting an in-flight
                        // text edit (focused field is left alone).
                        propertyRefresh &+= 1
                    }
                } else {
                    RenderImageView()
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }

                Divider()

                // Bottom area: controls (left) + log output (right)
                HStack(spacing: 0) {
                    // Bottom-left: Controls panel
                    controlsPanel
                        .frame(width: 260)

                    Divider()

                    // Bottom-right: Log output
                    LogOutputView()
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .frame(height: 280)

                Divider()

                // Status bar
                statusBar
            }
        }
        .navigationTitle(windowTitle)
        .navigationSubtitle("RISE \(viewModel.versionString)")
        .frame(minWidth: 600, minHeight: 500)
        .onChange(of: viewModel.sceneSize) { _, newSize in
            guard let size = newSize else { return }
            resizeWindowToFitScene(size)
        }
        .onChange(of: viewModel.isEditorVisible) { _, isVisible in
            adjustWindowForEditor(visible: isVisible)
        }
    }

    // MARK: - Controls Panel

    private var controlsPanel: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Controls")
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                Spacer()
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            ScrollView(.vertical, showsIndicators: false) {
                VStack(alignment: .leading, spacing: 10) {
                // Scene actions
                FlowLayout(spacing: 8) {
                    Button {
                        viewModel.openScene()
                    } label: {
                        Label("Open Scene", systemImage: "doc.badge.plus")
                    }
                    .disabled(viewModel.renderState == .rendering
                              || viewModel.renderState == .cancelling
                              || viewModel.renderState == .loading)
                    .help("Open a .RISEscene file")

                    Button {
                        withAnimation(.easeInOut(duration: 0.25)) {
                            viewModel.editSceneFile()
                        }
                    } label: {
                        Label("Edit", systemImage: "pencil")
                    }
                    .disabled(viewModel.loadedFilePath == nil)
                    .help(viewModel.isEditorVisible ? "Close the scene editor" : "Edit the scene file")

                    Button {
                        viewModel.clearScene()
                    } label: {
                        Label("Clear", systemImage: "trash")
                    }
                    .disabled(viewModel.renderState == .rendering
                              || viewModel.renderState == .cancelling
                              || viewModel.renderState == .idle)
                    .help("Clear the current scene")
                }

                Divider()

                // Render actions
                FlowLayout(spacing: 8) {
                    Button {
                        viewModel.startRender()
                    } label: {
                        Label("Render", systemImage: "play.fill")
                    }
                    .disabled(!canRender)
                    .help("Start rendering the loaded scene")

                    Button {
                        viewModel.startAnimationRender()
                    } label: {
                        Label("Render Animation", systemImage: "film")
                    }
                    .disabled(!canRender || !viewModel.hasAnimation)
                    .help("Render all animation frames")

                    Button {
                        viewModel.cancelRender()
                    } label: {
                        Label("Cancel", systemImage: "stop.fill")
                    }
                    .disabled(viewModel.renderState != .rendering)
                    .help("Cancel the current render")
                }

                // Cancelling indicator
                if viewModel.renderState == .cancelling {
                    HStack(spacing: 6) {
                        ProgressView()
                            .controlSize(.small)
                        Text("Cancelling — waiting for active block to finish…")
                            .font(.caption)
                            .foregroundColor(.orange)
                    }
                }

                // Progress
                if viewModel.renderState == .rendering || viewModel.renderState == .cancelling {
                    VStack(alignment: .leading, spacing: 4) {
                        ProgressView(value: viewModel.progress)
                            .progressViewStyle(.linear)

                        HStack {
                            Text(String(format: "%.1f%%", viewModel.progress * 100))
                                .font(.caption)
                                .monospacedDigit()

                            if !viewModel.progressTitle.isEmpty {
                                Text(viewModel.progressTitle)
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                    .lineLimit(1)
                            }

                            Spacer()

                            Text("Elapsed: \(viewModel.formattedElapsedTime)")
                                .font(.caption)
                                .monospacedDigit()
                                .foregroundColor(.secondary)
                        }

                        HStack {
                            Spacer()
                            if let remaining = viewModel.formattedRemainingTime {
                                Text("Remaining: ~\(remaining)")
                                    .font(.caption)
                                    .monospacedDigit()
                                    .foregroundColor(.secondary)
                            } else {
                                Text("Remaining: estimating\u{2026}")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }
                    }
                }

                }
                .padding(10)
            }
        }
        .clipped()
    }

    // MARK: - Status Bar

    private var statusBar: some View {
        HStack(spacing: 12) {
            Text(statusText)
                .font(.caption)
                .foregroundColor(.secondary)

            Spacer()

            if viewModel.renderState == .completed {
                Text(viewModel.formattedElapsedTime)
                    .font(.caption)
                    .monospacedDigit()
                    .foregroundColor(.secondary)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    // MARK: - Window Sizing

    /// Resizes the key window so the content area fits the scene plus the bottom panels.
    private func resizeWindowToFitScene(_ sceneSize: CGSize) {
        guard let window = NSApplication.shared.keyWindow else { return }
        guard let screen = window.screen else { return }
        guard sceneSize.width > 0 && sceneSize.height > 0 else { return }

        let chromeHeight = window.frame.height - window.contentLayoutRect.height
        let bottomPanelHeight: CGFloat = 280
        let statusBarHeight: CGFloat = 30
        let fixedHeight = bottomPanelHeight + statusBarHeight

        let editorPanelTotal: CGFloat = viewModel.isEditorVisible ? editorWidth + 5 : 0
        let maxWidth = screen.visibleFrame.width
        let maxHeight = screen.visibleFrame.height

        // Start with 1:1 pixel mapping for the scene
        var renderWidth = sceneSize.width
        var renderHeight = sceneSize.height

        // If the frame would exceed screen bounds, scale down the render area
        // while preserving the scene's aspect ratio.
        let maxRenderHeight = maxHeight - chromeHeight - fixedHeight
        let maxRenderWidth = maxWidth - editorPanelTotal
        let scale = min(1.0, maxRenderWidth / renderWidth, maxRenderHeight / renderHeight)
        renderWidth = ceil(renderWidth * scale)
        renderHeight = ceil(renderHeight * scale)

        let frameWidth = renderWidth + editorPanelTotal
        let frameHeight = renderHeight + fixedHeight + chromeHeight

        let originX = screen.visibleFrame.midX - frameWidth / 2
        let originY = screen.visibleFrame.midY - frameHeight / 2
        let newFrame = NSRect(x: originX, y: originY, width: frameWidth, height: frameHeight)

        // If the editor panel is already included in this resize, suppress
        // the adjustWindowForEditor that may fire in the same update cycle.
        if editorPanelTotal > 0 {
            suppressEditorAdjust = true
        }

        window.setFrame(newFrame, display: true, animate: true)
    }

    /// Grows or shrinks the window to accommodate the editor sidebar,
    /// keeping the right edge anchored so the render area stays in place.
    private func adjustWindowForEditor(visible: Bool) {
        if suppressEditorAdjust {
            suppressEditorAdjust = false
            return
        }
        guard let window = NSApplication.shared.keyWindow else { return }
        guard let screen = window.screen else { return }

        let delta = editorWidth + 5 // panel + resize handle
        var frame = window.frame

        if visible {
            frame.size.width += delta
            frame.origin.x -= delta
        } else {
            frame.size.width -= delta
            frame.origin.x += delta
        }

        // Clamp to screen bounds
        let visibleFrame = screen.visibleFrame
        if frame.origin.x < visibleFrame.minX {
            frame.origin.x = visibleFrame.minX
        }
        if frame.maxX > visibleFrame.maxX {
            frame.size.width = visibleFrame.maxX - frame.origin.x
        }
        frame.size.width = max(frame.size.width, 900)

        window.setFrame(frame, display: true, animate: true)
    }

    // MARK: - Computed Properties

    private var canRender: Bool {
        switch viewModel.renderState {
        case .sceneLoaded, .completed, .cancelled:
            return true
        default:
            return false
        }
    }

    private var statusText: String {
        switch viewModel.renderState {
        case .idle:
            return "Ready"
        case .loading:
            return "Loading scene..."
        case .sceneLoaded:
            return "Scene loaded. Press Render to begin."
        case .rendering:
            return "Rendering..."
        case .cancelling:
            return "Cancelling — waiting for active block to finish…"
        case .completed:
            return "Render complete"
        case .cancelled:
            return "Render cancelled"
        case .error(let msg):
            return "Error: \(msg)"
        }
    }

    private var windowTitle: String {
        if let path = viewModel.loadedFilePath {
            return (path as NSString).lastPathComponent
        }
        return "RISE"
    }
}
