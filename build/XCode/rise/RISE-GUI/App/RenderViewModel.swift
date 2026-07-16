import SwiftUI
import Combine
import CoreGraphics
import AppKit
import UniformTypeIdentifiers

// RISEBridge is an Obj-C class; Swift can't auto-verify thread-safety.
// The methods captured across threads here — etaBegin, etaUpdateProgress,
// etaRemainingSeconds — are all guarded by a std::mutex on the C++ side.
// The one-way setup methods (setProgressBlock, setImageOutputBlock,
// rasterize) are called serially from the view model. Vouching for
// Sendable here silences the @Sendable-capture warnings without wrapping
// every call site.
extension RISEBridge: @unchecked Sendable {}

/// A single log message captured from the RISE engine.
struct LogMessage: Identifiable {
    let id = UUID()
    let level: RISELogLevel
    let text: String
    let timestamp: Date
}

enum RenderState: Equatable {
    case idle
    case loading
    case sceneLoaded
    case rendering
    case cancelling
    case completed
    case cancelled
    case error(String)
}

/// Which tab is frontmost in the left panel.  Lives on the view model
/// (moved from a ContentView-private `@State` in the P1-6 fix) so
/// `loadScene`'s failure branch can switch it programmatically —
/// ContentView-local state isn't reachable from RenderViewModel.
enum LeftPanelTab {
    case agent
    case sceneFile
}

/// Thread-safe image buffer that accumulates progressive RGBA16 render updates
/// and converts them to NSImage for display.
final class RenderImageBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var pixelBuffer: [UInt8] = []
    private var width: Int = 0
    private var height: Int = 0

    func reset() {
        lock.lock()
        pixelBuffer = []
        width = 0
        height = 0
        lock.unlock()
    }

    /// Called from the render thread. Converts RGBA16 region to RGBA8 and builds an NSImage.
    func handleOutput(
        pImageData: UnsafePointer<UInt16>,
        width: UInt32, height: UInt32,
        rcTop: UInt32, rcLeft: UInt32,
        rcBottom: UInt32, rcRight: UInt32
    ) -> NSImage? {
        let w = Int(width)
        let h = Int(height)
        let totalBytes = w * h * 4

        lock.lock()

        // Initialize on first call or dimension change
        if self.width != w || self.height != h {
            self.width = w
            self.height = h
            pixelBuffer = [UInt8](repeating: 0, count: totalBytes)
        }

        // Convert the updated region from 16-bit to 8-bit
        let top = Int(rcTop)
        let left = Int(rcLeft)
        let bottom = min(Int(rcBottom), h - 1)
        let right = min(Int(rcRight), w - 1)

        for y in top...bottom {
            for x in left...right {
                let idx = (y * w + x) * 4
                pixelBuffer[idx + 0] = UInt8(min(255, Int(pImageData[idx + 0]) >> 8))
                pixelBuffer[idx + 1] = UInt8(min(255, Int(pImageData[idx + 1]) >> 8))
                pixelBuffer[idx + 2] = UInt8(min(255, Int(pImageData[idx + 2]) >> 8))
                pixelBuffer[idx + 3] = 255
            }
        }

        // Build CGImage from current buffer state
        let data = Data(pixelBuffer)
        lock.unlock()

        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)

        guard let provider = CGDataProvider(data: data as CFData),
              let cgImage = CGImage(
                  width: w,
                  height: h,
                  bitsPerComponent: 8,
                  bitsPerPixel: 32,
                  bytesPerRow: w * 4,
                  space: colorSpace,
                  bitmapInfo: bitmapInfo,
                  provider: provider,
                  decode: nil,
                  shouldInterpolate: false,
                  intent: .defaultIntent
              ) else { return nil }

        return NSImage(cgImage: cgImage, size: NSSize(width: CGFloat(w), height: CGFloat(h)))
    }
}

/// Atomic flag for cross-thread cancellation signaling.
final class AtomicBool: @unchecked Sendable {
    private let lock = NSLock()
    private var _value: Bool

    init(_ value: Bool) { _value = value }

    var value: Bool {
        get { lock.lock(); defer { lock.unlock() }; return _value }
        set { lock.lock(); _value = newValue; lock.unlock() }
    }
}

/// Keeps a hot render worker from queuing an unbounded number of UI updates.
/// Each consumer receives the latest value available when it reaches the main
/// actor; producer-side cancellation checks remain synchronous.
final class CoalescedProgressDelivery: @unchecked Sendable {
    struct Update: Sendable {
        let progress: Double
        let total: Double
        let title: String
    }

    private let lock = NSLock()
    private var latest: Update? = nil
    private var deliveryScheduled = false

    /// Returns true exactly when the caller must schedule a main-actor drain.
    func submit(progress: Double, total: Double, title: String) -> Bool {
        lock.lock()
        latest = Update(progress: progress, total: total, title: title)
        defer { lock.unlock() }
        guard !deliveryScheduled else { return false }
        deliveryScheduled = true
        return true
    }

    /// Returns the newest pending value and opens the gate for a subsequent
    /// producer update to schedule the next main-actor drain.
    func takeLatest() -> Update? {
        lock.lock()
        defer { lock.unlock() }
        deliveryScheduled = false
        defer { latest = nil }
        return latest
    }
}

/// Same coalescing contract as `CoalescedProgressDelivery`, but for display
/// images produced by the background VFS polling queue.
final class CoalescedImageDelivery: @unchecked Sendable {
    private let lock = NSLock()
    private var latest: NSImage? = nil
    private var deliveryScheduled = false

    func submit(_ image: NSImage) -> Bool {
        lock.lock()
        latest = image
        defer { lock.unlock() }
        guard !deliveryScheduled else { return false }
        deliveryScheduled = true
        return true
    }

    func takeLatest() -> NSImage? {
        lock.lock()
        defer { lock.unlock() }
        deliveryScheduled = false
        defer { latest = nil }
        return latest
    }
}

@MainActor
final class RenderViewModel: ObservableObject {
    @Published var renderState: RenderState = .idle
    @Published var progress: Double = 0.0
    @Published var progressTitle: String = ""
    @Published var renderedImage: NSImage? = nil
    @Published var loadedFilePath: String? = nil
    /// Phase 6.5: true iff there's at least one in-memory edit since
    /// the last load / save that the SaveEngine would actually write
    /// to disk.  Drives the PropertiesPanel's "Save Scene" button
    /// enable state.  Mirrors the C++ controller's
    /// `HasUnsavedChanges()` via the bridge's `setDirtyChangedBlock`,
    /// updated only on transitions (clean→dirty / dirty→clean).
    @Published var sceneEditsDirty: Bool = false
    @Published var versionString: String = ""
    @Published var elapsedTime: TimeInterval = 0
    /// When the active rasterizer is the auto-dispatcher, the concrete
    /// integrator it resolved to ("pt"/"bdpt"/"vcm") and the one-line reason —
    /// surfaced as "Auto -> X" after a render.  nil for a normal rasterizer.
    @Published var resolvedIntegrator: String? = nil
    @Published var resolveReason: String? = nil
    // nil while the ETA is still warming up or otherwise unavailable.
    @Published var remainingTime: TimeInterval? = nil
    @Published var logMessages: [LogMessage] = []
    @Published var editorText: String = ""
    @Published var editorOriginalText: String = ""
    /// CST <-> scene-file live sync (item 1): true when the live CST has
    /// moved on (`getSceneTextVersionUuid:revision:` differs from the
    /// last-synced version) while the editor buffer has unsaved text
    /// edits (`isEditorDirty`), so the poll couldn't safely overwrite
    /// `editorText`/`editorOriginalText` without discarding them.
    /// Cleared whenever a sync succeeds or the buffer stops being dirty
    /// — see `pollRefinementState`.
    @Published var editorBehindLiveScene: Bool = false
    /// Which left-panel tab is frontmost — see `LeftPanelTab`.
    @Published var leftTab: LeftPanelTab = .agent

    /// "Reveal in scene file" (design comp ⌗ affordance): where to scroll
    /// + select in the Scene-file tab's editor, set by
    /// `revealEntityInSceneText`.  `generation` is a monotonic counter
    /// (not just the byte offset) so a REPEAT click on the SAME entity —
    /// same offset, buffer unchanged — still re-triggers the scroll/flash;
    /// without it, SceneTextEditor's Coordinator would see an unchanged
    /// value and skip the reveal on the second click.
    struct EditorRevealRequest {
        let byteOffset: UInt64
        /// UTF-8 byte length of the exact span to select+flash.  0 = select the
        /// whole line containing byteOffset (the entity/whole-chunk reveal); > 0 =
        /// highlight exactly [byteOffset, byteOffset+byteLength) (a param row).
        let byteLength: UInt64
        let generation: Int
    }
    @Published var editorRevealRequest: EditorRevealRequest? = nil
    private var nextEditorRevealGeneration = 1

    // Facet 5 slice 1c-1: the live agent (JSON-RPC) panel.  A minimal
    // "agent + user co-edit" affordance — a typed JSON-RPC request is
    // handed to the viewport bridge's `agentHandleLine`, which drives the
    // SAME live dispatcher/session/controller the GUI edits through, so a
    // `propose_patch` edits the running scene and the viewport reflects it.
    //
    // Superseded by the Chat panel for everyday use (2026-07); kept ONLY
    // as the raw-wire debug surface against the LIVE controller (the
    // headless CLI agent session is a separate process/session and isn't
    // affected either way).  Gated behind a developer toggle, default
    // OFF — see `showAgentDebugPanel` below.  (The panel's own
    // show/hide toggle, `isAgentPanelVisible`, was removed as dead state
    // in the UI redesign's P3 cleanup — the Agent tab's visibility is
    // driven entirely by `showAgentDebugPanel` now; see ContentView's
    // `agentTabBody`.)

    /// Developer toggle: reveals the Agent (JSON-RPC) debug panel's
    /// button + inline panel at all.  Persisted in UserDefaults (a UI
    /// preference, not a secret) so it survives relaunch; default OFF —
    /// the Chat panel is the everyday surface, this is a wire-debug tool.
    @Published var showAgentDebugPanel: Bool = false {
        didSet {
            UserDefaults.standard.set(showAgentDebugPanel, forKey: Self.showAgentDebugPanelKey)
        }
    }
    /// The JSON-RPC request text the user is composing.
    @Published var agentRequestText: String =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"read_document\"}"
    /// The last JSON-RPC response line (read-only display).
    @Published var agentResponseText: String = ""

    // Facet 5 slice B2: the LLM chat panel.  All chat state + the
    // turn driver live in ChatViewModel (which wraps the C++
    // AgentChatLoop via RISEAgentChatBridge); this view model only
    // owns its lifetime and forwards the per-scene bind/unbind so the
    // chat driver never touches a torn-down viewport bridge.  (The
    // panel's own show/hide toggle, `isChatPanelVisible`, was removed
    // as dead state in the UI redesign's P3 cleanup — the Agent tab is
    // permanently part of the left panel now, not a togglable overlay.)
    /// App-lifetime chat state (provider/model selection outlives any
    /// one scene; the conversation itself resets per scene).
    let chat = ChatViewModel()
    /// Named-views and other scene controls observe this view model, while
    /// the chat render-in-flight state lives on `chat`.  Forward that change
    /// notification so their `isSceneEditableForAgents` bindings invalidate
    /// as soon as a chat-owned render starts or ends.
    private var chatStateChangeObserver: AnyCancellable? = nil

    /// B2 review round 1 (refined Model-B F2 slice S2b — see the S2b note
    /// below): may agent tool calls (the chat driver and the Agent
    /// JSON-RPC panel) mutate the live scene RIGHT NOW?  THE single gate:
    /// both call sites — `ChatViewModel.sceneEditable` (wired in `init`
    /// below) and `sendAgentRequest` (the raw JSON-RPC debug panel) —
    /// consume this ONE property.  It in turn is built from
    /// `isProductionRenderRunning`, the SINGLE source for the
    /// `renderState` case-list — neither this property nor
    /// `productionRenderActive` below hand-copies that list a second
    /// time (see `isProductionRenderRunning`'s doc for the S2b P2-1
    /// fix that single-sourced it).
    ///
    /// Only while the interactive viewport is the sole scene reader.
    /// During a production render (.rendering / .cancelling — the
    /// production rasterizer's worker threads read Scene state
    /// off-main until the render task completes) a propose_patch's
    /// CST re-derive would destroy scene objects under those workers
    /// — the same race the Clear & Load teardown dance guards (the
    /// "deep in IntegratePixel" crash).  .loading is excluded too:
    /// the parser mutates job state on a background thread (the chat
    /// executor is nil then anyway — belt and suspenders).
    /// MainActor invariant: this predicate, startRender, and every
    /// caller run on the main actor, so check-then-execute cannot
    /// interleave with a render kick.
    ///
    /// Model-B F2 slice S2b ADDITIVE clause: `chat.isChatRenderOutstanding`
    /// folds in truth `isProductionRenderRunning` cannot see on its
    /// own — a chat-driven `render` tool call's async render runs on the
    /// controller's dedicated agent-render worker WITHOUT ever flipping
    /// `renderState` out of `.sceneLoaded`/`.completed` (that state
    /// machine only tracks the PRODUCTION rasterizer).  Without this
    /// clause, the Agent JSON-RPC debug panel could fire a `propose_patch`
    /// (or a second render) concurrently with a chat-driven render still
    /// in flight on the SAME controller.  This is genuinely NEW truth,
    /// not a redundant restatement of the renderState cases — the two
    /// clauses gate two DIFFERENT concurrent-render sources (production
    /// rasterizer vs. chat-driven agent render) that share the same
    /// underlying "don't let two things touch the scene/controller at
    /// once" concern.
    var isSceneEditableForAgents: Bool {
        !chat.isChatRenderOutstanding && !isProductionRenderRunning
    }

    /// P1-1 fix: single-sourced gate for the refinement-transport +
    /// undo/redo controls (TopBar, Render menu, Edit menu).  Before this
    /// property existed, `pauseRefinement`/`resumeRefinement`/
    /// `restartRefinement`/`undo`/`redo` each guarded on
    /// `!isProductionRenderRunning` alone — which does NOT cover a
    /// chat-driven agent render outstanding on the controller's
    /// single-slot worker (see `isSceneEditableForAgents`'s S2b clause).
    /// A user could hit Undo, or the TopBar pause button, while a chat
    /// `render` tool call's async render was still in flight, racing the
    /// SAME controller the chat driver's own tool calls touch.  This
    /// property folds in that ADDITIVE clause by reusing
    /// `isSceneEditableForAgents` wholesale (not a hand-copy) plus the
    /// bridge-presence check every one of those five callers already
    /// needed.
    var canUseSceneTransport: Bool { viewportBridge != nil && isSceneEditableForAgents }

    /// Reverse source traceability (text -> UI select): may a right-click
    /// "Select in Inspector" act right now?  Needs the scene transport (no
    /// render owns the controller) AND a clean editor buffer -- the click's
    /// byte offset comes from the buffer but resolves against the live CST
    /// serialization, so they must agree.  The SINGLE source of truth for
    /// both the menu-item enablement (so the affordance is "right or absent,
    /// never misleading", matching the forward reveal affordances) and the
    /// `reverseSelectEntity` guard.
    var canReverseSelect: Bool { canUseSceneTransport && !isEditorDirty }

    /// S2b P2-1: THE single source for the `renderState` case-list that
    /// means "a production render is currently occupying the
    /// controller/scene" — `.rendering`, `.cancelling`, `.loading`.
    /// Both `isSceneEditableForAgents` above and `productionRenderActive`
    /// below (wired into `ChatViewModel` in `init`) read this ONE
    /// property instead of each hand-copying the same three-case
    /// switch — the exact duplication the "one gate" goal exists to
    /// kill, caught in S2b review as P2-1 (both copies lived on this
    /// same type, so drift between them was a `case` away).
    private var isProductionRenderRunning: Bool {
        switch renderState {
        case .rendering, .cancelling, .loading:
            return true
        default:
            return false
        }
    }
    @Published var hasAnimation: Bool = false
    @Published var recentFiles: [String] = []
    /// Last-opened times for `recentFiles` entries (start-screen "2m ago"
    /// labels).  Persisted under the sibling `recentSceneMeta` key; a path
    /// with no entry renders without a time (old installs).
    @Published var recentFileTimes: [String: Date] = [:]

    /// Live time-scrubber state, displayed on the viewport's bottom slider.
    @Published var sceneTime: Double = 0

    /// True while the Play button is looping the active animation through
    /// the fast interactive preview renderer (frame-by-frame, looping until
    /// stopped).  Distinct from `renderState == .rendering` (production).
    @Published var isPreviewPlaying: Bool = false

    // L5a — EDR (extended dynamic range) state.
    //
    // `edrAvailable` mirrors the active screen's
    // `maximumExtendedDynamicRangeColorComponentValue > 1.0`.  If
    // false (e.g. a vanilla SDR display), the EDR toggle in the UI
    // stays disabled regardless of `edrEnabled`.
    //
    // `edrEnabled` is the user's toggle.  When both are true, the
    // bridge fires the HDR block (binary16 in extended-linear-sRGB)
    // and `RenderImageView` swaps in `MetalEDRView`; otherwise the
    // legacy LDR `Image(nsImage:)` path runs.  Toggling at any
    // time is supported — the bridge's `setHDREnabled:` triggers
    // an immediate Repaint so the screen reflects the new mode
    // without waiting for the next render pass.
    @Published var edrAvailable: Bool = false
    @Published var edrEnabled:   Bool = false {
        didSet {
            bridge.setHDREnabled(edrEnabled && edrAvailable)
        }
    }

    // L5e — LDR preview controls.  Both apply at display read-back
    // time only (no rasterizer re-run); the bridge's setters trip
    // an immediate Repaint so the on-screen image swaps live.
    //
    // `viewExposureEV` clamps to [-6, +6] in the slider UI but the
    // Published value isn't itself clamped — programmatic resets to
    // 0 (double-click on the slider track) and any future scene-
    // metadata exposure import won't fight the clamp.  Range chosen
    // to match the convergent 12-stop dynamic range of typical
    // scene-referred radiance values without making the slider's
    // sub-pixel granularity feel too coarse.
    //
    // `viewToneCurve` raw int matches RISE's DISPLAY_TRANSFORM
    // enum (0 None, 1 Reinhard, 2 ACES, 3 AgX, 4 Hable).  Default
    // 2 (ACES) is the modern preview standard — see L5e design
    // briefing in the conversation log.  Stored as Int instead of
    // a Swift enum so the same value can flow through the
    // bridge's `setViewToneCurve:` ObjC method without an extra
    // mapping layer.
    @Published var viewExposureEV: Double = 0.0 {
        didSet {
            bridge.setViewExposureEV(viewExposureEV)
        }
    }
    @Published var viewToneCurve: Int = 2 /* ACES */ {
        didSet {
            bridge.setViewToneCurve(Int32(viewToneCurve))
        }
    }

    /// Returns the underlying RISEBridge — needed by the SwiftUI
    /// `MetalEDRView` so it can hook the HDR block + push the
    /// resulting frames through Metal.  Callers MUST NOT retain
    /// this bridge; its lifetime is bounded by the RenderViewModel.
    var bridgeForEDR: RISEBridge { bridge }

    /// L5a round-5 — TWO durable EDR renderers, one per bridge
    /// path.  Production renderer drives the "main image" Metal
    /// layer (per-tile + per-frame production fires).  Interactive
    /// renderer drives an "overlay" Metal layer with transparent
    /// background that composites over production (frame-complete
    /// only — no per-tile flash, no preview-scale resolution
    /// thrash).  Both built lazily at init when Metal is
    /// available; nil on Metal-incapable hosts.
    var productionEDRRenderer:  MetalEDRRenderer?
    var interactiveEDRRenderer: MetalEDRRenderer?

    /// Back-compat alias for callers that just want the
    /// "primary" EDR renderer (used by the no-scene-loaded
    /// `RenderImageView` placeholder, which only has a single
    /// surface to fill).  Defaults to production.
    var edrRenderer: MetalEDRRenderer? { productionEDRRenderer }

    /// L5a round-2 P1-3 — handle to the host NSWindow used for
    /// per-screen EDR-headroom probing (vs `mainScreen` which
    /// follows keyboard focus, not the RISE window).  Set by
    /// SwiftUI via the window-tracking onAppear handler in
    /// ContentView; unset on window-will-close.
    weak var hostWindow: NSWindow? = nil {
        didSet {
            // (Re-)subscribe to the new window's screen-change
            // notification.  Old subscription is leaked-as-token
            // since we drop the reference; NotificationCenter
            // tolerates that.
            if let win = hostWindow {
                NotificationCenter.default.addObserver(
                    forName: NSWindow.didChangeScreenNotification,
                    object: win, queue: .main
                ) { [weak self] _ in
                    Task { @MainActor [weak self] in
                        self?.refreshEDRAvailability()
                    }
                }
            }
            refreshEDRAvailability()
        }
    }

    private let bridge = RISEBridge()

    // MARK: - UI redesign (design brief A2/A4): refinement status, undo/redo, panel toggles

    /// Interactive-refinement phase, mirrored from
    /// `RISEViewportBridge.refinementPhase(scaleDivisor:)`:
    /// -1 no controller, 0 Idle, 1 Rendering, 2 Refining, 3 Polishing,
    /// 4 Paused.  Driven by `refinementPollTimer` below; TopBar reads
    /// this to build its status readout.
    @Published var refinementPhase: Int = -1
    /// Preview-resolution divisor accompanying `refinementPhase` — a
    /// power of two in 1...32 (1 = full res).  Only meaningful while
    /// `refinementPhase == 2` (Refining); TopBar derives a "rung R/6"
    /// label from it (R = 6 - log2(divisor)).
    @Published var refinementScaleDivisor: UInt32 = 1
    /// Mirrors `RISEViewportBridge.isRefinementPaused()`.
    /// UI redesign design brief A4 — the active interactive region,
    /// in full-res FILM-PIXEL space (matches
    /// `RISEViewportBridge.setInteractiveRegionLeft:top:right:bottom:`),
    /// or nil when no region is set.  Mirrored from
    /// `getInteractiveRegionLeft:top:right:bottom:` by the same
    /// `refinementPollTimer` cadence as `refinementPhase` below, and
    /// updated immediately (bypassing the poll) by ViewportView right
    /// after a region drag commits, for instant UI feedback.  The
    /// core auto-clears the region before any production render;
    /// this poll picks that up within 0.5s like every other
    /// refinement-status field.
    @Published var activeRegion: CGRect? = nil
    /// Mirrors `RISEViewportBridge.undoActionLabel()` / `redoActionLabel()`
    /// — kept as published state (rather than a computed passthrough) so
    /// the Edit-menu item titles update reliably off the same poll that
    /// drives the refinement readout, independent of whatever else
    /// happens to trigger a SwiftUI re-render.
    @Published var undoLabel: String = ""
    @Published var redoLabel: String = ""

    /// Entity-creation slice: bumped after a successful `addEntity` /
    /// `duplicateSelectedOrNamed` / `removeEntity`.  `OutlinerView`
    /// observes it via `@EnvironmentObject` and force-repulls its
    /// category entity lists, giving an IMMEDIATE, deterministic refresh
    /// the moment a GUI CRUD commits (rather than waiting for the next
    /// epoch-driven reload).  The core additionally bumps the
    /// controller's `sceneEpoch` on every landed chunk CRUD now
    /// (ApplyAgentChunkCrud_), which is the mechanism that also refreshes
    /// the outliner for AGENT-driven adds/removes — those never touch
    /// this counter.
    @Published var entityListEpoch: Int = 0

    /// Reverse source traceability (text -> UI select): bumped when a
    /// right-click "Select in Inspector" in the scene-file editor resolves
    /// to an entity and selects it on the bridge.  ContentView observes this
    /// and bumps its shared `propertyRefresh`, so the inspector re-snapshots
    /// and the outliner's highlight follows -- selection lives on the bridge
    /// and needs an explicit refresh nudge (a reverse-select changes no
    /// pixels, so no rendered-frame refresh fires).  See `reverseSelectEntity`.
    @Published var reverseSelectEpoch: Int = 0

    /// View menu toggles (design brief workspace chrome).  Both default
    /// on; persistence isn't required for slice 1.
    @Published var showLeftPanel: Bool = true
    @Published var showLogDrawer: Bool = true

    /// ~2 Hz poll of the interactive controller's refinement/undo state
    /// while a viewport bridge is attached.  Started/stopped from
    /// `viewportBridge`'s didSet so every one of the four assignment
    /// sites (`loadScene`, `continueClearAndLoad`,
    /// `finishSaveAndReload`, `clearScene`) tracks bridge lifetime
    /// automatically — mirrors the `hostWindow` didSet pattern above.
    private var refinementPollTimer: Timer? = nil

    /// CST <-> scene-file live sync — cheap change-detector pair from
    /// `RISEViewportBridge.getSceneTextVersionUuid:revision:`.  uuid is
    /// fresh per load; revision bumps iff content changed.
    private struct SceneTextVersion: Equatable {
        let uuid: UInt64
        let revision: UInt64
    }

    /// The version last mirrored into `editorText`/`editorOriginalText`
    /// (item 1), or set at load time so the file-on-disk bytes and the
    /// live CST start in agreement.  nil before any scene has loaded.
    private var lastSyncedSceneTextVersion: SceneTextVersion? = nil

    /// Reads the live CST's current (uuid, revision) pair.  Same
    /// do-not-call-during-renders caveat as `serializedSceneText` — only
    /// call while `isSceneEditableForAgents`.
    private func currentSceneTextVersion(_ vb: RISEViewportBridge) -> SceneTextVersion? {
        var uuid: UInt64 = 0
        var revision: UInt64 = 0
        guard vb.getSceneTextVersionUuid(&uuid, revision: &revision) else { return nil }
        return SceneTextVersion(uuid: uuid, revision: revision)
    }

    /// Lazily constructed when a scene successfully loads; torn down on
    /// clearScene().  The viewport bridge borrows `bridge`'s job — its
    /// lifetime must not exceed `bridge`'s.
    private(set) var viewportBridge: RISEViewportBridge? = nil {
        didSet {
            refinementPollTimer?.invalidate()
            refinementPollTimer = nil
            guard let vb = viewportBridge else {
                refinementPhase = -1
                refinementScaleDivisor = 1
                undoLabel = ""
                redoLabel = ""
                activeRegion = nil
                lastSyncedSceneTextVersion = nil
                editorBehindLiveScene = false
                return
            }
            // CST <-> scene-file live sync (item 1): a freshly-attached
            // bridge's current version becomes the sync baseline — the
            // bytes that were just loaded and the live CST agree
            // at this instant, so there is nothing to pull into the
            // editor yet.  Only a LATER agent/GUI edit that bumps the
            // revision should overwrite `editorText`.  Also resets the
            // per-scene behind-live-scene state so it doesn't leak
            // across scene loads.  Gated on `isSceneEditableForAgents`
            // like every other `serializedSceneText`-family call — every
            // assignment site sets `viewportBridge` only after
            // `renderState` has already left `.loading`, so this is
            // always safe in practice, but the gate is kept anyway
            // rather than assuming that invariant holds forever.
            lastSyncedSceneTextVersion = isSceneEditableForAgents ? currentSceneTextVersion(vb) : nil
            editorBehindLiveScene = false
            pollRefinementState(vb)
            // Re-read `self.viewportBridge` inside the Task rather than
            // capturing the local `vb` across the closure boundary —
            // RISEViewportBridge isn't Sendable (unlike RISEBridge,
            // which is vouched-for at this file's top), so a weak
            // capture of it in a @Sendable Task closure trips Swift
            // concurrency's capture check.
            let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
                guard let self else { return }
                Task { @MainActor [weak self] in
                    guard let self, let vb = self.viewportBridge else { return }
                    self.pollRefinementState(vb)
                }
            }
            RunLoop.main.add(timer, forMode: .common)
            refinementPollTimer = timer
        }
    }
    /// Drives the looping preview-play frame stepper (Play button).  Nil
    /// when not playing; cancelled + niled by stopPreviewPlay().
    private var previewPlayTask: Task<Void, Never>? = nil
    private let cancelFlag = AtomicBool(false)
    private let imageBuffer = RenderImageBuffer()
    private var renderStartTime: Date? = nil
    private var displayTimer: Timer? = nil
    /// L8 round 9 / 16 — drives the lockless progressive-update path.
    /// Worker threads no longer fire per-tile observer callbacks
    /// into the bridge; instead this Timer polls the production
    /// VFS's atomic generation counter at ~30 Hz.  When the counter
    /// advances the bridge emits one full-image refresh.  Started
    /// in startRender / start*Render, invalidated in finishRender
    /// + cancel paths.
    ///
    /// Round 16 — the Timer fires on the main run loop (cheap;
    /// just checks `pollInFlight` atomic + dispatches to
    /// `pollQueue`), but the actual poll work (`pollProductionVFS`
    /// → RenderToBuffer → handleOutput → NSImage create) runs on
    /// `pollQueue`, a serial `.userInteractive` background queue.
    /// Pre-round-16 the whole poll ran on the main thread —
    /// ~20 ms / tick × 30 Hz = 60% main-thread utilisation, which
    /// felt like a perf regression (UI jitter) even though workers
    /// were unaffected.
    private var progressivePollTimer: Timer? = nil

    /// L8 round 16 — serial background queue for the heavy poll
    /// work.  Serial so only one poll runs at a time
    /// (`pollProductionVFS` is internally re-entrant-safe via
    /// `bufferMutex_`, but funnelling through a serial queue
    /// avoids needing to skip-on-busy at the dispatch level).
    private let pollQueue = DispatchQueue(
        label: "rise.progressive-poll", qos: .userInteractive)

    /// Re-entrancy guard for the round-16 dispatch.  Atomically
    /// flipped to `true` when a poll is queued; flipped back to
    /// `false` when the queued poll completes.  Subsequent Timer
    /// fires while `true` skip the dispatch — backlog avoidance.
    private let pollInFlight = AtomicBool(false)

    private var renderTask: Task<Void, Never>? = nil

    private static let maxLogMessages = 10000
    private static let maxRecentFiles = 10
    private static let recentFilesKey = "recentSceneFiles"
    /// Start-screen spec §5.4: SIBLING key to `recentFilesKey` mapping
    /// path -> last-opened unix time, so the start screen can show "2m ago"
    /// labels.  Deliberately a separate key (not a schema change to the
    /// shared path list) so old installs and the Windows client's identical
    /// `recentSceneFiles` list stay compatible; a path with no meta entry
    /// simply renders without a time.
    private static let recentFilesMetaKey = "recentSceneMeta"
    private static let showAgentDebugPanelKey = "showAgentDebugPanel"

    init() {
        versionString = RISEBridge.versionString()
        recentFiles = UserDefaults.standard.stringArray(forKey: Self.recentFilesKey) ?? []
        if let meta = UserDefaults.standard.dictionary(forKey: Self.recentFilesMetaKey) as? [String: Double] {
            recentFileTimes = meta.mapValues { Date(timeIntervalSince1970: $0) }
        }
        showAgentDebugPanel = UserDefaults.standard.bool(forKey: Self.showAgentDebugPanelKey)
        chatStateChangeObserver = chat.objectWillChange.sink { [weak self] _ in
            self?.objectWillChange.send()
        }
        // B2 review round 1: the chat driver re-checks this predicate
        // after every await and before each tool call, so a turn
        // resuming from its HTTP suspension can never mutate the
        // scene under a production render (the predicate's other
        // half: startRender / startAnimationRender cancel the turn
        // BEFORE kicking production).
        chat.sceneEditable = { [weak self] in
            self?.isSceneEditableForAgents ?? false
        }
        // Model-B F2 slice S2b: the NARROWER sibling `productionRenderActive`
        // — see its doc on ChatViewModel for why the render tool call's own
        // poll loop needs this instead of the combined `isSceneEditableForAgents`
        // (which now also reports false for that SAME call's own outstanding
        // render).  S2b P2-1: forwards to `isProductionRenderRunning`, the
        // SAME single-sourced case-list `isSceneEditableForAgents` reads —
        // no second hand-copy of the `renderState` switch.
        chat.productionRenderActive = { [weak self] in
            guard let self else { return true }   // fail closed, matching sceneEditable's convention
            return self.isProductionRenderRunning
        }
        bridge.setLogBlock { [weak self] (level: RISELogLevel, message: String) in
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                let entry = LogMessage(level: level, text: message, timestamp: Date())
                self.logMessages.append(entry)
                // Trim oldest entries to prevent unbounded growth during long renders
                if self.logMessages.count > Self.maxLogMessages {
                    self.logMessages.removeFirst(self.logMessages.count - Self.maxLogMessages)
                }
            }
        }

        // L5a round-5 — two durable EDR renderers, one per role.
        // Each binds to its respective bridge HDR-block slot once
        // for the renderer's lifetime; both attach to their own
        // CAMetalLayer (stacked production-bottom / interactive-
        // top) when ViewportNSView lays them out.  nil if Metal
        // is unavailable; in that case `edrAvailable` stays false
        // and the toggle is disabled.
        self.productionEDRRenderer  = MetalEDRRenderer(bridge: bridge,
                                                       role: .production)
        self.interactiveEDRRenderer = MetalEDRRenderer(bridge: bridge,
                                                       role: .interactive)

        // L5a — initial EDR availability probe + subscribe to
        // screen-config changes so the toggle dims/lights as the
        // monitor configuration changes (resolution, attach/detach).
        // Per-window screen tracking (window-dragged-to-different-
        // monitor) is set up in `hostWindow.didSet` once SwiftUI
        // hands us the window reference (round-2 P1-3 fix —
        // `[NSScreen mainScreen]` follows keyboard focus, not the
        // RISE window).
        refreshEDRAvailability()
        NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refreshEDRAvailability()
            }
        }
    }

    /// L5a — refresh `edrAvailable` from the screen the RISE
    /// window is currently on.  Falls back to `[NSScreen
    /// mainScreen]` (keyboard-focus screen) when no host window
    /// is registered yet — typical at app launch before SwiftUI
    /// attaches the window reference.
    ///
    /// Round-3 fix: probe `maximumPotentialExtendedDynamicRange…`
    /// (CAPABILITY) instead of `maximumExtendedDynamicRange…`
    /// (CURRENT, gated on the OS having actually transitioned the
    /// screen into EDR mode).  The previous code read `current`,
    /// which returns 1.0 for many EDR-capable Macs while the
    /// screen is in SDR mode — making the toggle un-checkable
    /// even though setting `wantsExtendedDynamicRangeContent =
    /// YES` would trigger the transition automatically.  Headroom
    /// values: ~1.6 on a recent MacBook Pro mini-LED in
    /// SDR-Reference mode, ~5.5 with HDR enabled in System
    /// Settings, up to ~16.0 on a Pro Display XDR.  When toggled
    /// off (e.g. window dragged to an SDR monitor), force
    /// `edrEnabled = false` so the bridge switches back to the
    /// LDR pipeline immediately.
    func refreshEDRAvailability() {
        // Prefer the host window's screen if registered; the
        // bridge's `displayMaxEDRHeadroom` reads `mainScreen`
        // which follows focus rather than the window.
        let headroom: Float
        if let screen = hostWindow?.screen {
            headroom = Float(screen.maximumPotentialExtendedDynamicRangeColorComponentValue)
        } else {
            headroom = bridge.displayMaxEDRHeadroom()
        }
        // Renderer must also be available — Metal-incapable hosts
        // (extremely rare on modern Macs) have productionEDRRenderer
        // == nil even on EDR-capable screens.
        let available = headroom > 1.0 && productionEDRRenderer != nil
        let availabilityChanged = (edrAvailable != available)
        if availabilityChanged {
            edrAvailable = available
        }
        if !available && edrEnabled {
            edrEnabled = false   // setter pushes setHDREnabled:NO
        }
        // L5a round-3 — when EDR transitions from unavailable → available
        // (typical at app launch on an EDR-capable Mac after the host
        // window attaches and the screen probe runs), auto-enable the
        // toggle so the user gets EDR rendering by default.  This is
        // the user-friendly "if your hardware supports it, use it"
        // policy.  The user can still toggle it off manually; they can
        // also toggle it back on if it auto-flipped off due to a
        // window-screen change.  We only auto-enable on the false→true
        // transition (not on every refresh) so we don't fight the user
        // if they explicitly disabled the toggle while available.
        if availabilityChanged && available && !edrEnabled {
            edrEnabled = true   // setter pushes setHDREnabled:YES
        }
    }

    // MARK: - Public Actions

    func openScene() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [
            UTType(filenameExtension: "RISEscene") ?? .data
        ]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.message = "Select a RISE Scene File"

        guard panel.runModal() == .OK, let url = panel.url else { return }
        prepareAndLoadScene(at: url.path)
    }

    func openRecentScene(at path: String) {
        guard FileManager.default.fileExists(atPath: path) else {
            // Remove stale entry
            recentFiles.removeAll { $0 == path }
            UserDefaults.standard.set(recentFiles, forKey: Self.recentFilesKey)
            return
        }
        prepareAndLoadScene(at: path)
    }

    private func prepareAndLoadScene(at path: String) {
        // If the editor has unsaved changes, prompt the user before
        // switching.  UI redesign: the scene-file editor is now a
        // permanently-available left-panel tab (not a togglable
        // sidebar), so the guard is purely dirty-driven rather than
        // also checking whether the editor happens to be the
        // frontmost tab right now.
        if isEditorDirty {
            let saveAlert = NSAlert()
            saveAlert.messageText = "Unsaved Changes"
            saveAlert.informativeText = "The scene editor has unsaved changes. Would you like to save them before loading a new scene?"
            saveAlert.alertStyle = .warning
            saveAlert.addButton(withTitle: "Save")
            saveAlert.addButton(withTitle: "Discard")
            saveAlert.addButton(withTitle: "Cancel")

            let saveResponse = saveAlert.runModal()
            switch saveResponse {
            case .alertFirstButtonReturn:
                saveEditorFile()
            case .alertSecondButtonReturn:
                break
            default:
                return
            }
        }

        // If a scene is already loaded, confirm the clear-and-load.
        //
        // The pre-CST "Merge" option is GONE (2026-07-12, in lockstep with
        // the Windows client): every GUI load routes through the canonical
        // CST path, whose load-once guard refuses a second load into a
        // populated Job (Job::LoadAsciiSceneViaCst: "this Job already
        // loaded a scene; re-load is not supported") -- the old Merge
        // button could only ever end in the load-failure alert.  If scene
        // merging is ever wanted, it is a CST-document-level feature, not
        // a second parser pass over the live Job.
        if loadedFilePath != nil {
            let alert = NSAlert()
            alert.messageText = "A scene is already loaded"
            alert.informativeText = "Clear the current scene and load the new one?"
            alert.alertStyle = .warning
            alert.addButton(withTitle: "Clear & Load")
            alert.addButton(withTitle: "Cancel")

            let response = alert.runModal()
            guard response == .alertFirstButtonReturn else { return }
            // Clear & Load.  We must:
            //   1. Cancel any in-flight production render and await
            //      its task — workers spawned by the rasterizer hold
            //      pointers into Scene state that bridge.clearAll
            //      is about to destroy.
            //   2. Stop and shutdown the interactive viewport bridge
            //      — its render thread also reads Scene state.
            //   3. THEN call clearAll, which is now safe to destroy
            //      managers because no thread can still be reading
            //      them.
            // saveAndReloadScene already implements this dance via
            // finishSaveAndReload; we mirror it here.  Pre-fix, the
            // production-render Tasks parked in workerLoop after
            // rasterize() returned would still be reachable through
            // the controller's interactive render thread (and
            // through the production rasterizer's persisted thread
            // pool), and clearAll would race them, manifesting as a
            // crash deep in IntegratePixel / DestroyContainers.
            if renderState == .rendering || renderState == .cancelling {
                cancelFlag.value = true
                renderState = .cancelling
                let task = renderTask
                Task { @MainActor [weak self] in
                    await task?.value
                    self?.continueClearAndLoad(at: path)
                }
                return
            }
            continueClearAndLoad(at: path)
            return
        }

        loadScene(at: path)
    }

    /// Clear the current scene's state with the right teardown
    /// ordering, then load the new file.  Called from
    /// prepareAndLoadScene's "Clear & Load" branch — extracted so the
    /// render-still-in-flight path (which has to await the cancelled
    /// task before continuing) and the idle path can share the body.
    private func continueClearAndLoad(at path: String) {
        // Stop and tear down the interactive viewport bridge BEFORE
        // bridge.clearAll().  shutdown() joins the controller's render
        // thread; once it returns, no other thread holds pointers into
        // Scene state that clearAll is about to destroy.
        stopPreviewPlay()   // halt a looping preview-play before the bridge it drives is torn down
        chat.sceneClosed()  // stop the chat driver before its tool executor is torn down
        viewportBridge?.shutdown()
        viewportBridge = nil

        bridge.clearAll()
        renderedImage = nil
        progress = 0.0
        progressTitle = ""
        elapsedTime = 0
        remainingTime = nil
        imageBuffer.reset()
        logMessages.removeAll()

        loadScene(at: path)
    }

    func addToRecentFiles(_ path: String) {
        recentFiles.removeAll { $0 == path }
        recentFiles.insert(path, at: 0)
        if recentFiles.count > Self.maxRecentFiles {
            recentFiles = Array(recentFiles.prefix(Self.maxRecentFiles))
        }
        recentFileTimes[path] = Date()
        // Prune meta for paths that fell off the list so the sibling key
        // stays bounded by maxRecentFiles rather than growing forever.
        recentFileTimes = recentFileTimes.filter { recentFiles.contains($0.key) }
        persistRecents()
    }

    /// Start screen: remove one entry (the ✕ on a missing-file row).
    func removeRecentFile(_ path: String) {
        recentFiles.removeAll { $0 == path }
        recentFileTimes.removeValue(forKey: path)
        persistRecents()
    }

    private func persistRecents() {
        UserDefaults.standard.set(recentFiles, forKey: Self.recentFilesKey)
        UserDefaults.standard.set(
            recentFileTimes.mapValues { $0.timeIntervalSince1970 },
            forKey: Self.recentFilesMetaKey)
    }

    func clearRecentFiles() {
        recentFiles.removeAll()
        recentFileTimes.removeAll()
        UserDefaults.standard.removeObject(forKey: Self.recentFilesKey)
        UserDefaults.standard.removeObject(forKey: Self.recentFilesMetaKey)
    }

    /// Load a scene from `path`.  `untitled` (start-screen create path):
    /// the file is a bundled template — the loaded scene has NO document
    /// path (`loadedFilePath` stays nil, Save routes to Save-As) and never
    /// enters the recents list.  Everything else (controller, bridge, CST
    /// retention) is identical to a normal file open by design, so the
    /// agent edits an ordinary live scene.
    func loadScene(at path: String, untitled: Bool = false) {
        if !untitled {
            addToRecentFiles(path)
        }
        renderState = .loading
        renderedImage = nil
        progress = 0.0
        cancelFlag.value = false

        // Add the scene file's directory as a media path
        let sceneDir = (path as NSString).deletingLastPathComponent
        bridge.addMediaPath(sceneDir)

        // Walk up from the scene directory to find the RISE project root
        // (identified by the presence of global.options).
        // This sets RISE_MEDIA_PATH env var needed by FileRasterizerOutput.
        var searchDir = sceneDir
        while !searchDir.isEmpty && searchDir != "/" {
            let candidatePath = (searchDir as NSString).appendingPathComponent("global.options")
            if FileManager.default.fileExists(atPath: candidatePath) {
                bridge.setProjectRoot(searchDir)
                break
            }
            searchDir = (searchDir as NSString).deletingLastPathComponent
        }

        let cancelRef = cancelFlag
        let progressDelivery = CoalescedProgressDelivery()
        bridge.setProgressBlock { [weak self] (prog: Double, total: Double, title: String) -> Bool in
            if progressDelivery.submit(progress: prog, total: total, title: title) {
                Task { @MainActor [weak self] in
                    guard let update = progressDelivery.takeLatest() else { return }
                    guard let self = self else { return }
                    self.progress = update.total > 0 ? update.progress / update.total : 0
                    self.progressTitle = update.title
                }
            }
            return !cancelRef.value
        }

        let bridgeRef = bridge
        Task.detached { [weak self] in
            let success = bridgeRef.loadAsciiScene(path)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                // Untitled (start-screen create): the scene came from the
                // bundled starter template — it has no user-facing document
                // path.  TopBar shows "Untitled", Save routes to Save-As.
                self.loadedFilePath = untitled ? nil : path
                if success {
                    self.renderState = .sceneLoaded
                    self.progressTitle = ""
                    self.hasAnimation = bridgeRef.hasAnimatedObjects()
                    // Tear down any previous viewport bridge (e.g. from
                    // a prior scene) and stand up a fresh one over the
                    // newly-loaded job.
                    self.stopPreviewPlay()   // halt any looping preview-play before swapping the bridge
                    self.chat.sceneClosed()  // stop the chat driver before its tool executor is torn down
                    self.viewportBridge?.shutdown()
                    let vb = RISEViewportBridge(hostBridge: bridgeRef)
                    self.viewportBridge = vb
                    self.sceneTime = 0
                    // Wire the live-preview image callback.  The block
                    // is invoked on the main thread by the bridge.
                    vb?.setImageBlock { [weak self] (image: NSImage) in
                        guard let self = self else { return }
                        self.renderedImage = image
                    }
                    // Phase 6.5: track dirty edits so the Properties-
                    // panel's Save button can enable/disable.  The
                    // bridge always marshals the block onto the main
                    // queue (see -_fireDirtyChangedFromBackground:),
                    // so a direct @Published assignment is safe.
                    // Reset to false on fresh scene load — even if a
                    // previous scene was dirty, this scene starts clean.
                    self.sceneEditsDirty = false
                    vb?.setDirtyChangedBlock { [weak self] (hasUnsaved: Bool) in
                        guard let self = self else { return }
                        self.sceneEditsDirty = hasUnsaved
                    }
                    // Override the scene's authored Film with a screen-
                    // appropriate size for the INTERACTIVE preview.  The
                    // available surface is the screen's visible area
                    // (main NSScreen minus menubar / Dock); the long
                    // edge is capped at 800 px so we never burn cycles
                    // pushing pixels the viewport view would just
                    // downscale.  Must run BEFORE start() so the first
                    // render pass picks up the new dims.  Production
                    // renders launched from this app will use these
                    // dims too — author a larger value in the Output
                    // Settings panel to override.
                    if let screen = NSScreen.main {
                        let visible = screen.visibleFrame
                        vb?.scaleFilm(
                            toFitSurfaceW: UInt(max(1, Int(visible.size.width))),
                            surfaceH: UInt(max(1, Int(visible.size.height))),
                            maxLongEdge: 800)
                    }
                    // The viewport is always on once a scene is loaded —
                    // there is no separate "interact mode" toggle.  Start
                    // the bridge's render thread now so the user can drag,
                    // orbit, scrub, and edit immediately.  Render and
                    // Render-Animation stop the bridge before kicking the
                    // production rasterizer; both restart it on completion.
                    vb?.start()
                    // Facet 5 slice B2: bind the chat driver to the
                    // fresh scene.  Resets the conversation (it was
                    // about the previous scene) and fetches the skills
                    // index once via the live dispatcher's read_skill.
                    if let vb = vb {
                        self.chat.sceneOpened(viewportBridge: vb, scenePath: self.loadedFilePath ?? "")
                    }
                } else {
                    self.renderState = .error("Failed to load scene")
                }
                self.refreshEditorContents()
                if !success {
                    // P1-6 fix: switch the left panel to the Scene-file
                    // tab on a load failure so the user actually sees
                    // the file content that needs fixing, instead of
                    // leaving the Agent tab frontmost with only the
                    // TopBar's error readout as a clue.  (The old
                    // `isEditorVisible = true` write here predated the
                    // tab-based left panel and had no observable effect
                    // — see SceneEditorPanel's header comment.)
                    self.leftTab = .sceneFile
                }
            }
        }
    }

    func startRender() {
        guard renderState == .sceneLoaded || renderState == .completed
              || renderState == .cancelled else { return }

        // Stop the interactive viewport's render thread BEFORE the
        // production rasterizer runs.  Both rasterizers read the same
        // scene state; running them concurrently is a data race.
        // The viewport is restarted in finishRender() once production
        // completes (or is cancelled).  Stop is synchronous — it
        // joins the viewport's render thread before returning.
        // Halt any looping preview-play first — it drives scrubTime on the
        // viewport renderer, which must be idle before production starts.
        stopPreviewPlay()
        // B2 review round 1: cancel any in-flight chat turn BEFORE
        // production kicks, mirroring the sceneClosed discipline at
        // the teardown sites.  A turn suspended in its HTTP await
        // would otherwise resume mid-render — its cancelled / stop /
        // executor checks all passing — and execute tool calls
        // against Scene state the production workers read off-main.
        chat.productionRenderStarting()
        viewportBridge?.stop()

        // Capture the canonical scrubbed time now, but advance the scene
        // inside the coordinated background render path below. A full
        // SetSceneTime regenerates photon maps and can take minutes; doing
        // it synchronously on MainActor made the app appear hung before it
        // could even publish `.rendering` or repaint the Cancel control.
        // The viewport's scrub path calls SetSceneTimeForPreview, which
        // skips photon regeneration for responsiveness, so production still
        // must run the full operation before rasterization.
        //
        // We prefer the controller's LastSceneTime over the SwiftUI
        // `sceneTime` because Undo / Redo can change scene time
        // without going through the slider — passing the slider's
        // local value in that window would roll the scene back to
        // a stale time.  Fall back to `sceneTime` when no viewport
        // bridge is attached (no controller, no scrubs possible).
        let canonicalSceneTime = viewportBridge?.lastSceneTime() ?? sceneTime

        renderState = .rendering
        progress = 0.0
        cancelFlag.value = false
        resetProductionPauseState()
        renderedImage = nil
        imageBuffer.reset()
        elapsedTime = 0
        remainingTime = nil
        renderStartTime = Date()
        bridge.etaBegin()

        let bridgeForTimer = bridge
        displayTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self = self, let start = self.renderStartTime else { return }
                // Elapsed = wall time minus time spent paused (item 4) —
                // the readout reflects actual render work and freezes
                // visibly while paused.
                let pausedNow = self.productionPauseBegan.map { Date().timeIntervalSince($0) } ?? 0
                self.elapsedTime = Date().timeIntervalSince(start)
                                   - self.productionPausedAccum - pausedNow
                self.remainingTime = self.isProductionRenderPaused
                    ? nil : bridgeForTimer.etaRemainingSeconds()?.doubleValue
            }
        }

        // L8 round 9 — progressive-update Timer.  Drives the
        // lockless polling path: every ~33 ms (30 Hz) we call
        // `bridge.pollProductionVFS`, which checks the production
        // VFS's atomic generation counter and emits a full-image
        // refresh only when workers have produced new pixels.
        // Workers no longer fire per-tile observer callbacks into
        // the bridge (round-9 design), so this Timer is the sole
        // driver of progressive updates during a render.
        //
        // Timer fires on the main run loop — fine for the polling
        // (atomic load + compare is ~10 ns when nothing has changed,
        // a one-shot emit when dirty).  The emit-when-dirty path
        // does the per-pixel encode + Swift block dispatch in
        // ~5 ms at 800x600, well under the 33 ms tick budget.
        //
        // L8 round 10 — added to `.common` run-loop mode so the
        // timer fires during user interaction (button hovers, menu
        // tracking, scrolling).  `Timer.scheduledTimer(...)` and
        // `RunLoop.main.add(timer, forMode: .default)` only fire in
        // `.default` mode, which is paused during AppKit event
        // tracking — net visible effect was "no progressive updates
        // until I release the cancel button", which the user
        // reported as "only the whole frame updates."  `.common`
        // ties the timer to every mode that AppKit promotes into the
        // common-modes set (default + tracking + modal etc.).
        //
        // Invalidated in the renderTask's finishRender block below.
        let bridgeForPoll = bridge
        let queueForPoll = pollQueue
        let inFlightForPoll = pollInFlight
        let pollTimer = Timer(timeInterval: 1.0 / 30.0, repeats: true) { _ in
            // L8 round 16 — dispatch the heavy poll work to a
            // background queue so it doesn't tie up the main thread
            // (the bridge's `pollProductionVFS` synchronously calls
            // the Swift image block, which does the RGBA16→RGBA8
            // conversion + NSImage allocation + Task @MainActor
            // dispatch — adding up to ~20 ms on main per tick if
            // run there).  Skip the dispatch if a prior poll is
            // still in flight to avoid backlog.
            if inFlightForPoll.value { return }
            inFlightForPoll.value = true
            queueForPoll.async {
                bridgeForPoll.pollProductionVFS()
                inFlightForPoll.value = false
            }
        }
        RunLoop.main.add(pollTimer, forMode: .common)
        progressivePollTimer = pollTimer

        let cancelRef = cancelFlag
        let bridgeForProgress = bridge
        let pausedRef = productionPausedRef
        let progressDelivery = CoalescedProgressDelivery()
        bridge.setProgressBlock { [weak self] (prog: Double, total: Double, title: String) -> Bool in
            // Don't feed the ETA estimator while paused (item 4): wall
            // time advances with zero progress, which would poison the
            // estimate.  Re-converges after resume.
            if !pausedRef.value {
                bridgeForProgress.etaUpdateProgress(prog, total: total)
            }
            if progressDelivery.submit(progress: prog, total: total, title: title) {
                Task { @MainActor [weak self] in
                    guard let update = progressDelivery.takeLatest() else { return }
                    guard let self = self else { return }
                    self.progress = update.total > 0 ? update.progress / update.total : 0
                    self.progressTitle = update.title
                }
            }
            return !cancelRef.value
        }

        let buffer = imageBuffer
        let imageDelivery = CoalescedImageDelivery()
        bridge.setImageOutputBlock { [weak self]
            (pImageData: UnsafePointer<UInt16>?,
             width: UInt32, height: UInt32,
             rcTop: UInt32, rcLeft: UInt32,
             rcBottom: UInt32, rcRight: UInt32) in
            guard let pImageData = pImageData else { return }

            guard let nsImage = buffer.handleOutput(
                pImageData: pImageData,
                width: width, height: height,
                rcTop: rcTop, rcLeft: rcLeft,
                rcBottom: rcBottom, rcRight: rcRight
            ) else { return }

            if imageDelivery.submit(nsImage) {
                Task { @MainActor [weak self] in
                    guard let image = imageDelivery.takeLatest() else { return }
                    self?.renderedImage = image
                }
            }
        }

        let bridgeRef = bridge
        renderTask = Task.detached { [weak self] in
            let success = bridgeRef.rasterize(atSceneTime: canonicalSceneTime)

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                self.renderTask = nil
                self.displayTimer?.invalidate()
                self.displayTimer = nil
                // L8 round 9 / 16 — stop the progressive-update poll.
                // The final OnFrameComplete observer (which still
                // fires from the rasterizer's main thread) has
                // already emitted the last image bytes, BUT we
                // poll one more time below to guarantee any
                // generation bumps between the last 30 Hz tick and
                // the OnFrameComplete are surfaced too.  Final
                // poll runs on the background queue (round 16) so
                // we don't hop back onto main for a final ~20 ms
                // emit.
                self.progressivePollTimer?.invalidate()
                self.progressivePollTimer = nil
                let bridgeRefFinal = self.bridge
                self.pollQueue.async {
                    bridgeRefFinal.pollProductionVFS()
                }
                if let start = self.renderStartTime {
                    self.elapsedTime = Date().timeIntervalSince(start)
                }
                self.renderStartTime = nil
                // Review P2: clear pause bookkeeping on EVERY exit path
                // (normal completion, cancel, Clear & Load,
                // save-and-reload) — not just cancelRender + next start.
                // The readers re-derive off renderState first today, but
                // that's a duplicated invariant, not a guarantee.
                self.resetProductionPauseState()
                self.remainingTime = nil

                if cancelRef.value {
                    self.resolvedIntegrator = nil
                    self.resolveReason = nil
                    self.renderState = .cancelled
                } else if success {
                    // Auto-dispatcher resolved at render time; surface its pick.
                    self.resolvedIntegrator = self.bridge.autoResolvedIntegrator()
                    self.resolveReason = self.bridge.autoResolveReason()
                    self.renderState = .completed
                } else {
                    self.resolvedIntegrator = nil
                    self.resolveReason = nil
                    self.renderState = .error("Rasterization failed")
                }

                // Production render is done (success / cancel / error).
                // Restart the interactive viewport so the user can keep
                // editing on the freshly-updated scene state — but WITHOUT
                // its initial render pass, so the just-finished production
                // image stays on screen until the user actually interacts.
                // Without this the interactive rasterizer's first pass
                // would overwrite the production result (visible as the
                // render "flashing then flipping back to the live
                // preview").  The render thread stays parked until the
                // first edit / gesture; this works for both the LDR
                // NSImage path and the EDR Metal-layer path, which the
                // old sink-level frame-drop did not (the EDR frame reaches
                // the layer through the FrameStore observer, bypassing the
                // sink entirely).
                self.viewportBridge?.startSuppressingInitialRender()
            }
        }
    }

    func startAnimationRender() {
        guard hasAnimation else { return }
        guard renderState == .sceneLoaded || renderState == .completed
              || renderState == .cancelled else { return }

        // Stop the viewport before production renders (see startRender).
        stopPreviewPlay()
        // B2 review round 1: cancel any in-flight chat turn before
        // production kicks (see startRender).
        chat.productionRenderStarting()
        viewportBridge?.stop()

        renderState = .rendering
        progress = 0.0
        cancelFlag.value = false
        resetProductionPauseState()
        renderedImage = nil
        imageBuffer.reset()
        elapsedTime = 0
        remainingTime = nil
        renderStartTime = Date()
        bridge.etaBegin()

        let bridgeForTimer = bridge
        displayTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self = self, let start = self.renderStartTime else { return }
                // Elapsed = wall time minus time spent paused (item 4) —
                // the readout reflects actual render work and freezes
                // visibly while paused.
                let pausedNow = self.productionPauseBegan.map { Date().timeIntervalSince($0) } ?? 0
                self.elapsedTime = Date().timeIntervalSince(start)
                                   - self.productionPausedAccum - pausedNow
                self.remainingTime = self.isProductionRenderPaused
                    ? nil : bridgeForTimer.etaRemainingSeconds()?.doubleValue
            }
        }

        // L8 round 9 — progressive-update poll for animation render.
        // Same as `startRender`'s loop; see comment there for the
        // architecture rationale + the round-10 .common-mode note.
        // Animation passes through many frames; each frame's
        // intra-render progress refreshes the viewport via this 30 Hz
        // poll, then the per-frame OnFrameComplete delivers the
        // final frame bytes.
        let bridgeForPoll = bridge
        let queueForPoll = pollQueue
        let inFlightForPoll = pollInFlight
        let pollTimer = Timer(timeInterval: 1.0 / 30.0, repeats: true) { _ in
            // L8 round 16 — dispatch the heavy poll work to a
            // background queue so it doesn't tie up the main thread
            // (the bridge's `pollProductionVFS` synchronously calls
            // the Swift image block, which does the RGBA16→RGBA8
            // conversion + NSImage allocation + Task @MainActor
            // dispatch — adding up to ~20 ms on main per tick if
            // run there).  Skip the dispatch if a prior poll is
            // still in flight to avoid backlog.
            if inFlightForPoll.value { return }
            inFlightForPoll.value = true
            queueForPoll.async {
                bridgeForPoll.pollProductionVFS()
                inFlightForPoll.value = false
            }
        }
        RunLoop.main.add(pollTimer, forMode: .common)
        progressivePollTimer = pollTimer

        let cancelRef = cancelFlag
        let bridgeForProgress = bridge
        let pausedRef = productionPausedRef
        let progressDelivery = CoalescedProgressDelivery()
        bridge.setProgressBlock { [weak self] (prog: Double, total: Double, title: String) -> Bool in
            // Don't feed the ETA estimator while paused (item 4): wall
            // time advances with zero progress, which would poison the
            // estimate.  Re-converges after resume.
            if !pausedRef.value {
                bridgeForProgress.etaUpdateProgress(prog, total: total)
            }
            if progressDelivery.submit(progress: prog, total: total, title: title) {
                Task { @MainActor [weak self] in
                    guard let update = progressDelivery.takeLatest() else { return }
                    guard let self = self else { return }
                    self.progress = update.total > 0 ? update.progress / update.total : 0
                    self.progressTitle = update.title
                }
            }
            return !cancelRef.value
        }

        let buffer = imageBuffer
        let imageDelivery = CoalescedImageDelivery()
        bridge.setImageOutputBlock { [weak self]
            (pImageData: UnsafePointer<UInt16>?,
             width: UInt32, height: UInt32,
             rcTop: UInt32, rcLeft: UInt32,
             rcBottom: UInt32, rcRight: UInt32) in
            guard let pImageData = pImageData else { return }

            guard let nsImage = buffer.handleOutput(
                pImageData: pImageData,
                width: width, height: height,
                rcTop: rcTop, rcLeft: rcLeft,
                rcBottom: rcBottom, rcRight: rcRight
            ) else { return }

            if imageDelivery.submit(nsImage) {
                Task { @MainActor [weak self] in
                    guard let image = imageDelivery.takeLatest() else { return }
                    self?.renderedImage = image
                }
            }
        }

        // Derive video output path from scene file path
        if let scenePath = loadedFilePath {
            let basePath = (scenePath as NSString).deletingPathExtension
            let videoPath = basePath + ".mov"
            bridge.setAnimationVideoOutputPath(videoPath)
        }

        let bridgeRef = bridge
        renderTask = Task.detached { [weak self] in
            let success = bridgeRef.rasterizeAnimation()

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                self.renderTask = nil
                self.displayTimer?.invalidate()
                self.displayTimer = nil
                // L8 round 9 / 16 — stop the progressive-update poll.
                // The final OnFrameComplete observer (which still
                // fires from the rasterizer's main thread) has
                // already emitted the last image bytes, BUT we
                // poll one more time below to guarantee any
                // generation bumps between the last 30 Hz tick and
                // the OnFrameComplete are surfaced too.  Final
                // poll runs on the background queue (round 16) so
                // we don't hop back onto main for a final ~20 ms
                // emit.
                self.progressivePollTimer?.invalidate()
                self.progressivePollTimer = nil
                let bridgeRefFinal = self.bridge
                self.pollQueue.async {
                    bridgeRefFinal.pollProductionVFS()
                }
                if let start = self.renderStartTime {
                    self.elapsedTime = Date().timeIntervalSince(start)
                }
                self.renderStartTime = nil
                // Review P2: clear pause bookkeeping on EVERY exit path
                // (normal completion, cancel, Clear & Load,
                // save-and-reload) — not just cancelRender + next start.
                // The readers re-derive off renderState first today, but
                // that's a duplicated invariant, not a guarantee.
                self.resetProductionPauseState()
                self.remainingTime = nil

                if cancelRef.value {
                    self.renderState = .cancelled
                } else if success {
                    self.renderState = .completed
                } else {
                    self.renderState = .error("Animation rasterization failed")
                }
                // Restart the interactive viewport (see startRender) —
                // without its initial render pass so the finished
                // animation's last frame stays on screen until the user
                // interacts.
                self.viewportBridge?.startSuppressingInitialRender()
            }
        }
    }

    func cancelRender() {
        cancelFlag.value = true
        // Cancel-while-paused: unpark the workers so the cancel lands
        // promptly (the pause gate also self-detects cancellation via
        // the progress block, but resuming here is snappier and keeps
        // the pause UI state honest during the cancelling window).
        if isProductionRenderPaused { setProductionRenderPaused(false) }
        renderState = .cancelling
    }

    // MARK: - Production render pause (UI refinement item 4)

    /// TRUE while the in-flight production render is paused (workers
    /// parked at the bridge's pause gate; partial image stays up).
    @Published var isProductionRenderPaused = false

    /// Wall-clock spent paused this render — subtracted from the
    /// elapsed-time display so the readout reflects actual work time.
    /// (The ETA estimator is NOT fed while paused; its estimate
    /// re-converges over the first ticks after resume.)
    private var productionPausedAccum: TimeInterval = 0
    private var productionPauseBegan: Date? = nil
    /// Captured by the progress blocks (cancelRef pattern) so render
    /// threads can read pause state without touching MainActor self.
    let productionPausedRef = AtomicBool(false)

    func toggleProductionRenderPause() {
        guard renderState == .rendering else { return }
        setProductionRenderPaused(!isProductionRenderPaused)
    }

    private func setProductionRenderPaused(_ paused: Bool) {
        bridge.setProductionRenderPaused(paused)
        productionPausedRef.value = paused
        if paused {
            productionPauseBegan = Date()
        } else if let began = productionPauseBegan {
            productionPausedAccum += Date().timeIntervalSince(began)
            productionPauseBegan = nil
        }
        isProductionRenderPaused = paused
    }

    /// Reset all pause bookkeeping — called at render start and finish.
    private func resetProductionPauseState() {
        productionPausedRef.value = false
        productionPausedAccum = 0
        productionPauseBegan = nil
        if isProductionRenderPaused { isProductionRenderPaused = false }
        // Also clear the C++-side pause gate on the bridge's persistent
        // callback.  Normally redundant (a successful setProgressBlock:
        // creates a fresh, un-paused callback per render) -- but a REFUSED
        // swap (see RISEBridge setProgressBlock:'s hardening) keeps the OLD
        // callback alive, and if its gate was left paused, the next
        // composed render would park its workers instantly while the UI
        // shows un-paused.  Safe here: this runs at production render
        // start/finish, where no paused production occupant can exist (the
        // callers await in-flight renders), and agent renders never route
        // through this callback's gate.
        bridge.setProductionRenderPaused(false)
    }

    // MARK: - Looping preview play (the Play button by the timeline)
    //
    // The ACTIVE animation is chosen in the right-side "Animation" accordion
    // category (it routes through SetSelection -> SetActiveAnimation).  The
    // loop below just plays whichever animation is active, reading its range
    // and frame count from the bridge (which follow the active animation).

    /// Toggle looping preview playback (the Play button by the timeline).
    func togglePreviewPlay() {
        if isPreviewPlaying { stopPreviewPlay() } else { startPreviewPlay() }
    }

    /// Loop the active animation through the fast interactive preview
    /// renderer: step the playhead from time_start to time_end across
    /// num_frames at a fixed ~30 Hz cadence, wrapping back to the start
    /// (loop) until stopped.  Paced strictly by the timer — frames the
    /// renderer can't keep up with are coalesced, never queued.
    func startPreviewPlay() {
        guard !isPreviewPlaying,
              let vb = viewportBridge,
              hasAnimation,
              // Same worker-alive set as isSceneEditableForAgents:
              // .cancelling means rasterize() has not returned yet, so
              // production workers may still read animator state.
              renderState != .rendering, renderState != .cancelling else { return }
        let t0 = vb.animationTimeStart
        let t1 = vb.animationTimeEnd
        let frames = max(Int(vb.animationNumFrames), 2)
        let span = t1 - t0
        guard span > 0 else { return }
        let dt = span / Double(frames - 1)

        isPreviewPlaying = true
        vb.scrubTimeBegin()           // one undo bracket for the whole run
        sceneTime = t0                // onChange(sceneTime) -> bridge.scrubTime

        previewPlayTask = Task { [weak self] in
            var t = t0
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 33_000_000)   // ~30 fps
                guard let self, self.isPreviewPlaying, !Task.isCancelled else { break }
                t += dt
                if t > t1 + 1e-9 { t = t0 }                      // loop
                self.sceneTime = t
            }
        }
    }

    /// Stop looping preview playback (idempotent; also called before any
    /// production render and when the user manually scrubs).
    func stopPreviewPlay() {
        guard isPreviewPlaying else { return }
        isPreviewPlaying = false
        previewPlayTask?.cancel()
        previewPlayTask = nil
        viewportBridge?.scrubTimeEnd()
    }


    // MARK: - UI redesign: refinement status poll

    /// Refresh `refinementPhase` / `refinementScaleDivisor` /
    /// `isRefinementPaused` / `undoLabel` / `redoLabel` from `vb`.
    /// Called once immediately on bridge attach, then every 0.5 s by
    /// `refinementPollTimer` — see `viewportBridge`'s didSet.
    private func pollRefinementState(_ vb: RISEViewportBridge) {
        var divisor: UInt32 = 1
        let phase = vb.refinementPhase(withScaleDivisor: &divisor)
        refinementPhase = Int(phase)
        refinementScaleDivisor = divisor
        undoLabel = vb.undoActionLabel()
        redoLabel = vb.redoActionLabel()

        // Design brief A4 — mirror the active region (full-res
        // film-pixel space).  The core clears this automatically
        // before any production render, so this poll is also how the
        // UI notices that auto-clear rather than going stale.
        var left: UInt32 = 0, top: UInt32 = 0, right: UInt32 = 0, bottom: UInt32 = 0
        if vb.getInteractiveRegionLeft(&left, top: &top, right: &right, bottom: &bottom) {
            // `right`/`bottom` are INCLUSIVE (see RISEViewportBridge.h)
            // — +1 so width/height reflect the pixel COUNT restricted,
            // matching ViewportView.commitRegionDrag's convention.
            activeRegion = CGRect(x: CGFloat(left), y: CGFloat(top),
                                  width: CGFloat(right) - CGFloat(left) + 1,
                                  height: CGFloat(bottom) - CGFloat(top) + 1)
        } else {
            activeRegion = nil
        }

        // CST <-> scene-file live sync (item 1 only — explicit-save-only
        // design, user decision 2026-07-12: UI edits never write the
        // .RISEscene to disk automatically, so there is no item-2
        // debounced auto-save here anymore; a disk write happens ONLY
        // from a user-initiated `saveScene()` call).  Gated on
        // `isSceneEditableForAgents` — `serializedSceneText` and
        // `getSceneTextVersionUuid:revision:` both take the controller's
        // commit mutex, which a production render (or an outstanding
        // chat-driven render) owns; calling either then would hang this
        // poll on the main actor.  `viewportBridge != nil` is already
        // guaranteed by every caller of this method.
        if isSceneEditableForAgents, let currentVersion = currentSceneTextVersion(vb) {
            // Item 1 — mirror the live CST into the editor buffer.
            if !isEditorDirty {
                // No unsaved buffer edits: safe to pull fresh text,
                // and this is also how a `editorBehindLiveScene` stale
                // flag clears once the user's buffer catches back up
                // (e.g. after Revert) without waiting for a new sync.
                if currentVersion != lastSyncedSceneTextVersion {
                    let text = vb.serializedSceneText()
                    editorText = text
                    editorOriginalText = text
                    lastSyncedSceneTextVersion = currentVersion
                }
                editorBehindLiveScene = false
            } else if currentVersion != lastSyncedSceneTextVersion {
                // The user has unsaved buffer edits AND the live scene
                // has moved on since the last sync — don't clobber the
                // buffer; just flag that it's stale.
                editorBehindLiveScene = true
            }
        }
    }

    // MARK: - UI redesign: refinement transport (TopBar / Render menu)

    // (pauseRefinement/resumeRefinement/togglePauseRefinement were
    // removed by user request — refinement restarts on every edit, so a
    // user-facing pause for it was a niche control.  The controller's
    // PauseRefinement API remains for future/programmatic use; the
    // status formatter's Paused phase stays reachable through it.)

    /// Restart refinement from scratch: stop the interactive render
    /// thread and start it again (with its normal initial-render pass,
    /// unlike the post-production `startSuppressingInitialRender` path).
    /// No-op with no bridge, while a production render owns the scene,
    /// or while a chat-driven agent render is outstanding — see
    /// `canUseSceneTransport`.
    func restartRefinement() {
        guard canUseSceneTransport, let vb = viewportBridge else { return }
        vb.stop()
        vb.start()
    }

    // MARK: - UI redesign: undo/redo passthrough (Edit menu)

    /// Same gate as `pauseRefinement` — see `canUseSceneTransport`.
    func undo() {
        guard canUseSceneTransport, let vb = viewportBridge else { return }
        vb.undo()
    }

    /// Same gate as `pauseRefinement` — see `canUseSceneTransport`.
    func redo() {
        guard canUseSceneTransport, let vb = viewportBridge else { return }
        vb.redo()
    }

    // MARK: - UI redesign: TopBar Save

    /// Writes in-memory edits to the originally-loaded path.  Mirrors
    /// `PropertiesPanel.performSceneSave(useLoadedPath: true)`'s
    /// in-place branch — kept as a small, deliberate duplication rather
    /// than threading a shared helper through PropertiesPanel this
    /// slice (PropertiesPanel is explicitly out of scope for slice 1;
    /// it gets its own restyling pass later).  No-op with no bridge or
    /// no loaded path (the TopBar Save button has no Save-As affordance
    /// — that stays in the Properties panel).
    ///
    /// Explicit-save-only (user decision 2026-07-12): this is the ONLY
    /// place a .RISEscene write ever happens.  UI edits mutate the live
    /// CST in memory and the CST-to-editor mirror (`pollRefinementState`
    /// item 1) keeps the Scene-file tab following those edits live, but
    /// none of that touches disk — a write happens only when the user
    /// explicitly triggers this method (the TopBar Save button or
    /// File > Save Scene).  Always alerts on refusal / I/O failure;
    /// there is no silent/suspended auto-save path to distinguish from.
    /// Start-screen create path (spec §4.3): load the bundled EMPTY starter
    /// scene as an untitled document, then hand `prompt` to the agent — the
    /// chat consumes it exactly once in `sceneOpened` (after the bridge is
    /// attached), so a slow load can neither drop nor double-send it.
    /// Returns false only when the bundled template is missing (a packaging
    /// bug — surfaced as a load error, not a silent no-op).
    @discardableResult
    func loadStarterScene(withPrompt prompt: String) -> Bool {
        guard let templatePath = Bundle.main.path(
            forResource: "empty_starter", ofType: "RISEscene") else {
            renderState = .error("The starter scene template is missing from the app bundle")
            return false
        }
        // Stash BEFORE the load so the prompt survives however long the
        // load takes; sceneOpened is the single consumer.
        chat.pendingFirstPrompt = prompt
        leftTab = .agent
        showLeftPanel = true
        loadScene(at: templatePath, untitled: true)
        return true
    }

    /// Save the scene to a user-chosen path (NSSavePanel).  Also the Save
    /// fallback for UNTITLED scenes (start-screen create path): the first
    /// Save becomes Save-As.  On success the scene gains the chosen path
    /// (re-anchor, matching SaveEngine's own FileIdentity re-anchor), joins
    /// the recents list, and behaves as a normal named scene from then on.
    @discardableResult
    func saveSceneAs() -> Bool {
        guard let vb = viewportBridge else { return false }
        let panel = NSSavePanel()
        panel.allowedContentTypes = []  // accept any extension
        panel.nameFieldStringValue = (loadedFilePath as NSString?)?.lastPathComponent
            ?? "untitled.RISEscene"
        if let lp = loadedFilePath {
            panel.directoryURL = URL(fileURLWithPath: lp).deletingLastPathComponent()
        }
        panel.title = "Save Scene As"
        panel.message = "Choose a destination for the .RISEscene file."
        guard panel.runModal() == .OK, let path = panel.url?.path, !path.isEmpty else {
            return false
        }
        var errMsg: NSString? = nil
        let status = vb.saveScene(to: path, errorMessage: &errMsg)
        switch status {
        case 0, 1:
            // Saved (or NoOp).  Re-anchor so subsequent in-place saves
            // target the just-written file, and an untitled scene becomes
            // a normal named scene (title, recents, in-place Save).
            if path != loadedFilePath {
                loadedFilePath = path
                addToRecentFiles(path)
            }
            if !isEditorDirty {
                refreshEditorContents()
            }
            return true
        case 2:
            showSaveAlert(title: "Save Refused",
                          message: (errMsg as String?) ?? "The save engine declined to write this file.")
            return false
        default:
            showSaveAlert(title: "Save Failed",
                          message: (errMsg as String?) ?? "An I/O error occurred while saving the file.")
            return false
        }
    }

    @discardableResult
    func saveScene() -> Bool {
        // Untitled scene (start-screen create): nothing on disk to save
        // in-place — the first Save IS Save-As (spec §5.2).
        if loadedFilePath == nil {
            return saveSceneAs()
        }
        guard let vb = viewportBridge, let path = loadedFilePath else { return false }
        var errMsg: NSString? = nil
        let status = vb.saveScene(to: path, errorMessage: &errMsg)
        switch status {
        case 0:
            // Saved.  Pull the just-written bytes back into the
            // scene-editor pane so it reflects the round-tripped
            // edits — unless the editor has its own unsaved text
            // edits, in which case overwriting would silently discard
            // them (same conflict surfaced in PropertiesPanel).
            if !isEditorDirty {
                refreshEditorContents()
            } else {
                let alert = NSAlert()
                alert.messageText = "Scene editor has unsaved text changes"
                alert.informativeText =
                    "Your interactive edits were saved to \(path).  " +
                    "The scene editor pane still shows the pre-save text " +
                    "plus your unsaved edits.  Clicking Save in the scene " +
                    "editor will overwrite the just-saved interactive " +
                    "changes — use Revert in the scene editor to discard " +
                    "your text edits and pull the new file content."
                alert.alertStyle = .warning
                alert.addButton(withTitle: "OK")
                alert.runModal()
            }
            return true
        case 1:
            return true   // NoOp — nothing to write.
        case 2:
            let message = (errMsg as String?) ?? "The save engine declined to write this file."
            showSaveAlert(title: "Save Refused", message: message)
            return false
        case 3:
            let message = (errMsg as String?) ?? "An I/O error occurred while saving the file."
            showSaveAlert(title: "Save Failed", message: message)
            return false
        default:
            let message = "Unexpected save result (status \(status))."
            showSaveAlert(title: "Save Failed", message: message)
            return false
        }
    }

    private func showSaveAlert(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    // MARK: - Agent surface (Facet 5 slice 1c-1)

    /// Facet 5 slice 1c-1: hand one JSON-RPC request line to the live
    /// agent dispatcher and return its response line.
    ///
    /// Runs SYNCHRONOUSLY on the main actor — the bridge's
    /// `agentHandleLine` runs on the calling (main) thread, exactly like
    /// GUI SetProperty drives the controller's cancel-and-park from main.
    /// After a `propose_patch` the viewport re-renders and the Save button
    /// enables AUTOMATICALLY: the controller's commit kicks a fresh render
    /// pass (delivered via the live-preview image block) and flips the
    /// dirty flag (delivered via `setDirtyChangedBlock` → `sceneEditsDirty`).
    /// So there is NO manual viewport refresh or Save-state poke here.
    @discardableResult
    func sendAgentRequest(_ line: String) -> String {
        guard let vb = viewportBridge else {
            let err = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":" +
                      "{\"code\":-32603,\"message\":\"no scene loaded (no viewport bridge)\"}}"
            agentResponseText = err
            return err
        }
        // B2 review round 1 (audit-by-bug-pattern sibling of the chat
        // gate): refuse while a production render owns the scene —
        // `agentHandleLine` would mutate Scene state the production
        // rasterizer's workers read off-main.  Well-formed JSON-RPC
        // error, like the nil-dispatcher path above, so the refusal
        // is visible in the panel's Response area (the panel's Send
        // is also disabled on the same predicate).
        guard isSceneEditableForAgents else {
            let err = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":" +
                      "{\"code\":-32603,\"message\":\"a production render is running — " +
                      "wait for it to finish, then send again\"}}"
            agentResponseText = err
            return err
        }
        let response = vb.agentHandleLine(line)
        agentResponseText = response
        return response
    }

    func saveEditorFile() {
        guard let path = loadedFilePath else { return }
        do {
            try editorText.write(toFile: path, atomically: true, encoding: .utf8)
            editorOriginalText = editorText
        } catch {
            let alert = NSAlert()
            alert.messageText = "Failed to save file"
            alert.informativeText = error.localizedDescription
            alert.alertStyle = .critical
            alert.runModal()
        }
    }

    /// Discards the editor buffer's unsaved text edits.  In a
    /// live-synced world "revert" means "go back to what the scene
    /// actually IS right now" (the live CST's serialization), not "go
    /// back to a stale at-load/at-last-sync snapshot" — so this pulls
    /// `bridge.serializedSceneText` fresh rather than reusing
    /// `editorOriginalText`.  Falls back to `editorOriginalText` (the
    /// pre-redesign behavior) when the live text isn't reachable right
    /// now — no bridge, or a production render / outstanding chat render
    /// currently owns the scene (`isSceneEditableForAgents` false) —
    /// same gate as every other `serializedSceneText` call site.
    func revertEditorFile() {
        guard let vb = viewportBridge, isSceneEditableForAgents else {
            editorText = editorOriginalText
            return
        }
        let text = vb.serializedSceneText()
        editorText = text
        editorOriginalText = text
        lastSyncedSceneTextVersion = currentSceneTextVersion(vb)
        editorBehindLiveScene = false
    }

    /// "Reveal in scene file" (design comp's ⌗ affordance): resolve
    /// `(category, name)` to its position in the scene text, switch to
    /// the Scene-file tab, and publish `editorRevealRequest` so
    /// `SceneTextEditor` scrolls/selects/flashes that line.  Same
    /// scene-editable gate as every other bridge call that reads the
    /// live CST (`isSceneEditableForAgents` — do not call while a
    /// production render or an outstanding chat render owns the
    /// controller).  A no-op (no tab switch, no publish) when the bridge
    /// is absent, the gate is closed, or the entity doesn't resolve
    /// (unnamed/ambiguous, or a category with no chunk-name addressing
    /// scheme — see `SceneEditController::EntitySourceLocation`).
    func revealEntityInSceneText(category: RISEViewportCategory, name: String) {
        guard let vb = viewportBridge, isSceneEditableForAgents else { return }
        var offset: UInt64 = 0
        var line: UInt32 = 0
        guard vb.getEntitySourceLocation(for: category, name: name, byteOffset: &offset, line: &line) else { return }
        leftTab = .sceneFile
        // Review P2: the offset addresses the LIVE serialization.  When
        // the editor buffer has unsaved hand edits it may differ without
        // being shorter, so a scroll could land on the WRONG line while
        // looking authoritative.  Switch tabs only in that case (the
        // stale-buffer warning is already showing) — a reveal must be
        // right or absent, never misleading.
        guard !isEditorDirty else { return }
        editorRevealRequest = EditorRevealRequest(byteOffset: offset, byteLength: 0, generation: nextEditorRevealGeneration)
        nextEditorRevealGeneration += 1
    }

    /// Source traceability: reveal a specific UI element's span in the Scene-file
    /// editor, highlighting the EXACT byte range (a param row) rather than the whole
    /// line.  `param` empty falls back to a whole-chunk (line) reveal.  Same
    /// stale-buffer guard as revealEntityInSceneText — a reveal must be right or
    /// absent, never misleading.
    func revealSourceSpan(category: RISEViewportCategory, name: String, param: String, occurrence: Int = 0) {
        guard let vb = viewportBridge, isSceneEditableForAgents else { return }
        var offset: UInt64 = 0
        var length: UInt64 = 0
        var line: UInt32 = 0
        var col: UInt32 = 0
        guard vb.resolveSourceSpan(for: category, name: name, param: param, occurrence: Int32(occurrence),
                                   byteOffset: &offset, byteLength: &length, line: &line, column: &col) else { return }
        leftTab = .sceneFile
        guard !isEditorDirty else { return }
        editorRevealRequest = EditorRevealRequest(byteOffset: offset, byteLength: length, generation: nextEditorRevealGeneration)
        nextEditorRevealGeneration += 1
    }

    /// Reverse source traceability (text -> UI select): the inverse of the
    /// two reveals above.  Resolve the scene entity whose source contains
    /// UTF-8 `byteOffset` (a right-click in the scene-file editor) and select
    /// it so the outliner highlight + inspector land on it.  Same editability
    /// gate as the forward reveals; additionally requires a CLEAN editor
    /// buffer -- the offset comes from the buffer but `sourceRef` resolves it
    /// against the live CST serialization, and the two agree only when clean
    /// (the inverse reasoning of the forward stale-buffer guard).  Selection
    /// never mutates the document text, so this cannot dirty the buffer or
    /// feed back into another resolve.  A no-resolve (inter-chunk trivia, a
    /// non-entity chunk, EOF, or a dirty buffer) is a silent no-op.
    func reverseSelectEntity(atByteOffset byteOffset: UInt64) {
        guard let vb = viewportBridge, canReverseSelect else { return }
        var cat: RISEViewportCategory = .none
        var name: NSString? = nil
        var param: NSString? = nil
        var occurrence: Int32 = 0
        guard vb.sourceRef(atByteOffset: byteOffset, category: &cat,
                           name: &name, param: &param, occurrence: &occurrence) else { return }
        // Only refresh the panels if the selection actually took (the
        // resolver already validated addressability, so this normally
        // succeeds; gating avoids a stale re-snapshot on the rare miss).
        if vb.setSelection(cat, name: (name as String?) ?? "") {
            reverseSelectEpoch &+= 1
        }
    }

    // MARK: - Entity creation + painter CRUD (entity-creation slice)
    //
    // All three mutating calls below take the controller's commit mutex
    // (same reason as every other scene-edit call) -- gated on
    // `isSceneEditableForAgents` so a click during a production render
    // refuses cleanly instead of wedging on the mutex.  Success bumps
    // `entityListEpoch` (see its doc) and re-selects the affected
    // entity so the properties panel and outliner both land on it;
    // failure surfaces the core's honest refusal message via NSAlert.

    /// Templates registered for `category` (empty for categories with
    /// none). `index` is the template's position, stable for the
    /// lifetime of the call -- pass straight through to `addEntity`.
    func entityTemplates(for category: RISEViewportCategory) -> [(index: Int, label: String)] {
        guard let vb = viewportBridge else { return [] }
        let count = vb.entityTemplateCount(for: category)
        guard count > 0 else { return [] }
        return (0..<count).map { i in
            (index: Int(i), label: vb.entityTemplateLabel(for: category, index: i))
        }
    }

    /// Instantiate template `templateIndex` within `category`.  On
    /// success, selects the new entity (so the properties panel jumps
    /// to it) and bumps `entityListEpoch` (so the outliner re-pulls
    /// that category's list).  On failure, shows the core's refusal
    /// message.
    func addEntity(category: RISEViewportCategory, templateIndex: Int) {
        guard let vb = viewportBridge, isSceneEditableForAgents else { return }
        var outName: NSString? = nil
        var outMessage: NSString? = nil
        let applied = vb.instantiateEntityTemplate(
            for: category, index: UInt(templateIndex),
            outName: &outName, outMessage: &outMessage)
        if applied {
            entityListEpoch &+= 1
            if let name = outName as String? {
                _ = vb.setSelection(category, name: name)
            }
        } else {
            presentEntityEditAlert(
                title: "Couldn't add entity",
                message: (outMessage as String?) ?? "The edit was refused.")
        }
    }

    /// Duplicate the named entity in `category`.  Same success/failure
    /// pattern as `addEntity`.
    func duplicateSelectedOrNamed(category: RISEViewportCategory, name: String) {
        guard let vb = viewportBridge, isSceneEditableForAgents else { return }
        var outName: NSString? = nil
        var outMessage: NSString? = nil
        let applied = vb.duplicateEntity(
            for: category, name: name,
            outName: &outName, outMessage: &outMessage)
        if applied {
            entityListEpoch &+= 1
            if let dupName = outName as String? {
                _ = vb.setSelection(category, name: dupName)
            }
        } else {
            presentEntityEditAlert(
                title: "Couldn't duplicate \"\(name)\"",
                message: (outMessage as String?) ?? "The edit was refused.")
        }
    }

    /// Remove the named entity in `category`.  Confirms via NSAlert
    /// first (irreversible from the user's point of view within a
    /// session -- there IS an Undo, but the confirm is still the
    /// honest "are you sure" gesture for a destructive action); the
    /// core's still-referenced refusal (e.g. a material a
    /// standard_object still binds) surfaces as a follow-up alert so
    /// the user understands WHY it didn't go through.
    func removeEntity(category: RISEViewportCategory, name: String) {
        guard let vb = viewportBridge, isSceneEditableForAgents else { return }
        let confirm = NSAlert()
        confirm.messageText = "Delete \"\(name)\"?"
        confirm.informativeText = "This can be undone with Edit > Undo."
        confirm.alertStyle = .warning
        confirm.addButton(withTitle: "Delete")
        confirm.addButton(withTitle: "Cancel")
        guard confirm.runModal() == .alertFirstButtonReturn else { return }

        // Re-check the edit gate AFTER the modal returns: the confirm
        // dialog blocks the main thread, but a chat-driven agent render
        // (folded into isSceneEditableForAgents via isChatRenderOutstanding)
        // can begin or end while it's up, so the gate state at the top of
        // this function may be stale by now.  Calling into the commit
        // mutex while a render owns it would wedge the UI.
        guard isSceneEditableForAgents else { return }

        var outMessage: NSString? = nil
        let applied = vb.removeEntity(for: category, name: name, outMessage: &outMessage)
        if applied {
            entityListEpoch &+= 1
        } else {
            presentEntityEditAlert(
                title: "Couldn't delete \"\(name)\"",
                message: (outMessage as String?) ?? "The edit was refused.")
        }
    }

    private func presentEntityEditAlert(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.runModal()
    }

    /// Public surface for the Environment panel to report a refused / partial
    /// environment edit (e.g. "already bound", or a live bind that couldn't be
    /// recorded in the scene file).  Routes through the same warning alert the
    /// entity-CRUD failures use.
    func presentEnvironmentNotice(_ message: String) {
        presentEntityEditAlert(title: "Environment", message: message)
    }

    func saveAndReloadScene() {
        guard let path = loadedFilePath else { return }

        // Save current editor text to disk
        do {
            try editorText.write(toFile: path, atomically: true, encoding: .utf8)
            editorOriginalText = editorText
        } catch {
            let alert = NSAlert()
            alert.messageText = "Failed to save file"
            alert.informativeText = error.localizedDescription
            alert.alertStyle = .critical
            alert.runModal()
            return
        }

        // Cancel any active render and wait for it to finish before reloading
        if renderState == .rendering || renderState == .cancelling {
            cancelFlag.value = true
            renderState = .cancelling
            let task = renderTask
            Task { @MainActor [weak self] in
                await task?.value
                self?.finishSaveAndReload(path: path)
            }
        } else {
            finishSaveAndReload(path: path)
        }
    }

    private func finishSaveAndReload(path: String) {
        // Clear the current scene.  Tear the viewport bridge down
        // first so its render thread is joined before clearAll
        // destroys the scene it's referencing.
        stopPreviewPlay()   // halt a looping preview-play before the bridge it drives is torn down
        chat.sceneClosed()  // stop the chat driver before its tool executor is torn down
        viewportBridge?.shutdown()
        viewportBridge = nil
        bridge.clearAll()
        renderedImage = nil
        progress = 0.0
        progressTitle = ""
        elapsedTime = 0
        remainingTime = nil
        imageBuffer.reset()
        logMessages.removeAll()

        // Reload the scene from disk
        loadScene(at: path)
    }

    var isEditorDirty: Bool {
        editorText != editorOriginalText
    }

    var canOpenScene: Bool {
        renderState != .rendering && renderState != .cancelling && renderState != .loading
    }

    /// UI redesign: gate for the Render menu's "Production Render" /
    /// "Render Animation…" items and the old controls panel's Render
    /// buttons before it — moved here (from a private ContentView
    /// computed property) so RISEApp.swift's menu commands can read the
    /// same rule.
    var canStartProductionRender: Bool {
        switch renderState {
        case .sceneLoaded, .completed, .cancelled:
            return true
        default:
            return false
        }
    }

    /// UI redesign: gate for File > Close Scene — matches the retired
    /// controls panel's "Clear" button (disabled while rendering,
    /// cancelling, or when there's nothing loaded yet).
    var canCloseScene: Bool {
        renderState != .rendering && renderState != .cancelling && renderState != .idle
    }

    /// L5a round-9 — gate for File > Save Rendered Image.  The
    /// production VFS's FrameStore exists once the rasterizer has
    /// emitted at least one OutputImage; that happens any time we
    /// transition through `.rendering`.  After completion or cancel
    /// the FrameStore retains its last contents (the bridge's
    /// `clearAll` does NOT free the VFS, per L4 §7.5), so saving
    /// from `.completed` or `.cancelled` produces the user's
    /// last-rendered frame.  Loading a fresh scene transitions
    /// through `.loading` → `.sceneLoaded`, neither of which has
    /// fresh output yet — disable until the next render starts.
    var canSaveImage: Bool {
        switch renderState {
        case .rendering, .cancelling, .completed, .cancelled:
            return true
        default:
            return false
        }
    }

    /// L5a round-9 — File > Save Rendered Image.  Opens an
    /// NSSavePanel pre-populated with HDR-first format choices
    /// (EXR default, then PNG / TIFF), looks up the encoder by
    /// extension, and dispatches through the bridge's format-aware
    /// `saveAs`.  HDR formats (EXR) write scene-referred linear
    /// half/float values; LDR formats (PNG, TIFF-8) bake in the
    /// current view exposure (today: 0 EV — no slider exposed yet).
    /// No-op if `canSaveImage` is false (e.g. no render yet).
    func saveRenderedImage() {
        guard canSaveImage else { return }

        let panel = NSSavePanel()
        panel.title = "Save Rendered Image"
        panel.message = "Choose a file format. EXR preserves the full HDR " +
                        "result; PNG / TIFF clip to SDR with the current display EV."
        panel.canCreateDirectories = true
        panel.isExtensionHidden = false

        // HDR-first dropdown.  NSSavePanel uses `allowedContentTypes`
        // to populate the Format popup; the FIRST entry is the
        // default selection.  We keep the list short — EXR (HDR
        // archival), PNG (LDR most-portable), TIFF (LDR/16-bit
        // archival).  HDR Radiance / RGBEA / TGA / PPM are niche
        // and accessible via the CLI; adding them here clutters the
        // dropdown without obvious user benefit.
        var allowedTypes: [UTType] = []
        if let exr = UTType(filenameExtension: "exr") { allowedTypes.append(exr) }
        allowedTypes.append(.png)
        if let tif = UTType(filenameExtension: "tiff") { allowedTypes.append(tif) }
        panel.allowedContentTypes = allowedTypes

        // Default filename: scene basename + ".exr" (the HDR-first
        // default).  If no scene loaded somehow (shouldn't happen
        // given canSaveImage gate), fall back to a generic name.
        let baseName: String
        if let path = loadedFilePath {
            baseName = (path as NSString).lastPathComponent
                .replacingOccurrences(of: ".RISEscene", with: "")
        } else {
            baseName = "rendered"
        }
        panel.nameFieldStringValue = "\(baseName).exr"

        guard panel.runModal() == .OK, let url = panel.url else { return }

        // Map extension → bridge format name.  ByExtension is
        // case-insensitive in the registry, but we normalise here
        // so the bridge log line is consistent.
        let ext = url.pathExtension.lowercased()
        let formatName: String
        switch ext {
        case "exr":           formatName = "EXR"
        case "png":           formatName = "PNG"
        case "tif", "tiff":   formatName = "TIFF"
        case "hdr":           formatName = "HDR"
        case "rgbea":         formatName = "RGBEA"
        case "tga":           formatName = "TGA"
        case "ppm":           formatName = "PPM"
        default:
            // Unknown extension — fall back to EXR so we don't
            // silently produce an unwritable file.
            formatName = "EXR"
        }

        let ok = bridge.save(as: url.path, format: formatName, exposureEV: 0.0)
        if !ok {
            let alert = NSAlert()
            alert.messageText = "Save Failed"
            alert.informativeText = "Could not write \(url.lastPathComponent) " +
                                    "as \(formatName).  See the log for details."
            alert.alertStyle = .warning
            alert.runModal()
        }
    }

    /// Reload editor contents from the current loadedFilePath.
    ///
    /// On failure: if the pane has never loaded content (editorText is
    /// empty — initial-toggle case), drop a placeholder string so the
    /// pane isn't a blank canvas with no diagnostic.  If the pane
    /// ALREADY has content (post-round-trip-save refresh case), keep
    /// it intact and surface a one-shot alert — wiping content the
    /// user can still see and copy is more destructive than the rare
    /// IO failure justifies (matches the Windows SceneEditor::loadFile
    /// pattern; adversarial-review round 2 P3).
    func refreshEditorContents() {
        guard let path = loadedFilePath else { return }
        do {
            let content = try String(contentsOfFile: path, encoding: .utf8)
            editorText = content
            editorOriginalText = content
        } catch {
            if editorText.isEmpty {
                editorText = "// Failed to load file: \(error.localizedDescription)"
                editorOriginalText = editorText
            } else {
                let alert = NSAlert()
                alert.messageText = "Failed to reload scene file"
                alert.informativeText = error.localizedDescription
                alert.alertStyle = .warning
                alert.addButton(withTitle: "OK")
                alert.runModal()
            }
        }
    }

    func clearScene() {
        // Viewport bridge borrows the underlying job — tear it down
        // BEFORE bridge.clearAll() so the controller's render thread
        // is joined before the scene is destroyed.
        stopPreviewPlay()   // halt a looping preview-play before the bridge it drives is torn down
        chat.sceneClosed()  // stop the chat driver before its tool executor is torn down
        viewportBridge?.shutdown()
        viewportBridge = nil
        sceneTime = 0
        bridge.clearAll()
        renderState = .idle
        renderedImage = nil
        loadedFilePath = nil
        progress = 0.0
        progressTitle = ""
        elapsedTime = 0
        remainingTime = nil
        imageBuffer.reset()
        logMessages.removeAll()
        hasAnimation = false
        editorText = ""
        editorOriginalText = ""
        // The Agent debug panel's response text is per-scene too — its
        // dispatcher lives in the (now torn-down) viewport bridge.
        agentResponseText = ""
    }

    func clearLog() {
        logMessages.removeAll()
    }

    // MARK: - Helpers

    var formattedElapsedTime: String {
        RISEBridge.formatDuration(elapsedTime)
    }

    var formattedRemainingTime: String? {
        guard let r = remainingTime else { return nil }
        return RISEBridge.formatDuration(r)
    }
}
