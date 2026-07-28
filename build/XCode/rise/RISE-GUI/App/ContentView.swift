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

    /// Bumped on every frame update so the right-side properties panel
    /// re-snapshots from the live camera (which may have just been
    /// mutated by a drag).
    @State private var propertyRefresh: Int = 0

    /// Left panel width — user-resizable via the drag handle between the
    /// panel and the center column, persisted across launches.  Wider
    /// default than the comp's 404 so the scene file is actually
    /// readable (user refinement).
    @AppStorage("leftPanelWidth") private var leftPanelWidth: Double = 520
    @GestureState private var leftPanelDragStart: Double? = nil
    private static let leftPanelMinWidth: Double = 360
    // Review P2: 700 max (not 900) — at the 1320pt minimum window the
    // fixed 344pt right panel + a 900pt left panel would leave the
    // center viewport ~71pt.  700 keeps the center >= ~270pt worst-case.
    private static let leftPanelMaxWidth: Double = 700

    /// Theme mode re-render hook (light/dark switching — see
    /// ThemeMode.swift). Mirrors the SAME `"themeMode"` UserDefaults
    /// key that `ThemeState.setMode(_:)` writes, so a theme toggle
    /// anywhere in the app (the File-menu toggle calls
    /// `ThemeState.setMode(_:)` directly, not through this view model)
    /// is observed here via `@AppStorage` and forces the `.id()` below
    /// to rebuild the whole tree — the only way to get every
    /// already-evaluated `Theme.*` token in the view hierarchy to
    /// re-read `ThemeState.mode`, since Theme.swift's tokens are plain
    /// computed properties, not something SwiftUI can subscribe to on
    /// its own. This is a rare user action, so the rebuild cost is
    /// acceptable.
    @AppStorage(ThemeState.userDefaultsKey) private var themeModeRaw: String = ThemeMode.dark.rawValue

    // MARK: - UI redesign center-column slice: viewport toolbar state
    //
    // Moved up from ViewportView (which used to own `selectedTool` as
    // private @State) because the tool-category buttons now live in
    // this view's own toolbar row, above the viewport rather than
    // floating over it.  `regionArmed` / `showEVPopover` are new
    // design-brief-A4 / EV-cluster state with no pre-redesign
    // equivalent.

    /// The active viewport tool.  Bound into both `ViewportToolbar`
    /// (button state) and `ViewportView` (cursor + gizmo + pointer
    /// routing all still live there).
    @State private var selectedTool: ViewportTool = .select
    /// True while the REGION chip is armed (see design brief A4) —
    /// the next mouse-drag in the viewport draws a region box.
    @State private var regionArmed: Bool = false
    /// Popover-shown state for the toolbar's EV chip.
    @State private var showEVPopover: Bool = false

    /// N-up multi-viewport (docs/gui/RENDER_MODES.md §7): the active
    /// layout.  `.single` keeps the pre-N-up `ViewportView` path
    /// byte-for-byte unchanged (see `centerColumn` below); any other
    /// value engages `MultiPaneViewportView`.  Reset to `.single` on
    /// every fresh bridge (new scene load) via the `.task(id:)` on
    /// `viewportToolbarRow` below — a freshly-constructed controller
    /// always defaults to Single anyway, so this just keeps the
    /// SwiftUI-local mirror in sync.
    @State private var viewportLayout: ViewportLayoutOption = .single

    var body: some View {
        VStack(spacing: 0) {
            TopBar()
            mainArea
        }
        .navigationTitle(windowTitle)
        .navigationSubtitle("RISE \(viewModel.versionString)")
        .frame(minWidth: 1320, minHeight: 760)
        // Theme mode re-render hook — see `themeModeRaw` doc above.
        .id(themeModeRaw)
    }

    // MARK: - Main workspace (left panel / center / right panel)

    private var mainArea: some View {
        HStack(spacing: 0) {
            if viewModel.viewportBridge != nil && viewModel.showLeftPanel {
                leftPanel
                leftPanelResizeHandle
            }

            centerColumn
                .frame(maxWidth: .infinity, maxHeight: .infinity)

            if let vb = viewModel.viewportBridge {
                rightPanel(vb)
            }
        }
    }

    /// True while the interactive viewport accepts pointer / drag input
    /// — false during a production render (both the viewport's own
    /// gesture handling and the properties panel disable themselves on
    /// this so they can't race the production rasterizer's workers).
    private var interacting: Bool {
        // Chat-inclusive.  A chat/agent-driven render holds the controller's
        // commit mutex for its WHOLE duration but does NOT flip renderState,
        // so a renderState-only gate let viewport pointer gestures
        // (OnPointerMove, mMutex) and the nav overlay's freeFlyActive /
        // hasHomeView reads wedge the main thread during a chat render — the
        // same class as the EnvironmentPanel hang.  isSceneEditableForAgents
        // folds in !isChatRenderOutstanding on top of the render-state check.
        viewModel.isSceneEditableForAgents
    }

    // MARK: - Left panel (Agent / Scene file tabs)

    private var leftPanel: some View {
        VStack(spacing: 0) {
            leftPanelTabStrip
            switch viewModel.leftTab {
            case .agent:
                agentTabBody
            case .sceneFile:
                SceneEditorPanel()
            }
        }
        .frame(width: CGFloat(leftPanelWidth))
        .background(Theme.bgPanel)
        .overlay(alignment: .trailing) {
            Rectangle().fill(Theme.borderHairline).frame(width: 1)
        }
    }

    /// 5pt drag strip between the left panel and the center column —
    /// same interaction the pre-redesign editor sidebar had.  Cursor
    /// flips to resize on hover; width clamps to [360, 900].
    private var leftPanelResizeHandle: some View {
        Rectangle()
            .fill(Color.clear)
            .frame(width: 5)
            .contentShape(Rectangle())
            .onHover { hovering in
                if hovering { NSCursor.resizeLeftRight.push() }
                else { NSCursor.pop() }
            }
            .gesture(
                DragGesture(minimumDistance: 1)
                    .updating($leftPanelDragStart) { _, start, _ in
                        if start == nil { start = leftPanelWidth }
                    }
                    .onChanged { value in
                        let start = leftPanelDragStart ?? leftPanelWidth
                        leftPanelWidth = min(Self.leftPanelMaxWidth,
                                             max(Self.leftPanelMinWidth,
                                                 start + Double(value.translation.width)))
                    }
            )
    }

    private var leftPanelTabStrip: some View {
        HStack(spacing: 2) {
            leftPanelTabButton(title: "Agent", isActive: viewModel.leftTab == .agent, showDot: true) {
                viewModel.leftTab = .agent
            }
            leftPanelTabButton(title: "Scene file", isActive: viewModel.leftTab == .sceneFile, showDot: false) {
                viewModel.leftTab = .sceneFile
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 8)
        .frame(height: 42)
        .background(Theme.bgPanel)
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
        }
    }

    private func leftPanelTabButton(title: String, isActive: Bool, showDot: Bool,
                                     action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 0) {
                HStack(spacing: 8) {
                    if showDot {
                        Circle().fill(Theme.accent).frame(width: 6, height: 6)
                    }
                    Text(title)
                        .font(isActive ? Theme.sans(12, .semibold) : Theme.mono(11.5))
                        .foregroundColor(isActive ? Theme.textPrimary : Theme.textFaint)
                }
                .padding(.horizontal, 15)
                .frame(height: 40)
                Rectangle()
                    .fill(isActive ? AnyShapeStyle(Theme.spectralGradient) : AnyShapeStyle(Color.clear))
                    .frame(height: 2)
            }
        }
        .buttonStyle(.plain)
    }

    /// The Agent tab: the LLM Chat panel filling the column's FULL
    /// height, plus (behind the Developer toggle) the raw JSON-RPC
    /// Agent debug panel at its intrinsic height below.  Deliberately
    /// NOT wrapped in a ScrollView: inside one, nothing stretches, so
    /// the panel used to sit at its intrinsic height in the top half of
    /// the column with dead space beneath.  The transcript inside
    /// ChatPanel is the flexible element (minHeight 220, maxHeight
    /// .infinity) and scrolls internally, so the column cannot overflow:
    /// worst-case fixed content (proposals + 220 transcript floor +
    /// composer + compact debug panel) stays well inside the window's
    /// 760pt minimum height.
    private var agentTabBody: some View {
        VStack(alignment: .leading, spacing: 10) {
            ChatPanel(chat: viewModel.chat)

            if viewModel.showAgentDebugPanel {
                Divider()
                AgentPanel()
            }
        }
        .padding(10)
        .frame(maxHeight: .infinity, alignment: .top)
    }

    // MARK: - Center column (viewport / log drawer)

    private var centerColumn: some View {
        VStack(spacing: 0) {
            if let vb = viewModel.viewportBridge {
                viewportToolbarRow(vb)
                    // N-up (§7): reset the SwiftUI-local layout mirror
                    // to Single on every fresh bridge (new scene load).
                    // The underlying controller always constructs with
                    // Single anyway; this just keeps this view's state
                    // from carrying a prior scene's layout choice
                    // forward, mirroring `ViewportView`'s own
                    // `.task(id: ObjectIdentifier(bridge))` reset
                    // pattern for `selectedTool`.
                    .task(id: ObjectIdentifier(vb)) {
                        viewportLayout = .single
                    }
                    // user-review P2#5 (round 2): region refinement is a
                    // single-viewport affordance (N-up panes have no region-
                    // drag gesture).  Leaving Single with a region still ARMED
                    // or ACTIVE would strand it -- the chip is disabled in N-up,
                    // so the user couldn't clear it, and there's no RegionOverlay
                    // in the N-up panes to even show it.  Clear it on the way out,
                    // mirroring the chip's own clear path.
                    .onChange(of: viewportLayout) { _, newLayout in
                        if newLayout != .single {
                            // ViewportView owns the sole sceneTime ->
                            // native scrub observer. Leaving Single removes
                            // that view, so retire its playback task and close
                            // the native scrub composite before N-up appears.
                            viewModel.stopPreviewPlay()
                            if viewModel.activeRegion != nil {
                                vb.clearInteractiveRegion()
                                viewModel.activeRegion = nil
                            }
                            regionArmed = false
                        }
                    }

                if viewportLayout == .single {
                    // Single layout: EXACTLY the pre-N-up code path,
                    // unchanged — see docs/gui/RENDER_MODES.md §7.0/§7.5.
                    let edrActive = viewModel.edrAvailable && viewModel.edrEnabled
                    ViewportView(
                        bridge: vb,
                        image: $viewModel.renderedImage,
                        timelineVisible: viewModel.hasAnimation,
                        sceneTime: $viewModel.sceneTime,
                        // Pull the timeline range from the scene's
                        // animation_options chunk via the bridge.  Falls
                        // back to 5.0 only if the scene declares no
                        // animation options at all (animationTimeEnd == 0),
                        // so we avoid a 0-length slider that would clamp
                        // every scrub to t=0.
                        timelineMax: vb.animationTimeEnd > 0 ? vb.animationTimeEnd : 5.0,
                        interactionEnabled: interacting,
                        isProductionRendering: (viewModel.renderState == .rendering),
                        onSelectionMayHaveChanged: { propertyRefresh += 1 },
                        isPreviewPlaying: viewModel.isPreviewPlaying,
                        onPlayToggle: { viewModel.togglePreviewPlay() },
                        onUserScrubBegan: { viewModel.stopPreviewPlay() },
                        productionEDRRenderer: viewModel.productionEDRRenderer,
                        interactiveEDRRenderer: viewModel.interactiveEDRRenderer,
                        edrEnabled: edrActive,
                        selectedTool: $selectedTool,
                        regionArmed: $regionArmed,
                        refreshTrigger: $propertyRefresh
                    )
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .onChange(of: viewModel.renderedImage) { _, _ in
                        // Each rendered frame implies the camera may have
                        // moved; bump the refresh counter so the right
                        // panel re-snapshots.  The panel itself protects
                        // against overwriting an in-flight text edit
                        // (focused field is left alone).
                        propertyRefresh &+= 1
                    }
                    .onChange(of: viewModel.reverseSelectEpoch) { _, _ in
                        // Reverse source traceability: a right-click "Select in
                        // Inspector" changed the bridge selection but rendered no
                        // frame, so drive the shared refresh here — the inspector
                        // re-snapshots and the outliner highlight follows.
                        propertyRefresh &+= 1
                    }
                    .onChange(of: viewModel.regionDrawRequestEpoch) { _, _ in
                        guard interacting, viewportLayout == .single,
                              viewModel.activeRegion == nil else { return }
                        regionArmed = true
                    }
                } else {
                    // N-up (§7): TwoH / OnePlusTwo / Quad.
                    MultiPaneViewportView(
                        bridge: vb,
                        layout: viewportLayout,
                        interactionEnabled: interacting,
                        isProductionRendering: (viewModel.renderState == .rendering),
                        selectedTool: selectedTool,
                        // user-review P2#4: a pane pick drives the same
                        // shared Inspector/Outliner refresh the single
                        // viewport does.
                        onSelectionMayHaveChanged: { propertyRefresh += 1 }
                    )
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .onChange(of: viewModel.reverseSelectEpoch) { _, _ in
                        // External-review P2 (2026-07-22): jump-to-definition
                        // (and reverse source-select) bump reverseSelectEpoch
                        // but render no frame.  The single-viewport branch
                        // above observes it; the N-up branch must too, else the
                        // bridge selection changes while the Inspector/Outliner
                        // stay stale in TwoH / OnePlusTwo / Quad layouts.
                        propertyRefresh &+= 1
                    }
                }
            } else if viewModel.renderState == .loading {
                // Scene load in flight — RenderImageView carries the
                // progress overlay.
                RenderImageView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                // No scene (fresh launch, after Close Scene, or a failed
                // load) — the three-path start screen
                // (docs/gui/START_SCREEN.md).  A load error renders as a
                // banner atop the start screen rather than a dead
                // placeholder.
                StartView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }

            if viewModel.showLogDrawer {
                logDrawer
            } else {
                collapsedLogStrip
            }
        }
        .background(Theme.bgCenter)
    }

    // MARK: - Viewport toolbar row (design comp: ~239-264)

    /// A 52pt row above the viewport: tool group (every tool an
    /// individual labeled button — see ViewportToolbar.swift's file
    /// header for the discoverability redesign), active-camera chip,
    /// region-of-interest toggle, then (right-aligned) the EV readout
    /// + EDR chip.  Replaces the pre-redesign floating toolbar that
    /// used to overlay the rendered image itself.  Row grew from the
    /// original 40pt to fit the toolbar's now-labeled ~44pt-tall
    /// buttons; the other chips are unchanged in height and simply
    /// re-center within the taller row (HStack's default `.center`
    /// alignment).
    private func viewportToolbarRow(_ vb: RISEViewportBridge) -> some View {
        HStack(spacing: 10) {
            ViewportToolbar(selectedTool: $selectedTool)
                .disabled(!interacting)
                .opacity(interacting ? 1.0 : 0.5)

            Rectangle().fill(Theme.borderLight).frame(width: 1, height: 18)

            cameraChip(vb)

            regionChip(vb)

            viewportRenderModeChip

            // N-up (docs/gui/RENDER_MODES.md §7.5): the layout picker.
            // Kept enabled/disabled in step with the rest of this row's
            // interactive chips.
            ViewportLayoutPicker(bridge: vb, layout: $viewportLayout)
                .disabled(!interacting)
                .opacity(interacting ? 1.0 : 0.5)

            Spacer(minLength: 0)

            evChip
            edrChip
        }
        .padding(.horizontal, 12)
        .frame(height: 52)
        .background(Theme.bgCenter)
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
        }
    }

    /// "◉ <activeCameraName> 50mm · f/4.0" — non-interactive (switching
    /// cameras stays the outliner's job).  The lens summary is appended
    /// ONLY when real per-parameter data is available for the ACTIVE
    /// camera (see `cameraLensSummary` below) — no fabricated units, no
    /// guessed defaults.  Refreshes on the same trigger every sibling
    /// chip/panel in this row uses (`propertyRefresh`, bumped on
    /// selection change / property edits — see the view-body callers),
    /// not per-frame; reads the bridge unconditionally, same as the
    /// pre-existing name-only read did (no extra scene-editable gate —
    /// this is a read-only display, not an interactive affordance, matching
    /// `regionChip`'s own pattern of gating INTERACTION, not the read).
    private func cameraChip(_ vb: RISEViewportBridge) -> some View {
        let name = vb.activeName(for: .camera)
        let lens = cameraLensSummary(vb, activeCameraName: name)
        return HStack(spacing: 6) {
            Text("◉").foregroundColor(Theme.accentSoft)
            Text(name.isEmpty ? "—" : name)
            if let lens {
                Text(lens).foregroundColor(Theme.textDim)
            }
        }
        .font(Theme.mono(11.5))
        .foregroundColor(Theme.textPrimary)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(Theme.bgPanel, in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMedium)
                .stroke(Theme.borderHairline, lineWidth: 1)
        )
        .help("Active camera — switch in the outliner")
    }

    /// Lens summary for the active camera, e.g. "50mm · f/4.0" (ThinLens)
    /// or "62° FOV · f/8" (Pinhole — no focal_length parameter exists on
    /// that camera type, so this honestly shows FOV instead of fabricating
    /// a focal length). Returns nil (name-only fallback) when there is
    /// nothing honest to show.
    ///
    /// DATA SOURCE CAVEAT: reads `propertySnapshot(for: .camera)`, which is
    /// keyed to the Properties panel's OWN camera-category SELECTION
    /// (`SceneEditController::mSelectionByCategory[Camera]`), not directly
    /// to the scene's active camera. Those two are the SAME thing for any
    /// camera the user has actually clicked in the outliner/panel —
    /// `SetSelection(Camera, name)` is what calls `IScene::SetActiveCamera`
    /// in the first place — but immediately after scene load, before any
    /// camera selection, the panel's selection can be empty while a camera
    /// is nonetheless active by scene default. This function ONLY trusts
    /// the snapshot when the panel's current selection name already
    /// echoes `activeCameraName`; otherwise it returns nil rather than
    /// either showing stale/wrong data or forcing `SetSelection` as a side
    /// effect of a toolbar read (that call is a real scene mutation with
    /// its own cancel-and-park serialization — not something a read-only
    /// chip should trigger).
    private func cameraLensSummary(_ vb: RISEViewportBridge, activeCameraName: String) -> String? {
        guard !activeCameraName.isEmpty,
              vb.selectionCategory == .camera,
              vb.selectionName == activeCameraName
        else { return nil }

        var focalLengthMm: String?
        var fovDegrees: String?
        var fstop: String?
        for row in vb.propertySnapshot(for: .camera) {
            switch row.name {
            case "focal_length": focalLengthMm = row.value
            case "fov":          fovDegrees = row.value
            case "fstop":        fstop = row.value
            default: break
            }
        }

        var parts: [String] = []
        if let focalLengthMm {
            parts.append("\(focalLengthMm)mm")
        } else if let fovDegrees {
            parts.append("\(fovDegrees)° FOV")
        }
        if let fstop {
            parts.append("f/\(fstop)")
        }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }

    /// Design brief A4 region-of-interest action. Three visual states:
    /// dimmed "▧ Draw Region" (off), accent "▧ Cancel Draw" while the
    /// viewport is waiting for a drag, and warn "▧ Region active ×"
    /// once a box is active. The internal state remains `regionArmed`,
    /// but product copy tells the user the available action instead of
    /// exposing that state-machine term. Disabled + explained via `.help` when the
    /// active interactive rasterizer doesn't honor regions at all.
    private func regionChip(_ vb: RISEViewportBridge) -> some View {
        let honored = vb.interactiveRasterizerHonorsRegion()
        // Also gated on `interacting` — arming (or clearing) a region
        // while a production render owns the viewport can't lead
        // anywhere (pointer events are dropped by ViewportView's own
        // `interactionEnabled` guard while a render is in flight),
        // and would otherwise leave a "Cancel Draw" chip stuck
        // with no drag ever able to consume it.
        // user-review P2#5: N-up panes have no region-drag gesture, so
        // arming a region there would strand the chip at "· armed" with no
        // drag able to consume it.  Region refinement is a single-viewport
        // affordance -- gate the control on the Single layout.
        let singleViewport = ( viewportLayout == .single )
        let usable = honored && interacting && singleViewport
        let active = viewModel.activeRegion != nil
        let text = active ? "▧ Region active ×" : (regionArmed ? "▧ Cancel Draw" : "▧ Draw Region")
        let color: Color = active ? Theme.warn : (regionArmed ? Theme.accent : Theme.textFaint)
        return Text(text)
            .font(Theme.mono(10))
            .foregroundColor(color)
            .padding(.horizontal, 7)
            .padding(.vertical, 4)
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(color.opacity(0.4), lineWidth: 1)
            )
            .contentShape(Rectangle())
            .opacity(usable ? 1.0 : 0.4)
            .onTapGesture {
                guard usable else { return }
                if active {
                    vb.clearInteractiveRegion()
                    viewModel.activeRegion = nil
                    regionArmed = false
                } else {
                    regionArmed.toggle()
                }
            }
            .help(!honored
                  ? "The active integrator ignores regions"
                  : (!singleViewport
                     ? "Region refinement is available in the single viewport"
                     : (interacting
                        ? (regionArmed
                           ? "Cancel drawing the render region"
                           : "Draw a region to focus interactive refinement")
                        : "Unavailable while a render is in flight")))
    }

    /// P1 render modes (docs/gui/RENDER_MODES.md §5): the viewport's
    /// mode dropdown — "what does the interactive viewport render right
    /// now" (Shaded Preview / Normals / Depth / Facets / Wireframe).
    /// Deliberately kept OUT of the evChip/edrChip cluster on the right:
    /// §4 ratified decision 1 draws a hard line between render MODES
    /// (what gets rendered — this chip) and view TRANSFORMS (exposure /
    /// tone curve / EDR — framebuffer re-transforms of whatever was
    /// rendered); grouping them visually would blur a distinction the
    /// design doc is explicit about.  Hidden entirely until the
    /// registry list has loaded (`viewModel.viewportBridge`'s didSet
    /// populates it once per scene open) rather than flashing an empty
    /// menu for one frame.
    @ViewBuilder
    private var viewportRenderModeChip: some View {
        if !viewModel.viewportRenderModes.isEmpty {
            let activeMode = viewModel.viewportRenderModes.first { $0.name == viewModel.viewportRenderMode }
            Menu {
                ForEach(viewModel.viewportRenderModes) { mode in
                    Button {
                        viewModel.setViewportRenderMode(mode.name)
                    } label: {
                        if mode.name == viewModel.viewportRenderMode {
                            Label(mode.title, systemImage: "checkmark")
                        } else {
                            Text(mode.title)
                        }
                    }
                    .help(mode.question)
                }
                // X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): an
                // orthogonal boolean, not a fifth mode — applies to EVERY
                // mode including the shaded preview (caster-layer resolve,
                // default ON; user decision 2026-07-17).
                Divider()
                Toggle(isOn: Binding(
                    get: { viewModel.viewportXray },
                    set: { viewModel.setViewportXray($0) })) {
                    Text("See Through Glass")
                }
                // P2a review fix: while the active mode is a BeautyVariant
                // row (deep_reflect/direct), this toggle would silently
                // no-op -- those modes drive a wholly separate ephemeral PT
                // pipeline that never reads the x-ray flag/caster.  Disable
                // rather than let it stamp a flag with no visible effect;
                // tooltip/help text unchanged (still accurate once the
                // user is back on a mode where it applies).
                .disabled(activeMode?.isVariant == true)
                .help("X-ray: skip transmissive surfaces (straight-line) and show the first opaque hit")
            } label: {
                HStack(spacing: 5) {
                    Text(activeMode?.title ?? viewModel.viewportRenderMode)
                    Image(systemName: "chevron.down").font(.system(size: 8))
                }
                .font(Theme.mono(11))
                .foregroundColor(Theme.textPrimary)
                .padding(.horizontal, 9)
                .padding(.vertical, 5)
                .background(Theme.bgPanel, in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusMedium)
                        .stroke(Theme.borderHairline, lineWidth: 1)
                )
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            .disabled(!interacting)
            .opacity(interacting ? 1.0 : 0.4)
            .help(activeMode?.question ?? "Viewport render mode")
        }
    }

    /// Compact "EV +0.3" readout; click pops a popover with the full
    /// exposure slider (moved here from the log-drawer header, which
    /// only parked it there temporarily — see the log drawer's
    /// history in git blame for L5e).
    private var evChip: some View {
        Button {
            showEVPopover = true
        } label: {
            Text(formattedEV)
                .font(Theme.mono(10))
                .foregroundColor(Theme.textFaint)
                .padding(.horizontal, 6)
                .padding(.vertical, 4)
        }
        .buttonStyle(.plain)
        .popover(isPresented: $showEVPopover, arrowEdge: .bottom) {
            ExposureSliderRow(
                ev: $viewModel.viewExposureEV,
                enabled: viewModel.renderState != .idle && !viewModel.edrEnabled
            )
            .frame(width: 220)
            .padding(12)
        }
        .help("Display exposure — click for the full slider")
    }

    private var formattedEV: String {
        let v = viewModel.viewExposureEV
        let sign = v > 0 ? "+" : ""
        return String(format: "EV \(sign)%.1f", v)
    }

    /// EDR Preview toggle chip, mirrored from the View menu's "EDR
    /// Preview" item (RISEApp.swift) — same `edrEnabled` /
    /// `edrAvailable` state, just reachable without opening a menu.
    private var edrChip: some View {
        Button {
            viewModel.edrEnabled.toggle()
        } label: {
            Text("EDR")
                .font(Theme.mono(10))
                .foregroundColor(viewModel.edrEnabled ? Theme.accent : Theme.textFaint)
                .padding(.horizontal, 7)
                .padding(.vertical, 4)
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(viewModel.edrEnabled ? Theme.accent.opacity(0.5) : Theme.borderLight, lineWidth: 1)
                )
        }
        .buttonStyle(.plain)
        .disabled(!viewModel.edrAvailable)
        .opacity(viewModel.edrAvailable ? 1.0 : 0.4)
        .help(viewModel.edrAvailable ? "Toggle EDR Preview" : "This display has no EDR headroom")
    }

    // MARK: - Log drawer
    //
    // LogOutputView owns the full drawer content (header: title,
    // severity pills, WARN/ERR counts, filter field, follow toggle,
    // clear, collapse chevron; body: the filtered row list) — this
    // view only supplies the outer frame/border/corner-radius and the
    // collapse callback.  The exposure control that used to be parked
    // in this header (L5e) moved to the viewport toolbar's EV chip.

    private var logDrawer: some View {
        LogOutputView(onCollapse: {
            withAnimation(.easeInOut(duration: 0.15)) {
                viewModel.showLogDrawer = false
            }
        })
        .frame(height: 150)
        .background(Theme.bgWell)
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.borderLight, lineWidth: 1))
        .padding(.horizontal, 10)
        .padding(.bottom, 8)
    }

    /// Collapsed strip shown in place of the log drawer — a single
    /// 28 pt row with the most recent log line + a WARN count chip,
    /// tap to expand.
    private var collapsedLogStrip: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.15)) {
                viewModel.showLogDrawer = true
            }
        } label: {
            HStack(spacing: 10) {
                Text("Log")
                    .font(Theme.sans(11, .semibold))
                    .foregroundColor(Theme.textPrimary)
                if let last = viewModel.logMessages.last {
                    Text(Self.collapsedLogTimeFormatter.string(from: last.timestamp))
                        .font(Theme.mono(10))
                        .foregroundColor(Theme.textGhost)
                    Text(last.text)
                        .font(Theme.mono(10.5))
                        .foregroundColor(Theme.textMuted)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
                Spacer(minLength: 0)
                if collapsedWarnCount > 0 {
                    Text("WARN \(collapsedWarnCount)")
                        .font(Theme.mono(9))
                        .foregroundColor(Theme.warn)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 1)
                        .overlay(
                            RoundedRectangle(cornerRadius: 4)
                                .stroke(Theme.warn.opacity(0.35), lineWidth: 1)
                        )
                }
                Image(systemName: "chevron.up")
                    .font(.system(size: 11))
                    .foregroundColor(Theme.textDim)
            }
            .padding(.horizontal, 10)
            .frame(height: 28)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .background(Theme.bgWell)
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.borderLight, lineWidth: 1))
        .padding(.horizontal, 10)
        .padding(.bottom, 8)
        .help("Expand log (⌥L)")
    }

    private var collapsedWarnCount: Int {
        viewModel.logMessages.filter { $0.level == .warning }.count
    }

    private static let collapsedLogTimeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    // MARK: - Right panel

    private func rightPanel(_ vb: RISEViewportBridge) -> some View {
        VStack(spacing: 0) {
            OutlinerView(bridge: vb, refreshTrigger: $propertyRefresh)
            EnvironmentPanel(bridge: vb, refreshTrigger: $propertyRefresh)
            NamedViewsPanel(bridge: vb, refreshTrigger: $propertyRefresh)
            PropertiesPanel(bridge: vb, refreshTrigger: $propertyRefresh)
        }
        .frame(width: 344)
        .disabled(!interacting)
        .background(Theme.bgPanel)
        .overlay(alignment: .leading) {
            Rectangle().fill(Theme.borderHairline).frame(width: 1)
        }
    }

    // MARK: - Computed Properties

    private var windowTitle: String {
        if let path = viewModel.loadedFilePath {
            return (path as NSString).lastPathComponent
        }
        if viewModel.viewportBridge != nil {
            // Untitled scene (start-screen create path) — keep the window
            // title consistent with the TopBar chip.
            return "Untitled"
        }
        return "RISE"
    }
}

// L5e — Exposure slider row.  Title + value readout above a clamped
// [-6, +6] EV slider.  Double-clicking anywhere on the slider's
// bounding rect resets to 0 (TapGesture(count: 2) layered as a
// background; SwiftUI's gesture-priority gives the higher-count tap
// precedence over the Slider's own drag handling, so a quick
// double-tap doesn't first land a stray drag).  Greyed out before
// the first scene load — the slider has no semantically meaningful
// effect there because the FrameStore doesn't exist yet, and an
// active-looking slider whose drags do nothing is confusing UX.
private struct ExposureSliderRow: View {
    @Binding var ev: Double
    let enabled: Bool

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "sun.max")
                .imageScale(.small)
                .foregroundColor(Theme.textDim)
            Slider(value: $ev, in: -6.0...6.0)
                .controlSize(.small)
                .background(
                    // Reset-to-zero double-click target.  Color.clear
                    // is hit-testable but invisible.  Sized to the
                    // Slider's bounds via .background placement; the
                    // gesture only fires on count == 2 so single-
                    // clicks pass through to the Slider's drag gate.
                    Color.clear
                        .contentShape(Rectangle())
                        .gesture(
                            TapGesture(count: 2).onEnded {
                                ev = 0.0
                            }
                        )
                )
            Text(formatEV(ev))
                .font(Theme.mono(9.5))
                .foregroundColor(Theme.textDim)
                .frame(width: 44, alignment: .trailing)
        }
        .disabled(!enabled)
        .help("Display exposure in EV stops.  Double-click the slider to reset to 0.")
    }

    private func formatEV(_ v: Double) -> String {
        // "+1.2 EV" / "0.0 EV" / "-3.5 EV" — matches the convention
        // used by photo apps (Lightroom etc.) and DCC viewers.
        let sign = v > 0 ? "+" : ""
        return String(format: "\(sign)%.1f EV", v)
    }
}

// MARK: - Agent panel (Facet 5 slice 1c-1)

/// The minimal live "agent + user co-edit" affordance: a multiline field
/// for a JSON-RPC request, a Send button that drives the viewport bridge's
/// `agentHandleLine` (which routes through the SAME live dispatcher /
/// session / controller the GUI edits through), and a read-only response
/// area.  A `propose_patch` edits the running scene; the viewport re-renders
/// and the Save button enables automatically via the controller's kick +
/// dirty-changed block (see `RenderViewModel.sendAgentRequest`), so this
/// view does NOT poke the viewport or the Save state by hand.
private struct AgentPanel: View {
    @EnvironmentObject var viewModel: RenderViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Image(systemName: "wand.and.stars")
                    .imageScale(.small)
                Text("Agent (JSON-RPC)")
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                Spacer()
            }

            Text("Request")
                .font(.caption2)
                .foregroundColor(.secondary)
            TextEditor(text: $viewModel.agentRequestText)
                .font(.system(.caption, design: .monospaced))
                .frame(height: 64)
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(Color(nsColor: .separatorColor))
                )

            HStack {
                Button {
                    // Runs synchronously on the main actor (see
                    // sendAgentRequest); the viewport + Save state update
                    // themselves via the controller's kick + dirty block.
                    viewModel.sendAgentRequest(viewModel.agentRequestText)
                } label: {
                    Label("Send", systemImage: "paperplane.fill")
                }
                .disabled(viewModel.viewportBridge == nil
                          || !viewModel.isSceneEditableForAgents
                          || viewModel.agentRequestText.isEmpty)
                .help(viewModel.isSceneEditableForAgents
                      ? "Send this JSON-RPC request to the live agent dispatcher"
                      : "Disabled while a production render is running — "
                        + "agent edits would race the render workers")
                Spacer()
            }

            Text("Response")
                .font(.caption2)
                .foregroundColor(.secondary)
            ScrollView {
                Text(viewModel.agentResponseText.isEmpty
                     ? "(no response yet)"
                     : viewModel.agentResponseText)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(viewModel.agentResponseText.isEmpty
                                     ? .secondary : .primary)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(4)
            }
            .frame(height: 72)
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(Color(nsColor: .separatorColor))
            )
        }
        .padding(.top, 4)
    }
}
