//////////////////////////////////////////////////////////////////////
//
//  ViewportView.swift - Interactive 3D viewport for the macOS app.
//
//    Composition: render image + overlay toolbar + timeline + mouse
//    event routing to the RISEViewportBridge.  All edit logic lives
//    in C++ (SceneEditController); this view is purely a thin
//    sink + event router.
//
//    Phase 3 wires events through to the controller but the
//    controller is in skeleton mode (no interactive rasterizer):
//    the user mutates the in-memory scene, presses "Render" to see
//    the production result.  Phase 5 wires up live preview.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import AppKit

struct ViewportView: View {
    let bridge: RISEViewportBridge
    @Binding var image: NSImage?
    let timelineVisible: Bool
    @Binding var sceneTime: Double
    let timelineMax: Double
    /// True while the user can interact (drag, scrub, edit).  False
    /// while a production render is in flight — the toolbar greys
    /// out and the canvas ignores pointer events so the production
    /// rasterizer doesn't race with edits.
    var interactionEnabled: Bool = true
    /// Called whenever the active tool changes or a pointer-down
    /// event fires (which may have updated picking).  ContentView
    /// uses this to bump the property-panel refresh trigger so the
    /// panel re-reads bridge.panelMode + propertySnapshot.
    var onSelectionMayHaveChanged: () -> Void = {}

    @State private var selectedTool: ViewportTool = .select

    var body: some View {
        VStack(spacing: 0) {
            ZStack(alignment: .top) {
                ViewportCanvas(
                    image: $image,
                    cursor: interactionEnabled ? selectedTool.nsCursor : .arrow,
                    surfaceDimensionsProvider: { [weak bridge] in
                        bridge?.cameraSurfaceDimensions ?? .zero
                    },
                    onPointerDown: { p in
                        guard interactionEnabled else { return }
                        bridge.pointerDown(x: Double(p.x), y: Double(p.y))
                        // Pointer-down on Select tool may have just
                        // picked an object; pointer-down on motion
                        // tools doesn't change selection but the
                        // panel refresh is cheap so we always notify.
                        onSelectionMayHaveChanged()
                    },
                    onPointerMove: { p in
                        guard interactionEnabled else { return }
                        bridge.pointerMove(x: Double(p.x), y: Double(p.y))
                    },
                    onPointerUp: { p in
                        guard interactionEnabled else { return }
                        bridge.pointerUp(x: Double(p.x), y: Double(p.y))
                    }
                )

                ViewportToolbar(
                    selectedTool: $selectedTool,
                    onUndo: { bridge.undo() },
                    onRedo: { bridge.redo() }
                )
                .padding(.top, 8)
                .disabled(!interactionEnabled)
                .opacity(interactionEnabled ? 1.0 : 0.5)
                .onChange(of: selectedTool) { _, newValue in
                    bridge.currentTool = newValue.bridgeValue
                    // Panel mode is derived from the current tool;
                    // bump the refresh so the panel switches between
                    // empty / camera / object as the user toggles.
                    onSelectionMayHaveChanged()
                }
            }
            // Re-sync the toolbar's selection to the underlying
            // controller whenever the bridge identity changes (a new
            // scene was loaded, or the bridge was rebuilt for any
            // other reason).  The new controller defaults to .select
            // internally; without this the toolbar's persisted
            // @State would still highlight (say) Orbit while pointer
            // events go to the Select tool.  `.task(id:)` runs on
            // appear AND on id change, so it covers both initial
            // attach and subsequent scene loads.
            .task(id: ObjectIdentifier(bridge)) {
                bridge.currentTool = selectedTool.bridgeValue
                onSelectionMayHaveChanged()
            }

            if timelineVisible {
                TimelineSlider(
                    time: $sceneTime,
                    range: 0...timelineMax,
                    onScrubBegin: { bridge.scrubTimeBegin() },
                    onScrubEnd:   { bridge.scrubTimeEnd() }
                )
                .padding([.horizontal, .bottom], 8)
                .disabled(!interactionEnabled)
                .opacity(interactionEnabled ? 1.0 : 0.5)
                .onChange(of: sceneTime) { _, newValue in
                    guard interactionEnabled else { return }
                    bridge.scrubTime(newValue)
                }
            }
        }
        // Bridge lifetime is owned by RenderViewModel — it runs from
        // scene-load until clearScene.  This view's appear/disappear
        // intentionally does NOT start/stop the bridge.
    }
}

/// SwiftUI host for an NSImage with NSResponder-style mouse events
/// translated into the viewport surface coordinate system.
private struct ViewportCanvas: NSViewRepresentable {
    @Binding var image: NSImage?
    let cursor: NSCursor
    /// Closure that returns the camera's stable full-resolution
    /// dimensions for surface-point math.  Closure (rather than a
    /// captured value) so each pointer event reads the freshest
    /// value without us having to invalidate the SwiftUI view tree
    /// on every dim refresh — the bridge updates the underlying
    /// values on its own schedule (scene reload, etc.).
    let surfaceDimensionsProvider: () -> NSSize
    let onPointerDown: (CGPoint) -> Void
    let onPointerMove: (CGPoint) -> Void
    let onPointerUp:   (CGPoint) -> Void

    func makeNSView(context: Context) -> ViewportNSView {
        let v = ViewportNSView()
        v.onPointerDown = onPointerDown
        v.onPointerMove = onPointerMove
        v.onPointerUp   = onPointerUp
        v.toolCursor    = cursor
        v.surfaceDimensionsProvider = surfaceDimensionsProvider
        return v
    }

    func updateNSView(_ nsView: ViewportNSView, context: Context) {
        nsView.image = image
        nsView.onPointerDown = onPointerDown
        nsView.onPointerMove = onPointerMove
        nsView.onPointerUp   = onPointerUp
        nsView.toolCursor = cursor
        nsView.surfaceDimensionsProvider = surfaceDimensionsProvider
        // Force AppKit to recompute the cursor rect so the new tool's
        // cursor takes effect immediately even if the pointer is still
        // hovering over the view.
        nsView.window?.invalidateCursorRects(for: nsView)
        nsView.needsDisplay = true
    }
}

/// NSView that displays an NSImage and forwards pointer events.
final class ViewportNSView: NSView {
    var image: NSImage? {
        didSet {
            // Image dimensions / aspect changed → cursor rect needs to
            // be recomputed (the rect tracks the aspect-fit draw area,
            // not the full view bounds).
            window?.invalidateCursorRects(for: self)
        }
    }
    var onPointerDown: ((CGPoint) -> Void)?
    var onPointerMove: ((CGPoint) -> Void)?
    var onPointerUp:   ((CGPoint) -> Void)?

    /// Returns the camera's stable full-resolution dimensions so
    /// `surfacePoint` converts to a coord space that doesn't drift
    /// with the preview-scale state machine.  When the closure
    /// returns (0,0) (e.g. before the bridge is wired) we fall back
    /// to `image.size` — same as the old behaviour, harmless because
    /// no rendering / dragging is yet underway.
    var surfaceDimensionsProvider: (() -> NSSize)?

    /// Cursor displayed when the pointer is over the rendered image
    /// area of this view.  Outside the image (the empty surround
    /// produced by aspect-fit), AppKit reverts to whatever cursor
    /// the parent / window provides — typically the system arrow.
    /// Set by ViewportCanvas.updateNSView whenever the toolbar's
    /// selection changes; the view invalidates its cursor rects on
    /// tool change so the new cursor takes effect immediately.
    var toolCursor: NSCursor = .arrow

    override var isFlipped: Bool { true }   // top-left origin like UIKit / Metal

    /// Compute the aspect-fit rect that `draw(_:)` will use for the
    /// current image inside the current bounds.  Returns nil when
    /// there is no image — the cursor rect collapses to nothing and
    /// the default system cursor wins.
    private func currentImageDrawRect() -> NSRect? {
        guard let image = image else { return nil }
        let imgSize = image.size
        guard imgSize.width > 0, imgSize.height > 0 else { return nil }
        let scale = min(bounds.width / imgSize.width, bounds.height / imgSize.height)
        let drawW = imgSize.width * scale
        let drawH = imgSize.height * scale
        return NSRect(x: (bounds.width - drawW) / 2,
                      y: (bounds.height - drawH) / 2,
                      width: drawW, height: drawH)
    }

    override func resetCursorRects() {
        // Cursor rect tracks the aspect-fit draw area only.  The
        // empty letterbox / pillarbox surround inherits the parent's
        // cursor (system arrow) — that's the "moves outside the
        // manipulation area" behaviour the user expects.
        if let drawRect = currentImageDrawRect() {
            addCursorRect(drawRect, cursor: toolCursor)
        }
    }

    override func resize(withOldSuperviewSize oldSize: NSSize) {
        super.resize(withOldSuperviewSize: oldSize)
        // The aspect-fit rect depends on view bounds; invalidate so
        // resetCursorRects re-runs with the new geometry.
        window?.invalidateCursorRects(for: self)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.windowBackgroundColor.setFill()
        bounds.fill()

        guard let image = image else {
            // Placeholder — Phase 3 has no live preview yet.
            let para = NSMutableParagraphStyle()
            para.alignment = .center
            let attrs: [NSAttributedString.Key: Any] = [
                .foregroundColor: NSColor.secondaryLabelColor,
                .font: NSFont.systemFont(ofSize: 13),
                .paragraphStyle: para,
            ]
            let str = "Render to see the scene"
            let size = (str as NSString).size(withAttributes: attrs)
            let r = NSRect(x: bounds.midX - size.width/2,
                           y: bounds.midY - size.height/2,
                           width: size.width, height: size.height)
            (str as NSString).draw(in: r, withAttributes: attrs)
            return
        }

        // Aspect-fit the image into bounds (kept in sync with
        // currentImageDrawRect() above — both must produce the same
        // rect or the cursor will land somewhere the image isn't).
        guard let drawRect = currentImageDrawRect() else { return }
        image.draw(in: drawRect,
                   from: .zero,
                   operation: .sourceOver,
                   fraction: 1.0,
                   respectFlipped: true,
                   hints: [.interpolation: NSImageInterpolation.none.rawValue])
    }

    /// Convert a window-coordinate event to image-pixel space — i.e.
    /// the same coord system the rasterizer / camera use internally
    /// (0..GetWidth() × 0..GetHeight()).  Two adjustments map view
    /// points to image pixels:
    ///
    ///   1. Aspect-fit offset: the image is drawn centred in the view
    ///      with letterbox / pillarbox margins.  Subtract drawRect's
    ///      origin so view-relative coords become image-relative.
    ///   2. Pixel-density rescale: the view's draw-rect width is in
    ///      view points; the image's width is in rasterizer pixels.
    ///      Divide by drawRect.size and multiply by image.size to
    ///      land in pixel space.
    ///
    /// HiDPI is handled implicitly because the rescale step uses
    /// drawRect (in points) and image.size (in pixels), not the
    /// backing-scale factor — both ratios bake in the right number.
    /// Returns nil when there's no image to map against.  Coords may
    /// land outside [0, image.size] if the user drags past the
    /// image's edge — the controller treats that as "no hit" for
    /// picking and as a clamped delta for orbit / pan / zoom.
    private func surfacePoint(from event: NSEvent) -> CGPoint? {
        let p = self.convert(event.locationInWindow, from: nil)
        guard let drawRect = currentImageDrawRect() else { return nil }
        guard let image = image else { return nil }
        guard drawRect.width > 0, drawRect.height > 0 else { return nil }

        // Use the camera's STABLE full-resolution dimensions — not
        // image.size — as the conversion target.  During a fast
        // drag, image.size shrinks to the preview-scale subsample
        // dims (e.g. 200×200 from a 800×800 camera at scale=4) and
        // grows back to full-res when motion adapts down.  If
        // surfacePoint reported coords in image.size space, mLastPx
        // captured at one scale level would be in a different coord
        // system from the next event's px — the controller's
        // (px - mLastPx) delta would mix scale levels and produce
        // 4×–32× pan/orbit jumps whenever the state machine steps.
        // The bridge's cameraSurfaceDimensions reads from the
        // controller's mFullResW/H which DO NOT flicker, so the
        // delta is stable and the camera tunings (×0.0087 rad/px
        // etc.) behave consistently independent of subsample state.
        // Fall back to image.size when no provider is wired or the
        // cache is uninitialised — same as the old behaviour.
        let providedDims = surfaceDimensionsProvider?() ?? .zero
        let surfaceSize: NSSize
        if providedDims.width > 0 && providedDims.height > 0 {
            surfaceSize = providedDims
        } else {
            surfaceSize = image.size
        }
        guard surfaceSize.width > 0, surfaceSize.height > 0 else { return nil }

        let nx = (p.x - drawRect.minX) / drawRect.width
        let ny = (p.y - drawRect.minY) / drawRect.height
        return CGPoint(x: nx * surfaceSize.width, y: ny * surfaceSize.height)
    }

    override func mouseDown(with event: NSEvent) {
        guard let p = surfacePoint(from: event) else { return }
        onPointerDown?(p)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let p = surfacePoint(from: event) else { return }
        onPointerMove?(p)
    }

    override func mouseUp(with event: NSEvent) {
        guard let p = surfacePoint(from: event) else { return }
        onPointerUp?(p)
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var acceptsFirstResponder: Bool { true }
}
