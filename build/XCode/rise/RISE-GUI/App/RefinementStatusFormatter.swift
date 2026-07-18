//////////////////////////////////////////////////////////////////////
//
//  RefinementStatusFormatter.swift - Shared "refinement status" text
//    mapping: (phase, scaleDivisor, renderState) -> display strings +
//    a progress-bar fraction.
//
//  Both readouts consume this one implementation: TopBar.swift's
//  `renderStatusCluster` (via its `status` computed property) and
//  ViewportView's bottom-left refinement pill — so the two can never
//  disagree at a glance.  Add any new refinement-status surface here,
//  not as a hand-copy.
//
//////////////////////////////////////////////////////////////////////

import Foundation
import CoreGraphics

enum RefinementStatusFormatter {
    struct Status {
        /// Primary readout text — "Refining · rung 3/6", "Paused",
        /// "Polishing", "Settled", "Production 42%", "Cancelling…".
        let text: String
        /// Small tracked-letter-spacing label shown beside the text.
        let label: String
        /// 0...1 — how full the mini progress bar should draw.
        let fraction: CGFloat
    }

    /// Refinement ladder rung, 1...6, derived from the preview-scale
    /// divisor (a power of two, 1...32 — 1 = full res = rung 6).
    /// Mirrors TopBar's private `rung` computed property.
    static func rung(scaleDivisor: UInt32) -> Int {
        let d = max(1, scaleDivisor)
        let log2d = Int(log2(Double(d)).rounded())
        return max(1, min(6, 6 - log2d))
    }

    /// `phase`: -1 no controller, 0 Idle/Settled, 1 Rendering (ladder),
    /// 2 Refining, 3 Polishing, 4 Paused — mirrors
    /// `RISEViewportBridge.refinementPhase(scaleDivisor:)`.
    static func status(phase: Int,
                        scaleDivisor: UInt32,
                        isProduction: Bool,
                        isCancelling: Bool,
                        productionProgress: Double,
                        isProductionPaused: Bool = false,
                        viewportRenderModeWantsDenoise: Bool = true) -> Status {
        // Label policy (user feedback 2026-07-16: "why does the rendering
        // text say things in double?"): the small tracked label is shown
        // ONLY when it adds information the primary text doesn't already
        // carry — an empty label means the consumers render nothing beside
        // the text.  Pre-fix, most states echoed themselves ("Settled
        // SETTLED", "Production 42% PRODUCTION 42%").  The one label that
        // always stays is Polishing's "DENOISED — NOT FINAL" honesty tag.
        if isCancelling {
            return Status(text: "Cancelling…", label: "",
                          fraction: CGFloat(productionProgress))
        }
        if isProduction {
            let pct = Int(productionProgress * 100)
            if isProductionPaused {
                // Item 4: workers parked at the bridge's pause gate —
                // progress honestly frozen at the pause point.  The label
                // adds WHAT is paused (a production render, not refinement).
                return Status(text: "Paused \(pct)%", label: "PRODUCTION",
                              fraction: CGFloat(productionProgress))
            }
            return Status(text: "Production \(pct)%", label: "",
                          fraction: CGFloat(productionProgress))
        }
        let r = rung(scaleDivisor: scaleDivisor)
        switch phase {
        case 4: return Status(text: "Paused", label: "", fraction: CGFloat(r) / 6.0)
        case 2: return Status(text: "Refining · rung \(r)/6", label: "", fraction: CGFloat(r) / 6.0)
        case 1: return Status(text: "Rendering", label: "", fraction: CGFloat(r) / 6.0)
        case 3:
            // docs/gui/RENDER_MODES.md §4 denoise policy: data modes
            // (normals/depth/facets/wireframe) are wantsDenoise=false —
            // the caster-swap machinery never queues a polish/denoise
            // pass for them, so Polishing shouldn't normally even be
            // reachable outside "preview" and the BeautyVariant modes
            // (deep_reflect/direct, GUI render modes P2a -- these DO
            // genuinely denoise, via their own ephemeral pipeline's fixed
            // OIDN config, not the polish-pass machinery, but the label
            // should still read DENOISED for them). Gate the honesty label
            // on the registry's wantsDenoise flag rather than a hardcoded
            // mode-name comparison: labeling a non-denoised data-mode pass
            // "DENOISED" would be exactly the kind of lying status chrome
            // this pill exists to avoid, and the inverse (omitting the
            // label for a mode that DOES denoise) under-claims.
            let label = viewportRenderModeWantsDenoise ? "DENOISED — NOT FINAL" : ""
            return Status(text: "Polishing", label: label, fraction: 1.0)
        case 0: return Status(text: "Settled", label: "", fraction: 1.0)
        default: return Status(text: "—", label: "", fraction: 0)
        }
    }
}
