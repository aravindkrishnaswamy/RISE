import SwiftUI
import Combine
import CoreGraphics
import AppKit
import UniformTypeIdentifiers

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
    @Published var sceneSize: CGSize? = nil
    @Published var logMessages: [LogMessage] = []
    @Published var isEditorVisible: Bool = false
    @Published var editorText: String = ""
    @Published var editorOriginalText: String = ""
    @Published var hasAnimation: Bool = false

    private let bridge = RISEBridge()
    private let cancelFlag = AtomicBool(false)
    private let imageBuffer = RenderImageBuffer()
    private var renderStartTime: Date? = nil
    private var displayTimer: Timer? = nil

    init() {
        versionString = RISEBridge.versionString()
        bridge.setLogBlock { [weak self] (level: RISELogLevel, message: String) in
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                let entry = LogMessage(level: level, text: message, timestamp: Date())
                self.logMessages.append(entry)
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
                // Clear the current scene, then load the new one
                bridge.clearAll()
                renderedImage = nil
                progress = 0.0
                progressTitle = ""
                elapsedTime = 0
                sceneSize = nil
                imageBuffer.reset()
                logMessages.removeAll()
            case .alertSecondButtonReturn:
                // Merge — just proceed with loading
                break
            default:
                return
            }
        }

        loadScene(at: url.path)
    }

    func loadScene(at path: String) {
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

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                if success {
                    self.loadedFilePath = path
                    self.renderState = .sceneLoaded
                    self.progressTitle = ""
                    self.hasAnimation = bridgeRef.hasAnimatedObjects()
                    // Refresh the editor contents if it's open
                    if self.isEditorVisible {
                        self.refreshEditorContents()
                    }
                } else {
                    self.renderState = .error("Failed to load scene")
                }
            }
        }
    }

    func startRender() {
        guard renderState == .sceneLoaded || renderState == .completed
              || renderState == .cancelled else { return }

        renderState = .rendering
        progress = 0.0
        cancelFlag.value = false
        renderedImage = nil
        imageBuffer.reset()
        elapsedTime = 0
        renderStartTime = Date()

        displayTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self = self, let start = self.renderStartTime else { return }
                self.elapsedTime = Date().timeIntervalSince(start)
            }
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
        Task.detached { [weak self] in
            let success = bridgeRef.rasterize()

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                self.displayTimer?.invalidate()
                self.displayTimer = nil
                if let start = self.renderStartTime {
                    self.elapsedTime = Date().timeIntervalSince(start)
                }
                self.renderStartTime = nil

                if cancelRef.value {
                    self.renderState = .cancelled
                } else if success {
                    self.renderState = .completed
                } else {
                    self.renderState = .error("Rasterization failed")
                }
            }
        }
    }

    func startAnimationRender() {
        guard hasAnimation else { return }
        guard renderState == .sceneLoaded || renderState == .completed
              || renderState == .cancelled else { return }

        renderState = .rendering
        progress = 0.0
        cancelFlag.value = false
        renderedImage = nil
        imageBuffer.reset()
        elapsedTime = 0
        renderStartTime = Date()

        displayTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self = self, let start = self.renderStartTime else { return }
                self.elapsedTime = Date().timeIntervalSince(start)
            }
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
        Task.detached { [weak self] in
            let success = bridgeRef.rasterizeAnimation()

            await MainActor.run { [weak self] in
                guard let self = self else { return }
                self.displayTimer?.invalidate()
                self.displayTimer = nil
                if let start = self.renderStartTime {
                    self.elapsedTime = Date().timeIntervalSince(start)
                }
                self.renderStartTime = nil

                if cancelRef.value {
                    self.renderState = .cancelled
                } else if success {
                    self.renderState = .completed
                } else {
                    self.renderState = .error("Animation rasterization failed")
                }
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

        // Clear the current scene
        bridge.clearAll()
        renderedImage = nil
        progress = 0.0
        progressTitle = ""
        elapsedTime = 0
        sceneSize = nil
        imageBuffer.reset()
        logMessages.removeAll()

        // Reload the scene from disk
        loadScene(at: path)
    }

    var isEditorDirty: Bool {
        editorText != editorOriginalText
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
        bridge.clearAll()
        renderState = .idle
        renderedImage = nil
        loadedFilePath = nil
        progress = 0.0
        progressTitle = ""
        elapsedTime = 0
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
        let total = Int(elapsedTime)
        let hours = total / 3600
        let minutes = (total % 3600) / 60
        let seconds = total % 60
        if hours > 0 {
            return String(format: "%d:%02d:%02d", hours, minutes, seconds)
        } else {
            return String(format: "%d:%02d", minutes, seconds)
        }
    }
}
