//////////////////////////////////////////////////////////////////////
//
//  MetalEDRView.swift - L5a Mac EDR display path for the RISE
//  viewport.
//
//  Round-3 architecture (after the "doesn't compose with toolbar"
//  feedback): the renderer drives a raw CAMetalLayer instead of
//  owning an MTKView.  This lets it be hosted as either:
//    1. A sublayer of `ViewportNSView` (interactive editor + EDR)
//       — toolbar / scrubber / properties panel keep working
//       because they're SwiftUI overlays / siblings, and pointer
//       events stay on ViewportNSView (CALayers don't intercept
//       mouse events).
//    2. A sublayer of `MetalEDRSimpleView`'s NSView (no-scene-
//       loaded case shown via RenderImageView).
//
//  The renderer owns ONE layer at a time — the active client
//  (ViewportNSView or MetalEDRSimpleView) attaches via
//  `attach(layer:)` and detaches via `detach()`.  SwiftUI's
//  view-tree swap between ViewportView and RenderImageView
//  drives the attach/detach handoff naturally.
//
//  Pixel-data path (carried over from round-2 P2-B fix):
//    Worker-thread upload writes regions into a CPU-side
//    `stagingBuffer: [UInt16]` under stagingLock.  `present()`
//    (main thread) takes inflightSem, locks staging, full-image
//    `replace`s a GPU `displayTexture` from staging, releases
//    staging, encodes a fragment-shader pass that aspect-fit-
//    samples the texture into the layer's drawable, presents,
//    and arms a `cmd.addCompletedHandler { inflightSem.signal() }`.
//    The semaphore caps in-flight GPU work at 1 so CPU never
//    overwrites a texture the GPU is currently sampling.
//
//////////////////////////////////////////////////////////////////////

import AppKit
import Metal
import QuartzCore
import SwiftUI

/// L5a round-5 — distinguishes which bridge slot a renderer
/// drives (and what default clear-alpha its CAMetalLayer uses).
enum MetalEDRRendererRole {
    /// Drives `setHDRImageOutputBlock:` — fires per-tile + per-frame
    /// from the production rasterizer.  Layer is opaque base.
    case production
    /// Drives `setInteractiveHDRImageOutputBlock:` — fires only on
    /// frame-complete from the interactive rasterizer.  Layer
    /// composites over production with transparent background, so
    /// regions the interactive renderer hasn't touched fall through
    /// to whatever production drew underneath.
    case interactive
}

/// L5a — durable holder of Metal state + bridge HDR-block binding.
/// Owned by `RenderViewModel` for its full lifetime.  Round-3
/// reshaped to drive a raw CAMetalLayer (vs the prior MTKView
/// host) so SwiftUI clients can compose EDR rendering into
/// arbitrary NSView containers without losing toolbar / scrubber
/// / pointer-event integration.  Round-5 added the `role`
/// parameter so the same class instances both production and
/// interactive renderers, each bound to its own bridge block
/// + with appropriate clear-color semantics.
///
/// Concurrency: deliberately NOT `@MainActor` — `upload()` runs
/// on rasterizer worker threads.  `stagingLock` (NSLock) protects
/// the CPU-side staging buffer; `inflightSem` caps GPU-bound work
/// at one frame in flight.  `attachedLayer` is read/written only
/// from the main thread (SwiftUI lifecycle calls).
final class MetalEDRRenderer: NSObject, @unchecked Sendable {
    let role: MetalEDRRendererRole
    let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLRenderPipelineState
    private let bridge: RISEBridge

    private let stagingLock = NSLock()
    private var stagingBuffer: [UInt16] = []
    private var stagingWidth:  Int = 0
    private var stagingHeight: Int = 0

    private var displayTexture: MTLTexture? = nil
    private var displayWidth:   Int = 0
    private var displayHeight:  Int = 0

    private let inflightSem = DispatchSemaphore(value: 1)

    // Currently-attached presentation layer.  Main-thread access only.
    // Weak so the layer's lifetime is bounded by its host NSView /
    // SwiftUI lifecycle; an upload's main-thread `present()` no-ops
    // when nil (a fresh attach immediately after replays the
    // staging buffer to the new layer).
    private weak var attachedLayer: CAMetalLayer? = nil

    // Coalescing: multiple worker uploads in quick succession should
    // schedule a SINGLE main-thread present, not N.  `presentScheduled`
    // is set when a worker dispatches an async present and cleared
    // once main begins running it.  Atomic so the worker thread
    // checks safely without taking stagingLock.
    private let presentScheduled = NSLock()
    private var presentInFlight = false

    init?(bridge: RISEBridge, role: MetalEDRRendererRole) {
        guard let dev = MTLCreateSystemDefaultDevice(),
              let queue = dev.makeCommandQueue() else {
            return nil
        }
        self.bridge = bridge
        self.role   = role
        self.device = dev
        self.commandQueue = queue
        guard let pipeline = MetalEDRRenderer.buildPipeline(device: dev) else {
            return nil
        }
        self.pipelineState = pipeline
        super.init()

        // Bind the HDR block ONCE for the renderer's lifetime to
        // the appropriate bridge slot.  Production renderers wire
        // into `setHDRImageOutputBlock:` (per-tile + per-frame
        // fires); interactive renderers wire into
        // `setInteractiveHDRImageOutputBlock:` (frame-complete only,
        // no per-tile flash).  Lifetime: renderer is owned by
        // RenderViewModel; bridge is also owned by RenderViewModel.
        // Renderer is dealloc'd BEFORE bridge in RenderViewModel's
        // deinit order.
        let hdrCallback: RISEHDRImageOutputBlock = { [weak self]
            (pImageData: UnsafePointer<UInt16>?,
             width: UInt32, height: UInt32,
             rcTop: UInt32, rcLeft: UInt32,
             rcBottom: UInt32, rcRight: UInt32) in
            guard let self = self, let p = pImageData else { return }
            self.upload(
                pixels: p,
                width: Int(width), height: Int(height),
                top: Int(rcTop), left: Int(rcLeft),
                bottom: Int(rcBottom), right: Int(rcRight))
        }
        switch role {
        case .production:  bridge.setHDRImageOutputBlock(hdrCallback)
        case .interactive: bridge.setInteractiveHDRImageOutputBlock(hdrCallback)
        }
    }

    deinit {
        switch role {
        case .production:  bridge.setHDRImageOutputBlock(nil)
        case .interactive: bridge.setInteractiveHDRImageOutputBlock(nil)
        }
    }

    // MARK: - Layer attach / detach (called from SwiftUI lifecycle)

    /// Configure `layer` for EDR composition and start using it as
    /// the present target.  Call from main thread.  Triggers an
    /// immediate present so the layer picks up the current staging
    /// state without waiting for the next bridge fire.
    ///
    /// L5a round-6 — both production and interactive renderers
    /// attach to the SAME CAMetalLayer.  The architectural
    /// separation lives at the VFS level (two FrameStores with
    /// independent lifecycles); the display surface is shared so
    /// the user sees ONE unified image (latest-renderer-wins,
    /// matching the legacy NSImage path's UX).  Both renderers
    /// configure the layer to be opaque (no alpha bleed-through)
    /// and the fragment shader forces alpha=1 — areas where the
    /// rasterizer wrote `c.a == 0` (e.g. open-sky background)
    /// composite as opaque-black rather than letting the previous
    /// layer's pixels show through.  Layer config is idempotent;
    /// the second attach() (from the other renderer) re-applies
    /// the same settings without harm.
    func attach(layer: CAMetalLayer) {
        layer.device = device
        layer.pixelFormat = .rgba16Float
        if let cs = CGColorSpace(name: CGColorSpace.extendedLinearSRGB) {
            layer.colorspace = cs
        }
        layer.wantsExtendedDynamicRangeContent = true
        layer.framebufferOnly = false
        layer.isOpaque = true
        attachedLayer = layer
        // Push the latest staging-buffer state immediately so the
        // first frame after attach isn't blank.
        present()
    }

    /// Stop driving the previously-attached layer.  Call from main
    /// thread before the layer is removed from the view hierarchy.
    /// Safe to call when nothing is attached.
    func detach() {
        attachedLayer = nil
    }

    // MARK: - Bridge HDR-block fan-in

    private func upload(pixels: UnsafePointer<UInt16>,
                        width: Int, height: Int,
                        top: Int, left: Int,
                        bottom: Int, right: Int) {
        stagingLock.lock()
        // Resize staging buffer on first call / dim change.
        let need = width * height * 4
        if stagingWidth != width
            || stagingHeight != height
            || stagingBuffer.count != need {
            stagingBuffer = [UInt16](repeating: 0, count: need)
            stagingWidth  = width
            stagingHeight = height
        }
        let regionWidth  = right  - left + 1
        let regionHeight = bottom - top  + 1
        if regionWidth > 0 && regionHeight > 0 {
            let bytesPerPixel  = 8                              // 4 channels × binary16
            let regionRowBytes = regionWidth * bytesPerPixel
            stagingBuffer.withUnsafeMutableBufferPointer { dstBuf in
                guard let dstBase = dstBuf.baseAddress else { return }
                let srcBytes = UnsafeRawPointer(pixels)
                let dstBytes = UnsafeMutableRawPointer(dstBase)
                for y in 0..<regionHeight {
                    let srcRow = srcBytes.advanced(
                        by: ((top + y) * width + left) * bytesPerPixel)
                    let dstRow = dstBytes.advanced(
                        by: ((top + y) * width + left) * bytesPerPixel)
                    memcpy(dstRow, srcRow, regionRowBytes)
                }
            }
        }
        stagingLock.unlock()

        // Coalesce — schedule at most one main-thread present per
        // burst of uploads.  Multiple worker fires set the flag to
        // true; only the first dispatches.  `present()` clears the
        // flag at its start so the next burst gets a fresh dispatch.
        presentScheduled.lock()
        let needSchedule = !presentInFlight
        presentInFlight = true
        presentScheduled.unlock()
        if needSchedule {
            DispatchQueue.main.async { [weak self] in
                self?.present()
            }
        }
    }

    // MARK: - Main-thread present

    /// Push the latest staging-buffer state to the attached layer
    /// via a fragment-shader-driven aspect-fit blit.  Call from
    /// main thread.  Safe to call without an attached layer (no-op).
    func present() {
        // Clear the coalesce flag at the very start.  Subsequent
        // uploads will re-dispatch.
        presentScheduled.lock()
        presentInFlight = false
        presentScheduled.unlock()

        guard let layer = attachedLayer else { return }

        // L5a round-2 P2-B: gate concurrent draws on the
        // semaphore; cmd completion handler signals.
        inflightSem.wait()

        var fitScale = SIMD2<Float>(1, 1)
        var srcTexture: MTLTexture? = nil
        stagingLock.lock()
        let srcW = stagingWidth
        let srcH = stagingHeight
        if srcW > 0 && srcH > 0 {
            if displayTexture == nil
                || displayWidth != srcW
                || displayHeight != srcH {
                let desc = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: .rgba16Float,
                    width: srcW, height: srcH,
                    mipmapped: false)
                desc.usage = [.shaderRead, .renderTarget]
                desc.storageMode = .shared
                displayTexture = device.makeTexture(descriptor: desc)
                displayWidth   = srcW
                displayHeight  = srcH
            }
            if let dt = displayTexture {
                let bytesPerRow = srcW * 8
                stagingBuffer.withUnsafeBufferPointer { buf in
                    if let base = buf.baseAddress {
                        dt.replace(
                            region: MTLRegionMake2D(0, 0, srcW, srcH),
                            mipmapLevel: 0,
                            withBytes: UnsafeRawPointer(base),
                            bytesPerRow: bytesPerRow)
                    }
                }
                srcTexture = dt
            }
        }
        stagingLock.unlock()

        guard let src = srcTexture,
              let drawable = layer.nextDrawable() else {
            inflightSem.signal()
            return
        }

        // Aspect-fit scaling for the fullscreen-quad pass — letterbox
        // the source texture inside the drawable's actual pixel
        // dimensions.  When the host has already laid out the layer
        // at the source's aspect ratio (the common case in
        // ViewportNSView), the scale is (1, 1) and no letterboxing
        // is visible.
        let dstW = drawable.texture.width
        let dstH = drawable.texture.height
        let srcAspect = Double(src.width) / Double(src.height)
        let dstAspect = Double(dstW) / Double(dstH)
        if srcAspect > dstAspect {
            fitScale.y = Float(dstAspect / srcAspect)
        } else {
            fitScale.x = Float(srcAspect / dstAspect)
        }

        // L5a round-6 — clear opaque black for all roles.  The
        // shared single-layer architecture means we don't want
        // ANY transparent regions (would let the NSView's
        // background fill or stale drawable contents leak through);
        // letterbox bars from aspect-fit are intentionally
        // black-on-screen.
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture     = drawable.texture
        descriptor.colorAttachments[0].loadAction  = .clear
        descriptor.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1)
        descriptor.colorAttachments[0].storeAction = .store

        guard let cmd = commandQueue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: descriptor) else {
            inflightSem.signal()
            return
        }
        enc.setRenderPipelineState(pipelineState)
        enc.setVertexBytes(&fitScale, length: MemoryLayout<SIMD2<Float>>.size, index: 0)
        enc.setFragmentTexture(src, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()

        cmd.addCompletedHandler { [inflightSem] _ in
            inflightSem.signal()
        }
        cmd.present(drawable)
        cmd.commit()
    }

    // MARK: - Pipeline construction

    private static func buildPipeline(device: MTLDevice) -> MTLRenderPipelineState? {
        let source = """
        #include <metal_stdlib>
        using namespace metal;

        struct VertexOut {
            float4 position [[position]];
            float2 uv;
        };

        vertex VertexOut edrVertex(uint vid [[vertex_id]],
                                   constant float2 &fitScale [[buffer(0)]]) {
            float2 positions[4] = {
                float2(-1.0, -1.0),
                float2( 1.0, -1.0),
                float2(-1.0,  1.0),
                float2( 1.0,  1.0)
            };
            float2 uvs[4] = {
                float2(0.0, 1.0),
                float2(1.0, 1.0),
                float2(0.0, 0.0),
                float2(1.0, 0.0)
            };
            VertexOut out;
            out.position = float4(positions[vid] * fitScale, 0.0, 1.0);
            out.uv       = uvs[vid];
            return out;
        }

        fragment float4 edrFragment(VertexOut in [[stage_in]],
                                    texture2d<float> src [[texture(0)]]) {
            constexpr sampler s(filter::linear, address::clamp_to_edge);
            float4 c = src.sample(s, in.uv);
            // L5a round-6 — force alpha=1 regardless of source.
            // The rasterizer writes `c.a == 0` for open-sky / no-
            // hit pixels; we don't want those treated as "see-
            // through" by the layer compositor.  The single-layer
            // architecture composites everything as opaque-on-
            // black; transparent regions would only reveal stale
            // drawable contents or the NSView background fill,
            // never the "previous renderer" data the user might
            // have hoped for.  Architectural separation between
            // renderers lives at the VFS level; the display layer
            // is a unified opaque surface (latest-write-wins).
            return float4(c.rgb, 1.0);
        }
        """
        do {
            let library = try device.makeLibrary(source: source, options: nil)
            guard let vfn = library.makeFunction(name: "edrVertex"),
                  let ffn = library.makeFunction(name: "edrFragment") else {
                NSLog("MetalEDRRenderer: shader function lookup failed")
                return nil
            }
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction   = vfn
            desc.fragmentFunction = ffn
            desc.colorAttachments[0].pixelFormat = .rgba16Float
            return try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            NSLog("MetalEDRRenderer: pipeline build failed: \(error)")
            return nil
        }
    }
}

/// Simple SwiftUI-hosted EDR surface for the "no scene loaded"
/// fallback (RenderImageView).  Wraps an NSView whose sole job is
/// to host a CAMetalLayer that the renderer drives.  When a scene
/// is loaded the interactive editor (ViewportNSView) takes over
/// EDR rendering instead of this view.
struct MetalEDRSimpleView: NSViewRepresentable {
    let renderer: MetalEDRRenderer

    final class HostView: NSView {
        var metalLayer: CAMetalLayer? = nil
        override var isFlipped: Bool { true }
        override var wantsUpdateLayer: Bool { true }
        override func layout() {
            super.layout()
            metalLayer?.frame = bounds
            // Drawable size scales with backing-store factor for
            // Retina sharpness — drawableSize is in physical pixels.
            if let scale = window?.backingScaleFactor {
                metalLayer?.drawableSize = CGSize(
                    width: bounds.width * scale,
                    height: bounds.height * scale)
            }
        }
    }

    func makeNSView(context: Context) -> HostView {
        let v = HostView()
        v.wantsLayer = true
        let m = CAMetalLayer()
        v.metalLayer = m
        v.layer?.addSublayer(m)
        renderer.attach(layer: m)
        return v
    }

    func updateNSView(_ nsView: HostView, context: Context) {
        // No-op: layout(), the layer's geometry, and renderer
        // attachment are stable across SwiftUI updates.
    }

    static func dismantleNSView(_ nsView: HostView, coordinator: ()) {
        nsView.metalLayer?.removeFromSuperlayer()
        nsView.metalLayer = nil
    }
}
