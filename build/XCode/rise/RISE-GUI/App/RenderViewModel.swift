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

@MainActor
final class RenderViewModel: ObservableObject {
    @Published var renderState: RenderState = .idle
    @Published var progress: Double = 0.0
    @Published var progressTitle: String = ""
    @Published var renderedImage: NSImage? = nil
    @Published var loadedFilePath: String? = nil
    @Published var versionString: String = ""
    @Published var elapsedTime: TimeInterval = 0
    // nil while the ETA is still warming up or otherwise unavailable.
    @Published var remainingTime: TimeInterval? = nil
    @Published var sceneSize: CGSize? = nil
    @Published var logMessages: [LogMessage] = []
    @Published var isEditorVisible: Bool = false
    @Published var editorText: String = ""
    @Published var editorOriginalText: String = ""
    @Published var hasAnimation: Bool = false
    @Published var recentFiles: [String] = []

    /// Live time-scrubber state, displayed on the viewport's bottom slider.
    @Published var sceneTime: Double = 0

    private let bridge = RISEBridge()

    /// Lazily constructed when a scene successfully loads; torn down on
    /// clearScene().  The viewport bridge borrows `bridge`'s job — its
    /// lifetime must not exceed `bridge`'s.
    private(set) var viewportBridge: RISEViewportBridge? = nil
    private let cancelFlag = AtomicBool(false)
    private let imageBuffer = RenderImageBuffer()
    private var renderStartTime: Date? = nil
    private var displayTimer: Timer? = nil
    private var renderTask: Task<Void, Never>? = nil

    private static let maxLogMessages = 10000
    private static let maxRecentFiles = 10
    private static let recentFilesKey = "recentSceneFiles"

    init() {
        versionString = RISEBridge.versionString()
        recentFiles = UserDefaults.standard.stringArray(forKey: Self.recentFilesKey) ?? []
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
        // If the editor has unsaved changes, prompt the user before switching
        if isEditorVisible && isEditorDirty {
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

        // If a scene is already loaded, warn about merge behavior
        if loadedFilePath != nil {
            let alert = NSAlert()
            alert.messageText = "A scene is already loaded"
            alert.informativeText = "Loading a new scene file will merge it with the current scene. Would you like to clear the current scene first, or merge the new scene into it?"
            alert.alertStyle = .warning
            alert.addButton(withTitle: "Clear & Load")
            alert.addButton(withTitle: "Merge")
            alert.addButton(withTitle: "Cancel")

            let response = alert.runModal()
            switch response {
            case .alertFirstButtonReturn:
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
            case .alertSecondButtonReturn:
                // Merge — the parser will add chunks to the existing
                // job.  This mutates manager state that the viewport's
                // interactive render thread (and any in-flight
                // production render) reads concurrently.  Same teardown
                // dance as Clear & Load, minus the clearAll: stop the
                // workers first so the merge can mutate state without a
                // race.
                if renderState == .rendering || renderState == .cancelling {
                    cancelFlag.value = true
                    renderState = .cancelling
                    let task = renderTask
                    Task { @MainActor [weak self] in
                        await task?.value
                        self?.continueMergeLoad(at: path)
                    }
                    return
                }
                continueMergeLoad(at: path)
                return
            default:
                return
            }
        }

        loadScene(at: path)
    }

    /// Merge-load: stop the viewport bridge so the parser's chunk
    /// additions don't race with the interactive render thread, then
    /// load the new file (which appends into the existing job).  The
    /// fresh viewport bridge is recreated inside loadScene's success
    /// path.
    private func continueMergeLoad(at path: String) {
        viewportBridge?.shutdown()
        viewportBridge = nil

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
        viewportBridge?.shutdown()
        viewportBridge = nil

        bridge.clearAll()
        renderedImage = nil
        progress = 0.0
        progressTitle = ""
        elapsedTime = 0
        remainingTime = nil
        sceneSize = nil
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
        UserDefaults.standard.set(recentFiles, forKey: Self.recentFilesKey)
    }

    func clearRecentFiles() {
        recentFiles.removeAll()
        UserDefaults.standard.removeObject(forKey: Self.recentFilesKey)
    }

    func loadScene(at path: String) {
        addToRecentFiles(path)
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
        bridge.setProgressBlock { [weak self] (prog: Double, total: Double, title: String) -> Bool in
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                self.progress = total > 0 ? prog / total : 0
                self.progressTitle = title
            }
            return !cancelRef.value
        }

        let bridgeRef = bridge
        Task.detached { [weak self] in
            let success = bridgeRef.loadAsciiScene(path)

            // Read camera dimensions on the background thread before
            // switching to MainActor, since the bridge accesses C++ state.
            let camWidth = bridgeRef.cameraWidth()
            let camHeight = bridgeRef.cameraHeight()

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                self.loadedFilePath = path
                if success {
                    self.renderState = .sceneLoaded
                    self.progressTitle = ""
                    self.hasAnimation = bridgeRef.hasAnimatedObjects()
                    if camWidth > 0 && camHeight > 0 {
                        self.sceneSize = CGSize(width: CGFloat(camWidth),
                                                height: CGFloat(camHeight))
                    }
                    // Tear down any previous viewport bridge (e.g. from
                    // a prior scene) and stand up a fresh one over the
                    // newly-loaded job.
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
                    // The viewport is always on once a scene is loaded —
                    // there is no separate "interact mode" toggle.  Start
                    // the bridge's render thread now so the user can drag,
                    // orbit, scrub, and edit immediately.  Render and
                    // Render-Animation stop the bridge before kicking the
                    // production rasterizer; both restart it on completion.
                    vb?.start()
                } else {
                    self.renderState = .error("Failed to load scene")
                }
                // Always open the editor so the user can fix errors
                self.refreshEditorContents()
                self.isEditorVisible = true
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
        viewportBridge?.stop()

        // Advance scene state to the canonical scrubbed time AND
        // regenerate photon maps before the production rasterizer
        // fires.  The viewport's scrub path calls
        // SetSceneTimeForPreview, which advances the animator but
        // skips photon regen for responsiveness; without this full
        // SetSceneTime, hitting Render after scrubbing renders the
        // right object positions but caustics frozen at the
        // pre-scrub time.
        //
        // We prefer the controller's LastSceneTime over the SwiftUI
        // `sceneTime` because Undo / Redo can change scene time
        // without going through the slider — passing the slider's
        // local value in that window would roll the scene back to
        // a stale time.  Fall back to `sceneTime` when no viewport
        // bridge is attached (no controller, no scrubs possible).
        let canonical = viewportBridge?.lastSceneTime() ?? sceneTime
        bridge.setSceneTime(canonical)

        renderState = .rendering
        progress = 0.0
        cancelFlag.value = false
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
                self.elapsedTime = Date().timeIntervalSince(start)
                self.remainingTime = bridgeForTimer.etaRemainingSeconds()?.doubleValue
            }
        }

        let cancelRef = cancelFlag
        let bridgeForProgress = bridge
        bridge.setProgressBlock { [weak self] (prog: Double, total: Double, title: String) -> Bool in
            bridgeForProgress.etaUpdateProgress(prog, total: total)
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                self.progress = total > 0 ? prog / total : 0
                self.progressTitle = title
            }
            return !cancelRef.value
        }

        let buffer = imageBuffer
        let sceneSizeReported = AtomicBool(false)
        bridge.setImageOutputBlock { [weak self]
            (pImageData: UnsafePointer<UInt16>?,
             width: UInt32, height: UInt32,
             rcTop: UInt32, rcLeft: UInt32,
             rcBottom: UInt32, rcRight: UInt32) in
            guard let pImageData = pImageData else { return }

            // Capture scene dimensions from the first output block
            if !sceneSizeReported.value {
                sceneSizeReported.value = true
                let w = CGFloat(width)
                let h = CGFloat(height)
                Task { @MainActor [weak self] in
                    self?.sceneSize = CGSize(width: w, height: h)
                }
            }

            guard let nsImage = buffer.handleOutput(
                pImageData: pImageData,
                width: width, height: height,
                rcTop: rcTop, rcLeft: rcLeft,
                rcBottom: rcBottom, rcRight: rcRight
            ) else { return }

            Task { @MainActor [weak self] in
                self?.renderedImage = nsImage
            }
        }

        let bridgeRef = bridge
        renderTask = Task.detached { [weak self] in
            let success = bridgeRef.rasterize()

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                self.renderTask = nil
                self.displayTimer?.invalidate()
                self.displayTimer = nil
                if let start = self.renderStartTime {
                    self.elapsedTime = Date().timeIntervalSince(start)
                }
                self.renderStartTime = nil
                self.remainingTime = nil

                if cancelRef.value {
                    self.renderState = .cancelled
                } else if success {
                    self.renderState = .completed
                } else {
                    self.renderState = .error("Rasterization failed")
                }

                // Production render is done (success / cancel / error).
                // Restart the interactive viewport so the user can keep
                // editing on the freshly-updated scene state.  Suppress
                // the very next preview frame at the sink layer so the
                // production image stays on screen until the user
                // actually starts dragging — otherwise the bridge's
                // initial render would flash a half-rendered preview
                // image right after a clean production result.
                self.viewportBridge?.suppressNextFrame()
                self.viewportBridge?.start()
            }
        }
    }

    func startAnimationRender() {
        guard hasAnimation else { return }
        guard renderState == .sceneLoaded || renderState == .completed
              || renderState == .cancelled else { return }

        // Stop the viewport before production renders (see startRender).
        viewportBridge?.stop()

        renderState = .rendering
        progress = 0.0
        cancelFlag.value = false
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
                self.elapsedTime = Date().timeIntervalSince(start)
                self.remainingTime = bridgeForTimer.etaRemainingSeconds()?.doubleValue
            }
        }

        let cancelRef = cancelFlag
        let bridgeForProgress = bridge
        bridge.setProgressBlock { [weak self] (prog: Double, total: Double, title: String) -> Bool in
            bridgeForProgress.etaUpdateProgress(prog, total: total)
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                self.progress = total > 0 ? prog / total : 0
                self.progressTitle = title
            }
            return !cancelRef.value
        }

        let buffer = imageBuffer
        let sceneSizeReported = AtomicBool(false)
        bridge.setImageOutputBlock { [weak self]
            (pImageData: UnsafePointer<UInt16>?,
             width: UInt32, height: UInt32,
             rcTop: UInt32, rcLeft: UInt32,
             rcBottom: UInt32, rcRight: UInt32) in
            guard let pImageData = pImageData else { return }

            if !sceneSizeReported.value {
                sceneSizeReported.value = true
                let w = CGFloat(width)
                let h = CGFloat(height)
                Task { @MainActor [weak self] in
                    self?.sceneSize = CGSize(width: w, height: h)
                }
            }

            guard let nsImage = buffer.handleOutput(
                pImageData: pImageData,
                width: width, height: height,
                rcTop: rcTop, rcLeft: rcLeft,
                rcBottom: rcBottom, rcRight: rcRight
            ) else { return }

            Task { @MainActor [weak self] in
                self?.renderedImage = nsImage
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
                if let start = self.renderStartTime {
                    self.elapsedTime = Date().timeIntervalSince(start)
                }
                self.renderStartTime = nil
                self.remainingTime = nil

                if cancelRef.value {
                    self.renderState = .cancelled
                } else if success {
                    self.renderState = .completed
                } else {
                    self.renderState = .error("Animation rasterization failed")
                }
                // Restart the interactive viewport (see startRender).
                self.viewportBridge?.suppressNextFrame()
                self.viewportBridge?.start()
            }
        }
    }

    func cancelRender() {
        cancelFlag.value = true
        renderState = .cancelling
    }

    func editSceneFile() {
        guard loadedFilePath != nil else { return }

        if isEditorVisible {
            isEditorVisible = false
            return
        }

        refreshEditorContents()
        isEditorVisible = true
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

    func revertEditorFile() {
        editorText = editorOriginalText
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
        viewportBridge?.shutdown()
        viewportBridge = nil
        bridge.clearAll()
        renderedImage = nil
        progress = 0.0
        progressTitle = ""
        elapsedTime = 0
        remainingTime = nil
        sceneSize = nil
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

    /// Reload editor contents from the current loadedFilePath.
    func refreshEditorContents() {
        guard let path = loadedFilePath else { return }
        do {
            let content = try String(contentsOfFile: path, encoding: .utf8)
            editorText = content
            editorOriginalText = content
        } catch {
            editorText = "// Failed to load file: \(error.localizedDescription)"
            editorOriginalText = editorText
        }
    }

    func clearScene() {
        // Viewport bridge borrows the underlying job — tear it down
        // BEFORE bridge.clearAll() so the controller's render thread
        // is joined before the scene is destroyed.
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
        sceneSize = nil
        imageBuffer.reset()
        logMessages.removeAll()
        hasAnimation = false
        isEditorVisible = false
        editorText = ""
        editorOriginalText = ""
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
