//////////////////////////////////////////////////////////////////////
//
//  ViewportGizmoOverlay.swift - Transparent overlay drawn on top of
//    the live preview when an Object-transform tool is active.
//    Subscribes to the C++ controller's gizmo handle array and
//    renders the per-tool glyphs (arrows, plane quads, rings, scale
//    cubes) in widget-space.
//
//  Coordinate convention: the bridge returns positions in the
//  camera's image-pixel space (matching pointer events).  The overlay
//  uses the same `surfaceDimensionsProvider` as ViewportCanvas to
//  normalise to widget points — image pixel `(0, H)` is widget top,
//  `(0, 0)` is widget bottom (matches the Y-up `(x, height-y)`
//  convention `PixelBasedPelRasterizer` feeds GenerateRay).
//
//  This is the platform-side counterpart of the screen-space layout
//  computed by SceneEditController; all math lives in C++ so the
//  three platform implementations agree on the gizmo geometry.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import AppKit

struct ViewportGizmoOverlay: View {
    let bridge: RISEViewportBridge
    /// Re-render trigger — the parent bumps this each preview frame
    /// (or on tool / selection change) so the overlay refreshes its
    /// snapshot of the handle array.
    let refreshTrigger: Int
    /// Same provider ViewportCanvas uses, so the overlay's widget-
    /// space mapping matches the underlying image.  Returns NSSize
    /// in image-pixel space.
    let surfaceDimensionsProvider: () -> CGSize

    @State private var handles: [RISEViewportGizmoHandle] = []

    var body: some View {
        GeometryReader { geom in
            let surface = surfaceDimensionsProvider()
            Canvas { ctx, size in
                guard surface.width > 0, surface.height > 0 else { return }
                // Aspect-fit the image-pixel space inside the widget,
                // matching ViewportCanvas's letter-box behaviour.
                let scaleX = size.width  / surface.width
                let scaleY = size.height / surface.height
                let scale  = min(scaleX, scaleY)
                let dispW  = surface.width  * scale
                let dispH  = surface.height * scale
                let ox     = (size.width  - dispW) * 0.5
                let oy     = (size.height - dispH) * 0.5

                // Image-pixel-space `(sx, sy)` uses +Y = up (see
                // PixelBasedPelRasterizer.cpp:614 which feeds
                // `Point2(x, height - y)`).  Widget-space has +Y =
                // down, so flip Y around the surface height.
                func toWidget(_ px: CGFloat, _ py: CGFloat) -> CGPoint {
                    let wx = ox + px * scale
                    let wy = oy + (surface.height - py) * scale
                    return CGPoint(x: wx, y: wy)
                }

                let activeKind: RISEViewportGizmoKind? =
                    bridge.gizmoDragActive ? bridge.activeGizmoKind : nil
                let activeAxis: NSInteger = bridge.activeGizmoAxis

                for h in handles {
                    let p = toWidget(h.screenX, h.screenY)
                    let isActive = bridge.gizmoDragActive
                        && activeKind == h.kind
                        && activeAxis == h.axis
                    drawHandle(ctx: ctx, handle: h, center: p,
                               scale: scale, isActive: isActive)
                }
            }
            .allowsHitTesting(false)
            .onChange(of: refreshTrigger) { _, _ in
                bridge.refreshGizmoHandles()
                handles = bridge.gizmoHandles
            }
            .onAppear {
                bridge.refreshGizmoHandles()
                handles = bridge.gizmoHandles
            }
        }
    }

    private func axisColor(_ axis: NSInteger) -> Color {
        switch axis {
        case 0: return .red
        case 1: return .green
        case 2: return .blue
        default: return .yellow  // screen-aligned
        }
    }

    private func drawHandle(ctx: GraphicsContext,
                            handle: RISEViewportGizmoHandle,
                            center: CGPoint,
                            scale: CGFloat,
                            isActive: Bool)
    {
        let color = axisColor(handle.axis)
        let stroke = isActive ? Color.white : color
        let r = handle.screenRadius * scale
        switch handle.kind {
        case .axisArrow, .axisScaleHandle:
            // Filled circle at the tip; for scale, a square is the
            // canonical glyph but a disc reads as "knob" in either
            // role and the renderer overhead is the same.
            let rect = CGRect(x: center.x - r, y: center.y - r,
                              width: 2*r, height: 2*r)
            ctx.fill(Path(ellipseIn: rect), with: .color(color.opacity(0.85)))
            ctx.stroke(Path(ellipseIn: rect),
                       with: .color(stroke), lineWidth: isActive ? 2.5 : 1.5)
        case .axisPlane:
            // Small square offset toward the plane's quadrant.  Plane
            // colour follows the axis NOT in the plane (matches the
            // controller convention).
            let s = r * 1.4
            let rect = CGRect(x: center.x - s, y: center.y - s,
                              width: 2*s, height: 2*s)
            ctx.fill(Path(rect), with: .color(color.opacity(0.40)))
            ctx.stroke(Path(rect),
                       with: .color(stroke), lineWidth: isActive ? 2.5 : 1.5)
        case .screenCenter, .uniformScaleCube:
            // Yellow disc / square at the pivot.
            let rect = CGRect(x: center.x - r, y: center.y - r,
                              width: 2*r, height: 2*r)
            ctx.fill(Path(ellipseIn: rect), with: .color(color.opacity(0.30)))
            ctx.stroke(Path(ellipseIn: rect),
                       with: .color(stroke), lineWidth: isActive ? 2.5 : 1.5)
        case .axisRing, .screenRing:
            // Stroke a circle at the captured radius.  For now drawn
            // as a flat ring — the full elliptical projection from
            // world to screen is a future enhancement (B5+).
            let rect = CGRect(x: center.x - r, y: center.y - r,
                              width: 2*r, height: 2*r)
            ctx.stroke(Path(ellipseIn: rect),
                       with: .color(stroke.opacity(isActive ? 1.0 : 0.80)),
                       lineWidth: isActive ? 3.0 : 2.0)
        @unknown default:
            break
        }
    }
}
