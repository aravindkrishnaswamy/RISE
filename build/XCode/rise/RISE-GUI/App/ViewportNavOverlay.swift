//
//  ViewportNavOverlay.swift
//
//  Blender-style navigation axis-ball (Tier 2 / Direction B §4).  Six
//  ±X/±Y/±Z nubs the user clicks to snap the VIEW down a world axis
//  (SnapViewToAxis — non-destructive; auto-enters the free-fly ViewportPose
//  and never mutates a scene camera).  Plus Home / "set as home" and, while
//  free-flying, a "back to scene camera" control.
//
//  All layout + hit-test math is shared C++ (RefreshNavGizmo / NavGizmoNubAt,
//  mirroring the object gizmo); this view only draws the nubs and routes a
//  click.  The ball geometry is passed in WIDGET space, so nub positions come
//  back ready to draw + hit-test with no letterbox mapping.
//

import SwiftUI

struct ViewportNavOverlay: View {
    let bridge: RISEViewportBridge
    /// Bumped when a new preview frame arrives (camera may have moved), so the
    /// ball re-layouts to track the live view.  Reuses the viewport's gizmo
    /// refresh trigger.
    let refreshTrigger: Int

    @State private var nubs: [RISEViewportNavNub] = []
    @State private var freeFly = false
    @State private var homeSet = false

    private let ballRadius: CGFloat = 30
    private let nubRadius:  CGFloat = 9
    private var ballBox: CGFloat { (ballRadius + nubRadius) * 2 }

    var body: some View {
        // All-or-nothing: the ball + controls appear only when the gizmo lays
        // out (a supported pinhole interactive camera → non-empty nubs), so
        // there's no invisible corner that swallows clicks.  `reload` lives on
        // the container (not the ball) so it still runs while hidden and can
        // bring the overlay back when a pinhole frame arrives.  Matches the
        // Windows twin, which is likewise gated on RefreshNavGizmo success.
        VStack(alignment: .trailing, spacing: 6) {
            if !nubs.isEmpty {
                axisBall
                controls
            }
        }
        .onAppear(perform: reload)
        .onChange(of: refreshTrigger) { _, _ in reload() }
    }

    // MARK: - Axis ball

    private var axisBall: some View {
        let center = CGPoint(x: ballBox / 2, y: ballBox / 2)
        return ZStack {
            Circle()
                .fill(Color.black.opacity(0.22))
                .frame(width: ballRadius * 2, height: ballRadius * 2)
                .position(center)
            Canvas { ctx, _ in
                // Back-facing nubs first so front-facing ones draw on top —
                // matches the C++ hit-test's front-facing tie-break.
                for nub in nubs.sorted(by: { (a, b) in (a.facing ? 1 : 0) < (b.facing ? 1 : 0) }) {
                    drawNub(ctx: ctx, nub: nub)
                }
            }
        }
        .frame(width: ballBox, height: ballBox)
        .contentShape(Rectangle())
        .onTapGesture(coordinateSpace: .local) { loc in
            let idx = bridge.navGizmoNubAt(x: loc.x, y: loc.y)
            if idx >= 0, Int(idx) < nubs.count {
                let n = nubs[Int(idx)]
                _ = bridge.snapView(toAxis: n.axis, negative: n.negative)
                reload()
            }
        }
        .help("Click an axis to snap the view down it")
    }

    // MARK: - Home / free-fly controls

    @ViewBuilder private var controls: some View {
        HStack(spacing: 6) {
            navButton(system: "house.fill", help: "Go to home view", enabled: homeSet) {
                _ = bridge.goToHomeView(); reload()
            }
            navButton(system: "mappin.and.ellipse", help: "Set current view as home", enabled: true) {
                _ = bridge.setHomeView(); reload()
            }
            if freeFly {
                navButton(system: "camera.badge.plus", help: "Stamp this view into a new camera", enabled: true) {
                    // Auto-named + dedup-suffixed by the core; the new camera
                    // becomes active. The outliner picks it up on its next poll.
                    _ = bridge.stampViewToNewCamera("view_camera")
                    reload()
                }
                navButton(system: "camera.fill", help: "Back to scene camera", enabled: true) {
                    _ = bridge.exitFreeFly(); reload()
                }
            }
        }
    }

    private func navButton(system: String, help: String, enabled: Bool,
                           action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: system)
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(.white.opacity(enabled ? 0.92 : 0.4))
                .frame(width: 26, height: 22)
                .background(Color.black.opacity(0.28))
                .clipShape(RoundedRectangle(cornerRadius: 5))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .help(help)
    }

    // MARK: - State pull + drawing

    private func reload() {
        let center = CGPoint(x: ballBox / 2, y: ballBox / 2)
        _ = bridge.refreshNavGizmo(centerX: center.x, centerY: center.y,
                                   ballRadius: ballRadius, nubRadius: nubRadius)
        nubs = bridge.navGizmoNubs
        freeFly = bridge.freeFlyActive
        homeSet = bridge.hasHomeView
    }

    private func axisColor(_ axis: Int) -> Color {
        switch axis {
        case 0:  return .red     // X
        case 1:  return .green   // Y
        case 2:  return .blue    // Z
        default: return .yellow
        }
    }

    private func drawNub(ctx: GraphicsContext, nub: RISEViewportNavNub) {
        let color = axisColor(Int(nub.axis))
        let r = nub.screenRadius
        let rect = CGRect(x: nub.screenX - r, y: nub.screenY - r, width: r * 2, height: r * 2)
        let path = Path(ellipseIn: rect)
        let dim = nub.facing ? 1.0 : 0.42

        if nub.negative {
            // Hollow ring for −axis nubs (Blender convention).
            ctx.fill(path, with: .color(Color.black.opacity(nub.facing ? 0.35 : 0.15)))
            ctx.stroke(path, with: .color(color.opacity(dim)), lineWidth: 2)
        } else {
            // Solid disc + axis letter for +axis nubs.
            ctx.fill(path, with: .color(color.opacity(dim)))
            let letter = ["X", "Y", "Z"][max(0, min(2, Int(nub.axis)))]
            let text = Text(letter)
                .font(.system(size: r * 1.05, weight: .bold))
                .foregroundColor(.white.opacity(nub.facing ? 0.95 : 0.5))
            ctx.draw(text, at: CGPoint(x: nub.screenX, y: nub.screenY))
        }
    }
}
